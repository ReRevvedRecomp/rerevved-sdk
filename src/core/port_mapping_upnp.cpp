#include "port_mapping_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include <rex/logging.h>

#include "relay_transport.h"

// Every request goes to the default gateway and every answer must come from
// it, so nothing else on the network can redirect the mapping.
namespace rex::port_mapping::internal {
namespace {
using Clock = std::chrono::steady_clock;

constexpr size_t kMaxReply = 256 * 1024;
constexpr auto kExchangeTime = std::chrono::seconds(4);
constexpr std::array<uint8_t, 4> kSsdpGroup{239, 255, 255, 250};

std::array<uint8_t, 4> Octets(uint32_t address) {
  std::array<uint8_t, 4> octets{};
  std::memcpy(octets.data(), &address, octets.size());
  return octets;
}
std::string AddressText(uint32_t address) {
  const auto octets = Octets(address);
  return fmt::format("{}.{}.{}.{}", octets[0], octets[1], octets[2], octets[3]);
}
std::optional<uint32_t> ParseAddress(std::string_view text) {
  std::array<uint8_t, 4> octets{};
  const char* at = text.data();
  const char* end = text.data() + text.size();
  for (size_t i = 0; i < 4; ++i) {
    const auto parsed = std::from_chars(at, end, octets[i]);
    if (parsed.ec != std::errc{})
      return std::nullopt;
    at = parsed.ptr;
    if (i < 3 && (at == end || *at++ != '.'))
      return std::nullopt;
  }
  if (at != end)
    return std::nullopt;
  uint32_t address = 0;
  std::memcpy(&address, octets.data(), 4);
  return address;
}

std::string Lower(std::string_view text) {
  std::string lower(text);
  for (char& c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lower;
}
std::string_view Trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    text.remove_suffix(1);
  return text;
}
// Matches `tag` under any namespace prefix.
size_t OpeningTag(std::string_view xml, std::string_view tag, size_t from, std::string_view* name,
                  size_t* name_end) {
  for (size_t at = xml.find('<', from); at != std::string_view::npos; at = xml.find('<', at + 1)) {
    size_t end = at + 1;
    while (end < xml.size() && xml[end] != '>' && xml[end] != '/' &&
           !std::isspace(static_cast<unsigned char>(xml[end])))
      ++end;
    const std::string_view written = xml.substr(at + 1, end - at - 1);
    const size_t colon = written.rfind(':');
    if ((colon == std::string_view::npos ? written : written.substr(colon + 1)) != tag)
      continue;
    *name = written;
    *name_end = end;
    return at;
  }
  return std::string_view::npos;
}

bool HasElement(std::string_view xml, std::string_view tag) {
  std::string_view name;
  size_t name_end = 0;
  return OpeningTag(xml, tag, 0, &name, &name_end) != std::string_view::npos;
}

std::optional<std::string_view> Element(std::string_view xml, std::string_view tag, size_t from = 0,
                                        size_t* after = nullptr) {
  std::string_view name;
  size_t name_end = 0;
  for (size_t at = OpeningTag(xml, tag, from, &name, &name_end); at != std::string_view::npos;
       at = OpeningTag(xml, tag, at + 1, &name, &name_end)) {
    const size_t open_end = xml.find('>', name_end);
    if (open_end == std::string_view::npos)
      return std::nullopt;
    if (xml[open_end - 1] == '/')
      continue;
    const std::string close = fmt::format("</{}>", name);
    const size_t end = xml.find(close, open_end + 1);
    if (end == std::string_view::npos)
      return std::nullopt;
    if (after)
      *after = end + close.size();
    return Trim(xml.substr(open_end + 1, end - open_end - 1));
  }
  return std::nullopt;
}
std::string Escape(std::string_view text) {
  std::string escaped;
  for (const char c : text) {
    switch (c) {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      default:
        escaped += c;
    }
  }
  return escaped;
}

struct Url {
  relay::Endpoint endpoint;
  std::string path;
};
// Accepts only http://a.b.c.d[:port][/path]; a router names itself by address.
std::optional<Url> ParseUrl(std::string_view text) {
  constexpr std::string_view scheme = "http://";
  if (text.size() < scheme.size() || Lower(text.substr(0, scheme.size())) != scheme)
    return std::nullopt;
  text.remove_prefix(scheme.size());
  const size_t slash = text.find('/');
  std::string_view host = text.substr(0, slash);
  Url url;
  url.path = slash == std::string_view::npos ? "/" : std::string(text.substr(slash));
  url.endpoint.port = 80;
  if (const size_t colon = host.find(':'); colon != std::string_view::npos) {
    const std::string_view port = host.substr(colon + 1);
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), url.endpoint.port);
    if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() ||
        url.endpoint.port == 0)
      return std::nullopt;
    host = host.substr(0, colon);
  }
  const auto address = ParseAddress(host);
  if (!address)
    return std::nullopt;
  url.endpoint.ipv4 = *address;
  for (const char c : url.path) {
    if (static_cast<unsigned char>(c) <= ' ' || c == 0x7F)
      return std::nullopt;
  }
  return url;
}

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kNoSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kNoSocket = -1;
#endif

