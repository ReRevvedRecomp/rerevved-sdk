#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include <rex/port_mapping.h>

#include "core/port_mapping_internal.h"
#include "core/relay_transport.h"

namespace port_mapping = rex::port_mapping;
namespace internal = rex::port_mapping::internal;
namespace relay = rex::relay;

namespace {
#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kNoSocket = INVALID_SOCKET;
void CloseSocket(NativeSocket handle) {
  closesocket(handle);
}
#else
using NativeSocket = int;
constexpr NativeSocket kNoSocket = -1;
void CloseSocket(NativeSocket handle) {
  close(handle);
}
#endif

bool Readable(NativeSocket handle, int milliseconds) {
#if defined(_WIN32)
  fd_set ready;
  FD_ZERO(&ready);
  FD_SET(handle, &ready);
  timeval timeout{0, milliseconds * 1000};
  return select(0, &ready, nullptr, nullptr, &timeout) > 0;
#else
  pollfd descriptor{handle, POLLIN, 0};
  return poll(&descriptor, 1, milliseconds) > 0;
#endif
}

// A UPnP router on loopback that records the SOAP actions it receives.
struct FakeRouter {
  struct Behavior {
    std::string outside_address = "203.0.113.7";
    std::string service = "urn:schemas-upnp-org:service:WANIPConnection:1";
    std::set<uint16_t> taken;  // outside ports another computer holds
    std::set<uint16_t> own;    // outside ports that already map to the caller
    int add_error = 0;         // UPnP error for every AddPortMapping
    bool chunked = false;
    bool silent = false;
    bool prefixed = false;      // namespace prefixes, attributes, a relative control URL
    bool disabled = false;      // report the caller's own mappings as switched off
    std::string location_host;  // overrides the address in LOCATION
  };

  explicit FakeRouter(Behavior behavior) : behavior_(std::move(behavior)) {
    auto opened = relay::UdpSocket::Open("127.0.0.1", 0);
    if (!opened)
      return;
    search_ = std::move(*opened);
    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
#if defined(_WIN32)
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    if (listener_ == kNoSocket ||
        bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener_, 8) != 0 ||
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0)
      return;
    http_port_ = ntohs(address.sin_port);
    ready_ = true;
    ssdp_ = std::thread([this] { ServeSearches(); });
    http_ = std::thread([this] { ServeHttp(); });
  }
  FakeRouter() : FakeRouter(Behavior{}) {}
  ~FakeRouter() {
    stop_.store(true);
    if (ssdp_.joinable())
      ssdp_.join();
    if (http_.joinable())
      http_.join();
    if (listener_ != kNoSocket)
      CloseSocket(listener_);
  }
  bool ready() const { return ready_; }
  uint16_t ssdp_port() const { return search_->endpoint().port; }
  internal::Route route() const { return {search_->endpoint().ipv4, 0}; }
  std::vector<std::string> actions() {
    std::lock_guard lock(mutex_);
    return actions_;
  }
  std::string last(std::string_view action) {
    std::lock_guard lock(mutex_);
    for (auto it = bodies_.rbegin(); it != bodies_.rend(); ++it) {
      if (it->find(fmt::format("<u:{} ", action)) != std::string::npos)
        return *it;
    }
    return {};
  }

