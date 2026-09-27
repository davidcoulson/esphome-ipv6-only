"""Diagnostic sensors for the gateway watchdog.

Both default to ``internal: true`` - these are for on-device debugging
and the local web UI, not another pair of entities per node in Home
Assistant. Set ``internal: false`` explicitly on either one to surface it.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_INTERNAL,
    DEVICE_CLASS_DURATION,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_MILLISECOND,
    UNIT_PERCENT,
)

from . import CONF_GATEWAY_WATCHDOG_ID, GatewayWatchdog

DEPENDENCIES = ["gateway_watchdog"]

CONF_PACKET_LOSS = "packet_loss"
CONF_ROUND_TRIP_TIME = "round_trip_time"
CONF_REBOOTS_USED = "reboots_used"

_SENSORS = (CONF_PACKET_LOSS, CONF_ROUND_TRIP_TIME, CONF_REBOOTS_USED)


def _default_internal(config):
    for key in _SENSORS:
        if key in config:
            config[key].setdefault(CONF_INTERNAL, True)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_GATEWAY_WATCHDOG_ID): cv.use_id(GatewayWatchdog),
            cv.Optional(CONF_PACKET_LOSS): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                icon="mdi:lan-disconnect",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_ROUND_TRIP_TIME): sensor.sensor_schema(
                unit_of_measurement=UNIT_MILLISECOND,
                icon="mdi:timer-outline",
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_DURATION,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_REBOOTS_USED): sensor.sensor_schema(
                icon="mdi:restart-alert",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    ),
    cv.has_at_least_one_key(*_SENSORS),
    _default_internal,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_GATEWAY_WATCHDOG_ID])

    if CONF_PACKET_LOSS in config:
        sens = await sensor.new_sensor(config[CONF_PACKET_LOSS])
        cg.add(parent.set_packet_loss_sensor(sens))
    if CONF_ROUND_TRIP_TIME in config:
        sens = await sensor.new_sensor(config[CONF_ROUND_TRIP_TIME])
        cg.add(parent.set_round_trip_time_sensor(sens))
    if CONF_REBOOTS_USED in config:
        sens = await sensor.new_sensor(config[CONF_REBOOTS_USED])
        cg.add(parent.set_reboots_used_sensor(sens))
