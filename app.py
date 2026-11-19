import json
import math
import os
import socket
import struct
import threading
import time
from datetime import datetime
from pathlib import Path

from flask import Flask, jsonify, request, send_from_directory
import paho.mqtt.client as mqtt

BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"
HTTP_PORT = 5000
COAP_HOST = "0.0.0.0"
COAP_PORT = 5683
# Must match SERVER_IP in the ESP8266 firmware so both clients reach the same broker.
MQTT_HOST = os.getenv("MQTT_HOST", "10.176.181.174").strip()
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "iot/esp32/sensor").strip()
PROTOCOLS = ("http", "coap", "mqtt")

app = Flask(__name__, static_folder=str(STATIC_DIR))
lock = threading.RLock()
state = {
    "selected_protocol": "http",
    "running": False,
    "device_last_seen": None,
    "sensor_last_seen": None,
    "latest": None,
    "latest_by_protocol": {protocol: None for protocol in PROTOCOLS},
    "telemetry_history": [],
    "history": [],
    "metrics_history": [],
    "mqtt_last_message_at": None,
    "mqtt_message_count": 0,
    "mqtt_last_error": None,
    "mqtt_status": "connecting",
    "coap_status": "starting",
}
mqtt_client = None
services_started = False


def now_iso():
    return datetime.now().astimezone().isoformat(timespec="seconds")


def optional_finite_float(data, key):
    value = data.get(key)
    if value is None or value == "":
        return None
    value = float(value)
    if not math.isfinite(value):
        raise ValueError(f"{key} must be a finite number")
    return value


def valid_sensor_data(data):
    if not isinstance(data, dict):
        raise ValueError("JSON body must be an object")
    device_id = str(data.get("device_id", "NODEMCU_01")).strip()
    temperature = float(data["temperature"])
    humidity = float(data["humidity"])
    sequence = int(data.get("sequence", 0))
    if not device_id:
        raise ValueError("Device ID is empty")
    if not math.isfinite(temperature) or not -40 <= temperature <= 80:
        raise ValueError("Temperature must be between -40 and 80 C")
    if not math.isfinite(humidity) or not 0 <= humidity <= 100:
        raise ValueError("Humidity must be between 0 and 100 percent")

    # The three INA219 values are optional so older firmware can still connect.
    battery_voltage = optional_finite_float(data, "battery_voltage_v")
    battery_current = optional_finite_float(data, "battery_current_ma")
    battery_power = optional_finite_float(data, "battery_power_mw")
    # Keep valid DHT readings even if an INA219 value is outside its normal
    # operating range. The dashboard should expose the measured value so a
    # wiring/calibration problem can be diagnosed instead of dropping the
    # entire MQTT packet (including temperature and humidity).

    return {
        "device_id": device_id,
        "temperature": temperature,
        "humidity": humidity,
        "battery_voltage_v": battery_voltage,
        "battery_current_ma": battery_current,
        "battery_power_mw": battery_power,
        "sequence": sequence,
    }


def record_telemetry(protocol, data):
    clean = valid_sensor_data(data)
    item = {**clean, "protocol": protocol, "received_at": now_iso()}
    with lock:
        state["latest"] = item
        state["latest_by_protocol"][protocol] = item
        state["sensor_last_seen"] = time.time()
        state["device_last_seen"] = time.time()
        if protocol == "mqtt":
            state["mqtt_last_message_at"] = item["received_at"]
            state["mqtt_message_count"] += 1
            state["mqtt_last_error"] = None
        state["history"].insert(0, item)
        del state["history"][30:]
        state["telemetry_history"].insert(0, item)
        del state["telemetry_history"][500:]
    print(
        f"[{protocol.upper()}] {clean['device_id']} | seq={clean['sequence']} | "
        f"{clean['temperature']:.1f} C | {clean['humidity']:.1f} % | "
        f"V={clean['battery_voltage_v']} V | I={clean['battery_current_ma']} mA | "
        f"P={clean['battery_power_mw']} mW"
    )
    return item