 private:
  void ServeSearches() {
    while (!stop_.load()) {
      auto received = search_->Receive(20);
      if (!received || !*received || behavior_.silent)
        continue;
      const auto& bytes = (*received)->bytes;
      if (std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())
              .find("M-SEARCH") != 0)
        continue;
      const std::string host =
          behavior_.location_host.empty() ? "127.0.0.1" : behavior_.location_host;
      const std::string reply = fmt::format(
          "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: upnp:rootdevice\r\n"
          "Location: http://{}:{}/desc.xml\r\n\r\n",
          host, http_port_);
      (void)search_->Send({(*received)->endpoint, relay::Bytes(reply.begin(), reply.end())});
    }
  }

  static std::string Field(std::string_view body, std::string_view name) {
    const std::string open = fmt::format("<{}>", name);
    const size_t start = body.find(open);
    const size_t end = body.find(fmt::format("</{}>", name));
    if (start == std::string_view::npos || end == std::string_view::npos)
      return {};
    return std::string(body.substr(start + open.size(), end - start - open.size()));
  }
  static std::string Envelope(std::string_view inner) {
    return fmt::format(
        "<?xml version=\"1.0\"?><s:Envelope "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>{}</s:Body></s:Envelope>",
        inner);
  }
  static std::string Fault(int code) {
    return Envelope(fmt::format(
        "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
        "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>{}</errorCode>"
        "</UPnPError></detail></s:Fault>",
        code));
  }

  std::pair<int, std::string> Answer(std::string_view head, std::string_view body) {
    if (head.starts_with("GET /desc.xml ") && behavior_.prefixed) {
      return {200, fmt::format("<?xml version=\"1.0\"?><d:root xmlns:d=\"urn:x\"><d:service>"
                               "<d:serviceType kind=\"wan\">{}</d:serviceType>"
                               "<d:controlURL>ctl/wan</d:controlURL></d:service></d:root>",
                               behavior_.service)};
    }
    if (head.starts_with("GET /desc.xml ")) {
      return {200, fmt::format(
                       "<?xml version=\"1.0\"?><root><device><serviceList><service>"
                       "<serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
                       "<controlURL>/l3f</controlURL></service></serviceList><deviceList><device>"
                       "<serviceList><service>\n <serviceType>{}</serviceType>\n"
                       " <controlURL>/ctl/wan</controlURL>\n</service></serviceList></device>"
                       "</deviceList></device></root>",
                       behavior_.service)};
    }
    if (!head.starts_with("POST /ctl/wan "))
      return {404, {}};
    std::string action;
    for (const char* name : {"GetExternalIPAddress", "AddPortMapping", "DeletePortMapping",
                             "GetSpecificPortMappingEntry"}) {
      if (head.find(fmt::format("{}#{}\"", behavior_.service, name)) != std::string_view::npos)
        action = name;
    }
    {
      std::lock_guard lock(mutex_);
      actions_.push_back(action);
      bodies_.emplace_back(body);
    }
    const auto port = static_cast<uint16_t>(std::atoi(Field(body, "NewExternalPort").c_str()));
    if (action == "GetExternalIPAddress")
      return {200, Envelope(fmt::format("<u:GetExternalIPAddressResponse xmlns:u=\"{0}\">"
                                        "<{1}NewExternalIPAddress>{2}</{1}NewExternalIPAddress>"
                                        "</u:GetExternalIPAddressResponse>",
                                        behavior_.service, behavior_.prefixed ? "u:" : "",
                                        behavior_.outside_address))};
    if (action == "AddPortMapping") {
      if (behavior_.add_error)
        return {500, Fault(behavior_.add_error)};
      if (behavior_.taken.contains(port) || behavior_.own.contains(port))
        return {500, Fault(718)};
      return {200, Envelope("<u:AddPortMappingResponse/>")};
    }
    if (action == "GetSpecificPortMappingEntry") {
      if (behavior_.own.contains(port))
        return {200, Envelope(fmt::format(
                         "<u:GetSpecificPortMappingEntryResponse><NewInternalPort>30100"
                         "</NewInternalPort><NewInternalClient>127.0.0.1</NewInternalClient>"
                         "<NewEnabled>{}</NewEnabled><NewLeaseDuration>0</NewLeaseDuration>"
                         "</u:GetSpecificPortMappingEntryResponse>",
                         behavior_.disabled ? 0 : 1))};
      if (behavior_.taken.contains(port))
        return {200, Envelope("<u:GetSpecificPortMappingEntryResponse><NewInternalPort>30100"
                              "</NewInternalPort><NewInternalClient>192.168.1.50"
                              "</NewInternalClient></u:GetSpecificPortMappingEntryResponse>")};
      return {500, Fault(714)};
    }
    if (action == "DeletePortMapping")
      return {200, Envelope("<u:DeletePortMappingResponse/>")};
    return {500, Fault(401)};
  }

  void ServeHttp() {
    while (!stop_.load()) {
      if (!Readable(listener_, 20))
        continue;
      const NativeSocket client = accept(listener_, nullptr, nullptr);
      if (client == kNoSocket)
        continue;
      std::string request;
      std::array<char, 4096> buffer{};
      size_t head_end = std::string::npos;
      size_t length = 0;
      while (Readable(client, 2000)) {
        const auto count = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (count <= 0)
          break;
        request.append(buffer.data(), static_cast<size_t>(count));
        if (head_end == std::string::npos) {
          head_end = request.find("\r\n\r\n");
          if (head_end == std::string::npos)
            continue;
          constexpr std::string_view name = "Content-Length: ";
          if (const size_t at = request.find(name); at != std::string::npos && at < head_end)
            length = static_cast<size_t>(std::atoi(request.c_str() + at + name.size()));
        }
        if (request.size() >= head_end + 4 + length)
          break;
      }
      if (head_end != std::string::npos) {
        const auto [status, body] = Answer(std::string_view(request).substr(0, head_end),
                                           std::string_view(request).substr(head_end + 4));
        std::string reply;
        if (behavior_.chunked && status == 200) {
          const size_t half = body.size() / 2;
          reply = fmt::format(
              "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n{:x}\r\n{}\r\n{:x}\r\n{}\r\n"
              "0\r\n\r\n",
              half, body.substr(0, half), body.size() - half, body.substr(half));
        } else {
          reply = fmt::format(
              "HTTP/1.1 {} X\r\nContent-Type: text/xml\r\nContent-Length: {}\r\n"
              "\r\n{}",
              status, body.size(), body);
        }
        send(client, reply.data(), static_cast<int>(reply.size()), 0);
      }
      CloseSocket(client);
    }
  }

  Behavior behavior_;
  std::unique_ptr<relay::UdpSocket> search_;
  NativeSocket listener_ = kNoSocket;
  uint16_t http_port_ = 0;
  bool ready_ = false;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::vector<std::string> actions_;
  std::vector<std::string> bodies_;
  std::thread ssdp_;
  std::thread http_;
};