// A router that closes the connection early must not raise SIGPIPE.
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

struct Stream {
  NativeSocket handle = kNoSocket;
#if defined(_WIN32)
  bool started = false;
#endif
  Stream() {
#if defined(_WIN32)
    WSADATA data{};
    started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    if (!started)
      return;
#endif
    handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kNoSocket)
      return;
#if defined(_WIN32)
    u_long on = 1;
    if (ioctlsocket(handle, FIONBIO, &on) != 0)
      Close();
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) != 0)
      Close();
#if defined(SO_NOSIGPIPE)
    const int on = 1;
    if (handle != kNoSocket)
      setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
#endif
  }
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;
  ~Stream() {
    Close();
#if defined(_WIN32)
    if (started)
      WSACleanup();
#endif
  }
  void Close() {
    if (handle == kNoSocket)
      return;
#if defined(_WIN32)
    closesocket(handle);
#else
    close(handle);
#endif
    handle = kNoSocket;
  }
  static bool WouldBlock() {
#if defined(_WIN32)
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
#endif
  }
  // Waits in slices so a stop is noticed.
  bool Wait(bool write, Clock::time_point deadline, const std::atomic<bool>* stop) const {
    for (;;) {
      if (stop && stop->load())
        return false;
      const auto left =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (left <= 0)
        return false;
      const int slice = static_cast<int>(std::min<int64_t>(left, 100));
#if defined(_WIN32)
      fd_set ready, failed;
      FD_ZERO(&ready);
      FD_ZERO(&failed);
      FD_SET(handle, &ready);
      FD_SET(handle, &failed);
      timeval timeout{0, slice * 1000};
      const int count =
          select(0, write ? nullptr : &ready, write ? &ready : nullptr, &failed, &timeout);
      if (count < 0 || (count > 0 && FD_ISSET(handle, &failed)))
        return false;
      if (count > 0)
        return true;
#else
      pollfd descriptor{handle, static_cast<short>(write ? POLLOUT : POLLIN), 0};
      const int count = poll(&descriptor, 1, slice);
      if (count < 0 && errno != EINTR)
        return false;
      if (count > 0)
        return (descriptor.revents & (POLLERR | POLLNVAL)) == 0;
#endif
    }
  }
};

struct HttpReply {
  int status = 0;
  std::string body;
};

std::optional<std::string> Unchunk(std::string_view chunked) {
  std::string body;
  for (;;) {
    const size_t line = chunked.find("\r\n");
    if (line == std::string_view::npos)
      return std::nullopt;
    size_t size = 0;
    const auto parsed = std::from_chars(chunked.data(), chunked.data() + line, size, 16);
    if (parsed.ec != std::errc{} || parsed.ptr == chunked.data() ||
        (parsed.ptr != chunked.data() + line && *parsed.ptr != ';' && *parsed.ptr != ' '))
      return std::nullopt;
    chunked.remove_prefix(line + 2);
    if (size == 0) {
      for (;;) {
        const size_t trailer = chunked.find("\r\n");
        if (trailer == std::string_view::npos)
          return std::nullopt;
        if (trailer == 0)
          return body;
        chunked.remove_prefix(trailer + 2);
      }
    }
    if (size > chunked.size() || body.size() + size > kMaxReply)
      return std::nullopt;
    body.append(chunked.substr(0, size));
    chunked.remove_prefix(size);
    if (chunked.substr(0, 2) != "\r\n")
      return std::nullopt;
    chunked.remove_prefix(2);
  }
}

