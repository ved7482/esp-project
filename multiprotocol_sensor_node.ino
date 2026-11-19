#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

const char* WIFI_SSID = "vas";
const char* WIFI_PASSWORD = "unknown01";
IPAddress SERVER_IP(10, 176, 181, 174);  // Replace with laptop IPv4 from ipconfig

// ----------- NETWORK CONFIGURATION -----------------------------
const uint16_t HTTP_PORT = 5000;
const uint16_t COAP_PORT = 5683;
const uint16_t MQTT_PORT = 1883;
const char* DEVICE_ID = "NODEMCU_01";
const char* MQTT_TOPIC = "iot/esp32/sensor";

// ----------- SENSOR PIN CONFIGURATION --------------------------
#define DHT_PIN D4       // DHT11 DATA -> D4
#define DHT_TYPE DHT11
#define I2C_SDA D2       // INA219 SDA -> D2
#define I2C_SCL D1       // INA219 SCL -> D1

DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_INA219 ina219;
bool ina219Available = false;

WiFiClient mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);
WiFiUDP coapUdp;

const unsigned long CONFIG_POLL_MS = 2000;
const unsigned long DEFAULT_SAMPLE_INTERVAL_MS = 5000;
const unsigned long MQTT_RETRY_MS = 3000;
unsigned long lastConfigPoll = 0;
unsigned long lastTelemetry = 0;
unsigned long lastMqttAttempt = 0;
unsigned long sampleInterval = DEFAULT_SAMPLE_INTERVAL_MS;
uint16_t coapMessageId = 1;
uint32_t sequenceNumber = 0;
String selectedProtocol = "http";
bool transmissionEnabled = false;
int lastConfigStatus = -999;

struct Measurement {
  String protocol;
  uint32_t payloadBytes;
  uint32_t protocolOverheadBytes;
  uint32_t transportHeaderEstimateBytes;
  uint32_t estimatedWireBytes;
  uint32_t freeHeapBefore;
  uint32_t freeHeapMin;
  uint32_t freeHeapAfter;
  uint32_t ramPeakObservedBytes;
  uint32_t sendDurationMs;
  bool success;
};

void updateHeapMinimum(uint32_t& minimumHeap) {
  uint32_t currentHeap = ESP.getFreeHeap();
  if (currentHeap < minimumHeap) minimumHeap = currentHeap;
}

String serverUrl(const char* path) {
  return String("http://") + SERVER_IP.toString() + ":" + HTTP_PORT + path;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to Wi-Fi SSID: %s\n", WIFI_SSID);
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - started > 25000) {
      Serial.println("\nWi-Fi connection timed out. Rechecking credentials/network.");
      started = millis();
      WiFi.disconnect();
      delay(500);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
  Serial.println("\nWi-Fi connected");
  Serial.print("NodeMCU IP: "); Serial.println(WiFi.localIP());
  Serial.print("Laptop server IP: "); Serial.println(SERVER_IP);
  Serial.print("Dashboard URL: http://"); Serial.print(SERVER_IP);
  Serial.print(":"); Serial.println(HTTP_PORT);
}

String jsonStringValue(const String& json, const char* key) {
  String search = String("\"") + key + "\"";
  int keyPosition = json.indexOf(search);
  if (keyPosition < 0) return "";
  int colon = json.indexOf(':', keyPosition + search.length());
  if (colon < 0) return "";
  int start = json.indexOf('"', colon + 1);
  if (start < 0) return "";
  int end = json.indexOf('"', start + 1);
  if (end < 0) return "";
  return json.substring(start + 1, end);
}

bool jsonBoolValue(const String& json, const char* key, bool fallback) {
  String search = String("\"") + key + "\"";
  int keyPosition = json.indexOf(search);
  if (keyPosition < 0) return fallback;
  int colon = json.indexOf(':', keyPosition + search.length());
  if (colon < 0) return fallback;
  int valueStart = colon + 1;
  while (valueStart < (int)json.length() && (json[valueStart] == ' ' || json[valueStart] == '\t')) valueStart++;
  if (json.substring(valueStart, valueStart + 4) == "true") return true;
  if (json.substring(valueStart, valueStart + 5) == "false") return false;
  return fallback;
}