def record_measurement(data):
    protocol = str(data.get("protocol", "")).lower()
    if protocol not in PROTOCOLS:
        raise ValueError("Unsupported protocol in measurement report")
    fields = (
        "sequence", "payload_bytes", "protocol_overhead_bytes",
        "transport_header_est_bytes", "estimated_wire_bytes",
        "free_heap_before", "free_heap_min", "free_heap_after",
        "ram_peak_observed_bytes", "send_duration_ms",
    )
    item = {
        "protocol": protocol,
        "device_id": str(data.get("device_id", "NODEMCU_01")),
        "success": data.get("success") is True,
        "received_at": now_iso(),
    }
    for field in fields:
        number = float(data.get(field, 0))
        if not math.isfinite(number):
            raise ValueError(f"{field} must be finite")
        item[field] = max(0, number) if field == "send_duration_ms" else max(0, int(number))
    with lock:
        state["metrics_history"].insert(0, item)
        del state["metrics_history"][100:]
    print(
        f"[METRICS] {protocol.upper()} seq={item['sequence']} | "
        f"payload={item['payload_bytes']} B | overhead={item['protocol_overhead_bytes']} B | "
        f"RAM delta={item['ram_peak_observed_bytes']} B | "
        f"{'OK' if item['success'] else 'FAILED'}"
    )
    return item


def metrics_summary(metric_rows, telemetry_rows):
    summary = {}
    for protocol in PROTOCOLS:
        selected_metrics = [
            row for row in metric_rows
            if row["protocol"] == protocol and row["success"]
        ]
        selected_power = [
            row for row in telemetry_rows
            if row["protocol"] == protocol and row["battery_power_mw"] is not None
        ]
        metric_fields = (
            "payload_bytes", "protocol_overhead_bytes",
            "transport_header_est_bytes", "estimated_wire_bytes",
            "ram_peak_observed_bytes",
        )
        averages = {}
        if selected_metrics:
            averages = {
                "avg_" + field: round(
                    sum(row[field] for row in selected_metrics) / len(selected_metrics), 1
                )
                for field in metric_fields
            }
        avg_power = (
            round(sum(row["battery_power_mw"] for row in selected_power) / len(selected_power), 2)
            if selected_power else None
        )
        summary[protocol] = {
            "sample_count": len(selected_metrics),
            "last": selected_metrics[0] if selected_metrics else None,
            "power_sample_count": len(selected_power),
            "avg_battery_power_mw": avg_power,
            **averages,
        }
    return summary

@app.get("/")
def dashboard():
    return send_from_directory(STATIC_DIR, "index.html")


@app.get("/api/health")
def health():
    return jsonify({
        "status": "ok",
        "http_port": HTTP_PORT,
        "coap_port": COAP_PORT,
        "mqtt_host": MQTT_HOST,
        "mqtt_port": MQTT_PORT,
        "mqtt_topic": MQTT_TOPIC,
    })


@app.get("/api/state")
def get_state():
    with lock:
        snapshot = {
            "selected_protocol": state["selected_protocol"],
            "running": state["running"],
            "device_last_seen": state["device_last_seen"],
            "sensor_last_seen": state["sensor_last_seen"],
            "latest": state["latest"],
            "latest_by_protocol": dict(state["latest_by_protocol"]),
            "telemetry_history": list(state["telemetry_history"]),
            "history": list(state["history"]),
            "metrics_history": list(state["metrics_history"]),
            "mqtt_last_message_at": state["mqtt_last_message_at"],
            "mqtt_message_count": state["mqtt_message_count"],
            "mqtt_last_error": state["mqtt_last_error"],
            "mqtt_status": state["mqtt_status"],
            "mqtt_host": MQTT_HOST,
            "mqtt_port": MQTT_PORT,
            "mqtt_topic": MQTT_TOPIC,
            "coap_status": state["coap_status"],
            "api_version": 2,
        }
        snapshot["telemetry_counts"] = {
            protocol: sum(1 for item in state["telemetry_history"] if item["protocol"] == protocol)
            for protocol in PROTOCOLS
        }
    last_seen = snapshot["device_last_seen"]
    snapshot["device_online"] = bool(last_seen and time.time() - last_seen < 8)
    snapshot["metrics_summary"] = metrics_summary(
        snapshot["metrics_history"], snapshot["telemetry_history"]
    )
    return jsonify(snapshot)


@app.post("/api/config")
def set_config():
    data = request.get_json(silent=True) or {}
    protocol = str(data.get("protocol", "")).lower()
    running = data.get("running")
    if protocol not in PROTOCOLS:
        return jsonify({"error": "protocol must be http, coap or mqtt"}), 400
    if not isinstance(running, bool):
        return jsonify({"error": "running must be true or false"}), 400
    with lock:
        state["selected_protocol"] = protocol
        state["running"] = running
    print(f"[CONTROL] protocol={protocol.upper()} | transmission={'ON' if running else 'OFF'}")
    return jsonify({"status": "updated", "protocol": protocol, "running": running})


@app.get("/api/device-config")
def device_config():
    # A successful poll is the NodeMCU heartbeat shown on the dashboard.
    with lock:
        state["device_last_seen"] = time.time()
        config = {"protocol": state["selected_protocol"],
                  "running": state["running"], "sample_interval_ms": 5000}
    return jsonify(config)


