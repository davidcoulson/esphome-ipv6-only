#include "gateway_watchdog.h"

#ifdef USE_ESP32

#include <cmath>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/network/util.h"

#include "esp_netif.h"
#if LWIP_IPV6
// default_router_list[]: lwIP keeps the ND6 default routers in a private
// table with no public accessor. ESP-IDF ships the header.
#include "lwip/priv/nd6_priv.h"
#endif

namespace esphome {
namespace gateway_watchdog {

static const char *const TAG = "gateway_watchdog";

// If loop() itself is starved for longer than this multiple of the ping
// interval, the gap is credited rather than counted against the gateway.
// An OTA write blocks the main loop for seconds at a time; without this,
// the first loop() afterwards would see a > reboot_window gap and reboot
// a perfectly healthy node.
static const uint32_t STALL_FACTOR = 4;

// A live esp_ping session emits a callback - reply or timeout - every
// ping_interval. Going this many intervals with NO callback at all means the
// session has stopped working (socket error, netif torn down and rebuilt by a
// reconnect), not that the gateway is down. Rebuild rather than reboot.
static const uint32_t CALLBACK_STALL_FACTOR = 6;

static void ping_success_cb(esp_ping_handle_t hdl, void *args) {
  uint32_t elapsed = 0;
  esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
  static_cast<GatewayWatchdog *>(args)->on_reply(elapsed);
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args) {
  static_cast<GatewayWatchdog *>(args)->on_timeout();
}

void GatewayWatchdog::on_reply(uint32_t elapsed_ms) {
  this->last_callback_ms_ = millis();
  this->last_reply_ms_ = millis();
  this->replies_ = this->replies_ + 1;
  this->rtt_sum_ms_ = this->rtt_sum_ms_ + elapsed_ms;
  this->armed_ = true;
}

void GatewayWatchdog::on_timeout() {
  // Deliberately also stamps last_callback_ms_: a timeout proves the session
  // is alive and doing its job. Only silence means the session is gone.
  this->last_callback_ms_ = millis();
  this->timeouts_ = this->timeouts_ + 1;
}

bool GatewayWatchdog::resolve_target_(ip_addr_t *out) {
  if (this->target_str_ != nullptr) {
    // ipaddr_aton() parses both IPv4 and IPv6 literals.
    return ipaddr_aton(this->target_str_, out) == 1;
  }
  // No explicit target: follow the default gateway, so the same config works
  // on every VLAN and re-targets if the lease or the router changes.
  //
  // esp_netif_get_default_netif() rather than a hardcoded "WIFI_STA_DEF"
  // key: that key does not exist on an Ethernet-only node, and asking for
  // the current default route is the right question on a node that has
  // both. Returns whichever interface actually carries the default route.
  esp_netif_t *netif = esp_netif_get_default_netif();
  if (netif == nullptr)
    return false;
  esp_netif_ip_info_t info;
  if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.gw.addr != 0) {
    ip_addr_set_ip4_u32_val(*out, info.gw.addr);
    return true;
  }
#if LWIP_IPV6
  // No IPv4 gateway (no DHCPv4 lease, e.g. an IPv6-only network): watch the
  // first live IPv6 default router learned from router advertisements. Its
  // address is the router's link-local, which is exactly what the node
  // forwards through, so ICMPv6 echo to it is the same reachability test.
  {
    LwIPLock lock;
    for (int i = 0; i < LWIP_ND6_NUM_ROUTERS; i++) {
      const auto &router = default_router_list[i];
      if (router.neighbor_entry != nullptr && router.invalidation_timer > 0) {
        ip_addr_copy_from_ip6(*out, router.neighbor_entry->next_hop_address);
        return true;
      }
    }
  }
#endif
  return false;
}

void GatewayWatchdog::stop_session_() {
  if (this->handle_ != nullptr) {
    esp_ping_stop(this->handle_);
    esp_ping_delete_session(this->handle_);
    this->handle_ = nullptr;
  }
  this->armed_ = false;
}

bool GatewayWatchdog::start_session_(const ip_addr_t &addr) {
  this->stop_session_();

  // esp_ping picks ICMP or ICMPv6 from the address type.
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr = addr;
  cfg.count = ESP_PING_COUNT_INFINITE;
  cfg.interval_ms = this->ping_interval_;
  cfg.timeout_ms = this->ping_timeout_;

  esp_ping_callbacks_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.cb_args = this;
  cbs.on_ping_success = ping_success_cb;
  cbs.on_ping_timeout = ping_timeout_cb;

  if (esp_ping_new_session(&cfg, &cbs, &this->handle_) != ESP_OK) {
    this->handle_ = nullptr;
    ESP_LOGW(TAG, "esp_ping_new_session failed");
    return false;
  }
  if (esp_ping_start(this->handle_) != ESP_OK) {
    esp_ping_delete_session(this->handle_);
    this->handle_ = nullptr;
    ESP_LOGW(TAG, "esp_ping_start failed");
    return false;
  }

  this->target_addr_ = addr;
  this->last_reply_ms_ = millis();
  this->last_callback_ms_ = millis();
  this->session_rebuilt_for_window_ = false;
  ESP_LOGI(TAG, "watching %s", ipaddr_ntoa(&addr));
  return true;
}

void GatewayWatchdog::setup() {
  this->last_loop_ms_ = millis();

  // Restore the reboot budget. It has to outlive a reboot to mean anything.
  this->pref_ = global_preferences->make_preference<uint32_t>(fnv1_hash("gateway_watchdog_reboots"));
  uint32_t stored = 0;
  if (this->pref_.load(&stored))
    this->reboots_used_ = stored;
  if (this->reboots_used_ > 0)
    ESP_LOGW(TAG, "restored reboot budget: %" PRIu32 " of %" PRIu32 " used",
             this->reboots_used_, this->max_reboots_);
}

void GatewayWatchdog::save_budget_() {
  this->pref_.save(&this->reboots_used_);
  global_preferences->sync();
}

void GatewayWatchdog::loop() {
  const uint32_t now = millis();
  const uint32_t stall_limit = this->ping_interval_ * STALL_FACTOR;

  // Nothing at all until the node has settled. A freshly booted node is
  // still bringing up WiFi, DHCP and mDNS; starting a ping session into
  // that churn invites a broken socket, and treating the churn as an
  // outage would reboot a node that is merely still starting.
  //
  // Keep the stall clock running while we wait. setup() stamps last_loop_ms_,
  // and returning here without refreshing it meant the first armed loop() saw
  // the whole arm delay as a gap: every node logged "loop starved ~59700 ms"
  // on every boot (arm_delay minus setup time). Waiting on purpose is not a
  // stall, and a false warning there hides the real one an OTA can cause.
  if (now < this->arm_delay_) {
    this->last_loop_ms_ = now;
    return;
  }

  // Reward a long healthy run by returning the budget. Without this the
  // cap is one-way and a node that misbehaved once months ago would never
  // be allowed to protect itself again.
  if (!this->budget_reset_done_ && this->reboots_used_ > 0 && this->armed_ &&
      now > this->arm_delay_ + this->budget_reset_after_) {
    ESP_LOGW(TAG, "healthy for %" PRIu32 " ms - resetting reboot budget",
             this->budget_reset_after_);
    this->reboots_used_ = 0;
    this->budget_reset_done_ = true;
    this->save_budget_();
    if (this->reboots_used_sensor_ != nullptr)
      this->reboots_used_sensor_->publish_state(0);
  }

  if (this->last_loop_ms_ != 0 && (now - this->last_loop_ms_) > stall_limit) {
    ESP_LOGW(TAG, "loop starved %" PRIu32 " ms - crediting window",
             now - this->last_loop_ms_);
    this->last_reply_ms_ = now;
  }
  this->last_loop_ms_ = now;

  // A down link is the network component's own problem (and, on WiFi,
  // wifi.reboot_timeout's). Overlapping them would only make the reboot
  // reason ambiguous. network::is_connected() covers WiFi and Ethernet.
  if (!network::is_connected()) {
    this->last_reply_ms_ = now;
    return;
  }

  ip_addr_t addr;
  if (!this->resolve_target_(&addr)) {
    this->last_reply_ms_ = now;
    return;
  }

  if (this->handle_ == nullptr || !ip_addr_cmp(&addr, &this->target_addr_)) {
    this->start_session_(addr);
    return;
  }

  // The session should be emitting a callback every ping_interval, reply or
  // timeout. Total silence means the session died rather than the gateway.
  // Rebuild it; start_session_ clears armed_, so the node must reach the
  // gateway again before a reboot is even on the table.
  const uint32_t silence = now - this->last_callback_ms_;
  if (silence > this->ping_interval_ * CALLBACK_STALL_FACTOR) {
    ESP_LOGW(TAG, "no ping callbacks for %" PRIu32 " ms - rebuilding session", silence);
    this->start_session_(addr);
    return;
  }

  // Never reboot on a target we have not reached even once. A node that
  // has never seen its gateway (wrong VLAN, config sent to the wrong
  // device) must not sit in a reboot loop.
  if (!this->armed_)
    return;

  // End the outage the last rebuild was spent on. start_session_ clears the
  // flag, but the caller sets it straight back, so without this it stayed set
  // after a recovery and a separate outage days later rebooted after a single
  // window - no rebuild at all, contrary to the comment further down.
  //
  // "Recovered" = the gateway answered for longer than a reboot window after
  // the rebuild. Not just "armed again": re-arming on the new session is the
  // precondition for the reboot below, so clearing on it would mean never
  // rebooting. The loop-stall and link-down credits also move last_reply_ms_,
  // which can only err towards another rebuild instead of a reboot.
  if (this->session_rebuilt_for_window_ &&
      (this->last_reply_ms_ - this->rebuilt_at_ms_) > this->reboot_window_)
    this->session_rebuilt_for_window_ = false;

  const uint32_t since = now - this->last_reply_ms_;
  if (since <= this->reboot_window_)
    return;

  if (!this->reboot_enabled_) {
    ESP_LOGE(TAG, "gateway unreachable %" PRIu32 " ms (reboot disabled)", since);
    this->last_reply_ms_ = now;
    return;
  }

  // A window expired with a session that looked alive. Before power-cycling
  // whatever this node drives, spend one more window on a freshly built
  // session: if the old one was subtly broken, the rebuild fixes it and the
  // node re-arms instead of rebooting. Only a second full window - on a
  // session we just created, having reached the gateway since - reboots.
  if (!this->session_rebuilt_for_window_) {
    ESP_LOGW(TAG, "gateway unreachable %" PRIu32 " ms - rebuilding session before "
                  "considering a reboot", since);
    this->start_session_(addr);
    this->session_rebuilt_for_window_ = true;
    this->rebuilt_at_ms_ = now;
    return;
  }

  // The budget. Everything above tries to avoid a needless reboot; this is
  // the backstop that holds even if all of it is wrong about the cause. Two
  // reboots is recovery; the third is a node stuck in a loop, and a node
  // sitting up and reporting 100% loss is far more useful than one power
  // cycling every few minutes.
  // max_reboots: 0 means UNLIMITED - keep rebooting for as long as the
  // gateway stays unreachable. Use it when a node is more useful cycling
  // than sitting unreachable, and accept that a mistaken diagnosis then
  // costs an unbounded reboot loop. Any value above 0 caps it.
  if (this->max_reboots_ > 0 && this->reboots_used_ >= this->max_reboots_) {
    ESP_LOGE(TAG, "gateway unreachable %" PRIu32 " ms but reboot budget spent "
                  "(%" PRIu32 "/%" PRIu32 ") - staying up and reporting",
             since, this->reboots_used_, this->max_reboots_);
    this->last_reply_ms_ = now;  // don't spin on this branch every loop
    return;
  }

  this->reboots_used_ = this->reboots_used_ + 1;
  this->save_budget_();
  ESP_LOGE(TAG, "gateway unreachable %" PRIu32 " ms after session rebuild - rebooting "
                "(%" PRIu32 "/%" PRIu32 " of budget)",
           since, this->reboots_used_, this->max_reboots_);
  App.safe_reboot();
}

void GatewayWatchdog::update() {
  const uint32_t replies = this->replies_;
  const uint32_t timeouts = this->timeouts_;
  const uint32_t rtt_sum = this->rtt_sum_ms_;
  this->replies_ = 0;
  this->timeouts_ = 0;
  this->rtt_sum_ms_ = 0;

  const uint32_t total = replies + timeouts;

#ifdef USE_SENSOR
  if (this->packet_loss_sensor_ != nullptr) {
    // No echo requests completed in this window at all (session not up
    // yet, or WiFi down) - publishing 0% would read as "healthy".
    if (total == 0) {
      this->packet_loss_sensor_->publish_state(NAN);
    } else {
      this->packet_loss_sensor_->publish_state(100.0f * (float) timeouts / (float) total);
    }
  }
  if (this->reboots_used_sensor_ != nullptr)
    this->reboots_used_sensor_->publish_state((float) this->reboots_used_);
  if (this->round_trip_time_sensor_ != nullptr) {
    if (replies == 0) {
      this->round_trip_time_sensor_->publish_state(NAN);
    } else {
      this->round_trip_time_sensor_->publish_state((float) rtt_sum / (float) replies);
    }
  }
#endif
}

void GatewayWatchdog::dump_config() {
  ESP_LOGCONFIG(TAG, "Gateway Watchdog:");
  if (this->target_str_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Target: %s (static)", this->target_str_);
  } else {
    ESP_LOGCONFIG(TAG, "  Target: default gateway (DHCPv4, else IPv6 default router)");
  }
  ESP_LOGCONFIG(TAG, "  Ping interval: %" PRIu32 " ms", this->ping_interval_);
  ESP_LOGCONFIG(TAG, "  Ping timeout: %" PRIu32 " ms", this->ping_timeout_);
  ESP_LOGCONFIG(TAG, "  Reboot window: %" PRIu32 " ms", this->reboot_window_);
  ESP_LOGCONFIG(TAG, "  Reboot enabled: %s", YESNO(this->reboot_enabled_));
  if (this->max_reboots_ == 0) {
    ESP_LOGCONFIG(TAG, "  Reboot budget: UNLIMITED (%" PRIu32 " used so far)", this->reboots_used_);
  } else {
    ESP_LOGCONFIG(TAG, "  Reboot budget: %" PRIu32 " used of %" PRIu32,
                  this->reboots_used_, this->max_reboots_);
  }
  ESP_LOGCONFIG(TAG, "  Budget resets after: %" PRIu32 " ms healthy", this->budget_reset_after_);
  ESP_LOGCONFIG(TAG, "  Arm delay: %" PRIu32 " ms", this->arm_delay_);
}

}  // namespace gateway_watchdog
}  // namespace esphome

#endif  // USE_ESP32