void pollDashboardConfig() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, serverUrl("/api/device-config"))) {
    Serial.println("[CONTROL] Could not initialize HTTP connection to laptop.");
    return;
  }
  http.setTimeout(2500);
  int status = http.GET();
  if (status == HTTP_CODE_OK) {
    String body = http.getString();
    String protocol = jsonStringValue(body, "protocol");
    if (protocol == "http" || protocol == "coap" || protocol == "mqtt") {
      if (protocol != selectedProtocol) {
        selectedProtocol = protocol;
        lastTelemetry = 0;
        Serial.print("[CONTROL] Protocol changed to: "); Serial.println(selectedProtocol);
      }
    }
    transmissionEnabled = jsonBoolValue(body, "running", false);
    String intervalKey = "\"sample_interval_ms\"";
    int intervalPosition = body.indexOf(intervalKey);
    if (intervalPosition >= 0) {
      int colon = body.indexOf(':', intervalPosition + intervalKey.length());
      long configuredInterval = colon >= 0 ? body.substring(colon + 1).toInt() : 0;
      if (configuredInterval >= 1000 && configuredInterval <= 60000) sampleInterval = (unsigned long)configuredInterval;
    }
    if (lastConfigStatus != HTTP_CODE_OK) Serial.println("[CONTROL] Laptop dashboard connection OK.");
    lastConfigStatus = HTTP_CODE_OK;
  } else {
    if (status != lastConfigStatus) {
      Serial.printf("[CONTROL] Cannot reach laptop endpoint %s (HTTPClient result: %d, %s)\n",
                    "/api/device-config", status, HTTPClient::errorToString(status).c_str());
      Serial.println("[CONTROL] Check SERVER_IP, laptop Wi-Fi network and Windows Firewall TCP 5000.");
    }
    lastConfigStatus = status;
  }
  http.end();
}

// All sensor values are included in this one telemetry JSON packet.
// Battery voltage is calculated on the supply side: VIN- bus voltage + shunt drop (VIN+).
String makeSensorPayload(float temperature, float humidity,
                         bool powerSensorValid, float batteryVoltage,
                         float batteryCurrentMa, float batteryPowerMw,
                         uint32_t sequence) {
  String payload;
  payload.reserve(220);
  payload = "{\"device_id\":\"";
  payload += DEVICE_ID;
  payload += "\",\"temperature\":";
  payload += String(temperature, 2);
  payload += ",\"humidity\":";
  payload += String(humidity, 2);
  payload += ",\"battery_voltage_v\":";
  payload += powerSensorValid ? String(batteryVoltage, 3) : String("null");
  payload += ",\"battery_current_ma\":";
  payload += powerSensorValid ? String(batteryCurrentMa, 3) : String("null");
  payload += ",\"battery_power_mw\":";
  payload += powerSensorValid ? String(batteryPowerMw, 3) : String("null");
  payload += ",\"sequence\":";
  payload += String(sequence);
  payload += "}";
  return payload;
}

