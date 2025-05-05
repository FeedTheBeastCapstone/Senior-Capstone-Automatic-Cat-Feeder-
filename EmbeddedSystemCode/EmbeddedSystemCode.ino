#include <WiFi.h>
#include "time.h"
#include <Firebase_ESP_Client.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>

// --------------------------------
// Wi‑Fi & Firebase Credentials
// --------------------------------

// For using at home
const char* ssid          = "MyOptimum c4b903";
const char* password      = "umber-981-965";

const char* ntpServer     = "pool.ntp.org";
const long  gmtOffset     = -21600; // UTC‑6
const int   dstOffset     = 3600;

#define API_KEY       "AIzaSyB0xVGSfvV7Ok_SuGNgRV3X2lVsQlb1Y3w"      // Firebase API key
#define DATABASE_URL  "https://automatic-pet-feeder-44998-default-rtdb.firebaseio.com/"  // Firebase DB URL
#define USER_EMAIL    "test123@gmail.com"                            // Firebase user email
#define USER_PASSWORD "test123"                                     // Firebase user password

// --------------------------------
// Calibration: grams per second
// --------------------------------
const float dispenseRate = 1.5;  // Dispense rate in grams per second

// --------------------------------
// Firebase Objects
// --------------------------------
FirebaseData   fbdo;          // Firebase data handler
FirebaseAuth   auth;          // Firebase authentication object
FirebaseConfig config;        // Firebase config object

// --------------------------------
// RTC Memory for Feeding Schedule
// --------------------------------
struct FeedingSchedule {
  char dayOfWeek[10];
  int  hour;
  int  minute;
  int  portion_size;
};
RTC_DATA_ATTR FeedingSchedule schedule[50];      // Store feeding schedule in RTC memory
RTC_DATA_ATTR int scheduleCount     = 0;
RTC_DATA_ATTR int lastFeedingIndex = -1;

// --------------------------------
// Pin Definitions
// --------------------------------
const int DIR_PIN      = 12;   // Motor direction pin
const int STEP_PIN     = 14;   // Motor step pin
const int SLP_PIN      = 13;   // Motor sleep pin
const int EN_PIN       = 27;   // Motor enable pin
const int RST_PIN      = 32;   // Motor reset pin
const int TRIGGER_GPIO = 5;    // Ultrasonic sensor trigger
const int ECHO_GPIO    = 18;   // Ultrasonic sensor echo
#define SOUND_SPEED 0.034      // Speed of sound in cm/us

// Token Callback with Restart

void myTokenStatusCallback(TokenInfo info) {
  static int errorCount = 0;
  Serial.printf("Token status: %d\n", info.status);
  if (info.status != token_status_ready) {
    if (++errorCount > 100) ESP.restart();      // Restart if token repeatedly fails
  } else {
    errorCount = 0;
  }
}

// Wi‑Fi & Firebase Reconnect Logic

void ensureFirebaseReady() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Reconnecting Wi‑Fi...");
    WiFi.disconnect(true);
    WiFi.begin(ssid, password);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
      delay(500); Serial.print(".");
    }
    if (WiFi.status() != WL_CONNECTED) ESP.restart();
    delay(2000);
  }
  struct tm tm;
  if (!getLocalTime(&tm)) {
    configTime(gmtOffset, dstOffset, ntpServer);  // Re-sync NTP time
    delay(2000);
  }
  if (config.signer.tokens.status != token_status_ready) {
    Serial.println("Reinit Firebase...");
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    unsigned long t1 = millis();
    while (config.signer.tokens.status != token_status_ready && millis() - t1 < 30000) {
      delay(500); Serial.print(".");
    }
    if (config.signer.tokens.status != token_status_ready) ESP.restart();
  }
}

// Convert 12h → 24h

void convertTime(String s, int &h, int &m) {
  int c = s.indexOf(':');
  h = s.substring(0,c).toInt();
  m = s.substring(c+1,c+3).toInt();
  bool isPM = s.endsWith("PM");
  if (isPM && h<12) h+=12;
  if (!isPM && h==12) h=0;
}


// Fetch Feeding Schedule

void getFeedingSchedule() {
  if (Firebase.RTDB.getJSON(&fbdo, "/feedingSchedules")) {
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, fbdo.to<String>());
    scheduleCount = 0;
    for (JsonVariant item: doc.as<JsonArray>()) {
      if (scheduleCount>=50) break;
      String day = item["dayOfWeek"].as<String>();
      String tm  = item["feedingTime"].as<String>();
      int    p   = item["foodPortion"].as<int>();
      strcpy(schedule[scheduleCount].dayOfWeek, day.c_str());
      convertTime(tm, schedule[scheduleCount].hour, schedule[scheduleCount].minute);
      schedule[scheduleCount].portion_size = p;
      scheduleCount++;
    }
  } else {
    Serial.println("Schedule fetch failed: " + fbdo.errorReason());
  }
  toggleConnectionMonitor();  // Ping connection monitor path to show ESP is active
}

// Print All Feeding Schedules

void printFeedingSchedules() {
  Serial.println("---- Feeding Schedules ----");
  for (int i=0; i<scheduleCount; i++) {
    Serial.printf("%s %02d:%02d → %dg\n",
      schedule[i].dayOfWeek,
      schedule[i].hour,
      schedule[i].minute,
      schedule[i].portion_size
    );
  }
  Serial.println("---------------------------");
}


// Convert Day‐Name → Integer

