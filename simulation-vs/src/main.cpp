#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include <time.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

// Set to true when running inside Wokwi simulator********
// Set to false when flashing to real ESP32 hardware**********
#define IS_SIMULATION true

// --- Pin Definitions ---
const int TRIG_PIN = 5;
const int ECHO_PIN = 18;
const int STATUS_LED_PIN = 2;
const int CALIBRATION_BUTTON_PIN = 4;

// --- Physical Drain Configuration ---
const float DEFAULT_DRAIN_DEPTH_MM = 4000.0;
float drainDepthMM = DEFAULT_DRAIN_DEPTH_MM;
Preferences preferences;

// --- Power Saving Sleep Configuration ---
#define TIME_TO_SLEEP_SEC 5 
#define uS_TO_S_FACTOR 1000000ULL

// --- Filtering Configuration ---
const int FILTER_WINDOW_SIZE = 5;

// RTA wala thiyena hinda , deep sleep eken passe values reset wenne na 
RTC_DATA_ATTR float readings[FILTER_WINDOW_SIZE] = {0};
RTC_DATA_ATTR int readIndex = 0;
RTC_DATA_ATTR float total = 0.0;
RTC_DATA_ATTR float filteredWaterLevel = 0.0;
RTC_DATA_ATTR float previousWaterLevel = 0.0;
RTC_DATA_ATTR bool isFirstBoot = true;
RTC_DATA_ATTR uint8_t heartbeatSamplesSince = 0;
RTC_DATA_ATTR uint8_t lastNotifiedAlertState = 0;

enum class AlertState { NORMAL, SURGE, OVERFLOW };
AlertState alertState = AlertState::NORMAL;
unsigned long lastMeasurementMs = 0;
bool calibrationButtonWasPressed = false;
float measureRawDistanceMM();

// Surge Threshold (mm/sec rise rate)
const float SURGE_THRESHOLD_MM_S = 10.0;

void beginWiFiConnection() {
  if (IS_SIMULATION) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

bool synchronizeClock() {
  WiFi.mode(WIFI_STA);
  Serial.println("Time sync: connecting to Wi-Fi...");
  beginWiFiConnection();

  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000UL) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("Wi-Fi connection failed; status code: ");
    Serial.println(WiFi.status());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  Serial.print("Wi-Fi connected; IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("Time sync: requesting NTP time...");
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  struct tm utcTime;
  bool synchronized = getLocalTime(&utcTime, 10000);
  Serial.println(synchronized ? "NTP time synchronized." : "NTP request failed.");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return synchronized;
}

String currentTimestampUtc() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return "NOT_SYNCHRONIZED";
  }

  struct tm utcTime;
  gmtime_r(&now, &utcTime);
  char timestamp[25];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
  return String(timestamp);
}

void printTimestamp() {
  Serial.print("Timestamp (UTC): ");
  Serial.println(currentTimestampUtc());
}

String escapeJsonString(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char character = value[i];
    if (character == '"' || character == '\\') escaped += '\\';
    if (character == '\n') {
      escaped += "\\n";
    } else if (character == '\r') {
      escaped += "\\r";
    } else {
      escaped += character;
    }
  }
  return escaped;
}

bool sendTelegramMessage(const String& message) {
  if (strlen(TELEGRAM_BOT_TOKEN) == 0 || strlen(TELEGRAM_CHAT_ID) == 0 ||
      strlen(TELEGRAM_ROOT_CA) == 0) {
    Serial.println("Telegram skipped: configure bot token, channel ID, and root CA in src/secrets.h.");
    return false;
  }

  WiFi.mode(WIFI_STA);
  beginWiFiConnection();
  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000UL) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("Telegram failed: Wi-Fi connection unavailable.");
    return false;
  }

  WiFiClientSecure tlsClient;
  tlsClient.setCACert(TELEGRAM_ROOT_CA);
  HTTPClient http;
  String url = "https://api.telegram.org/bot";
  url += TELEGRAM_BOT_TOKEN;
  url += "/sendMessage";

  bool sent = false;
  if (http.begin(tlsClient, url)) {
    http.addHeader("Content-Type", "application/json");
    String payload = "{\"chat_id\":\"";
    payload += escapeJsonString(TELEGRAM_CHAT_ID);
    payload += "\",\"text\":\"";
    payload += escapeJsonString(message);
    payload += "\"}";
    http.setTimeout(10000);
    int responseCode = http.POST(payload);
    sent = responseCode == 200;
    if (!sent) {
      String responseBody = http.getString();
      Serial.print("Telegram send failed, HTTP status: ");
      Serial.println(responseCode);
      Serial.print("Telegram response: ");
      Serial.println(responseBody);
    }
    http.end();
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return sent;
}