bool sendHttpTelemetry(const String& payload, Measurement& m, uint32_t& heapMinimum) {
  WiFiClient client;
  client.setTimeout(2500);
  if (!client.connect(SERVER_IP, HTTP_PORT)) {
    Serial.println("[HTTP] TCP connection failed. Check laptop IP and firewall TCP 5000.");
    return false;
  }
  updateHeapMinimum(heapMinimum);
  String headers;
  headers.reserve(180);
  headers += "POST /api/telemetry/http HTTP/1.1\r\nHost: ";
  headers += SERVER_IP.toString();
  headers += ":"; headers += String(HTTP_PORT);
  headers += "\r\nContent-Type: application/json\r\nContent-Length: ";
  headers += String(payload.length());
  headers += "\r\nConnection: close\r\n\r\n";
  size_t headerCount = client.print(headers);
  size_t payloadCount = client.print(payload);
  updateHeapMinimum(heapMinimum);
  m.protocolOverheadBytes = headers.length();
  m.transportHeaderEstimateBytes = 40;
  if (headerCount != headers.length() || payloadCount != payload.length()) {
    client.stop();
    return false;
  }
  unsigned long waitStart = millis();
  while (!client.available() && client.connected() && millis() - waitStart < 2500) delay(5);
  bool success = false;
  if (client.available()) {
    String statusLine = client.readStringUntil('\n');
    success = statusLine.indexOf(" 200 ") >= 0;
    if (!success) { Serial.print("[HTTP] Server response: "); Serial.println(statusLine); }
  } else {
    Serial.println("[HTTP] No response from Flask route.");
  }
  while (client.available()) client.read();
  client.stop();
  updateHeapMinimum(heapMinimum);
  return success;
}

bool sendCoapTelemetry(const String& payload, Measurement& m, uint32_t& heapMinimum) {
  while (coapUdp.parsePacket() > 0) while (coapUdp.available()) coapUdp.read();
  uint8_t header[4] = {0x50, 0x02, (uint8_t)(coapMessageId >> 8), (uint8_t)(coapMessageId & 0xFF)};
  const uint8_t uriOption[7] = {0xB6, 's', 'e', 'n', 's', 'o', 'r'};
  const uint8_t contentFormatOption[2] = {0x11, 0x32};
  m.protocolOverheadBytes = 14;
  m.transportHeaderEstimateBytes = 28;
  if (!coapUdp.beginPacket(SERVER_IP, COAP_PORT)) return false;
  coapUdp.write(header, sizeof(header));
  coapUdp.write(uriOption, sizeof(uriOption));
  coapUdp.write(contentFormatOption, sizeof(contentFormatOption));
  coapUdp.write((uint8_t)0xFF);
  coapUdp.write((const uint8_t*)payload.c_str(), payload.length());
  updateHeapMinimum(heapMinimum);
  if (!coapUdp.endPacket()) { coapMessageId++; return false; }
  unsigned long waitStart = millis();
  bool success = false;
  while (millis() - waitStart < 1800) {
    int packetSize = coapUdp.parsePacket();
    if (packetSize >= 4) {
      uint8_t response[32];
      int length = coapUdp.read(response, sizeof(response));
      if (length >= 4 && response[1] == 0x45) success = true;
      break;
    }
    delay(5);
  }
  if (!success) Serial.println("[CoAP] No valid Changed response. Check UDP 5683/firewall.");
  coapMessageId++;
  updateHeapMinimum(heapMinimum);
  return success;
}

uint32_t mqttRemainingLengthBytes(uint32_t value) {
  uint32_t bytes = 0;
  do { value /= 128; bytes++; } while (value > 0);
  return bytes;
}

bool ensureMqttConnected() {
  if (mqttClient.connected()) return true;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (millis() - lastMqttAttempt < MQTT_RETRY_MS) return false;

  lastMqttAttempt = millis();
  String clientId = String("NODEMCU_") + String(ESP.getChipId(), HEX);
  bool connected = mqttClient.connect(clientId.c_str());
  if (connected) {
    Serial.printf("[MQTT] Connected to broker %s:%u as %s\n",
                  SERVER_IP.toString().c_str(), MQTT_PORT, clientId.c_str());
  } else {
    Serial.printf("[MQTT] Broker connection failed, state=%d. Check broker listener/firewall.\n",
                  mqttClient.state());
  }
  return connected;
}