std::optional<std::string_view> Header(std::string_view head, std::string_view name) {
  const std::string line = fmt::format("\r\n{}:", name);
  const size_t at = head.find(line);
  if (at == std::string_view::npos)
    return std::nullopt;
  const size_t end = head.find("\r\n", at + line.size());
  return Trim(head.substr(at + line.size(),
                          (end == std::string_view::npos ? head.size() : end) - at - line.size()));
}
// False only for a length that cannot be read; a reply may carry none.
bool ContentLength(std::string_view head, std::optional<size_t>* length) {
  length->reset();
  const auto value = Header(head, "content-length");
  if (!value)
    return true;
  size_t number = 0;
  const auto parsed = std::from_chars(value->data(), value->data() + value->size(), number);
  if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size())
    return false;
  *length = number;
  return true;
}
bool Chunked(std::string_view head) {
  const auto value = Header(head, "transfer-encoding");
  return value && value->find("chunked") != std::string_view::npos;
}

// Skips interim replies such as 100 Continue.
struct Framed {
  int status = 0;
  std::string head;
  std::string_view rest;
};
std::optional<Framed> FinalReply(std::string_view raw) {
  for (;;) {
    const size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string_view::npos || !raw.starts_with("HTTP/1.") || raw.size() < 12)
      return std::nullopt;
    Framed framed;
    const auto parsed = std::from_chars(raw.data() + 9, raw.data() + 12, framed.status);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + 12)
      return std::nullopt;
    if (framed.status >= 100 && framed.status < 200) {
      raw.remove_prefix(head_end + 4);
      continue;
    }
    framed.head = Lower(raw.substr(0, head_end));
    framed.rest = raw.substr(head_end + 4);
    return framed;
  }
}

std::optional<HttpReply> ParseReply(std::string_view raw) {
  const auto framed = FinalReply(raw);
  if (!framed)
    return std::nullopt;
  HttpReply reply;
  reply.status = framed->status;
  if (Chunked(framed->head)) {
    auto plain = Unchunk(framed->rest);
    if (!plain)
      return std::nullopt;
    reply.body = std::move(*plain);
    return reply;
  }
  std::optional<size_t> length;
  if (!ContentLength(framed->head, &length) || (length && framed->rest.size() < *length))
    return std::nullopt;
  reply.body = std::string(framed->rest.substr(0, length.value_or(framed->rest.size())));
  return reply;
}

// Lets the exchange end without waiting for the router to close.
bool Complete(std::string_view raw) {
  const auto framed = FinalReply(raw);
  if (!framed)
    return false;
  if (Chunked(framed->head))
    return Unchunk(framed->rest).has_value();
  std::optional<size_t> length;
  return ContentLength(framed->head, &length) && length && framed->rest.size() >= *length;
}

