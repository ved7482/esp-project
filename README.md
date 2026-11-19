# Multi-Protocol Sensor Node — INA219/MQTT Fix

This update keeps the existing project structure and replaces the newest firmware, Flask backend, and dashboard JavaScript with a more defensive version. Keep your existing `static/index.html`; it was not included in the supplied files, so its element IDs and script path could not be checked here.

## Files

- `multiprotocol_sensor_node.ino` — ESP8266 + DHT11 + INA219 firmware for HTTP, CoAP and MQTT.
- `app.py` — Flask API, CoAP UDP listener, and MQTT subscriber.
- `static/app.js` — dashboard code. Ensure your `static/index.html` loads this file (for example, `<script src="/static/app.js" defer></script>`).
- `requirements.txt` — Python packages.

## 1. Update the firmware settings

In `multiprotocol_sensor_node.ino`, set:

- `WIFI_SSID` and `WIFI_PASSWORD` to your Wi-Fi credentials.
- `SERVER_IP` to the laptop's current IPv4 address. It is used by Flask/CoAP and, in this version, by the ESP8266 as the MQTT broker address too.
- Keep `MQTT_TOPIC` the same as the backend topic: `iot/esp32/sensor`.

Install the Arduino libraries used by the sketch: ESP8266 board package, DHT sensor library, Adafruit INA219, Adafruit BusIO, and PubSubClient.

## 2. Check sensor wiring

- DHT11 DATA → D4; VCC → 3V3; GND → GND.
- INA219 SDA → D2; SCL → D1; VCC → 3V3; GND → GND.
- Connect the measured supply/load through INA219 VIN+ and VIN− in series, following the INA219 module's wiring diagram. Do not connect the battery/supply voltage to the ESP8266 3V3 pin.

The code uses the INA219 32 V / 2 A calibration profile. Use a suitable shunt/module and keep the measured voltage/current within its hardware rating.

## 3. MQTT broker configuration

This version assumes Mosquitto runs on the same laptop as Flask:

- Flask's MQTT subscriber connects to `127.0.0.1:1883`.
- The ESP8266 connects to the laptop's LAN address in `SERVER_IP` on port `1883`.

Therefore, Mosquitto must listen on the laptop's LAN interface as well as loopback. For a private lab network, a Mosquitto listener configuration can look like:

```conf
listener 1883 0.0.0.0
allow_anonymous true
```

Only use anonymous access on a trusted/private test network. For a shared or public network, configure authentication and restrict the firewall instead.

The backend topic/host can also be set with environment variables before starting Flask:

- `MQTT_HOST` (default `127.0.0.1`)
- `MQTT_PORT` (default `1883`)
- `MQTT_TOPIC` (default `iot/esp32/sensor`)

If you change the topic, change `MQTT_TOPIC` in the firmware to the exact same value.

## 4. Run the project

From the project folder:

```bash
python -m pip install -r requirements.txt
python app.py
```

Keep Mosquitto running, open the dashboard, select MQTT, and start transmission. Allow TCP 5000, TCP 1883, and UDP 5683 through the laptop firewall on the private network as appropriate.

## 5. Confirm the MQTT data path

Look for these messages:

1. ESP8266 Serial Monitor: `[MQTT] Connected to broker ...`
2. ESP8266 Serial Monitor: `[MQTT TX] Publish accepted by MQTT client.`
3. Flask terminal: `[MQTT] Subscribed and ready on topic iot/esp32/sensor.`
4. Flask terminal: `[MQTT RX] iot/esp32/sensor ... temp=... humidity=... voltage=... current=... power=...`

If step 1 fails, check the broker IP/listener/firewall. If step 1 succeeds but step 4 does not, compare the topic exactly and check the Flask terminal for `[MQTT RX ERROR]`.

## Dashboard HTML IDs

The uploaded JavaScript expects these IDs in `static/index.html`:

- `temperature`
- `humidity`
- `battery-voltage`
- `battery-current`
- `battery-power`
- `reading-time`
- `mqtt-status` and `mqtt-dot`
- `coap-status`
- `control-hint`

The JavaScript safely skips missing elements, so missing battery IDs would make the cards stay blank without necessarily causing a JavaScript error. Since `static/index.html` was not supplied, verify these IDs against your actual HTML.

## Validation performed

- Python source compiles successfully.
- JavaScript passes `node --check`.
- Backend parsing/recording logic was exercised with synthetic HTTP, CoAP, and MQTT payloads, including signed current/power and null INA219 values.
- A live Mosquitto broker, ESP8266 board, INA219 sensor, and Arduino compiler were not available in this environment, so hardware transmission has not been claimed as tested.