bool sendMqttTelemetry(const String& payload, Measurement& m, uint32_t& heapMinimum) {
  if (!ensureMqttConnected()) return false;
  mqttClient.loop();
  updateHeapMinimum(heapMinimum);

  uint32_t topicLength = strlen(MQTT_TOPIC);
  uint32_t remainingLength = 2 + topicLength + payload.length();
  m.protocolOverheadBytes = 1 + mqttRemainingLengthBytes(remainingLength) + 2 + topicLength;
  m.transportHeaderEstimateBytes = 40;

  Serial.printf("[MQTT TX] topic=%s | payload=%u bytes\n",
                MQTT_TOPIC, (unsigned int)payload.length());
  bool success = mqttClient.publish(
      MQTT_TOPIC,
      (const uint8_t*)payload.c_str(),
      payload.length(),
      false
  );
  mqttClient.loop();
  updateHeapMinimum(heapMinimum);

  if (!success) {
    Serial.printf("[MQTT TX ERROR] Publish rejected. client_state=%d, payload=%u bytes.\n",
                  mqttClient.state(), (unsigned int)payload.length());
  } else {
    Serial.println("[MQTT TX] Publish accepted by MQTT client.");
  }
  return success;
}

void reportMeasurement(const Measurement& m, uint32_t sequence) {
  if (WiFi.status() != WL_CONNECTED) return;
  String report;
  report.reserve(320);
  report = "{\"device_id\":\""; report += DEVICE_ID;
  report += "\",\"sequence\":"; report += String(sequence);
  report += ",\"protocol\":\""; report += m.protocol;
  report += "\",\"success\":"; report += m.success ? "true" : "false";
  report += ",\"payload_bytes\":"; report += String(m.payloadBytes);
  report += ",\"protocol_overhead_bytes\":"; report += String(m.protocolOverheadBytes);
  report += ",\"transport_header_est_bytes\":"; report += String(m.transportHeaderEstimateBytes);
  report += ",\"estimated_wire_bytes\":"; report += String(m.estimatedWireBytes);
  report += ",\"free_heap_before\":"; report += String(m.freeHeapBefore);
  report += ",\"free_heap_min\":"; report += String(m.freeHeapMin);
  report += ",\"free_heap_after\":"; report += String(m.freeHeapAfter);
  report += ",\"ram_peak_observed_bytes\":"; report += String(m.ramPeakObservedBytes);
  report += ",\"send_duration_ms\":"; report += String(m.sendDurationMs);
  report += "}";
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, serverUrl("/api/measurement"))) {
    Serial.println("[METRICS] Could not start HTTP measurement report.");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(2000);
  int status = http.POST(report);
  if (status != HTTP_CODE_OK) Serial.printf("[METRICS] Report failed: %d (%s)\n", status, HTTPClient::errorToString(status).c_str());
  http.end();
}

