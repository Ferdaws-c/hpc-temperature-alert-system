/*
 * HPC Temperature Monitoring System
 * ESP32 with DHT11 (Cooler Intake) + DHT22 (Exhaust)
 *
 * Features:
 *  - DHT11 + DHT22 sensor reading
 *  - LCD + RGB LED status display
 *  - Supabase HTTP POST
 *  - Ring buffer for unsent readings
 *  - Non-blocking WiFi reconnect with exponential backoff
 *  - HTTP POST retry with exponential backoff
 *  - Structured Serial JSON logs
 *  - Dynamic threshold fetching from Supabase
 */

#include <WiFi.h>
#include "wifi_config.h"
#include "esp_eap_client.h"
#include <HTTPClient.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>

// ─── Supabase Config ─────────────────────────────────────────────────────────
// Put your real values here, but do NOT show them in report screenshots.
const char* SUPABASE_URL    = "https://ahzocemntuurmqnicrmj.supabase.co/rest/v1/sensor_readings";
const char* SUPABASE_BASE   = "https://ahzocemntuurmqnicrmj.supabase.co/rest/v1/";
const char* SUPABASE_APIKEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImFoem9jZW1udHV1cm1xbmljcm1qIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzM3NjU4MzYsImV4cCI6MjA4OTM0MTgzNn0.Y-3KeaN3L6X3n6I4ZKhOcTYVA3O6GcSBl2qiA-VpbIE";

// ─── Sensor IDs ──────────────────────────────────────────────────────────────
const char* SENSOR_ID_DHT11 = "02952bb9-3804-490c-bf01-ec760a0a21dc";
const char* SENSOR_ID_DHT22 = "dd0e2f64-129c-4066-a1cb-29f1165ad4a7";

// ─── Pin Definitions ─────────────────────────────────────────────────────────
#define DHT11_PIN  21
#define DHT22_PIN  22
#define DHT11_TYPE DHT11
#define DHT22_TYPE DHT22

DHT dht11(DHT11_PIN, DHT11_TYPE);
DHT dht22(DHT22_PIN, DHT22_TYPE);

// ─── LCD Config ───────────────────────────────────────────────────────────────
#define LCD_ADDRESS 0x27
#define LCD_COLUMNS 16
#define LCD_ROWS    2

LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLUMNS, LCD_ROWS);

// ─── LED PWM Channels & Pins ─────────────────────────────────────────────────
const int redChannel   = 0;
const int greenChannel = 1;
const int blueChannel  = 2;

const int redPin   = 25;
const int greenPin = 26;
const int bluePin  = 27;

// ─── Timing ──────────────────────────────────────────────────────────────────
const unsigned long READ_INTERVAL      = 2000;    // 2s sensor read
const unsigned long SEND_INTERVAL      = 10000;   // 10s buffer + send
const unsigned long BLINK_INTERVAL     = 500;     // warning LED blink
const unsigned long THRESHOLD_INTERVAL = 30000;   // 30s threshold fetch

// ─── Fault Tolerance Timing ─────────────────────────────────────────────────
const unsigned long WIFI_RETRY_BASE = 2000;
const unsigned long HTTP_RETRY_BASE = 1000;

const int MAX_WIFI_RETRIES = 6;
const int MAX_HTTP_RETRIES = 3;

// No valid sensor read within this window = sensor offline, stop POSTing cached values.
// Must be < server-side SENSOR_OFFLINE_THRESHOLD_SECONDS so the DB stops getting rows
// before the server gives up.
const unsigned long SENSOR_STALE_MS = 10000;

// ─── Main Timers ─────────────────────────────────────────────────────────────
unsigned long lastReadTime      = 0;
unsigned long lastSendTime      = 0;
unsigned long lastBlinkTime     = 0;
unsigned long lastThresholdTime = 0;

// ─── Non-blocking WiFi Reconnect State ──────────────────────────────────────
unsigned long lastWiFiRetryTime = 0;
unsigned long wifiBackoff       = WIFI_RETRY_BASE;
int wifiAttempt                 = 0;
bool wifiReconnectStarted       = false;

// ─── Cached Sensor Values ───────────────────────────────────────────────────
float latestTemp11 = NAN;
float latestHumi11 = NAN;
float latestTemp22 = NAN;
float latestHumi22 = NAN;

