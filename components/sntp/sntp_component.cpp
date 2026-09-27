#include "sntp_component.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include "esp_sntp.h"
#ifdef USE_NETWORK_IPV6_ONLY
#include "esphome/components/network/util.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
// lwIP's own setter (from lwip/apps/sntp.h). esp_sntp.h neither includes that
// header nor wraps this function, and its wrappers for the other sntp_* names
// would clash with the header, so declare just this one.
extern "C" void sntp_setserver(u8_t idx, const ip_addr_t *server);
#endif  // USE_NETWORK_IPV6_ONLY
#elif USE_ESP8266
#include "sntp.h"
#else
#include "lwip/apps/sntp.h"
#endif

namespace esphome::sntp {

static const char *const TAG = "sntp";

#if defined(USE_ESP32)
SNTPComponent *SNTPComponent::instance = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
#endif

void SNTPComponent::setup() {
#if defined(USE_ESP32)
  SNTPComponent::instance = this;
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
#ifndef USE_NETWORK_IPV6_ONLY
  size_t i = 0;
  for (auto &server : this->servers_) {
    esp_sntp_setservername(i++, server);
  }
#endif
  // esphome-ipv6-only: with USE_NETWORK_IPV6_ONLY the servers are resolved in loop()
  // once the network is up (see resolve_servers_()) and esp_sntp_init() runs there.
  // lwIP's own resolution inside the SNTP client is IPv4-first
  // (LWIP_DNS_ADDRTYPE_IPV4_IPV6): a pool name with A records resolves to an
  // unreachable IPv4 address and it never falls back to AAAA.
  esp_sntp_set_sync_interval(this->get_update_interval());
  esp_sntp_set_time_sync_notification_cb([](struct timeval *tv) {
    if (SNTPComponent::instance != nullptr) {
      SNTPComponent::instance->defer([]() { SNTPComponent::instance->time_synced(); });
    }
  });
#ifndef USE_NETWORK_IPV6_ONLY
  esp_sntp_init();
#endif
#else
  sntp_stop();
  sntp_setoperatingmode(SNTP_OPMODE_POLL);

  size_t i = 0;
  for (auto &server : this->servers_) {
    sntp_setservername(i++, server);
  }

#if defined(USE_ESP8266)
  settimeofday_cb([this](bool from_sntp) {
    if (from_sntp)
      this->time_synced();
  });
#endif

  sntp_init();
#endif
}
void SNTPComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "SNTP Time:");
  size_t i = 0;
  for (auto &server : this->servers_) {
    ESP_LOGCONFIG(TAG, "  Server %zu: '%s'", i++, server);
  }
  RealTimeClock::dump_config();
}
#if defined(USE_ESP32) && defined(USE_NETWORK_IPV6_ONLY)
static void sntp_dns_found(const char *name, const ip_addr_t *ipaddr, void *arg) {
  // Runs on the lwIP task with the core lock held: use the plain lwIP setter, the
  // esp_sntp_* wrapper would try to take the lock again.
  const auto idx = static_cast<u8_t>(reinterpret_cast<uintptr_t>(arg));
  if (ipaddr == nullptr) {
    ESP_LOGW(TAG, "Server %u '%s': not resolved", idx, name);
    return;
  }
  sntp_setserver(idx, ipaddr);
}

bool SNTPComponent::resolve_servers_() {
  bool resolver_ok = true;
  size_t idx = 0;
  for (auto &server : this->servers_) {
    ip_addr_t addr;
    err_t err;
    {
      // Literal addresses (IPv4 or IPv6) return ERR_OK immediately via ipaddr_aton().
      LwIPLock lock;
      err = dns_gethostbyname_addrtype(server, &addr, sntp_dns_found, reinterpret_cast<void *>(idx),
                                       LWIP_DNS_ADDRTYPE_IPV6_IPV4);
    }
    if (err == ERR_OK) {
      ESP_LOGD(TAG, "Server %zu '%s' -> %s", idx, server, ipaddr_ntoa(&addr));
      esp_sntp_setserver(idx, &addr);  // outside the lock: this takes it itself
    } else if (err == ERR_INPROGRESS) {
      ESP_LOGV(TAG, "Server %zu '%s': resolving", idx, server);
    } else {
      // ERR_VAL: no DNS server configured yet (RDNSS / DHCPv6 not arrived).
      ESP_LOGV(TAG, "Server %zu '%s': resolver not ready (%d)", idx, server, err);
      resolver_ok = false;
    }
    idx++;
  }
  return resolver_ok;
}
#endif  // USE_ESP32 && USE_NETWORK_IPV6_ONLY

void SNTPComponent::update() {
#if defined(USE_ESP32) && defined(USE_NETWORK_IPV6_ONLY)
  // Refresh the resolved addresses at each sync interval; lwIP would have
  // re-resolved a server *name* on every request, a set address is static.
  if (this->sntp_started_ && network::is_connected()) {
    this->resolve_servers_();
  }
#endif
#if !defined(USE_ESP32)
  // Some platforms currently cannot set the sync interval at runtime so we need
  // to do the re-sync by hand for now.
  if (sntp_enabled()) {
    sntp_stop();
    this->has_time_ = false;
    sntp_init();
  }
#endif
}
void SNTPComponent::loop() {
#if defined(USE_ESP32) && defined(USE_NETWORK_IPV6_ONLY)
  if (!this->sntp_started_) {
    // Wait for the network, then for a usable resolver, retrying every 5 s.
    if (!network::is_connected())
      return;
    const uint32_t now = millis();
    if (this->last_resolve_attempt_ != 0 && now - this->last_resolve_attempt_ < 5000)
      return;
    this->last_resolve_attempt_ = now;
    if (!this->resolve_servers_())
      return;
    esp_sntp_init();
    this->sntp_started_ = true;
  }
#endif
// The loop is used to infer whether we have valid time on platforms where we
// cannot tell whether SNTP has succeeded.
// One limitation of this approach is that we cannot tell if it was the SNTP
// component that set the time.
// ESP-IDF and ESP8266 use callbacks from the SNTP task to trigger the
// `on_time_sync` trigger on successful sync events.
#if defined(USE_ESP32) || defined(USE_ESP8266)
  this->disable_loop();
#endif

  if (this->has_time_)
    return;

  this->time_synced();
}

void SNTPComponent::time_synced() {
  auto time = this->now();
  this->has_time_ = time.is_valid();
  if (!this->has_time_)
    return;

  ESP_LOGD(TAG, "Synchronized time: %04d-%02d-%02d %02d:%02d:%02d", time.year, time.month, time.day_of_month, time.hour,
           time.minute, time.second);
  this->time_sync_callback_.call();
}

}  // namespace esphome::sntp