void notifyTelegramOnAlert(const String& status, float waterLevelMM, float rateOfRise) {
  uint8_t currentState = static_cast<uint8_t>(alertState);
  if (currentState == 0) {
    lastNotifiedAlertState = 0;
    return;
  }
  if (currentState == lastNotifiedAlertState) return;

  String message = "Galano Galano!!! Flood alert: ";
  message += status;
  message += "\nTime (UTC): ";
  message += currentTimestampUtc();
  message += "\nWater level: ";
  message += String(waterLevelMM, 1);
  message += " mm\nRate of rise: ";
  message += String(rateOfRise, 1);
  message += " mm/s";

  if (sendTelegramMessage(message)) {
    lastNotifiedAlertState = currentState;
  }
}

void loadDrainDepth() {
  if (preferences.begin("flood-monitor", true)) {
    drainDepthMM = preferences.getFloat("depth_mm", DEFAULT_DRAIN_DEPTH_MM);
    preferences.end();
  }
}

void resetWaterLevelFilter() {
  for (int i = 0; i < FILTER_WINDOW_SIZE; i++) {
    readings[i] = 0;
  }
  readIndex = 0;
  total = 0;
  filteredWaterLevel = 0;
  previousWaterLevel = 0;
  isFirstBoot = true;
  alertState = AlertState::NORMAL;
}

void calibrateDrainDepth() {
  float measuredDepthMM = measureRawDistanceMM();
  if (measuredDepthMM <= 0 || measuredDepthMM > 6800.0) {
    printTimestamp();
    Serial.println("Calibration failed: invalid sensor distance.");
    return;
  }

  if (!preferences.begin("flood-monitor", false)) {
    printTimestamp();
    Serial.println("Calibration failed: settings storage unavailable.");
    return;
  }

  size_t bytesWritten = preferences.putFloat("depth_mm", measuredDepthMM);
  preferences.end();
  if (bytesWritten != sizeof(float)) {
    printTimestamp();
    Serial.println("Calibration failed: could not save drain depth.");
    return;
  }

  drainDepthMM = measuredDepthMM;
  resetWaterLevelFilter();
  printTimestamp();
  Serial.print("Drain depth calibrated and saved: ");
  Serial.print(drainDepthMM);
  Serial.println(" mm");
}

void handleCalibrationButton() {
  bool buttonPressed = digitalRead(CALIBRATION_BUTTON_PIN) == LOW;
  if (buttonPressed && !calibrationButtonWasPressed) {
    delay(30);
    if (digitalRead(CALIBRATION_BUTTON_PIN) == LOW) {
      calibrationButtonWasPressed = true;
      calibrateDrainDepth();
    }
  } else if (!buttonPressed) {
    calibrationButtonWasPressed = false;
  }
}

void updateStatusIndicator() {
  unsigned long elapsedMs = millis();
  bool ledOn = false;

  switch (alertState) {
    case AlertState::SURGE:
      ledOn = (elapsedMs % 1000UL) < 500UL;
      break;
    case AlertState::OVERFLOW:
      ledOn = true;
      break;
    case AlertState::NORMAL:
      ledOn = (elapsedMs % 30000UL) < 1000UL;
      break;
  }

  digitalWrite(STATUS_LED_PIN, ledOn ? HIGH : LOW);
}

void enterDeepSleep() {
  if (alertState == AlertState::SURGE) {
    return;
  }

  if (alertState == AlertState::NORMAL && heartbeatSamplesSince >= 6) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(1000);
    digitalWrite(STATUS_LED_PIN, LOW);
    heartbeatSamplesSince = 0;
  } else {
    digitalWrite(STATUS_LED_PIN, alertState == AlertState::OVERFLOW ? HIGH : LOW);
  }

  gpio_hold_en((gpio_num_t)STATUS_LED_PIN);
  gpio_deep_sleep_hold_en();
  Serial.println("Entering deep sleep...");
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)TIME_TO_SLEEP_SEC * uS_TO_S_FACTOR);
  rtc_gpio_pulldown_dis((gpio_num_t)CALIBRATION_BUTTON_PIN);
  rtc_gpio_pullup_en((gpio_num_t)CALIBRATION_BUTTON_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)CALIBRATION_BUTTON_PIN, 0);
  esp_deep_sleep_start();
}

float measureRawDistanceMM() {
  // Ensure trigger line is clear and settled
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(4);
  
  // 10 microsecond trigger pulse
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Measure echo pulse (40ms timeout corresponds to ~6.8m maximum distance)
  long duration = pulseIn(ECHO_PIN, HIGH, 40000);
  if (duration <= 0) {
    return -1.0;
  }
  
  // Speed of sound = 343 m/s = 0.343 mm/us
  return (duration * 0.343) / 2.0;
}