uint32_t Address(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  const std::array<uint8_t, 4> octets{a, b, c, d};
  uint32_t address = 0;
  std::memcpy(&address, octets.data(), 4);
  return address;
}

port_mapping::Options Port(uint16_t port) {
  port_mapping::Options options;
  options.port = port;
  options.name = "Online <game> & \"friends\"";
  return options;
}

using Actions = std::vector<std::string>;
}  // namespace

TEST_CASE("UPnP asks the router itself for the port and removes it", "[port_mapping][network]") {
  std::atomic<bool> stop{false};
  for (const bool chunked : {false, true}) {
    FakeRouter::Behavior behavior;
    behavior.chunked = chunked;
    FakeRouter router(behavior);
    if (!router.ready())
      SKIP("Loopback sockets unavailable");
    internal::DirectUpnp upnp(router.ssdp_port());
    const auto result = upnp.Open(router.route(), Port(30100), stop);
    REQUIRE(result.failure == port_mapping::Failure::kNone);
    CHECK(result.grant.address == Address(203, 0, 113, 7));
    CHECK(result.grant.port == 30100);
    CHECK(result.grant.lifetime == 0);
    CHECK(router.actions() == Actions{"GetExternalIPAddress", "AddPortMapping"});
    const std::string added = router.last("AddPortMapping");
    CHECK(added.find("<NewInternalClient>127.0.0.1</NewInternalClient>") != std::string::npos);
    CHECK(added.find("<NewInternalPort>30100</NewInternalPort>") != std::string::npos);
    CHECK(added.find("<NewProtocol>UDP</NewProtocol>") != std::string::npos);
    CHECK(added.find("<NewLeaseDuration>0</NewLeaseDuration>") != std::string::npos);
    // The label cannot break out of its element.
    CHECK(added.find("Online &lt;game&gt; &amp; &quot;friends&quot;") != std::string::npos);

    upnp.Close(result.grant.port);
    CHECK(router.actions().back() == "DeletePortMapping");
    CHECK(router.last("DeletePortMapping").find("<NewExternalPort>30100</NewExternalPort>") !=
          std::string::npos);
  }
}

TEST_CASE("UPnP works with the other WAN connection services", "[port_mapping][network]") {
  std::atomic<bool> stop{false};
  for (const char* service : {"urn:schemas-upnp-org:service:WANIPConnection:2",
                              "urn:schemas-upnp-org:service:WANPPPConnection:1"}) {
    FakeRouter::Behavior behavior;
    behavior.service = service;
    FakeRouter router(behavior);
    REQUIRE(router.ready());
    internal::DirectUpnp upnp(router.ssdp_port());
    CHECK(upnp.Open(router.route(), Port(30100), stop).failure == port_mapping::Failure::kNone);
    CHECK(router.last("AddPortMapping").find(fmt::format("xmlns:u=\"{}\"", service)) !=
          std::string::npos);
  }
}

TEST_CASE("UPnP reads prefixed elements and a relative control address",
          "[port_mapping][network]") {
  std::atomic<bool> stop{false};
  FakeRouter::Behavior behavior;
  behavior.prefixed = true;
  FakeRouter router(behavior);
  REQUIRE(router.ready());
  internal::DirectUpnp upnp(router.ssdp_port());
  const auto result = upnp.Open(router.route(), Port(30100), stop);
  REQUIRE(result.failure == port_mapping::Failure::kNone);
  CHECK(result.grant.address == Address(203, 0, 113, 7));
}

