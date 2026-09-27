#include "ethernet_info_text_sensor.h"
#include "esphome/core/log.h"

#ifdef USE_ETHERNET

namespace esphome::ethernet_info {

static const char *const TAG = "ethernet_info";

#ifdef USE_ETHERNET_IP_STATE_LISTENERS
void IPAddressEthernetInfo::setup() { ethernet::global_eth_component->add_ip_state_listener(this); }

void IPAddressEthernetInfo::dump_config() {
  LOG_TEXT_SENSOR("", "EthernetInfo IPAddress", this);
  if (this->ignore_link_local_)
    ESP_LOGCONFIG(TAG, "  Ignore link-local: YES");
}

// esphome-ipv6-only: upstream publishes ips[0], which is always the IPv4 slot and
// therefore "0.0.0.0" on an IPv6-only node. Publish the most useful address
// instead: IPv4 if set, else the first non-link-local IPv6 address (ULA or
// global), else - unless ignore_link_local is set - the link-local address.
static bool is_link_local(const network::IPAddress &ip) {
#ifdef USE_ESP32
  if (ip.is_ip6()) {
    const ip_addr_t addr = ip;
    return ip6_addr_islinklocal(ip_2_ip6(&addr));
  }
#endif
  return false;
}

// nullptr only when ignore_link_local is set and nothing else is usable yet.
static const network::IPAddress *pick_primary_address(const network::IPAddresses &ips, bool ignore_link_local) {
  if (ips[0].is_set())
    return &ips[0];
  for (const auto &ip : ips) {
    if (ip.is_set() && !is_link_local(ip))
      return &ip;
  }
  if (ignore_link_local)
    return nullptr;
  for (const auto &ip : ips) {
    if (ip.is_set())
      return &ip;
  }
  return &ips[0];
}

void IPAddressEthernetInfo::on_ip_state(const network::IPAddresses &ips, const network::IPAddress &dns1,
                                        const network::IPAddress &dns2) {
  char buf[network::IP_ADDRESS_BUFFER_SIZE];
  const network::IPAddress *primary = pick_primary_address(ips, this->ignore_link_local_);
  if (primary != nullptr) {
    primary->str_to(buf);
  } else {
    buf[0] = '\0';  // only a link-local address so far, and it is hidden
  }
  this->publish_state(buf);
  uint8_t sensor = 0;
  for (const auto &ip : ips) {
    if (ip.is_set() && !(this->ignore_link_local_ && is_link_local(ip))) {
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