unsigned long lastValidRead11 = 0;  // millis() of last non-NaN DHT11 read; 0 = never
unsigned long lastValidRead22 = 0;  // millis() of last non-NaN DHT22 read; 0 = never

// ─── Thresholds ─────────────────────────────────────────────────────────────
float thresholdWarnMin = 35.0;
float thresholdColdMax = 18.0;
bool thresholdsFetched = false;

// ─── State ──────────────────────────────────────────────────────────────────
bool ledState  = false;
bool isWarning = false;

// ─── Ring Buffer ────────────────────────────────────────────────────────────
#define BUFFER_SIZE 32

struct Reading {
  char sensor_id[37];
  float temperature;
  float humidity;
  bool valid;
};

Reading ringBuffer[BUFFER_SIZE];
int bufHead  = 0;
int bufTail  = 0;
int bufCount = 0;

// ─── Function Prototypes ────────────────────────────────────────────────────
void readSensorsAndDisplay();
void fetchThresholds();
void postToSupabase();
bool sendToSupabase(const char* sensorId, float temperature, float humidity);
void connectWiFi();
bool ensureWiFi();
void setColor(int r, int g, int b);
void bufferPush(const Reading& r);
bool bufferPeek(Reading& r);
void bufferPop();
void logJson(const char* event, const char* sensorId = "", int httpCode = 0, const char* detail = "");

// ────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("{\"event\":\"boot\",\"detail\":\"HPC Monitoring System Starting\"}");

  dht11.begin();
  dht22.begin();
  Serial.println("{\"event\":\"sensors_initialized\",\"detail\":\"DHT11 and DHT22 initialized\"}");

  Wire.begin(13, 14);
  lcd.init();
  lcd.backlight();
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("System Ready");
  lcd.setCursor(0, 1);
  lcd.print("WIFI Connecting");

  ledcAttachChannel(redPin,   5000, 8, redChannel);
  ledcAttachChannel(greenPin, 5000, 8, greenChannel);
  ledcAttachChannel(bluePin,  5000, 8, blueChannel);

  setColor(0, 0, 0);

  connectWiFi();
  fetchThresholds();
}

// ────────────────────────────────────────────────────────────────────────────
void loop() {
  ensureWiFi();   // non-blocking reconnect

  unsigned long now = millis();

  if (isWarning && (now - lastBlinkTime >= BLINK_INTERVAL)) {
    lastBlinkTime = now;
    ledState = !ledState;
    setColor(ledState ? 255 : 0, 0, 0);
  }

  if (now - lastReadTime >= READ_INTERVAL) {
    lastReadTime = now;
    readSensorsAndDisplay();
  }

  if (now - lastSendTime >= SEND_INTERVAL) {
    lastSendTime = now;
    postToSupabase();
  }

  if (now - lastThresholdTime >= THRESHOLD_INTERVAL) {
    lastThresholdTime = now;
    fetchThresholds();
  }
}

// ─── Fetch thresholds from Supabase led_thresholds table ────────────────────
void fetchThresholds() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[THRESHOLD] No WiFi — keeping last values.");
    logJson("threshold_no_wifi", "", 0, "keeping last threshold values");
    return;
  }

  HTTPClient http;

  String url = String(SUPABASE_URL);
  url.replace("sensor_readings", "led_thresholds");

  http.begin(url);
  http.addHeader("apikey",        SUPABASE_APIKEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_APIKEY);
  http.addHeader("Content-Type",  "application/json");

  int httpCode = http.GET();

  if (httpCode == 200) {
    String body = http.getString();
    Serial.println("[THRESHOLD] Response: " + body);

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
      Serial.println("[THRESHOLD] JSON parse error — keeping last values.");
      http.end();
      return;
    }

    for (JsonObject row : doc.as<JsonArray>()) {
      const char* color = row["led_color"];
      float minTemp    = row["min_temp"];

      if (strcmp(color, "red") == 0) {
        thresholdWarnMin = minTemp;
        Serial.printf("[THRESHOLD] Red threshold updated -> %.1f°C\n", thresholdWarnMin);
      }

      if (strcmp(color, "blue") == 0) {
        thresholdColdMax = minTemp;
        Serial.printf("[THRESHOLD] Blue threshold updated -> %.1f°C\n", thresholdColdMax);
      }
    }

    thresholdsFetched = true;
  } else {
    Serial.printf("[THRESHOLD] Fetch failed: %d — keeping last values.\n", httpCode);
  }

  http.end();
}