int dayStringToInt(String day) {
  day.toLowerCase();
  if (day=="sunday")   return 0;
  if (day=="monday")   return 1;
  if (day=="tuesday")  return 2;
  if (day=="wednesday")return 3;
  if (day=="thursday") return 4;
  if (day=="friday")   return 5;
  if (day=="saturday") return 6;
  return -1;
}

// Time‐Based Motor Dispense

void step_motor(int portion) {
  Serial.printf("Dispensing %dg @ %.2fg/s\n", portion, dispenseRate);
  unsigned long runMs = (unsigned long)((portion/dispenseRate)*1000.0);  // Compute run time
  digitalWrite(SLP_PIN, HIGH);
  digitalWrite(EN_PIN, LOW);
  delay(10);
  unsigned long t0 = millis();
  while (millis()-t0 < runMs) {
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(2000);
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(2000);
  }
  Serial.printf("Motor ran %.2fs\n", runMs / 1000.0);  // Log duration
  digitalWrite(EN_PIN, HIGH);
  digitalWrite(SLP_PIN, LOW);
}


// Ultrasonic Level → Firebase

void measure_food_level() {
  float last=0;
  float percentage = 0;
  for (int i=0;i<5;i++) {
    digitalWrite(TRIGGER_GPIO,LOW);
    delayMicroseconds(5);
    digitalWrite(TRIGGER_GPIO,HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIGGER_GPIO,LOW);
    long dur = pulseIn(ECHO_GPIO, HIGH);
    last = dur*SOUND_SPEED/2.0;
    percentage = 100 * (9 - last);
    delay(250);
  }
  Firebase.RTDB.setFloat(&fbdo,"/food_level/level",last);  // Write level to Firebase
  Serial.printf("Level: %.2f%%\n", percentage);
}

// ——— Toggle Connection Monitor ———
void toggleConnectionMonitor() {
  const char* path = "/Connection_error/monitor";
  if (Firebase.RTDB.getInt(&fbdo, path)) {
    int curr = fbdo.intData();
    int next = curr ? 0 : 1;
    Firebase.RTDB.setInt(&fbdo, path, next);  // Toggle current flag
  }
}


// Wake on Next Minute

void setNextWakeOnMinute() {
  struct tm tm; int r=0;
  while (!getLocalTime(&tm)&&r<10) { delay(500); r++; }
  int wait = 60 - tm.tm_sec;
  Serial.printf("Sleeping %ds → %02d:%02d:%02d\n", wait, tm.tm_hour, tm.tm_min, tm.tm_sec);
  delay(100);
  esp_sleep_enable_timer_wakeup(wait*1000000ULL);  // Set sleep timer
  esp_deep_sleep_start();
}


// Scheduled Feed Check

void checkScheduledFeeding() {
  struct tm tm; if(!getLocalTime(&tm)) return;
  int today = tm.tm_wday;
  int nowSec = tm.tm_hour*3600 + tm.tm_min*60 + tm.tm_sec;
  for (int i=0;i<scheduleCount;i++) {
    int d = dayStringToInt(String(schedule[i].dayOfWeek));
    if (d!=today) continue;
    int sched = schedule[i].hour*3600 + schedule[i].minute*60;
    if (abs(nowSec - sched) < 15) {
      step_motor(schedule[i].portion_size);
      measure_food_level();
      break;
    }
  }
}


// Manual Feed via Firebase

void checkManualFeed() {
  if (Firebase.RTDB.getBool(&fbdo,"/ManualFeedings/Manual_feedings/status")
      && fbdo.boolData()) {
    if (Firebase.RTDB.getInt(&fbdo,"/ManualFeedings/Manual_feedings/foodPortion")) {
      int p = fbdo.intData();
      step_motor(p);
      measure_food_level();
      Firebase.RTDB.setBool(&fbdo,"/ManualFeedings/Manual_feedings/status",false);
    }
  }
}


// Setup

void setup() {
  Serial.begin(115200);
  pinMode(DIR_PIN,OUTPUT);
  pinMode(STEP_PIN,OUTPUT);
  pinMode(SLP_PIN,OUTPUT);
  pinMode(EN_PIN,OUTPUT);
  pinMode(RST_PIN,OUTPUT);
  pinMode(TRIGGER_GPIO,OUTPUT);
  pinMode(ECHO_GPIO,INPUT);
  digitalWrite(EN_PIN, HIGH);
  digitalWrite(SLP_PIN, LOW);
  digitalWrite(RST_PIN, HIGH);

  WiFi.begin(ssid,password);
  while (WiFi.status()!=WL_CONNECTED) { delay(300); Serial.print("."); }
  configTime(gmtOffset,dstOffset,ntpServer);  // Sync time

  config.api_key               = API_KEY;
  config.database_url          = DATABASE_URL;
  auth.user.email             = USER_EMAIL;
  auth.user.password          = USER_PASSWORD;
  config.token_status_callback = myTokenStatusCallback;
  Firebase.begin(&config,&auth);
  Firebase.reconnectWiFi(true);
  unsigned long t0=millis();
  while (config.signer.tokens.status!=token_status_ready && millis()-t0<10000) {
    delay(200); Serial.print(".");
  }
  getFeedingSchedule();
  printFeedingSchedules();
}



void loop() {
  ensureFirebaseReady();           // Check Wi-Fi and Firebase readiness
  getFeedingSchedule();           // Update schedule
  checkScheduledFeeding();        // Execute feed if scheduled
  checkManualFeed();              // Check for manual feed command
  setNextWakeOnMinute();          // Enter low-power sleep
}