@app.post("/api/telemetry/http")
def receive_http_telemetry():
    try:
        item = record_telemetry("http", request.get_json(silent=True))
        return jsonify({"status": "success", "received": item}), 200
    except (ValueError, TypeError, KeyError) as exc:
        return jsonify({"error": str(exc)}), 400


@app.post("/api/measurement")
def receive_measurement():
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return jsonify({"error": "Invalid measurement JSON"}), 400
    try:
        item = record_measurement(data)
        return jsonify({"status": "recorded", "measurement": item}), 200
    except (ValueError, TypeError) as exc:
        return jsonify({"error": str(exc)}), 400


def decode_coap_extended(nibble, packet, index):
    if nibble < 13:
        return nibble, index
    if nibble == 13:
        if index >= len(packet):
            raise ValueError("Incomplete CoAP option")
        return 13 + packet[index], index + 1
    if nibble == 14:
        if index + 2 > len(packet):
            raise ValueError("Incomplete CoAP option")
        return 269 + struct.unpack("!H", packet[index:index + 2])[0], index + 2
    raise ValueError("Reserved CoAP option nibble")


def parse_coap(packet):
    if len(packet) < 4:
        raise ValueError("Short CoAP packet")
    version = packet[0] >> 6
    message_type = (packet[0] >> 4) & 0x03
    token_length = packet[0] & 0x0F
    if version != 1 or token_length > 8 or len(packet) < 4 + token_length:
        raise ValueError("Invalid CoAP header")
    code = packet[1]
    message_id = struct.unpack("!H", packet[2:4])[0]
    token = packet[4:4 + token_length]
    index = 4 + token_length
    option_number = 0
    uri_parts = []
    payload = b""
    while index < len(packet):
        if packet[index] == 0xFF:
            payload = packet[index + 1:]
            break
        option_header = packet[index]
        index += 1
        delta, index = decode_coap_extended(option_header >> 4, packet, index)
        length, index = decode_coap_extended(option_header & 0x0F, packet, index)
        if index + length > len(packet):
            raise ValueError("Incomplete CoAP option data")
        option_number += delta
        value = packet[index:index + length]
        index += length
        if option_number == 11:
            uri_parts.append(value.decode("utf-8"))
    return {"type": message_type, "code": code, "message_id": message_id,
            "token": token, "path": "/".join(uri_parts), "payload": payload}


def coap_response(request_data, code):
    response_type = 2 if request_data["type"] == 0 else 1
    token = request_data["token"]
    header = (1 << 6) | (response_type << 4) | len(token)
    return struct.pack("!BBH", header, code, request_data["message_id"]) + token