// ─── Read sensors, update LCD and LED ───────────────────────────────────────
void readSensorsAndDisplay() {
  float t11 = dht11.readTemperature();
  float h11 = dht11.readHumidity();
  float t22 = dht22.readTemperature();
  float h22 = dht22.readHumidity();

  Serial.println("──────────────────────────────");
  Serial.printf("[DHT11] Temp: %.1f°C | Humidity: %.1f%%\n", t11, h11);
  Serial.printf("[DHT22] Temp: %.1f°C | Humidity: %.1f%%\n", t22, h22);

  if (!isnan(t11) && !isnan(h11)) {
    latestTemp11 = t11;
    latestHumi11 = h11;
    lastValidRead11 = millis();
  }

  if (!isnan(t22) && !isnan(h22)) {
    if (t22 < 0.0 && latestTemp22 > 30.0) {
      Serial.println("[WARNING] DHT22 EMI glitch detected. Discarding reading.");
      logJson("sensor_read_error", "DHT22", 0, "EMI glitch suspected");
    } else {
      latestTemp22 = t22;
      latestHumi22 = h22;
      lastValidRead22 = millis();
    }
  }

  if (isnan(latestTemp11) || isnan(latestTemp22)) {
    lcd.setCursor(0, 0);
    lcd.print("Waiting...      ");
    lcd.setCursor(0, 1);
    lcd.print("Check sensors   ");
    return;
  }

  float avgTemp = (latestTemp11 + latestTemp22) / 2.0;
  float avgHumi = (latestHumi11 + latestHumi22) / 2.0;

  lcd.setCursor(0, 0);

  if (avgTemp > thresholdWarnMin) {
    isWarning = true;
    lcd.print("WARN! ");
    lcd.print(avgTemp, 1);
    lcd.print((char)223);
    lcd.print("C   ");
  }
  else if (avgTemp < thresholdColdMax) {
    isWarning = false;
    ledState = false;
    setColor(0, 0, 255);

    lcd.print("COLD! ");
    lcd.print(avgTemp, 1);
    lcd.print((char)223);
    lcd.print("C   ");
  }
  else {
    isWarning = false;
    ledState = false;
    setColor(0, 255, 0);

    lcd.print("Temp: ");
    lcd.print(avgTemp, 1);
    lcd.print((char)223);
    lcd.print("C   ");
  }

  char buffer[16];
  sprintf(buffer, "Humi: %.1f%%    ", avgHumi);
  lcd.setCursor(0, 1);
  lcd.print(buffer);
}

// ─── Queue latest values and flush buffer ───────────────────────────────────
void postToSupabase() {
  unsigned long now = millis();
  bool dht11_stale = (lastValidRead11 == 0) || (now - lastValidRead11 > SENSOR_STALE_MS);
  bool dht22_stale = (lastValidRead22 == 0) || (now - lastValidRead22 > SENSOR_STALE_MS);

  if (isnan(latestTemp11) || isnan(latestHumi11)) {
    Serial.println("[DHT11] No valid reading yet — skipping POST.");
    logJson("queue_skip", "DHT11", 0, "no valid reading yet");
  }
  else if (dht11_stale) {
    Serial.println("[DHT11] Sensor unresponsive — skipping POST.");
    logJson("queue_skip", "DHT11", 0, "sensor offline, no recent valid read");
  }
  else if (latestTemp11 < 50.0) {
    Reading r1;
    strncpy(r1.sensor_id, SENSOR_ID_DHT11, sizeof(r1.sensor_id));
    r1.sensor_id[sizeof(r1.sensor_id) - 1] = '\0';
    r1.temperature = latestTemp11;
    r1.humidity    = latestHumi11;
    r1.valid       = true;

    bufferPush(r1);
  }
  else {
    Serial.println("[DHT11] Temp >= 50°C — skipping POST.");
    logJson("queue_skip", "DHT11", 0, "temperature out of expected range");
  }

  if (isnan(latestTemp22) || isnan(latestHumi22)) {
    Serial.println("[DHT22] No valid reading yet — skipping POST.");
    logJson("queue_skip", "DHT22", 0, "no valid reading yet");
  }
  else if (dht22_stale) {
    Serial.println("[DHT22] Sensor unresponsive — skipping POST.");
    logJson("queue_skip", "DHT22", 0, "sensor offline, no recent valid read");
  }
  else {
    Reading r2;
    strncpy(r2.sensor_id, SENSOR_ID_DHT22, sizeof(r2.sensor_id));
    r2.sensor_id[sizeof(r2.sensor_id) - 1] = '\0';
    r2.temperature = latestTemp22;
    r2.humidity    = latestHumi22;
    r2.valid       = true;

    bufferPush(r2);
  }

  Reading next;

  while (bufferPeek(next)) {
    bool ok = sendToSupabase(next.sensor_id, next.temperature, next.humidity);

    if (ok) {
      bufferPop();
    } else {
      break;
    }
  }

  Serial.printf(
    "{\"event\":\"buffer_status\",\"count\":%d,\"capacity\":%d}\n",
    bufCount,
    BUFFER_SIZE
  );
}

