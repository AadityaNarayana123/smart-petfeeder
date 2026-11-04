/*
 * Smart Pet Feeder (Aami Pet-First Feeder)
 * ESP32 + ESP32-CAM, HX711 load cell, servo dispenser, buzzer, Supabase cloud.
 * RIT Kottayam - B.Tech ECE Mini Project 2025-26
 *
 * Setup: copy secrets.h.example -> secrets.h and fill in your credentials.
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include "HX711.h"
#include "time.h"
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "secrets.h"

// --- Configuration (from secrets.h) ---
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const String supabaseUrl = SUPABASE_URL;
const String supabaseAnonKey = SUPABASE_ANON_KEY;

// --- Pins ---
#define SERVO_PIN 13
#define BUZZER_PIN 19   // NOTE: GPIO19 is also camera D2 on AI-Thinker boards - see README "Known issues"
#define LOADCELL_DOUT 15
#define LOADCELL_SCK 14

// --- Globals ---
Servo feederServo;
HX711 scale;
float targetWt = 50.0;
float dispensedWt = 0.0;
String scheduledTime = "08:00";
String lastFedTime = "";
bool feedingInProgress = false;
int currentScheduleId = 1;
float calibration_factor = -130.0;

// --- Post-Feeding Monitoring ---
bool monitoringActive = false;
unsigned long monitorStartTime = 0;
float weightToCompare = 0.0;
const unsigned long MONITOR_DURATION = 1800000; // 30 mins

// --- Function Prototypes ---
void syncWithSupabase();
void executeFeedingSequence(bool isManual);
void updateDatabaseTrigger(int id, String column, bool value);
void logSessionToCloud(bool petSeen, float actualAmount);
void sendSupabaseNotification(String message, String status);
void checkPostFeedingStatus();
bool detectPetPresence();
String getClockTime();

void setup() {
  Serial.begin(115200);

  // Camera Init
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5; config.pin_d1 = 18; config.pin_d2 = 19; config.pin_d3 = 21;
  config.pin_d4 = 36; config.pin_d5 = 39; config.pin_d6 = 34; config.pin_d7 = 35;
  config.pin_xclk = 0; config.pin_pclk = 22; config.pin_vsync = 25;
  config.pin_href = 23; config.pin_sscb_sda = 26; config.pin_sscb_scl = 27;
  config.pin_pwdn = 32; config.pin_reset = -1;
  config.xclk_freq_hz = 20000000; config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_240X240; config.fb_count = 1;
  esp_camera_init(&config);

  feederServo.attach(SERVO_PIN);
  feederServo.write(0); delay(500); feederServo.detach();

  scale.begin(LOADCELL_DOUT, LOADCELL_SCK);
  scale.set_scale(calibration_factor);
  delay(2000); scale.tare(20);

  pinMode(BUZZER_PIN, OUTPUT);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }

  configTime(19800, 0, "pool.ntp.org", "time.nist.gov"); // IST (UTC+5:30)
  Serial.println("\n--- Aami Pet-First Feeder Online ---");
}

void loop() {
  checkPostFeedingStatus();
  syncWithSupabase();
  String currentTime = getClockTime();

  if (currentTime == scheduledTime && currentTime != lastFedTime && !feedingInProgress) {
    lastFedTime = currentTime;
    executeFeedingSequence(false); // Start the detection-first sequence
  }
  delay(5000);
}

void executeFeedingSequence(bool isManual) {
  feedingInProgress = true;
  Serial.println(">> System Triggered. Waiting for Pet...");

  // 1. Call the pet with the Buzzer
  digitalWrite(BUZZER_PIN, HIGH); delay(1000); digitalWrite(BUZZER_PIN, LOW);

  bool petArrived = false;

  // 2. 10-Minute Waiting Window (600,000 ms)
  unsigned long startWait = millis();
  while (millis() - startWait < 600000) {
    if (detectPetPresence()) {
      petArrived = true;
      Serial.println(">> Pet Detected! Proceeding to Dispense.");
      break;
    }
    delay(1000); // Check every second
  }

  // 3. Logic Gate: Only dispense if pet arrived OR if it was a manual override
  if (petArrived || isManual) {
    feederServo.attach(SERVO_PIN);
    float weightBefore = scale.get_units(15);
    float currentWeightFiltered = 0.0;

    feederServo.write(180);
    unsigned long dispenseStart = millis();

    while (currentWeightFiltered < targetWt) {
      float rawReading = scale.get_units(10);
      float latestDiff = rawReading - weightBefore;
      if (abs(latestDiff - currentWeightFiltered) < 80.0 || currentWeightFiltered == 0) {
          currentWeightFiltered = latestDiff;
      }
      if (millis() - dispenseStart > 15000) break;
      delay(50);
    }

    feederServo.write(0); delay(1000); feederServo.detach();

    float weightAfter = scale.get_units(20);
    dispensedWt = (weightAfter - weightBefore > 0) ? weightAfter - weightBefore : 0;

    // Start Post-Feeding Monitoring
    weightToCompare = weightAfter;
    monitorStartTime = millis();
    monitoringActive = true;

    logSessionToCloud(petArrived, dispensedWt);
  }
  else {
    // 4. Pet did not arrive within 10 minutes
    Serial.println(">> ALERT: Pet did not arrive for scheduled feeding.");
    sendSupabaseNotification("Pet did not arrive for scheduled feeding within 10 minutes", "alert");
    logSessionToCloud(false, 0.0); // Log a failed session with 0g dispensed
  }

  feedingInProgress = false;
}

// Helper for sending Notifications
void sendSupabaseNotification(String message, String status) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(supabaseUrl + "/rest/v1/notifications");
  http.addHeader("apikey", supabaseAnonKey);
  http.addHeader("Authorization", "Bearer " + supabaseAnonKey);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"message\": \"" + message + "\", \"status\": \""+ status +"\"}";
  http.POST(body);
  http.end();
}

void checkPostFeedingStatus() {
  if (monitoringActive && (millis() - monitorStartTime >= MONITOR_DURATION)) {
    float finalWeight = scale.get_units(20);
    if ((weightToCompare - finalWeight) < 5.0) {
      sendSupabaseNotification("Pet did not eat food after 30 minutes", "alert");
    }
    monitoringActive = false;
  }
}

void syncWithSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String schedUrl = supabaseUrl + "/rest/v1/feeding_schedules?select=id,time,amount_grams,manual_trigger,tare_trigger&is_enabled=eq.true&order=id.desc&limit=1";
  http.begin(schedUrl);
  http.addHeader("apikey", supabaseAnonKey);
  http.addHeader("Authorization", "Bearer " + supabaseAnonKey);

  int httpCode = http.GET();
  if (httpCode == 200) {
    StaticJsonDocument<1024> doc;
    deserializeJson(doc, http.getString());
    if (doc.size() > 0) {
      currentScheduleId = doc[0]["id"];
      scheduledTime = doc[0]["time"].as<String>().substring(0, 5);
      targetWt = doc[0]["amount_grams"];
      if (doc[0]["manual_trigger"] == true && !feedingInProgress) {
        updateDatabaseTrigger(currentScheduleId, "manual_trigger", false);
        executeFeedingSequence(true);
      }
      if (doc[0]["tare_trigger"] == true) {
        updateDatabaseTrigger(currentScheduleId, "tare_trigger", false);
        scale.tare(30);
      }
    }
  }
  http.end();
}

void updateDatabaseTrigger(int id, String column, bool value) {
  HTTPClient http;
  String url = supabaseUrl + "/rest/v1/feeding_schedules?id=eq." + String(id);
  http.begin(url);
  http.addHeader("apikey", supabaseAnonKey);
  http.addHeader("Authorization", "Bearer " + supabaseAnonKey);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"" + column + "\": " + (value ? "true" : "false") + "}";
  http.PATCH(body);
  http.end();
}

void logSessionToCloud(bool petSeen, float actualAmount) {
  HTTPClient http;
  http.begin(supabaseUrl + "/rest/v1/feeding_sessions");
  http.addHeader("apikey", supabaseAnonKey);
  http.addHeader("Authorization", "Bearer " + supabaseAnonKey);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"scheduled_time\": \"" + getClockTime() + "\", \"dispense_amount\": " + String(actualAmount) + ", \"pet_detected\": " + (petSeen ? "true" : "false") + "}";
  http.POST(body);
  http.end();
}

bool detectPetPresence() {
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) return false;

  // NOTE: For true AI detection, insert your Edge Impulse
  // classification code here. Currently returns 'true' (simulation).
  esp_camera_fb_return(fb);
  return true;
}

String getClockTime() {
  struct tm info;
  if(!getLocalTime(&info)) return "00:00";
  char buf[6]; strftime(buf, 6, "%H:%M", &info);
  return String(buf);
}
