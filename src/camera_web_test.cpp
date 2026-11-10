#ifdef CAMERA_TEST

#include <Arduino.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <esp_camera.h>
#include "secrets.h"

// GOOUUU ESP32-S3-CAM V1.5: Kamera-Pinout entspricht ESP32-S3-EYE.
constexpr int CAM_PWDN = -1;
constexpr int CAM_RESET = -1;
constexpr int CAM_XCLK = 15;
constexpr int CAM_SIOD = 4;
constexpr int CAM_SIOC = 5;
constexpr int CAM_D7 = 16;
constexpr int CAM_D6 = 17;
constexpr int CAM_D5 = 18;
constexpr int CAM_D4 = 12;
constexpr int CAM_D3 = 10;
constexpr int CAM_D2 = 8;
constexpr int CAM_D1 = 9;
constexpr int CAM_D0 = 11;
constexpr int CAM_VSYNC = 6;
constexpr int CAM_HREF = 7;
constexpr int CAM_PCLK = 13;

// Servo: Signalleitung an GPIO47, Versorgung an 5V und GND.
// GPIO47 ist PWM-fähig und wird von der Kamera nicht verwendet.
constexpr int SERVO_PIN = 47;
constexpr int SERVO_MIN_PULSE_US = 500;
// Der Servo erreicht bei 2400 us seinen mechanischen Anschlag, blockiert dort
// und verursacht durch den hohen Strom einen ESP32-Neustart. 1900 us bilden
// deshalb den sicheren rechten Endpunkt auf die logischen 180 Grad ab.
constexpr int SERVO_MAX_PULSE_US = 1900;
constexpr uint32_t SERVO_HOLD_TIME_MS = 700;

Servo servo;
uint8_t servoAngle = 90;
uint32_t servoMovedAt = 0;

// Zwei Bilder/s sind für die PWA flüssig genug und bleiben auch bei einem
// schwächeren 2,4-GHz-WLAN stabil. Nach mehreren Fehlern wird automatisch
// langsamer gesendet, damit sich die Verbindung erholen kann.
constexpr uint32_t RELAY_INTERVAL_MS = 500;
constexpr uint32_t DEGRADED_RELAY_INTERVAL_MS = 1500;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
uint32_t lastRelayUploadAt = 0;
uint32_t lastWifiRetryAt = 0;
uint8_t consecutiveUploadFailures = 0;

void startServo()
{
  // Timer 0 bleibt frei, weil die Kamera ihn für ihr XCLK-Signal verwendet.
  ESP32PWM::allocateTimer(1);
  // 50 Hz ist die übliche Wiederholrate für Modellbau-Servos.
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, SERVO_MIN_PULSE_US, SERVO_MAX_PULSE_US);
  servo.write(servoAngle);
  servoMovedAt = millis();
  Serial.printf("Servo an GPIO%d bereit: %u Grad.\n", SERVO_PIN,
                servoAngle);
}

void updateServoPower()
{
  if (servo.attached() && millis() - servoMovedAt >= SERVO_HOLD_TIME_MS)
  {
    servo.detach();
    Serial.println("Servo-PWM abgeschaltet.");
  }
}

void applyServoCommand(const String &responseBody)
{
  const int keyPosition = responseBody.indexOf("\"servo_angle\"");
  if (keyPosition < 0)
    return;
  const int colonPosition = responseBody.indexOf(':', keyPosition);
  if (colonPosition < 0)
    return;
  const int requestedAngle = responseBody.substring(colonPosition + 1).toInt();
  if (requestedAngle < 0 || requestedAngle > 180 ||
      requestedAngle == servoAngle)
    return;
  servoAngle = static_cast<uint8_t>(requestedAngle);
  if (!servo.attached())
    servo.attach(SERVO_PIN, SERVO_MIN_PULSE_US, SERVO_MAX_PULSE_US);
  servo.write(servoAngle);
  servoMovedAt = millis();
  Serial.printf("Servo-Befehl aus PWA: %u Grad.\n", servoAngle);
}