// ─── POST one reading to Supabase with HTTP retry ───────────────────────────
bool sendToSupabase(const char* sensorId, float temperature, float humidity) {
  unsigned long backoff = HTTP_RETRY_BASE;

  for (int attempt = 1; attempt <= MAX_HTTP_RETRIES; attempt++) {
    if (WiFi.status() != WL_CONNECTED) {
      logJson("post_no_wifi", sensorId, 0, "kept in buffer");
      return false;
    }

    HTTPClient http;

    http.begin(SUPABASE_URL);
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("apikey",        SUPABASE_APIKEY);
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_APIKEY);
    http.addHeader("Prefer",        "return=minimal");

    StaticJsonDocument<128> doc;
    doc["sensor_id"]   = sensorId;
    doc["temperature"] = temperature;
    doc["humidity"]    = humidity;

    String payload;
    serializeJson(doc, payload);

    Serial.printf("[HTTP] Sending -> %s\n", payload.c_str());

    int httpCode = http.POST(payload);

    if (httpCode == 201) {
      Serial.printf("[HTTP] Response code: %d\n", httpCode);
      Serial.println("[HTTP] Data inserted successfully.");
      logJson("post_success", sensorId, httpCode, "inserted");

      http.end();
      return true;
    }

    bool retryable = (httpCode <= 0) || (httpCode >= 500 && httpCode < 600);

    if (!retryable) {
      Serial.printf("[HTTP] Permanent error: %d\n", httpCode);
      logJson("post_permanent_error", sensorId, httpCode, "dropping");

      http.end();

      // Return true so it is removed from buffer.
      // Example: 401/403/404 should not retry forever.
      return true;
    }

    Serial.printf(
      "[HTTP] Retryable error: %d — attempt %d/%d\n",
      httpCode,
      attempt,
      MAX_HTTP_RETRIES
    );

    logJson("post_retryable_error", sensorId, httpCode, "will retry");

    http.end();

    delay(backoff);
    backoff = min(backoff * 2, (unsigned long)16000);
  }

  logJson("post_max_retries_exceeded", sensorId, -1, "kept in buffer");
  return false;
}

// ─── Initial WPA2-Enterprise WiFi Connect ───────────────────────────────────
void connectWiFi() {
  Serial.printf("[WIFI] Connecting to %s (WPA2-Enterprise)\n", WIFI_SSID);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);

  esp_eap_client_set_identity((uint8_t*)WIFI_USERNAME, strlen(WIFI_USERNAME));
  esp_eap_client_set_username((uint8_t*)WIFI_USERNAME, strlen(WIFI_USERNAME));
  esp_eap_client_set_password((uint8_t*)WIFI_PASSWORD, strlen(WIFI_PASSWORD));
  esp_wifi_sta_enterprise_enable();

  WiFi.begin(WIFI_SSID);

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] Failed to connect. Will retry in loop.");
  }
}