void sendSensorReading(float temperature, float humidity,
                       bool powerSensorValid, float batteryVoltage,
                       float batteryCurrentMa, float batteryPowerMw) {
  Measurement m;
  m.protocol = selectedProtocol;
  m.protocolOverheadBytes = 0;
  m.transportHeaderEstimateBytes = 0;
  m.success = false;
  uint32_t heapBefore = ESP.getFreeHeap();
  uint32_t heapMinimum = heapBefore;
  String payload = makeSensorPayload(temperature, humidity, powerSensorValid,
                                     batteryVoltage, batteryCurrentMa,
                                     batteryPowerMw, sequenceNumber);
  if (selectedProtocol == "mqtt") {
    Serial.print("[MQTT JSON] ");
    Serial.println(payload);
  }
  updateHeapMinimum(heapMinimum);
  m.payloadBytes = payload.length();
  m.freeHeapBefore = heapBefore;
  unsigned long sendStart = millis();
  if (selectedProtocol == "http") m.success = sendHttpTelemetry(payload, m, heapMinimum);
  else if (selectedProtocol == "coap") m.success = sendCoapTelemetry(payload, m, heapMinimum);
  else if (selectedProtocol == "mqtt") m.success = sendMqttTelemetry(payload, m, heapMinimum);
  m.sendDurationMs = millis() - sendStart;
  updateHeapMinimum(heapMinimum);
  m.freeHeapMin = heapMinimum;
  m.freeHeapAfter = ESP.getFreeHeap();
  m.ramPeakObservedBytes = heapBefore > heapMinimum ? heapBefore - heapMinimum : 0;
  m.estimatedWireBytes = m.payloadBytes + m.protocolOverheadBytes + m.transportHeaderEstimateBytes;
  Serial.printf("[%s] seq=%lu | payload=%lu B | overhead=%lu B | total~%lu B | RAM drop=%lu B | %s\n",
                selectedProtocol.c_str(), (unsigned long)sequenceNumber,
                (unsigned long)m.payloadBytes, (unsigned long)m.protocolOverheadBytes,
                (unsigned long)m.estimatedWireBytes, (unsigned long)m.ramPeakObservedBytes,
                m.success ? "SUCCESS" : "FAILED");
  if (!m.success) {
    Serial.println("[TELEMETRY] Send failed. Check the protocol-specific diagnostic above.");
  }
  if (powerSensorValid) {
    Serial.printf("[INA219] Supply=%.3f V | Current=%.3f mA | Power=%.3f mW\n",
                  batteryVoltage, batteryCurrentMa, batteryPowerMw);
  } else {
    Serial.println("[INA219] Unavailable; DHT11 data still transmitted, power fields are null.");
  }
  reportMeasurement(m, sequenceNumber);
  sequenceNumber++;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // Keep DHT11 on D4; INA219 uses the I2C pair D2/D1.
  Wire.begin(I2C_SDA, I2C_SCL);
  dht.begin();
  ina219Available = ina219.begin();
  if (ina219Available) {
    ina219.setCalibration_32V_2A();
    Serial.println("[INA219] Detected and calibrated (32V / 2A profile).");
  } else {
    Serial.println("[INA219] NOT FOUND. Check 3V3, GND, SDA=D2 and SCL=D1.");
  }

  connectWiFi();
  coapUdp.begin(5684);
  mqttClient.setServer(SERVER_IP, MQTT_PORT);
  bool mqttBufferReady = mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(3);
  Serial.printf("[MQTT] Broker target: %s:%u | topic: %s | buffer: %s\n",
                SERVER_IP.toString().c_str(), MQTT_PORT, MQTT_TOPIC,
                mqttBufferReady ? "512 bytes" : "allocation failed");
  Serial.println("Sensor node initialized.");
  Serial.println("Waiting for dashboard configuration at /api/device-config.");
  Serial.println("Select a protocol in the dashboard and press Start transmission.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (mqttClient.connected()) mqttClient.loop();
  unsigned long now = millis();
  if (now - lastConfigPoll >= CONFIG_POLL_MS) {
    lastConfigPoll = now;
    pollDashboardConfig();
  }
  if (!transmissionEnabled) { delay(2); return; }
  if (now - lastTelemetry >= sampleInterval) {
    lastTelemetry = now;

    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();
    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("[DHT11] Reading failed. Check VCC, GND and DATA pin D4.");
      return;
    }

    bool powerSensorValid = ina219Available;
    float batteryVoltage = 0.0f;
    float batteryCurrentMa = 0.0f;
    float batteryPowerMw = 0.0f;
    if (powerSensorValid) {
      float busVoltage = ina219.getBusVoltage_V();
      float shuntVoltageMv = ina219.getShuntVoltage_mV();
      batteryVoltage = busVoltage + (shuntVoltageMv / 1000.0f);
      batteryCurrentMa = ina219.getCurrent_mA();
      batteryPowerMw = ina219.getPower_mW();
      if (!isfinite(batteryVoltage) || !isfinite(batteryCurrentMa) || !isfinite(batteryPowerMw)) {
        powerSensorValid = false;
        Serial.println("[INA219] Invalid reading; sending null power fields for this sample.");
      }
    }
    sendSensorReading(temperature, humidity, powerSensorValid,
                      batteryVoltage, batteryCurrentMa, batteryPowerMw);
  }
  delay(2);
}
