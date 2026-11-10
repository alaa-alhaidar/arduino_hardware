#ifndef CAMERA_TEST

#include <Arduino.h>
#include <Adafruit_BME680.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <time.h>
#include <cmath>
#include "emqx_ca.h"
#include "secrets.h"

// Verschlüsselte Verbindung zum privaten EMQX-Cloud-Broker.
constexpr char MQTT_HOST[] = "jfa0c01f.ala.eu-central-1.emqxsl.com";
constexpr uint16_t MQTT_PORT = 8883;
constexpr char MQTT_TOPIC[] = "htw/alaa/iot2026/nano-01/telemetry";
constexpr char MQTT_CONTROL_TOPIC[] =
  "htw/alaa/iot2026/nano-01/commands/telemetry";

constexpr uint32_t SEND_INTERVAL_MS = 30000;
constexpr uint32_t RETRY_INTERVAL_MS = 3000;

WiFiClientSecure networkClient;
PubSubClient mqttClient(networkClient);
Adafruit_BME680 bme;

char bootId[37];
uint32_t sequence = 0;
uint32_t lastSendAt = 0;
uint32_t lastWifiAttemptAt = 0;
uint32_t lastMqttAttemptAt = 0;
uint32_t lastSensorAttemptAt = 0;
bool sensorReady = false;
bool telemetryEnabled = true;

void handleMqttMessage(char *topic, byte *payload, unsigned int length) {
  if (strcmp(topic, MQTT_CONTROL_TOPIC) != 0) {
    return;
  }

  const String command(reinterpret_cast<char *>(payload), length);
  if (command == "stop") {
    telemetryEnabled = false;
    Serial.println("Telemetrie durch Kontrollstation gestoppt.");
  } else if (command == "start") {
    telemetryEnabled = true;
    lastSendAt = millis() - SEND_INTERVAL_MS;
    Serial.println("Telemetrie durch Kontrollstation gestartet.");
  }
}

bool connectSensor() {
  if (sensorReady || millis() - lastSensorAttemptAt < RETRY_INTERVAL_MS) {
    return sensorReady;
  }
  lastSensorAttemptAt = millis();
  sensorReady = bme.begin(0x76) || bme.begin(0x77);
  if (!sensorReady) {
    Serial.println("BME688 nicht an I2C-Adresse 0x76 oder 0x77 gefunden.");
    return false;
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);
  Serial.println("BME688 bereit.");
  return true;
}

void createBootId() {
  uint8_t bytes[16];
  for (uint8_t &value : bytes) {
    value = static_cast<uint8_t>(esp_random());
  }

  // UUID Version 4 und RFC-4122-Variante setzen.
  bytes[6] = (bytes[6] & 0x0F) | 0x40;
  bytes[8] = (bytes[8] & 0x3F) | 0x80;

  snprintf(bootId, sizeof(bootId),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
           bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
           bytes[12], bytes[13], bytes[14], bytes[15]);
}

void formatUtcTimestamp(char *buffer, size_t bufferSize) {
  const time_t now = time(nullptr);
  struct tm utcTime {};

  // Ohne erfolgreiche NTP-Synchronisation keinen falschen Zeitstempel senden.
  if (now < 1700000000 || gmtime_r(&now, &utcTime) == nullptr) {
    buffer[0] = '\0';
    return;
  }

  strftime(buffer, bufferSize, "%Y-%m-%dT%H:%M:%SZ", &utcTime);
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED ||
      millis() - lastWifiAttemptAt < RETRY_INTERVAL_MS) {
    return;
  }

  lastWifiAttemptAt = millis();
  Serial.printf("WLAN-Verbindung zu %s wird aufgebaut ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected() ||
      millis() - lastMqttAttemptAt < RETRY_INTERVAL_MS) {
    return;
  }

  lastMqttAttemptAt = millis();
  if (time(nullptr) < 1700000000) {
    Serial.println("Warte auf Uhrzeitsynchronisierung für TLS ...");
    return;
  }
  const String clientId = "nano-esp32-" + String(bootId).substring(0, 8);

  Serial.printf("MQTT-Verbindung zu %s:%u wird aufgebaut ...\n", MQTT_HOST,
                MQTT_PORT);
  if (mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
    mqttClient.subscribe(MQTT_CONTROL_TOPIC, 1);
    Serial.println("MQTT verbunden. BME688-Sensordaten werden gesendet.");
  } else {
    Serial.printf("MQTT-Verbindung fehlgeschlagen (Status %d).\n",
                  mqttClient.state());
  }
}

bool publishMeasurements() {
  double temperature;
  double pressure;
  double humidity;
  double gasResistance;
  const char *source;

  if (connectSensor() && bme.performReading()) {
    temperature = bme.temperature;
    pressure = bme.pressure / 100.0;
    humidity = bme.humidity;
    gasResistance = bme.gas_resistance;
    source = "nano-bme688";
  } else {
    const double phase = static_cast<double>(sequence) / 12.0;
    temperature = 23.0 + 2.0 * sin(phase);
    pressure = 1013.0 + 3.0 * sin(phase / 3.0);
    humidity = 48.0 + 5.0 * cos(phase);
    gasResistance = 120000.0 + 10000.0 * sin(phase / 2.0);
    source = "nano-fake";
    Serial.println("BME688 fehlt: Übergangsweise werden Fake-Werte gesendet.");
  }

  char timestamp[25];
  formatUtcTimestamp(timestamp, sizeof(timestamp));

  char payload[512];
  const int length = snprintf(
      payload, sizeof(payload),
      "{\"schema_version\":1,\"node_id\":\"nano-01\",\"boot_id\":\"%s\","
      "\"sequence\":%lu,\"sent_at\":\"%s\",\"source\":\"%s\","
      "\"measurements\":{\"temperature_c\":%.2f,\"pressure_hpa\":%.2f,"
      "\"humidity_pct\":%.2f,\"gas_resistance_ohm\":%.2f}}",
      bootId, static_cast<unsigned long>(sequence), timestamp, source,
      temperature, pressure, humidity, gasResistance);

  if (length < 0 || static_cast<size_t>(length) >= sizeof(payload)) {
    Serial.println("JSON-Nachricht ist zu groß.");
    return false;
  }

  // Retained: Neue Abonnenten erhalten sofort den letzten Messwert.
  const bool erfolgreichGesendet =
      mqttClient.publish(MQTT_TOPIC, payload, true);

  if (erfolgreichGesendet) {
    Serial.printf("Messwerte seq=%lu %s\n", 
      static_cast<unsigned long>(sequence), payload);
    return true;
  }

  Serial.println("MQTT-Senden fehlgeschlagen.");
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin();
  connectSensor();

  createBootId();
  WiFi.mode(WIFI_STA);
  networkClient.setCACert(EMQX_ROOT_CA);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(handleMqttMessage);
  mqttClient.setBufferSize(768);

  // UTC-Zeit über NTP; Berlin-Zeitzone ist für den UTC-Zeitstempel unerheblich.
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  connectWifi();
  connectSensor();
}

void loop() {
  connectWifi();

  if (WiFi.status() == WL_CONNECTED) {
    connectMqtt();
  }

  if (!mqttClient.connected()) {
    return;
  }

  mqttClient.loop();

  if (!telemetryEnabled) {
    return;
  }

  const uint32_t now = millis();
  if (sequence == 0 || now - lastSendAt >= SEND_INTERVAL_MS) {
    if (publishMeasurements()) {
      sequence++;
      lastSendAt = now;
    } else {
      mqttClient.disconnect();
    }
  }
}

#endif  // CAMERA_TEST
