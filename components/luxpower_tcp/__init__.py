"""ESPHome external component: Luxpower inverter monitor over the WiFi dongle (LXP/TCP).

Ports the protocol used by the original VLux-Monitor firmware:
  - connects to the Luxpower dongle via TCP
  - sends ReadInput requests for register banks 0-39, 40-79, 160-170
  - parses the TranslatedData (0xC2) replies into input registers
  - exposes the decoded values as ESPHome sensors
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, binary_sensor, text_sensor, socket as socket_ns
from esphome.const import (
    CONF_ID,
    CONF_PORT,
    DEVICE_CLASS_CONNECTIVITY,
    CONF_UPDATE_INTERVAL,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_VOLTAGE,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_WATT,
    UNIT_VOLT,
    UNIT_PERCENT,
    UNIT_HERTZ,
    UNIT_KILOWATT_HOURS,
    UNIT_CELSIUS,
)

CODEOWNERS = ["@vlux"]
DEPENDENCIES = ["network", "socket"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor"]
MULTI_CONF = True

CONF_HOST = "host"
CONF_ONLINE = "online"
CONF_DONGLE_SERIAL = "dongle_serial"
CONF_INVERTER_SERIAL = "inverter_serial"
CONF_AUTO_DISCOVER = "auto_discover"
CONF_DONGLE_IP_TEXT = "dongle_ip_text"
CONF_DONGLE_SN_TEXT = "dongle_sn_text"
CONF_INVERTER_SN_TEXT = "inverter_sn_text"

# Số kết nối thử song song khi quét dải (phải khớp SCAN_SLOTS trong .h)
SCAN_SLOTS = 4

luxpower_ns = cg.esphome_ns.namespace("luxpower_tcp")
LuxpowerTcp = luxpower_ns.class_("LuxpowerTcp", cg.PollingComponent)

# (yaml key, factory setter, unit, device_class, state_class, accuracy decimals)
SENSORS = {
    "pv_power":          ("set_pv_power",          UNIT_WATT,           DEVICE_CLASS_POWER,       STATE_CLASS_MEASUREMENT, 0),
    "pv1_power":         ("set_pv1_power",         UNIT_WATT,           DEVICE_CLASS_POWER,       STATE_CLASS_MEASUREMENT, 0),
    "pv2_power":         ("set_pv2_power",         UNIT_WATT,           DEVICE_CLASS_POWER,       STATE_CLASS_MEASUREMENT, 0),
    "pv3_power":         ("set_pv3_power",         UNIT_WATT,           DEVICE_CLASS_POWER,       STATE_CLASS_MEASUREMENT, 0),
    "battery_voltage":   ("set_battery_voltage",   UNIT_VOLT,           DEVICE_CLASS_VOLTAGE,     STATE_CLASS_MEASUREMENT, 1),
    "battery_soc":       ("set_battery_soc",       UNIT_PERCENT,        DEVICE_CLASS_BATTERY,     STATE_CLASS_MEASUREMENT, 0),
    "battery_power":     ("set_battery_power",     UNIT_WATT,           DEVICE_CLASS_POWER,       STATE_CLASS_MEASUREMENT, 0),
    "grid_power":        ("set_grid_power",        UNIT_WATT,           DEVICE_CLASS_POWER,       STATE_CLASS_MEASUREMENT, 0),
    "load_power":        ("set_load_power",        UNIT_WATT,           DEVICE_CLASS_POWER,       STATE_CLASS_MEASUREMENT, 0),
    "grid_voltage":      ("set_grid_voltage",      UNIT_VOLT,           DEVICE_CLASS_VOLTAGE,     STATE_CLASS_MEASUREMENT, 1),
    "grid_freq":         ("set_grid_freq",         UNIT_HERTZ,          DEVICE_CLASS_FREQUENCY,   STATE_CLASS_MEASUREMENT, 2),
    "pv_today":          ("set_pv_today",          UNIT_KILOWATT_HOURS, DEVICE_CLASS_ENERGY,      STATE_CLASS_TOTAL_INCREASING, 1),
    "charge_today":      ("set_charge_today",      UNIT_KILOWATT_HOURS, DEVICE_CLASS_ENERGY,      STATE_CLASS_TOTAL_INCREASING, 1),
    "discharge_today":   ("set_discharge_today",   UNIT_KILOWATT_HOURS, DEVICE_CLASS_ENERGY,      STATE_CLASS_TOTAL_INCREASING, 1),
    "grid_import_today": ("set_grid_import_today", UNIT_KILOWATT_HOURS, DEVICE_CLASS_ENERGY,      STATE_CLASS_TOTAL_INCREASING, 1),
    "grid_export_today": ("set_grid_export_today", UNIT_KILOWATT_HOURS, DEVICE_CLASS_ENERGY,      STATE_CLASS_TOTAL_INCREASING, 1),
    "temp_inner":        ("set_temp_inner",        UNIT_CELSIUS,        DEVICE_CLASS_TEMPERATURE, STATE_CLASS_MEASUREMENT, 1),
    "frames_total":      ("set_frames_total",      "",                  None,                     STATE_CLASS_MEASUREMENT, 0),
    "temp_radiator":     ("set_temp_radiator",     UNIT_CELSIUS,        DEVICE_CLASS_TEMPERATURE, STATE_CLASS_MEASUREMENT, 1),
}


def _serial_or_empty(value):
    """Serial 10 ký tự, hoặc để trống để component tự học từ frame dongle."""
    value = cv.string_strict(value)
    if value == "":
        return value
    if len(value) != 10:
        raise cv.Invalid(
            "Luxpower serial must be exactly 10 characters (or omitted to auto-learn)"
        )
    return value


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LuxpowerTcp),
            # Tất cả đều tùy chọn: để trống -> tự dò IP và tự học serial
            cv.Optional(CONF_HOST, default=""): cv.string,
            cv.Optional(CONF_PORT, default=8000): cv.port,
            cv.Optional(CONF_AUTO_DISCOVER, default=True): cv.boolean,
            cv.Optional(CONF_DONGLE_SERIAL, default=""): _serial_or_empty,
            cv.Optional(CONF_INVERTER_SERIAL, default=""): _serial_or_empty,
            cv.Optional(CONF_ONLINE): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
            ),
            cv.Optional(CONF_DONGLE_IP_TEXT): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_DONGLE_SN_TEXT): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_INVERTER_SN_TEXT): text_sensor.text_sensor_schema(),
        }
    )
    .extend(
        {
            cv.Optional(key): sensor.sensor_schema(
                **{
                    k: v
                    for k, v in {
                        "unit_of_measurement": unit,
                        "device_class": dclass,
                        "state_class": sclass,
                        "accuracy_decimals": acc,
                    }.items()
                    if v is not None
                }
            )
            for key, (_, unit, dclass, sclass, acc) in SENSORS.items()
        }
    )
    .extend(cv.polling_component_schema("5s"))
    # Khai báo nhu cầu socket để ESPHome nâng CONFIG_LWIP_MAX_SOCKETS.
    # Nếu không khai, pool socket bị các component khác dùng hết -> không tạo nổi
    # socket để dò/kết nối dongle.
    .add_extra(
        # SCAN_SLOTS khi quét + 1 socket dữ liệu, cộng 4 socket dự phòng để
        # web_server/logger không bị đói khi có nhiều kết nối SSE cùng lúc
        # (ESPHome tính CONFIG_LWIP_MAX_SOCKETS theo tổng khai báo của các component).
        socket_ns.consume_sockets(
            SCAN_SLOTS + 1 + 4, "luxpower_tcp", socket_ns.SocketType.TCP
        )
    )
    .add_extra(
        socket_ns.consume_sockets(1, "luxpower_tcp", socket_ns.SocketType.UDP)
    )
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_dongle_serial(config[CONF_DONGLE_SERIAL]))
    cg.add(var.set_inverter_serial(config[CONF_INVERTER_SERIAL]))

    cg.add(var.set_auto_discover(config[CONF_AUTO_DISCOVER]))

    if CONF_ONLINE in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_ONLINE])
        cg.add(var.set_online(bs))

    for key, setter in (
        (CONF_DONGLE_IP_TEXT, "set_dongle_ip"),
        (CONF_DONGLE_SN_TEXT, "set_dongle_sn_text"),
        (CONF_INVERTER_SN_TEXT, "set_inverter_sn_text"),
    ):
        if key in config:
            ts = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, setter)(ts))

    for key, (setter, *_rest) in SENSORS.items():
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))
