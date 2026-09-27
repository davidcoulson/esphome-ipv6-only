"""Gateway reachability watchdog for ESPHome.

Works on both WiFi and Ethernet nodes: it depends on ``network`` rather
than ``wifi``, resolves the gateway from whichever netif is currently the
default route, and uses ``network::is_connected()`` for link state.

See README.md for the rationale; the short version is that ESPHome's
``wifi: reboot_timeout:`` is an *association* watchdog, not a
*reachability* one, so a node that stays associated to its AP while its
VLAN/gateway/uplink is dead will never recover on its own.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@davidcoulson"]
DEPENDENCIES = ["network"]
MULTI_CONF = False

gateway_watchdog_ns = cg.esphome_ns.namespace("gateway_watchdog")
GatewayWatchdog = gateway_watchdog_ns.class_("GatewayWatchdog", cg.PollingComponent)

CONF_GATEWAY_WATCHDOG_ID = "gateway_watchdog_id"
CONF_TARGET = "target"
CONF_REBOOT_WINDOW = "reboot_window"
CONF_PING_INTERVAL = "ping_interval"
CONF_PING_TIMEOUT = "ping_timeout"
CONF_REBOOT = "reboot"
CONF_MAX_REBOOTS = "max_reboots"
CONF_BUDGET_RESET_AFTER = "budget_reset_after"
CONF_ARM_DELAY = "arm_delay"


def _validate(config):
    """Reject combinations that would make the watchdog misbehave."""
    timeout_ms = config[CONF_PING_TIMEOUT].total_milliseconds
    interval_ms = config[CONF_PING_INTERVAL].total_milliseconds
    window_ms = config[CONF_REBOOT_WINDOW].total_milliseconds

    if timeout_ms >= interval_ms:
        raise cv.Invalid(
            f"{CONF_PING_TIMEOUT} ({timeout_ms}ms) must be shorter than "
            f"{CONF_PING_INTERVAL} ({interval_ms}ms), otherwise requests overlap "
            f"and a merely slow gateway looks like a dead one."
        )
    # Demand real evidence before a reboot: at least three echo requests
    # must have had the chance to fail inside the window.
    if window_ms < interval_ms * 3:
        raise cv.Invalid(
            f"{CONF_REBOOT_WINDOW} ({window_ms}ms) must be at least 3x "
            f"{CONF_PING_INTERVAL} ({interval_ms}ms) so a reboot is never "
            f"triggered by one or two dropped echo requests."
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(GatewayWatchdog),
            # Omit to track the default gateway (the DHCPv4 one, or on an
            # IPv6-only network the RA default router), which is what makes
            # one include work unmodified across every VLAN.
            cv.Optional(CONF_TARGET): cv.Any(cv.ipv4address, cv.ipv6address),
            cv.Optional(
                CONF_REBOOT_WINDOW, default="300s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_PING_INTERVAL, default="5s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_PING_TIMEOUT, default="2s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_REBOOT, default=True): cv.boolean,
            # Cap on reboots - the backstop that makes a reboot loop
            # impossible even if every other safeguard misjudges the cause.
            #
            #   N > 0  cap at N, then stay up and report (recommended)
            #   0      UNLIMITED - keep rebooting until the gateway returns
            #
            # Use reboot: false to disable rebooting entirely. Unlimited is
            # the right choice when an unreachable node is useless anyway and
            # you would rather it kept trying; the cost is that a wrong
            # diagnosis becomes an unbounded loop, which is exactly what took
            # out ~40 nodes on 2026-09-12.
            cv.Optional(CONF_MAX_REBOOTS, default=2): cv.int_range(min=0, max=100),
            cv.Optional(
                CONF_BUDGET_RESET_AFTER, default="1h"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_ARM_DELAY, default="60s"
            ): cv.positive_time_period_milliseconds,
        }
    ).extend(cv.polling_component_schema("60s")),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_TARGET in config:
        cg.add(var.set_target_str(str(config[CONF_TARGET])))
    cg.add(var.set_reboot_window(config[CONF_REBOOT_WINDOW]))
    cg.add(var.set_ping_interval(config[CONF_PING_INTERVAL]))
    cg.add(var.set_ping_timeout(config[CONF_PING_TIMEOUT]))
    cg.add(var.set_reboot_enabled(config[CONF_REBOOT]))
    cg.add(var.set_max_reboots(config[CONF_MAX_REBOOTS]))
    cg.add(var.set_budget_reset_after(config[CONF_BUDGET_RESET_AFTER]))
    cg.add(var.set_arm_delay(config[CONF_ARM_DELAY]))
