#include "ethernet_info_text_sensor.h"
#include "esphome/core/log.h"

#ifdef USE_ETHERNET

namespace esphome::ethernet_info {

static const char *const TAG = "ethernet_info";

#ifdef USE_ETHERNET_IP_STATE_LISTENERS
void IPAddressEthernetInfo::setup() { ethernet::global_eth_component->add_ip_state_listener(this); }

void IPAddressEthernetInfo::dump_config() { LOG_TEXT_SENSOR("", "EthernetInfo IPAddress", this); }

// esphome-ipv6-only: upstream publishes ips[0], which is always the IPv4 slot and
// therefore "0.0.0.0" on an IPv6-only node. Publish the most useful address
// instead: IPv4 if set, else the first routable IPv6 address, else the first
// address that is set at all (the link-local). The address_N sub-sensors are
// unchanged: they already list only the addresses that are set.
static const network::IPAddress &pick_primary_address(const network::IPAddresses &ips) {
  if (ips[0].is_set())
    return ips[0];
#ifdef USE_ESP32
  for (const auto &ip : ips) {
    if (ip.is_set() && ip.is_ip6()) {
      const ip_addr_t addr = ip;
      if (!ip6_addr_islinklocal(ip_2_ip6(&addr)))
        return ip;
    }
  }
#endif
  for (const auto &ip : ips) {
    if (ip.is_set())
      return ip;
  }
  return ips[0];
}

void IPAddressEthernetInfo::on_ip_state(const network::IPAddresses &ips, const network::IPAddress &dns1,
                                        const network::IPAddress &dns2) {
  char buf[network::IP_ADDRESS_BUFFER_SIZE];
  pick_primary_address(ips).str_to(buf);
  this->publish_state(buf);
  uint8_t sensor = 0;
  for (const auto &ip : ips) {
    if (ip.is_set()) {
      if (this->ip_sensors_[sensor] != nullptr) {
        ip.str_to(buf);
        this->ip_sensors_[sensor]->publish_state(buf);
      }
      sensor++;
    }
  }
}

void DNSAddressEthernetInfo::setup() { ethernet::global_eth_component->add_ip_state_listener(this); }

void DNSAddressEthernetInfo::dump_config() { LOG_TEXT_SENSOR("", "EthernetInfo DNS Address", this); }

void DNSAddressEthernetInfo::on_ip_state(const network::IPAddresses &ips, const network::IPAddress &dns1,
                                         const network::IPAddress &dns2) {
  // IP_ADDRESS_BUFFER_SIZE (40) = max IP (39) + null; space reuses first null's slot
  char buf[network::IP_ADDRESS_BUFFER_SIZE * 2];
  dns1.str_to(buf);
  size_t len1 = strlen(buf);
  buf[len1] = ' ';
  dns2.str_to(buf + len1 + 1);
  this->publish_state(buf);
}
#endif  // USE_ETHERNET_IP_STATE_LISTENERS

void MACAddressEthernetInfo::dump_config() { LOG_TEXT_SENSOR("", "EthernetInfo MAC Address", this); }

}  // namespace esphome::ethernet_info

#endif  // USE_ETHERNET