TEST_CASE("UPnP does not adopt its own mapping when it is switched off",
          "[port_mapping][network]") {
  std::atomic<bool> stop{false};
  FakeRouter::Behavior behavior;
  behavior.own = {30100};
  behavior.disabled = true;
  FakeRouter router(behavior);
  REQUIRE(router.ready());
  internal::DirectUpnp upnp(router.ssdp_port());
  const auto result = upnp.Open(router.route(), Port(30100), stop);
  REQUIRE(result.failure == port_mapping::Failure::kNone);
  CHECK(result.grant.port == 30101);
}

TEST_CASE("UPnP steps past ports other computers hold", "[port_mapping][network]") {
  std::atomic<bool> stop{false};
  {
    FakeRouter::Behavior behavior;
    behavior.taken = {30100, 30101};
    FakeRouter router(behavior);
    REQUIRE(router.ready());
    internal::DirectUpnp upnp(router.ssdp_port());
    const auto result = upnp.Open(router.route(), Port(30100), stop);
    REQUIRE(result.failure == port_mapping::Failure::kNone);
    CHECK(result.grant.port == 30102);
  }
  {
    // A mapping an earlier run left that already points here is kept.
    FakeRouter::Behavior behavior;
    behavior.own = {30100};
    FakeRouter router(behavior);
    REQUIRE(router.ready());
    internal::DirectUpnp upnp(router.ssdp_port());
    const auto result = upnp.Open(router.route(), Port(30100), stop);
    REQUIRE(result.failure == port_mapping::Failure::kNone);
    CHECK(result.grant.port == 30100);
  }
  {
    FakeRouter::Behavior behavior;
    behavior.taken = {30100, 30101, 30102, 30103, 30104, 30105, 30106, 30107, 30108, 30109};
    FakeRouter router(behavior);
    REQUIRE(router.ready());
    internal::DirectUpnp upnp(router.ssdp_port());
    CHECK(upnp.Open(router.route(), Port(30100), stop).failure ==
          port_mapping::Failure::kPortsTaken);
    upnp.Close(30100);  // Nothing was opened, so nothing is removed.
    REQUIRE_FALSE(router.actions().empty());
    CHECK(router.actions().back() != "DeletePortMapping");
  }
}

TEST_CASE("UPnP reports why the router gave no port", "[port_mapping][network]") {
  std::atomic<bool> stop{false};
  {
    // 606: action not authorized.
    FakeRouter::Behavior behavior;
    behavior.add_error = 606;
    FakeRouter router(behavior);
    REQUIRE(router.ready());
    internal::DirectUpnp upnp(router.ssdp_port());
    CHECK(upnp.Open(router.route(), Port(30100), stop).failure == port_mapping::Failure::kRefused);
    CHECK(router.actions() == Actions{"GetExternalIPAddress", "AddPortMapping"});
  }
  {
    // The provider shares the outside address, so no mapping is asked for.
    FakeRouter::Behavior behavior;
    behavior.outside_address = "100.64.3.4";
    FakeRouter router(behavior);
    REQUIRE(router.ready());
    internal::DirectUpnp upnp(router.ssdp_port());
    CHECK(upnp.Open(router.route(), Port(30100), stop).failure ==
          port_mapping::Failure::kSharedAddress);
    CHECK(router.actions() == Actions{"GetExternalIPAddress"});
  }
  {
    FakeRouter::Behavior behavior;
    behavior.silent = true;
    FakeRouter router(behavior);
    REQUIRE(router.ready());
    internal::DirectUpnp upnp(router.ssdp_port());
    CHECK(upnp.Open(router.route(), Port(30100), stop).failure == port_mapping::Failure::kNoRouter);
    CHECK(router.actions().empty());
  }
  {
    // A description anywhere but on the gateway is not followed.
    FakeRouter::Behavior behavior;
    behavior.location_host = "127.0.0.2";
    FakeRouter router(behavior);
    REQUIRE(router.ready());
    internal::DirectUpnp upnp(router.ssdp_port());
    CHECK(upnp.Open(router.route(), Port(30100), stop).failure == port_mapping::Failure::kNoRouter);
    CHECK(router.actions().empty());
  }
  {
    FakeRouter router;
    REQUIRE(router.ready());
    internal::DirectUpnp upnp(router.ssdp_port());
    std::atomic<bool> stopped{true};
    CHECK(upnp.Open(router.route(), Port(30100), stopped).failure != port_mapping::Failure::kNone);
    CHECK(router.actions().empty());
  }
}