bool postJpeg(camera_fb_t *frame, const char *path, String *responseBody = nullptr)
{
  WiFiClient client;
  HTTPClient request;
  const String url = String("http://") + CAMERA_RELAY_HOST + ':' +
                     CAMERA_RELAY_PORT + path;
  request.setConnectTimeout(1500);
  request.setTimeout(4000);
  request.begin(client, url);
  request.addHeader("Authorization", String("Bearer ") + CAMERA_UPLOAD_TOKEN);
  request.addHeader("Content-Type", "image/jpeg");
  request.addHeader("Connection", "close");
  const int status = request.POST(frame->buf, frame->len);
  if (responseBody != nullptr && status > 0)
    *responseBody = request.getString();
  request.end();
  return status == 202;
}

bool startCamera()
{
  camera_config_t config{};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_D0;
  config.pin_d1 = CAM_D1;
  config.pin_d2 = CAM_D2;
  config.pin_d3 = CAM_D3;
  config.pin_d4 = CAM_D4;
  config.pin_d5 = CAM_D5;
  config.pin_d6 = CAM_D6;
  config.pin_d7 = CAM_D7;
  config.pin_xclk = CAM_XCLK;
  config.pin_pclk = CAM_PCLK;
  config.pin_vsync = CAM_VSYNC;
  config.pin_href = CAM_HREF;
  config.pin_sccb_sda = CAM_SIOD;
  config.pin_sccb_scl = CAM_SIOC;
  config.pin_pwdn = CAM_PWDN;
  config.pin_reset = CAM_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  // Größere Zahl bedeutet stärkere JPEG-Kompression und kleinere WLAN-Pakete.
  config.jpeg_quality = 18;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  const esp_err_t error = esp_camera_init(&config);
  if (error != ESP_OK)
  {
    Serial.printf("Kamera-Initialisierung fehlgeschlagen: 0x%x\n", error);
    return false;
  }
  return true;
}

void uploadFrameToRelay()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return;
  }
  const uint32_t interval = consecutiveUploadFailures >= 3
                                ? DEGRADED_RELAY_INTERVAL_MS
                                : RELAY_INTERVAL_MS;
  if (millis() - lastRelayUploadAt < interval)
    return;
  lastRelayUploadAt = millis();

  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr)
  {
    Serial.println("Relay-Upload: kein Kamerabild.");
    return;
  }

  String responseBody;
  const bool uploaded = postJpeg(frame, "/frame", &responseBody);
  const size_t frameLength = frame->len;
  esp_camera_fb_return(frame);

  if (!uploaded)
  {
    if (consecutiveUploadFailures < UINT8_MAX)
      ++consecutiveUploadFailures;
    Serial.printf("Relay-Upload fehlgeschlagen (%u in Folge).\n",
                  consecutiveUploadFailures);
  }
  else
  {
    consecutiveUploadFailures = 0;
    applyServoCommand(responseBody);
    Serial.printf("Relay-Frame: %u Bytes\n",
                  static_cast<unsigned>(frameLength));
  }
}

void maintainWifi()
{
  if (WiFi.status() == WL_CONNECTED)
    return;
  if (millis() - lastWifiRetryAt < WIFI_RETRY_INTERVAL_MS)
    return;
  lastWifiRetryAt = millis();
  Serial.println("WLAN getrennt; neuer Verbindungsversuch ...");
  WiFi.reconnect();
}

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.printf("PSRAM: %s\n", psramFound() ? "erkannt" : "nicht erkannt");

  startServo();

  if (!startCamera())
    return;
  Serial.println("OV2640-Kamera bereit.");

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor != nullptr)
  {
    Serial.printf("Sensor PID: 0x%02x\n", sensor->id.PID);
  }
  Serial.println("Teste erstes Kamerabild ...");
  camera_fb_t *testFrame = esp_camera_fb_get();
  if (testFrame == nullptr)
  {
    Serial.println("FEHLER: Sensor erkannt, aber kein Bildsignal empfangen.");
  }
  else
  {
    Serial.printf("Testbild empfangen: %u Bytes\n",
                  static_cast<unsigned>(testFrame->len));
    esp_camera_fb_return(testFrame);
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Verbinde mit WLAN %s", WIFI_SSID);
  const uint32_t wifiStartedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStartedAt < 20000)
  {
    delay(500);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\nKamera verbunden: %s\n",
                  WiFi.localIP().toString().c_str());
  else
    Serial.println("\nWLAN noch nicht verbunden; Wiederholung läuft im Hintergrund.");
}

void loop()
{
  updateServoPower();
  maintainWifi();
  uploadFrameToRelay();
  delay(10);
}

#endif // CAMERA_TEST