std::optional<HttpReply> Exchange(relay::Endpoint to, std::string_view request,
                                  const std::atomic<bool>* stop, uint32_t* local = nullptr) {
  Stream stream;
  if (stream.handle == kNoSocket)
    return std::nullopt;
  const auto deadline = Clock::now() + kExchangeTime;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = to.ipv4;
  address.sin_port = htons(to.port);
  if (connect(stream.handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    if (!Stream::WouldBlock() || !stream.Wait(true, deadline, stop))
      return std::nullopt;
    int error = 0;
#if defined(_WIN32)
    int size = sizeof(error);
#else
    socklen_t size = sizeof(error);
#endif
    if (getsockopt(stream.handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) !=
            0 ||
        error != 0)
      return std::nullopt;
  }
  if (local) {
    sockaddr_in mine{};
#if defined(_WIN32)
    int size = sizeof(mine);
#else
    socklen_t size = sizeof(mine);
#endif
    if (getsockname(stream.handle, reinterpret_cast<sockaddr*>(&mine), &size) == 0)
      *local = mine.sin_addr.s_addr;
  }
  while (!request.empty()) {
    const auto sent =
        send(stream.handle, request.data(), static_cast<int>(request.size()), kSendFlags);
    if (sent > 0) {
      request.remove_prefix(static_cast<size_t>(sent));
      continue;
    }
    if (!Stream::WouldBlock() || !stream.Wait(true, deadline, stop))
      return std::nullopt;
  }
  std::string raw;
  std::array<char, 4096> buffer{};
  while (!Complete(raw)) {
    const auto count = recv(stream.handle, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (count == 0)
      break;
    if (count < 0) {
      if (!Stream::WouldBlock() || !stream.Wait(false, deadline, stop))
        return std::nullopt;
      continue;
    }
    raw.append(buffer.data(), static_cast<size_t>(count));
    if (raw.size() > kMaxReply)
      return std::nullopt;
  }
  return ParseReply(raw);
}

std::string HostHeader(relay::Endpoint endpoint) {
  return fmt::format("{}:{}", AddressText(endpoint.ipv4), endpoint.port);
}

std::vector<Url> Discover(const Route& route, uint16_t ssdp_port, const std::atomic<bool>& stop) {
  const uint32_t gateway = route.gateway;
  std::vector<Url> found;
  // A loopback gateway is a test; staying on loopback raises no firewall prompt.
  const bool loopback = Octets(gateway)[0] == 127;
  // Bound to this computer's address on the router's network, the group
  // search leaves by that adapter instead of one the system prefers.
  const std::string bind = loopback      ? "127.0.0.1"
                           : route.local ? AddressText(route.local)
                                         : "0.0.0.0";
  auto socket = relay::UdpSocket::Open(bind, 0);
  if (!socket)
    return found;
  uint32_t group = 0;
  std::memcpy(&group, kSsdpGroup.data(), 4);
  for (const uint32_t wait_ms : {1200u, 1500u}) {
    for (const std::string_view target :
         {"urn:schemas-upnp-org:device:InternetGatewayDevice:1", "upnp:rootdevice"}) {
      const auto search = [&](std::string_view host) {
        const std::string text = fmt::format(
            "M-SEARCH * HTTP/1.1\r\nHOST: {}\r\nMAN: \"ssdp:discover\"\r\n"
            "MX: 1\r\nST: {}\r\n\r\n",
            host, target);
        return relay::Bytes(text.begin(), text.end());
      };
      // Some routers answer only the group. Either answer returns to this
      // socket, which a firewall lets through as a reply.
      (void)(*socket)->Send({{gateway, ssdp_port}, search(HostHeader({gateway, ssdp_port}))});
      if (!loopback)
        (void)(*socket)->Send({{group, ssdp_port}, search("239.255.255.250:1900")});
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
      if (stop.load())
        return {};
      const auto left =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (left <= 0)
        break;
      auto received = (*socket)->Receive(static_cast<uint32_t>(std::min<int64_t>(left, 100)));
      if (!received)
        return found;
      if (!*received || (*received)->endpoint.ipv4 != gateway)
        continue;
      const auto& bytes = (*received)->bytes;
      const std::string_view reply(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      const std::string lower = Lower(reply);
      constexpr std::string_view name = "\r\nlocation:";
      const size_t at = lower.find(name);
      if (!lower.starts_with("http/1.") || at == std::string::npos)
        continue;
      const size_t end = lower.find("\r\n", at + name.size());
      const auto url = ParseUrl(Trim(reply.substr(
          at + name.size(), (end == std::string::npos ? reply.size() : end) - at - name.size())));
      if (!url || url->endpoint.ipv4 != gateway)
        continue;
      const bool known = std::any_of(found.begin(), found.end(), [&](const Url& other) {
        return other.endpoint == url->endpoint && other.path == url->path;
      });
      if (!known && found.size() < 4)
        found.push_back(*url);
    }
    if (!found.empty())
      break;
  }
  return found;
}
}  // namespace

struct DirectUpnp::Control {
  relay::Endpoint endpoint;
  std::string path;
  std::string service;
  uint32_t local = 0;
};

namespace {
constexpr std::array<std::string_view, 3> kServices{
    "urn:schemas-upnp-org:service:WANIPConnection:2",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
};

std::vector<DirectUpnp::Control> Controls(const Url& location, std::string_view description) {
  std::vector<DirectUpnp::Control> controls;
  Url base = location;
  if (const auto stated = Element(description, "URLBase")) {
    if (const auto url = ParseUrl(*stated); url && url->endpoint.ipv4 == location.endpoint.ipv4)
      base = *url;
  }
  size_t at = 0;
  while (controls.size() < 8) {
    size_t after = 0;
    const auto service = Element(description, "service", at, &after);
    if (!service)
      break;
    at = after;
    const auto type = Element(*service, "serviceType");
    const auto path = Element(*service, "controlURL");
    if (!type || !path || path->empty() ||
        std::find(kServices.begin(), kServices.end(), *type) == kServices.end())
      continue;
    DirectUpnp::Control control;
    control.service = std::string(*type);
    control.endpoint = base.endpoint;
    std::string reference(*path);
    for (size_t amp = reference.find("&amp;"); amp != std::string::npos;
         amp = reference.find("&amp;", amp + 1))
      reference.replace(amp, 5, "&");
    if (const auto absolute = ParseUrl(reference)) {
      if (absolute->endpoint.ipv4 != location.endpoint.ipv4)
        continue;
      control.endpoint = absolute->endpoint;
      control.path = absolute->path;
    } else {
      control.path =
          reference.front() == '/'
              ? reference
              : base.path.substr(0, base.path.rfind('/', base.path.find('?')) + 1) + reference;
      const auto checked =
          ParseUrl(fmt::format("http://{}{}", HostHeader(base.endpoint), control.path));
      if (!checked)
        continue;
    }
    controls.push_back(std::move(control));
  }
  return controls;
}

struct SoapReply {
  bool answered = false;
  int error = 0;  // the UPnP error code; 0 for success
  std::string body;
};
using Arguments = std::vector<std::pair<std::string_view, std::string>>;

SoapReply Soap(DirectUpnp::Control& control, std::string_view action, const Arguments& arguments,
               const std::atomic<bool>* stop) {
  std::string fields;
  for (const auto& [name, value] : arguments)
    fields += fmt::format("<{0}>{1}</{0}>", name, Escape(value));
  const std::string body = fmt::format(
      "<?xml version=\"1.0\"?>\r\n"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
      "<u:{0} xmlns:u=\"{1}\">{2}</u:{0}></s:Body></s:Envelope>\r\n",
      action, control.service, fields);
  const std::string request = fmt::format(
      "POST {} HTTP/1.1\r\nHost: {}\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
      "SOAPAction: \"{}#{}\"\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
      control.path, HostHeader(control.endpoint), control.service, action, body.size(), body);
  SoapReply reply;
  auto answer = Exchange(control.endpoint, request, stop, &control.local);
  if (!answer)
    return reply;
  reply.body = std::move(answer->body);
  if (answer->status == 200 && HasElement(reply.body, fmt::format("{}Response", action))) {
    reply.answered = true;
    return reply;
  }
  int code = 0;
  if (const auto text = Element(reply.body, "errorCode")) {
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), code);
    if (parsed.ec != std::errc{} || code <= 0)
      code = 0;
  }
  // Status 200 with neither the action's response nor an error code, such as
  // a login page or an empty body, leaves what the router did unknown.
  reply.answered = answer->status != 200 || code != 0;
  reply.error = code != 0 ? code : -1;
  return reply;
}

constexpr int kConflict = 718;  // ConflictInMappingEntry
}  // namespace