// ─── Non-blocking WPA2-Enterprise WiFi Reconnect ────────────────────────────
bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiReconnectStarted || wifiAttempt > 0) {
      Serial.printf(
        "{\"event\":\"wifi_reconnected\",\"attempt\":%d,\"ip\":\"%s\"}\n",
        wifiAttempt,
        WiFi.localIP().toString().c_str()
      );
    }

    wifiAttempt = 0;
    wifiBackoff = WIFI_RETRY_BASE;
    wifiReconnectStarted = false;

    return true;
  }

  unsigned long now = millis();

  if (!wifiReconnectStarted) {
    Serial.println("{\"event\":\"wifi_reconnect_start\"}");

    wifiReconnectStarted = true;
    lastWiFiRetryTime = 0;
    wifiAttempt = 0;
    wifiBackoff = WIFI_RETRY_BASE;
  }

  if (wifiAttempt >= MAX_WIFI_RETRIES) {
    Serial.println("{\"event\":\"wifi_reconnect_failed\"}");

    wifiAttempt = 0;
    wifiBackoff = WIFI_RETRY_BASE;
    wifiReconnectStarted = false;
    lastWiFiRetryTime = now;

    return false;
  }

  if (lastWiFiRetryTime == 0 || now - lastWiFiRetryTime >= wifiBackoff) {
    wifiAttempt++;
    lastWiFiRetryTime = now;

    WiFi.disconnect();

    esp_eap_client_set_identity((uint8_t*)WIFI_USERNAME, strlen(WIFI_USERNAME));
    esp_eap_client_set_username((uint8_t*)WIFI_USERNAME, strlen(WIFI_USERNAME));
    esp_eap_client_set_password((uint8_t*)WIFI_PASSWORD, strlen(WIFI_PASSWORD));
    esp_wifi_sta_enterprise_enable();

    WiFi.begin(WIFI_SSID);

    Serial.printf(
      "{\"event\":\"wifi_attempt\",\"attempt\":%d,\"backoff_ms\":%lu}\n",
      wifiAttempt,
      wifiBackoff
    );

    wifiBackoff = min(wifiBackoff * 2, (unsigned long)64000);
  }

  return false;
}

// ─── Ring Buffer Helpers ────────────────────────────────────────────────────
void bufferPush(const Reading& r) {
  if (bufCount == BUFFER_SIZE) {
    Serial.printf(
      "{\"event\":\"buffer_overflow\",\"dropped_sensor\":\"%s\"}\n",
      ringBuffer[bufTail].sensor_id
    );

    bufTail = (bufTail + 1) % BUFFER_SIZE;
    bufCount--;
  }

  ringBuffer[bufHead] = r;
  bufHead = (bufHead + 1) % BUFFER_SIZE;
  bufCount++;

  Serial.printf(
    "{\"event\":\"buffer_push\",\"sensor_id\":\"%s\",\"temperature\":%.1f,\"humidity\":%.1f,\"count\":%d,\"capacity\":%d}\n",
    r.sensor_id,
    r.temperature,
    r.humidity,
    bufCount,
    BUFFER_SIZE
  );
}

bool bufferPeek(Reading& r) {
  if (bufCount == 0) {
    return false;
  }

  r = ringBuffer[bufTail];
  return true;
}

void bufferPop() {
  if (bufCount == 0) {
    return;
  }

  Serial.printf(
    "{\"event\":\"buffer_pop\",\"sensor_id\":\"%s\",\"count_before\":%d}\n",
    ringBuffer[bufTail].sensor_id,
    bufCount
  );

  bufTail = (bufTail + 1) % BUFFER_SIZE;
  bufCount--;

  Serial.printf(
    "{\"event\":\"buffer_after_pop\",\"count\":%d,\"capacity\":%d}\n",
    bufCount,
    BUFFER_SIZE
  );
}

// ─── JSON Logger ────────────────────────────────────────────────────────────
void logJson(const char* event, const char* sensorId, int httpCode, const char* detail) {
  Serial.printf(
    "{\"event\":\"%s\",\"sensor_id\":\"%s\",\"http_code\":%d,\"detail\":\"%s\"}\n",
    event,
    sensorId,
    httpCode,
    detail
  );
}

// ─── RGB LED via PWM ────────────────────────────────────────────────────────
void setColor(int r, int g, int b) {
  ledcWrite(redPin,   r);
  ledcWrite(greenPin, g);
  ledcWrite(bluePin,  b);
}