#include "port_mapping_internal.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <iphlpapi.h>
#include <natupnp.h>
#include <netlistmgr.h>
#include <objbase.h>
#include <oleauto.h>
#include <winsvc.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include <rex/string/utf8.h>

namespace rex::port_mapping::internal {
namespace {
template <typename T>
struct Com {
  T* object = nullptr;
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  ~Com() {
    if (object)
      object->Release();
  }
  T* operator->() const { return object; }
  explicit operator bool() const { return object != nullptr; }
};

struct Text {
  BSTR text = nullptr;
  Text() = default;
  explicit Text(const std::wstring& value) : text(SysAllocString(value.c_str())) {}
  Text(const Text&) = delete;
  Text& operator=(const Text&) = delete;
  ~Text() { SysFreeString(text); }
  bool operator==(const std::wstring& other) const { return text && other == text; }
};

// The mapping thread is ours, so it can pick the apartment.
struct Apartment {
  bool entered = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  Apartment() = default;
  Apartment(const Apartment&) = delete;
  Apartment& operator=(const Apartment&) = delete;
  ~Apartment() {
    if (entered)
      CoUninitialize();
  }
};

std::wstring AddressText(uint32_t address) {
  wchar_t text[INET_ADDRSTRLEN] = {};
  in_addr value{};
  value.s_addr = address;
  InetNtopW(AF_INET, &value, text, INET_ADDRSTRLEN);
  return text;
}

// Windows can answer before it has found the router, so an empty answer is
// asked again twice, two seconds apart.
void FindMappings(Com<IStaticPortMappingCollection>& mappings, const std::atomic<bool>* stop) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    Com<IUPnPNAT> nat;
    if (FAILED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT),
                                reinterpret_cast<void**>(&nat.object))))
      return;
    if (SUCCEEDED(nat->get_StaticPortMappingCollection(&mappings.object)) && mappings)
      return;
    mappings.object = nullptr;
    if (attempt == 2)
      return;
    for (int slice = 0; slice < 20; ++slice) {
      if (stop->load())
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

UpnpResult UpnpOpen(const Route& route, const Options& options, const std::atomic<bool>& stop) {
  UpnpResult result;
  Apartment apartment;
  if (!apartment.entered || route.local == 0)
    return result;
  Com<IStaticPortMappingCollection> mappings;
  FindMappings(mappings, &stop);
  if (!mappings)
    return result;
  result.failure = Failure::kRefused;

  const std::u16string wide = rex::string::to_utf16(options.name);
  const std::wstring name(wide.begin(), wide.end());
  const std::wstring local = AddressText(route.local);
  const std::wstring udp = L"UDP";
  // Outside UDP ports held by anything but an earlier mapping of ours to this computer.
  std::set<long> taken;
  Com<IUnknown> unknown;
  Com<IEnumVARIANT> items;
  if (FAILED(mappings->get__NewEnum(&unknown.object)) || !unknown ||
      FAILED(unknown->QueryInterface(__uuidof(IEnumVARIANT),
                                     reinterpret_cast<void**>(&items.object))) ||
      !items)
    return result;
  for (;;) {
    VARIANT item;
    VariantInit(&item);
    const HRESULT next = items->Next(1, &item, nullptr);
    if (next == S_FALSE)
      break;
    // An incomplete list could hide a port that is taken.
    if (next != S_OK)
      return result;
    Com<IStaticPortMapping> mapping;
    if (item.vt == VT_DISPATCH && item.pdispVal)
      item.pdispVal->QueryInterface(__uuidof(IStaticPortMapping),
                                    reinterpret_cast<void**>(&mapping.object));
    VariantClear(&item);
    if (!mapping)
      continue;
    Text protocol, description, client;
    long outside = 0;
    if (FAILED(mapping->get_Protocol(&protocol.text)) ||
        FAILED(mapping->get_ExternalPort(&outside)))
      return result;
    if (!(protocol == udp))
      continue;
    mapping->get_Description(&description.text);
    mapping->get_InternalClient(&client.text);
    if (!(description == name && client == local))
      taken.insert(outside);
  }

  long outside = 0;
  for (long candidate = options.port; candidate <= std::min<long>(options.port + 9, 65535);
       ++candidate) {
    if (!taken.contains(candidate)) {
      outside = candidate;
      break;
    }
  }
  if (outside == 0) {
    result.failure = Failure::kPortsTaken;
    return result;
  }

  Com<IStaticPortMapping> added;
  Text address;
  if (FAILED(mappings->Add(outside, Text(udp).text, options.port, Text(local).text, VARIANT_TRUE,
                           Text(name).text, &added.object)) ||
      !added)
    return result;
  in_addr parsed{};
  if (FAILED(added->get_ExternalIPAddress(&address.text)) || !address.text ||
      InetPtonW(AF_INET, address.text, &parsed) != 1) {
    mappings->Remove(outside, Text(udp).text);
    return result;
  }
  result.failure = Failure::kNone;
  result.grant = {parsed.s_addr, static_cast<uint16_t>(outside)};
  return result;
}

void UpnpClose(uint16_t outside_port) {
  Apartment apartment;
  if (!apartment.entered)
    return;
  Com<IStaticPortMappingCollection> mappings;
  const std::atomic<bool> never{false};
  FindMappings(mappings, &never);
  if (mappings)
    mappings->Remove(outside_port, Text(L"UDP").text);
}

// An Ethernet or Wi-Fi adapter wins over any other kind, so a VPN adapter's
// lower metric does not hide the router; virtual Ethernet adapters are not
// told apart.
std::optional<Route> FindRoute() {
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buffer;
  ULONG error = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 3 && error == ERROR_BUFFER_OVERFLOW; ++attempt) {
    buffer.resize(size);
    error = GetAdaptersAddresses(AF_INET,
                                 GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                     GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                 &size);
  }
  if (error != ERROR_SUCCESS)
    return {};
  const auto ipv4 = [](const SOCKET_ADDRESS& address) -> uint32_t {
    if (!address.lpSockaddr || address.lpSockaddr->sa_family != AF_INET)
      return 0;
    return reinterpret_cast<const sockaddr_in*>(address.lpSockaddr)->sin_addr.s_addr;
  };
  std::optional<Route> best;
  bool best_physical = false;
  ULONG best_metric = 0;
  for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
       adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp)
      continue;
    Route route;
    for (auto* gateway = adapter->FirstGatewayAddress; gateway && !route.gateway;
         gateway = gateway->Next)
      route.gateway = ipv4(gateway->Address);
    for (auto* unicast = adapter->FirstUnicastAddress; unicast && !route.local;
         unicast = unicast->Next) {
      const uint32_t address = ipv4(unicast->Address);
      uint8_t octets[4];
      std::memcpy(octets, &address, 4);
      if (address && !(octets[0] == 169 && octets[1] == 254))
        route.local = address;
    }
    if (!route.gateway || !route.local)
      continue;
    const bool physical =
        adapter->IfType == IF_TYPE_ETHERNET_CSMACD || adapter->IfType == IF_TYPE_IEEE80211;
    if (!best || (physical && !best_physical) ||
        (physical == best_physical && adapter->Ipv4Metric < best_metric)) {
      best = route;
      best_physical = physical;
      best_metric = adapter->Ipv4Metric;
    }
  }
  return best;
}