DirectUpnp::DirectUpnp(uint16_t ssdp_port) : ssdp_port_(ssdp_port) {}
DirectUpnp::~DirectUpnp() = default;

UpnpResult DirectUpnp::Open(const Route& route, const Options& options,
                            const std::atomic<bool>& stop) {
  UpnpResult result;
  control_.reset();
  std::optional<uint32_t> outside_address;
  for (const Url& location : Discover(route, ssdp_port_, stop)) {
    const auto description =
        Exchange(location.endpoint,
                 fmt::format("GET {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n",
                             location.path, HostHeader(location.endpoint)),
                 &stop);
    if (!description || description->status != 200)
      continue;
    // A router can list a service it does not use; the one in use knows the
    // outside address.
    for (Control& control : Controls(location, description->body)) {
      const auto reply = Soap(control, "GetExternalIPAddress", {}, &stop);
      if (!reply.answered || reply.error != 0)
        continue;
      const auto text = Element(reply.body, "NewExternalIPAddress");
      const auto address = text ? ParseAddress(*text) : std::nullopt;
      if (!address || *address == 0 || control.local == 0)
        continue;
      outside_address = address;
      control_ = std::make_unique<Control>(std::move(control));
      break;
    }
    if (control_)
      break;
  }
  if (!control_ || stop.load()) {
    control_.reset();
    return result;
  }
  REXLOG_DEBUG("UPnP: the router answered with a WAN connection service");
  if (IsUnreachable(*outside_address)) {
    control_.reset();
    result.failure = Failure::kSharedAddress;
    return result;
  }

  const std::string local = AddressText(control_->local);
  const std::string inside = std::to_string(options.port);
  // `usable` also requires the mapping switched on and without an end.
  const auto ours = [&](const std::string& outside, bool usable,
                        const std::atomic<bool>* stopping) {
    const auto held = Soap(
        *control_, "GetSpecificPortMappingEntry",
        {{"NewRemoteHost", ""}, {"NewExternalPort", outside}, {"NewProtocol", "UDP"}}, stopping);
    if (!held.answered || held.error != 0 ||
        Element(held.body, "NewInternalClient") != std::string_view(local) ||
        Element(held.body, "NewInternalPort") != std::string_view(inside))
      return false;
    if (!usable)
      return true;
    const std::string enabled = Lower(Element(held.body, "NewEnabled").value_or(""));
    return (enabled == "1" || enabled == "true") &&
           Element(held.body, "NewLeaseDuration") == std::string_view("0");
  };
  result.failure = Failure::kPortsTaken;
  for (uint32_t candidate = options.port;
       candidate <= std::min<uint32_t>(uint32_t{options.port} + 9, 65535); ++candidate) {
    if (stop.load())
      break;
    const std::string outside = std::to_string(candidate);
    const auto added = Soap(*control_, "AddPortMapping",
                            {{"NewRemoteHost", ""},
                             {"NewExternalPort", outside},
                             {"NewProtocol", "UDP"},
                             {"NewInternalPort", inside},
                             {"NewInternalClient", local},
                             {"NewEnabled", "1"},
                             {"NewPortMappingDescription", options.name},
                             {"NewLeaseDuration", "0"}},
                            &stop);
    bool mapped = added.answered && added.error == 0;
    if (added.answered && added.error == kConflict) {
      // Some routers refuse to replace a mapping, even one left by an
      // earlier run that already points at this computer.
      if (!ours(outside, true, &stop))
        continue;
      mapped = true;
    }
    if (mapped) {
      result.failure = Failure::kNone;
      result.grant = {*outside_address, static_cast<uint16_t>(candidate), 0};
      return result;
    }
    result.failure = Failure::kRefused;
    // Without an answer the router may have made the mapping; remove it
    // only when the router says it is this computer's.
    if (!added.answered && ours(outside, false, nullptr))
      Close(static_cast<uint16_t>(candidate));
    break;
  }
  control_.reset();
  return result;
}

void DirectUpnp::Close(uint16_t outside_port) {
  if (!control_)
    return;
  const Arguments arguments{{"NewRemoteHost", ""},
                            {"NewExternalPort", std::to_string(outside_port)},
                            {"NewProtocol", "UDP"}};
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (Soap(*control_, "DeletePortMapping", arguments, nullptr).answered)
      return;
  }
}

}  // namespace rex::port_mapping::internal