def coap_worker():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind((COAP_HOST, COAP_PORT))
        with lock:
            state["coap_status"] = "listening"
        print(f"CoAP listener on UDP {COAP_PORT}")
    except OSError as exc:
        with lock:
            state["coap_status"] = f"error: {exc}"
        print(f"CoAP listener failed: {exc}")
        sock.close()
        return
    while True:
        packet, address = sock.recvfrom(2048)
        parsed = None
        try:
            parsed = parse_coap(packet)
            if parsed["code"] != 2:
                response = coap_response(parsed, 0x85)
            elif parsed["path"] != "sensor":
                response = coap_response(parsed, 0x84)
            else:
                data = json.loads(parsed["payload"].decode("utf-8"))
                record_telemetry("coap", data)
                response = coap_response(parsed, 0x45)
            sock.sendto(response, address)
        except (ValueError, TypeError, KeyError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            print(f"Invalid CoAP packet from {address}: {exc}")
            if parsed is not None:
                try:
                    sock.sendto(coap_response(parsed, 0x80), address)
                except OSError:
                    pass


def mqtt_reason_failed(reason_code):
    """Read success/failure across Paho MQTT callback API versions."""
    failure_flag = getattr(reason_code, "is_failure", None)
    if failure_flag is not None:
        return bool(failure_flag)
    try:
        return int(reason_code) != 0
    except (TypeError, ValueError):
        return str(reason_code).strip().lower() not in ("success", "0", "none")


def mqtt_on_connect(client, userdata, flags, reason_code, properties=None):
    if mqtt_reason_failed(reason_code):
        reason_text = str(reason_code)
        with lock:
            state["mqtt_status"] = f"connection failed: {reason_text}"
            state["mqtt_last_error"] = f"connection refused: {reason_text}"
        print(f"[MQTT] Connection failed: {reason_text}")
        return

    result, mid = client.subscribe(MQTT_TOPIC, qos=0)
    subscribed = result == mqtt.MQTT_ERR_SUCCESS
    with lock:
        state["mqtt_status"] = "subscribing" if subscribed else "subscribe failed"
        state["mqtt_last_error"] = None if subscribed else f"subscribe rc={result}"
    if subscribed:
        print(
            f"[MQTT] Broker connected; subscription requested for "
            f"{MQTT_TOPIC} (MID={mid}). Waiting for SUBACK."
        )
    else:
        print(f"[MQTT] Broker connected, but subscription failed (rc={result}).")


def mqtt_on_subscribe(client, userdata, mid, reason_codes, properties=None):
    # Paho v2 supplies ReasonCode objects; Paho v1 supplies integer QoS values.
    codes = reason_codes if isinstance(reason_codes, (list, tuple)) else [reason_codes]
    failed = any(mqtt_reason_failed(code) for code in codes)
    with lock:
        state["mqtt_status"] = "subscribe failed" if failed else "connected"
        state["mqtt_last_error"] = "broker rejected subscription" if failed else None
    if failed:
        print(f"[MQTT] Subscription rejected (MID={mid}, reason={reason_codes}).")
    else:
        print(f"[MQTT] Subscribed and ready on topic {MQTT_TOPIC}.")


def mqtt_on_disconnect(
    client, userdata, disconnect_flags=None, reason_code=None, properties=None
):
    # In Paho v1 the third argument is the return code; in v2 it is flags.
    if reason_code is None:
        reason_code = disconnect_flags
    with lock:
        state["mqtt_status"] = "disconnected"
        state["mqtt_last_error"] = (
            str(reason_code)
            if reason_code is not None and str(reason_code) not in ("Success", "0")
            else None
        )
    print(f"[MQTT] Subscriber disconnected: {reason_code}")


def mqtt_on_message(client, userdata, message):
    try:
        if message.topic != MQTT_TOPIC:
            raise ValueError(f"Unexpected topic {message.topic}")
        payload_text = message.payload.decode("utf-8")
        data = json.loads(payload_text)
        item = record_telemetry("mqtt", data)
        print(
            f"[MQTT RX] RECEIVED from {message.topic} | bytes={len(message.payload)} | "
            f"seq={item['sequence']} | temp={item['temperature']:.1f} C | "
            f"humidity={item['humidity']:.1f}% | "
            f"voltage={item['battery_voltage_v']} V | "
            f"current={item['battery_current_ma']} mA | "
            f"power={item['battery_power_mw']} mW"
        )
    except (ValueError, TypeError, KeyError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        with lock:
            state["mqtt_last_error"] = str(exc)
        print(f"[MQTT RX ERROR] topic={getattr(message, 'topic', '?')}: {exc}")


def start_background_services():
    global mqtt_client, services_started
    if services_started:
        return
    services_started = True

    threading.Thread(target=coap_worker, name="coap-listener", daemon=True).start()

    client_id = f"multiprotocol_dashboard_{os.getpid()}"
    callback_api = getattr(mqtt, "CallbackAPIVersion", None)
    if callback_api is not None:
        mqtt_client = mqtt.Client(
            callback_api.VERSION2,
            client_id=client_id,
        )
    else:
        # Compatibility with Paho MQTT 1.x.
        mqtt_client = mqtt.Client(client_id=client_id)

    mqtt_client.on_connect = mqtt_on_connect
    mqtt_client.on_disconnect = mqtt_on_disconnect
    mqtt_client.on_subscribe = mqtt_on_subscribe
    mqtt_client.on_message = mqtt_on_message
    if hasattr(mqtt_client, "reconnect_delay_set"):
        mqtt_client.reconnect_delay_set(min_delay=1, max_delay=15)

    try:
        mqtt_client.connect_async(MQTT_HOST, MQTT_PORT, 60)
        mqtt_client.loop_start()
        print(
            f"[MQTT] Subscriber connecting to {MQTT_HOST}:{MQTT_PORT}; "
            f"topic={MQTT_TOPIC}"
        )
    except Exception as exc:
        with lock:
            state["mqtt_status"] = f"error: {exc}"
            state["mqtt_last_error"] = str(exc)
        print(f"[MQTT] Could not start subscriber: {exc}")


if __name__ == "__main__":
    start_background_services()
    print("Dashboard: http://127.0.0.1:5000")
    print("Device control endpoint: http://<laptop-ip>:5000/api/device-config")
    app.run(host="0.0.0.0", port=HTTP_PORT, debug=False,
            threaded=True, use_reloader=False)