// UPnP discovery on Windows needs the service "SSDP Discovery".
bool DiscoveryDisabled() {
  bool disabled = false;
  if (const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
    if (const SC_HANDLE service = OpenServiceW(manager, L"SSDPSRV", SERVICE_QUERY_CONFIG)) {
      std::vector<uint8_t> buffer(8192);
      DWORD needed = 0;
      auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
      if (QueryServiceConfigW(service, config, static_cast<DWORD>(buffer.size()), &needed))
        disabled = config->dwStartType == SERVICE_DISABLED;
      CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
  }
  return disabled;
}

// Windows hides the router from UPnP on a network set to Public.
bool PublicNetwork() {
  Apartment apartment;
  if (!apartment.entered)
    return false;
  Com<INetworkListManager> manager;
  Com<IEnumNetworks> networks;
  if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL,
                              __uuidof(INetworkListManager),
                              reinterpret_cast<void**>(&manager.object))) ||
      FAILED(manager->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, &networks.object)) || !networks)
    return false;
  for (;;) {
    Com<INetwork> network;
    ULONG fetched = 0;
    if (networks->Next(1, &network.object, &fetched) != S_OK || !network)
      return false;
    NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PRIVATE;
    if (SUCCEEDED(network->GetCategory(&category)) && category == NLM_NETWORK_CATEGORY_PUBLIC)
      return true;
  }
}
}  // namespace

Environment SystemEnvironment() {
  Environment environment;
  environment.find_route = FindRoute;
  environment.upnp_open = UpnpOpen;
  environment.upnp_close = UpnpClose;
  environment.discovery_disabled = DiscoveryDisabled;
  environment.public_network = PublicNetwork;
  return environment;
}

}  // namespace rex::port_mapping::internal