void processDrainMeasurement() {
  float rawDistanceMM = measureRawDistanceMM();

  if (rawDistanceMM >= 0) {
    // 1. Calculate water level from surface distance
    float currentWaterLevel = drainDepthMM - rawDistanceMM;
    if (currentWaterLevel < 0) currentWaterLevel = 0;

    // 2. Moving Average Filter using RTC persistent buffer
    if (isFirstBoot) {
      for (int i = 0; i < FILTER_WINDOW_SIZE; i++) {
        readings[i] = currentWaterLevel;
      }
      total = currentWaterLevel * FILTER_WINDOW_SIZE;
      filteredWaterLevel = currentWaterLevel;
      previousWaterLevel = currentWaterLevel;
      isFirstBoot = false;
    } else {
      total = total - readings[readIndex];
      readings[readIndex] = currentWaterLevel;
      total = total + readings[readIndex];
      readIndex = (readIndex + 1) % FILTER_WINDOW_SIZE;
      filteredWaterLevel = total / FILTER_WINDOW_SIZE;
    }

    // 3. Compute Rate of Rise (dh/dt)
    float timeDeltaSeconds = (float)TIME_TO_SLEEP_SEC;
    float rateOfRise = (filteredWaterLevel - previousWaterLevel) / timeDeltaSeconds;

    // 4. Alert Status Logic
    String status = "NORMAL";
    if (filteredWaterLevel >= (drainDepthMM * 0.7)) {
      alertState = AlertState::OVERFLOW;
      status = "OVERFLOW CRITICAL (CAPACITY LIMIT)";
    } else if (rateOfRise >= SURGE_THRESHOLD_MM_S) {
      alertState = AlertState::SURGE;
      status = "SURGE WARNING (RAPID INFLOW)";
    } else {
      alertState = AlertState::NORMAL;
    }

    if (heartbeatSamplesSince < 6) heartbeatSamplesSince++;

    // 5. Output Telemetry Data to Serial Monitor
    Serial.println("----------------------------------------");
    printTimestamp();
    Serial.print("Raw Distance: "); Serial.print(rawDistanceMM); Serial.println(" mm");
    Serial.print("Drain Depth: "); Serial.print(drainDepthMM); Serial.println(" mm");
    Serial.print("Filtered Water Level: "); Serial.print(filteredWaterLevel); Serial.println(" mm");
    Serial.print("Rate of Rise (dh/dt): "); Serial.print(rateOfRise); Serial.println(" mm/s");
    Serial.print("System Status: "); Serial.println(status);
    notifyTelegramOnAlert(status, filteredWaterLevel, rateOfRise);

    previousWaterLevel = filteredWaterLevel;
  } else {
    Serial.println("Sensor read timeout or invalid reading. Skipping cycle update...");
  }
}

void setup() {
  Serial.begin(115200);
  loadDrainDepth();
  if (time(nullptr) < 1700000000 && !synchronizeClock()) {
    Serial.println("NTP time sync failed; calendar timestamps will be unavailable.");
  }

  // Configure pins and guarantee default idle state
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(CALIBRATION_BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(TRIG_PIN, LOW);

  if (IS_SIMULATION) {
    Serial.println("Simulation Mode Initialized.");
    lastMeasurementMs = millis() - (TIME_TO_SLEEP_SEC * 1000UL);
  } else {
    // Hardware deep sleep execution flow
    Serial.println("\n=== Node Wakeup ===");
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)STATUS_LED_PIN);
    
    // Allow sensor line voltage and transducer to stabilize after wake
    delay(50); 

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
      delay(30);
      calibrateDrainDepth();
      while (digitalRead(CALIBRATION_BUTTON_PIN) == LOW) delay(10);
    } else {
      processDrainMeasurement();
    }
    lastMeasurementMs = millis();
    enterDeepSleep();
  }
}

void loop() {
  updateStatusIndicator();
  handleCalibrationButton();

  if (IS_SIMULATION) {
    if (millis() - lastMeasurementMs >= TIME_TO_SLEEP_SEC * 1000UL) {
      lastMeasurementMs = millis();
      processDrainMeasurement();
    }
  } else if (alertState == AlertState::SURGE &&
             millis() - lastMeasurementMs >= TIME_TO_SLEEP_SEC * 1000UL) {
    lastMeasurementMs = millis();
    processDrainMeasurement();
    enterDeepSleep();
  }

  delay(10);
}