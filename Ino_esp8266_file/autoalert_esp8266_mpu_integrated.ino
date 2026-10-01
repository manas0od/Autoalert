/*
  AutoAlert - ESP8266 Firmware
  -----------------------------
  Matches the exact API contract expected by the AutoAlert Android app
  (see Esp8266ApiClient.kt):

    POST /pair    {"phone":"...","password":"..."}   -> {"status":"ok","topic":"...","message":"..."}
    POST /verify  {"password":"..."}                  -> {"status":"ok","message":"..."}
    GET  /battery                                      -> {"battery":NN,"voltage":X.XX,"charging":bool}
    GET  /ping                                          -> {"status":"ok","message":"...","staIp":"..."}
    POST /wifi-config {"slot":"primary"|"safety" (optional, default "primary"),
                        "ssid":"...","password":"...","currentPassword":"..."}
                       -> {"status":"ok","staIp":"..."}
    GET  /stand-info                                    -> {"standName":"..."}
    POST /stand-info  {"standName":"...","currentPassword":"..."} -> {"status":"ok","standName":"..."}
    POST /link-viewer {"password":"...","viewerPhone":"..."} -> {"status":"ok","tiltTopic":"..."}
    POST /revoke-viewers {"password":"..."} -> {"status":"ok","tiltTopic":"..."}

  Every outbound ntfy message (button alerts, WiFi status, low-battery
  warning, all of it) automatically gets " (Battery: NN%)" appended - added
  once inside sendNtfyMessage() so nothing has to remember to include it,
  current and future alerts alike.

  Backup viewers (family members who only see safety alerts):
    A backup viewer's app calls /link-viewer with the MASTER password (typed
    live by the driver, never persisted on the viewer's phone afterward) and
    gets back ONLY tiltTopic - a separate, independently random ntfy topic,
    never a suffix of the main storedTopic (see generateTopic() calls in
    /pair and here). The viewer's phone subscribes to tiltTopic and nothing
    else, so it can never see regular button-press alerts or replies no
    matter what the app UI shows - the restriction is enforced by which
    topic the device physically has, not by an app setting. Revoking is
    all-or-nothing (regenerates tiltTopic, clears the viewer list) because
    ntfy topics have no per-subscriber access control - a real limitation,
    documented rather than hidden.

  Network design (updated):
    - ESP8266 runs Wi-Fi in AP+STA mode simultaneously:
        STA -> connects to a real Wi-Fi network with internet (for ntfy.sh push)
        AP  -> its own PERMANENT, always-on local network "AutoAlert-Setup-XXXX"
               for pairing/battery/ping/wifi-config. This starts immediately on
               every boot, regardless of whether STA WiFi is connected - local
               functions never wait on internet WiFi.
    - WiFi is set/changed through the app's own WiFi setup screen: the app
      connects to the device's AP above (already reliable, used for pairing
      too) and POSTs the SSID/password directly to /wifi-config. No captive
      portal, no WiFiManager - this avoids a well-documented Android quirk
      where the OS aggressively disconnects from a WiFi network it detects
      has no internet access, which made captive-portal-based setup flaky.
    - Physical reset: hold the button at boot to clear saved WiFi, then use
      the app's WiFi setup screen again to reconfigure.
    - DUAL WiFi / PRIMARY+SAFETY FAILOVER: the device stores TWO sets of STA
      credentials itself (in LittleFS, not the ESP8266 SDK's single built-in
      slot), because the vehicle needs to stay reachable both at the auto
      stand and out on the road, which are two different networks:
        PRIMARY -> the fixed network at the auto stand (e.g. the govt.
                   "Free WiFi" / K-FON style hotspot). Short range - normal,
                   expected case.
        SAFETY  -> the driver's own phone hotspot. Always physically nearby
                   no matter where the vehicle goes, so it's the fallback
                   once PRIMARY drops out of range (e.g. mid-route, or after
                   an accident where a future MPU6050 tilt-alert needs to get
                   out no matter where the vehicle is).
      Runtime behavior:
        - While disconnected, the device retries ONLY primary for the first
          PRIMARY_TIMEOUT_MS (1 minute) - a normal, brief drop shouldn't
          bounce the device onto the driver's hotspot. If primary still
          hasn't reconnected after that minute, it gives up on primary for
          this spell and switches to retrying safety instead, so a real
          out-of-range situation (mid-route) fails over within ~1 minute.
        - Once connected via SAFETY, it periodically (every
          PRIMARY_RECHECK_MS, ~35s) makes a short bounded attempt to
          reconnect to PRIMARY in the background; if that succeeds it
          switches back (e.g. after returning to the stand), otherwise it
          reconnects to SAFETY and carries on - never left stranded either
          way. Each recheck briefly interrupts the SAFETY connection (up to
          WIFI_CONNECT_TIMEOUT_MS) since the ESP8266 has one WiFi radio, so
          this is a "frequent" recheck, not a literally continuous one.
        - A short ntfy notification is sent on failover to SAFETY and on
          switching back to PRIMARY, so the driver's app shows connectivity
          status without needing a Serial Monitor.
    - GET /ping includes the device's current STA IP address in its response,
      so the app can learn/display it automatically (e.g. while connected to
      the device's own AP) without anyone needing a Serial Monitor.
    - Pairing data (password hash + ntfy topic + phone label) is stored ONLY on
      the ESP8266's flash (LittleFS) - never sent to any cloud database.
    - /pair (re-register) and /verify share a brute-force lockout: after 5
      failed password attempts, further attempts are rejected for 30 seconds.
    - Button press -> HTTPS POST straight to ntfy.sh over the STA (internet) connection.
    - Two-way reply: after a button press, the device polls "{topic}-reply" every
      ~12s (outbound-only, never accepts inbound connections) for up to 5 minutes,
      looking for "status:coming" or "status:busy" published by the app. On match,
      it lights the green (D6) or red (D7) LED accordingly, and auto-turns it off
      10 seconds later regardless (LED_ON_DURATION_MS).
    - Button press is interrupt-driven (not polled) so it's never missed even
      while a blocking network call (send/poll) is in progress - only the
      processing of the press may be briefly delayed, never lost entirely.

  Libraries needed (install via Arduino Library Manager / Boards Manager):
    - ESP8266WiFi (bundled with ESP8266 board package)
    - ESP8266WebServer (bundled)
    - LittleFS (bundled, use "LittleFS.h")
    - ArduinoJson (by Benoit Blanchon) - install from Library Manager
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266mDNS.h>
#include <MPU9250_WE.h>

// ---------- USER CONFIG ----------
// NOTE: WiFi credentials are NOT hardcoded here. The driver sets their WiFi
// through the AutoAlert app instead - see POST /wifi-config below. The app
// connects to this device's own AP (already reliable, used for pairing too)
// and sends the SSID/password there directly - no captive portal.

const char* AP_SSID_PREFIX = "AutoAlert-Setup-";          // phone connects here to pair
const char* AP_PASSWORD    = "autoalert123";               // min 8 chars for WPA2 AP

const int   BUTTON_PIN     = D5;   // physical service-request button (active LOW, INPUT_PULLUP)
const int   BATTERY_PIN    = A0;   // analog pin reading battery voltage via divider
const int   LED_GREEN_PIN  = D6;   // "Coming" indicator
const int   LED_RED_PIN    = D7;   // "Busy" indicator
const unsigned long DEBOUNCE_MS       = 20000;  // cooldown between button presses (20s - prevents a customer's repeated impatient presses from spawning separate requests while one is already active)
const unsigned long BATTERY_CHECK_MS  = 30UL * 60UL * 1000UL; // check every 30 min
const unsigned long REPLY_POLL_MS     = 12UL * 1000UL;        // poll -reply topic every ~12s
const unsigned long REPLY_WINDOW_MS   = 5UL * 60UL * 1000UL;  // ignore replies after 5 min (matches app)
const unsigned long LED_ON_DURATION_MS = 10UL * 1000UL; // auto turn off LED 10 seconds after it lights up
const int   LOW_BATTERY_THRESHOLD     = 20;      // percent

// ---------- MPU6500 TILT SAFETY ----------
// The Robocraze board is sold as an MPU-6050, but this project measured
// WHO_AM_I = 0x70 at I2C address 0x68, so we use the MPU6500-compatible
// functionality from MPU9250_WE.
const uint8_t MPU_ADDRESS = 0x68;
const float TILT_TRIGGER_ANGLE = 50.0;  // alert at >= 50 degrees from normal
const float TILT_RESET_ANGLE   = 40.0;  // re-arm only after returning below 40 degrees
const unsigned long TILT_SAMPLE_MS = 100;
const int TILT_CALIBRATION_SAMPLES = 50;

MPU6500_WE mpu6500 = MPU6500_WE(MPU_ADDRESS);

// Gravity vector measured while the vehicle/device is in its normal upright
// orientation. Using the full vector instead of assuming Z is "up" makes the
// detector work even if the MPU is mounted on a different axis.
float normalGx = 0.0;
float normalGy = 0.0;
float normalGz = 1.0;
bool mpuReady = false;
bool tiltAlarmLatched = false;
bool tiltCalibrationValid = false;
unsigned long lastTiltSample = 0;


// Dual-WiFi (primary/safety) failover timing - see header comment above.
const unsigned long PRIMARY_TIMEOUT_MS      = 60UL * 1000UL;    // once disconnected, keep trying ONLY primary for up to 1 min before falling back to safety
const unsigned long WIFI_RETRY_MS           = 10UL * 1000UL;    // how often to fire a new connection attempt while disconnected
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 6UL * 1000UL;     // bounded wait per connection attempt
const unsigned long PRIMARY_RECHECK_MS      = 35UL * 1000UL;    // while on SAFETY, re-try PRIMARY in the background every ~35s

// ---------- STATE ----------
ESP8266WebServer server(80);

String storedPasswordHash = "";
String storedTopic        = "";
String storedPhoneLabel   = "";
// Dual WiFi credentials: PRIMARY is the fixed network (e.g. auto stand WiFi),
// tried first. SAFETY is meant to be the driver's own phone hotspot - always
// physically nearby no matter where the vehicle goes, used as a fallback so
// the IMU crash-alert can still reach the internet even far from any fixed
// WiFi. Both are stored ourselves (not relying on the ESP8266 SDK's single
// built-in credential slot), since we need to remember and switch between two.
String primarySsid = "";
String primaryPassword = "";
String safetySsid = "";
String safetyPassword = "";
String standLabel = ""; // free-text name/address of the auto stand, set once via /stand-info - no GPS, but lets an alert say roughly where the vehicle is
String tiltTopic = "";     // independent, randomly generated topic for safety/tilt alerts ONLY - deliberately NOT a suffix of storedTopic (see generateTopic() below), so a backup viewer who only ever learns this string can never derive or guess the main topic
String viewerPhones = ""; // comma-separated list of backup-viewer phone numbers linked to tiltTopic, capped at MAX_VIEWERS - kept for display/audit in the app, not itself a security boundary
const int MAX_VIEWERS = 5;
bool usingSafetyNetwork = false; // true once we're actually CONNECTED via the safety (driver hotspot) network
bool   isPaired            = false;

// Which credential set is being tried / is currently connected, for the
// failover logic in handleWifiConnection() below.
enum WifiSlot { SLOT_NONE, SLOT_PRIMARY, SLOT_SAFETY };
WifiSlot wifiAttemptSlot   = SLOT_NONE; // slot of the most recent WiFi.begin() call
WifiSlot wifiConnectedSlot = SLOT_NONE; // slot we are currently connected via (SLOT_NONE if disconnected)
unsigned long lastWifiRetry = 0;
unsigned long lastPrimaryRecheck = 0;
unsigned long disconnectedSince = 0;    // millis() when the current disconnected spell started
bool primaryGaveUp = false;             // true once the 1-minute primary-only window has elapsed for this spell

unsigned long lastButtonPress = 0;
unsigned long lastBatteryCheck = 0;
unsigned long lastReplyPoll = 0;
long lastHandledReplyTime = 0; // ntfy server timestamp (seconds) of the last reply we've already acted on - prevents an old reply from being re-used for a later, different request
unsigned long ledLitAt = 0;              // millis() timestamp when an LED was last turned on
bool ledActive = false;                  // true while a Coming/Busy LED is currently lit
bool lowBatteryAlertSent = false;
bool awaitingReply = false;              // true from button press until a reply arrives or window expires

// ---------- SIMPLE HASH (adequate for a science-project demo; not cryptographically strong) ----------
String simpleHash(const String &input) {
  uint32_t hash = 5381;
  for (size_t i = 0; i < input.length(); i++) {
    hash = ((hash << 5) + hash) + (uint8_t)input[i]; // djb2
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%08x", hash);
  return String(buf);
}

// ---------- FLASH STORAGE (LittleFS) ----------
void loadConfig() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed, formatting...");
    LittleFS.format();
    LittleFS.begin();
  }

  if (LittleFS.exists("/config.json")) {
    File f = LittleFS.open("/config.json", "r");
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (!err) {
      storedPasswordHash = doc["passwordHash"] | "";
      storedTopic        = doc["topic"] | "";
      storedPhoneLabel   = doc["phoneLabel"] | "";
      primarySsid        = doc["primarySsid"] | "";
      primaryPassword    = doc["primaryPassword"] | "";
      safetySsid         = doc["safetySsid"] | "";
      safetyPassword     = doc["safetyPassword"] | "";
      standLabel         = doc["standLabel"] | "";
      tiltTopic          = doc["tiltTopic"] | "";
      viewerPhones       = doc["viewerPhones"] | "";
      isPaired = storedPasswordHash.length() > 0 && storedTopic.length() > 0;
    }
  }

  Serial.println(isPaired ? "Loaded existing pairing." : "No existing pairing found.");
  Serial.println(primarySsid.length() > 0 ? "Primary WiFi configured: " + primarySsid : "No primary WiFi configured yet.");
  Serial.println(safetySsid.length() > 0 ? "Safety WiFi configured: " + safetySsid : "No safety WiFi configured yet.");

  // Migration: a device paired before this firmware update would have no
  // tiltTopic saved at all. Backfill one now so viewer-linking works without
  // requiring the driver to re-pair from scratch.
  if (isPaired && tiltTopic.length() == 0) {
    tiltTopic = generateTopic();
    saveConfig();
    Serial.println("Backfilled missing tiltTopic for already-paired device: " + tiltTopic);
  }
}

void saveConfig() {
  StaticJsonDocument<1024> doc;
  doc["passwordHash"]     = storedPasswordHash;
  doc["topic"]            = storedTopic;
  doc["phoneLabel"]       = storedPhoneLabel;
  doc["primarySsid"]      = primarySsid;
  doc["primaryPassword"]  = primaryPassword;
  doc["safetySsid"]       = safetySsid;
  doc["safetyPassword"]   = safetyPassword;
  doc["standLabel"]       = standLabel;
  doc["tiltTopic"]        = tiltTopic;
  doc["viewerPhones"]     = viewerPhones;

  File f = LittleFS.open("/config.json", "w");
  serializeJson(doc, f);
  f.close();
}

// ---------- TOPIC GENERATION ----------
String generateTopic() {
  randomSeed(micros());
  String topic = "autoalert-kerala-";
  const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  for (int i = 0; i < 10; i++) {
    topic += charset[random(0, sizeof(charset) - 1)];
  }
  return topic;
}

// ---------- BATTERY READING ----------
// Wiring: ONE external 100k resistor in series from Battery+ to A0.
// Your NodeMCU already has a built-in 220k+100k divider between A0 and the
// ESP8266 chip's raw ADC (this is why A0 reads 0-3.3V instead of the chip's
// native 0-1V). Adding one more 100k in series with that existing network
// scales your battery's 3.0-4.2V range down to a safe level the chip can read.
// Combined ratio -> multiply the ADC reading by ~5.7 to recover the real
// battery voltage (verify against a multimeter reading once wired, and adjust
// the constant below if your specific board's onboard divider differs slightly).
const float BATTERY_VOLTAGE_MULTIPLIER = 5.7;

float readBatteryVoltage() {
  int raw = analogRead(BATTERY_PIN);       // 0-1023
  float voltage = (raw / 1023.0) * BATTERY_VOLTAGE_MULTIPLIER;
  return voltage;
}

int voltageToPercent(float voltage) {
  // Rough Li-ion discharge curve mapping (3.0V = 0%, 4.2V = 100%)
  float pct = (voltage - 3.0) / (4.2 - 3.0) * 100.0;
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  return (int)pct;
}

// ---------- MPU6500 TILT SAFETY ----------
// Returns the angle between the current gravity vector and the calibrated
// normal-position gravity vector. 0 degrees means the same orientation as
// calibration; 90 degrees means the vehicle/device has been turned onto its
// side; 180 degrees means upside down.
float angleFromNormal(float x, float y, float z) {
  float currentMagnitude = sqrt(x * x + y * y + z * z);
  float normalMagnitude = sqrt(normalGx * normalGx + normalGy * normalGy + normalGz * normalGz);

  if (currentMagnitude < 0.50 || normalMagnitude < 0.50) return -1.0;

  float dot = (x * normalGx + y * normalGy + z * normalGz) /
              (currentMagnitude * normalMagnitude);
  dot = constrain(dot, -1.0, 1.0);
  return acos(dot) * 180.0 / PI;
}

void calibrateMpuNormalPosition() {
  Serial.println();
  Serial.println("MPU: keep the vehicle/device in its NORMAL position and still.");
  Serial.println("MPU: calibrating sensor offsets...");
  delay(1500);
  mpu6500.autoOffsets();

  Serial.println("MPU: measuring normal orientation...");
  float sx = 0.0;
  float sy = 0.0;
  float sz = 0.0;
  int validSamples = 0;

  for (int i = 0; i < TILT_CALIBRATION_SAMPLES; i++) {
    xyzFloat acc = mpu6500.getGValues();
    float magnitude = sqrt(acc.x * acc.x + acc.y * acc.y + acc.z * acc.z);

    // A stationary sensor should see roughly 1 G. Ignore an obviously bad
    // sample instead of allowing it to corrupt the normal orientation.
    if (magnitude > 0.70 && magnitude < 1.30) {
      sx += acc.x;
      sy += acc.y;
      sz += acc.z;
      validSamples++;
    }
    delay(20);
    yield();
  }

  if (validSamples < 10) {
    Serial.println("MPU: NORMAL ORIENTATION CALIBRATION FAILED.");
    Serial.println("MPU: tilt safety is disabled until a valid calibration is obtained.");
    tiltCalibrationValid = false;
    return;
  }

  normalGx = sx / validSamples;
  normalGy = sy / validSamples;
  normalGz = sz / validSamples;
  tiltCalibrationValid = true;
  tiltAlarmLatched = false;

  Serial.printf("MPU: normal vector X=%.3f G Y=%.3f G Z=%.3f G\n",
                normalGx, normalGy, normalGz);
  Serial.printf("MPU: safety alert threshold = %.1f degrees\n", TILT_TRIGGER_ANGLE);
  Serial.printf("MPU: reset/re-arm threshold = %.1f degrees\n", TILT_RESET_ANGLE);
}

void setupMpu() {
  Wire.begin(D2, D1);  // ESP8266: D2=SDA, D1=SCL
  delay(100);

  Serial.println();
  Serial.println("Initializing MPU6500 safety sensor...");

  if (!mpu6500.init()) {
    Serial.println("MPU6500 initialization FAILED.");
    Serial.println("MPU tilt safety is DISABLED; AutoAlert continues normally.");
    mpuReady = false;
    return;
  }

  mpu6500.setAccRange(MPU6500_ACC_RANGE_2G);
  mpu6500.setGyrRange(MPU6500_GYRO_RANGE_250);
  mpuReady = true;

  Serial.println("MPU6500 initialization SUCCESS.");
  calibrateMpuNormalPosition();
}

void checkMpuTilt() {
  if (!mpuReady || !tiltCalibrationValid) return;

  unsigned long now = millis();
  if (now - lastTiltSample < TILT_SAMPLE_MS) return;
  lastTiltSample = now;

  xyzFloat acc = mpu6500.getGValues();
  float angle = angleFromNormal(acc.x, acc.y, acc.z);
  if (angle < 0.0) return; // invalid acceleration magnitude

  // Trigger exactly at or above the configured angle. Once triggered, stay
  // latched so vibration/noise cannot generate repeated notifications.
  if (!tiltAlarmLatched && angle >= TILT_TRIGGER_ANGLE) {
    tiltAlarmLatched = true;

    Serial.printf("MPU SAFETY: tilt %.1f degrees >= %.1f degrees\n",
                  angle, TILT_TRIGGER_ANGLE);

    if (isPaired && tiltTopic.length() > 0 && WiFi.status() == WL_CONNECTED) {
      String message = "Vehicle tilt detected: " + String(angle, 1) +
                       " degrees from the calibrated normal position. " +
                       "Safety alert triggered. Location: " + locationContext() + ".";
      bool sent = sendNtfyMessage(tiltTopic,
                                  "SAFETY ALERT: Vehicle Tilt / Rollover",
                                  message);
      Serial.println(sent ? "MPU safety notification sent." : "MPU safety notification FAILED.");
    } else {
      Serial.println("MPU safety alert detected, but no internet/tilt topic is available.");
    }
  }

  // Re-arm only after the device has returned well inside the safe range.
  if (tiltAlarmLatched && angle < TILT_RESET_ANGLE) {
    tiltAlarmLatched = false;
    Serial.printf("MPU: returned to %.1f degrees - tilt alarm re-armed.\n", angle);
  }
}

// ---------- DUAL WIFI (PRIMARY/SAFETY) CONNECT + FAILOVER ----------
// See the header comment block for the overall design. These functions are
// the only places that call WiFi.begin() during normal operation (i.e. not
// counting the one-off verification inside handleWifiConfig()), so
// wifiAttemptSlot always reflects which credential set was tried last.

// Called while STA is disconnected, every WIFI_RETRY_MS. For the first
// PRIMARY_TIMEOUT_MS (1 min) of a disconnected spell, retries ONLY primary
// (the auto stand's normal, expected network - no need to bail to the
// driver's hotspot for a momentary drop). If primary still hasn't connected
// after that window, gives up on it for this spell and switches to
// retrying safety, so a real out-of-range situation (mid-route) fails over
// within roughly a minute rather than sitting disconnected indefinitely.
void attemptWifiReconnect() {
  if (primarySsid.length() == 0 && safetySsid.length() == 0) return; // nothing configured yet

  bool primaryWindowOpen = (primarySsid.length() > 0) &&
                           !primaryGaveUp &&
                           (millis() - disconnectedSince < PRIMARY_TIMEOUT_MS);

  if (primaryWindowOpen || safetySsid.length() == 0) {
    // Either still inside the 1-minute primary-only grace period, or there's
    // no safety network configured at all so primary is the only option.
    if (primarySsid.length() > 0) {
      Serial.println("WiFi: attempting PRIMARY (" + primarySsid + ")");
      WiFi.begin(primarySsid.c_str(), primaryPassword.c_str());
      wifiAttemptSlot = SLOT_PRIMARY;
    }
  } else {
    if (!primaryGaveUp) {
      primaryGaveUp = true;
      Serial.println("WiFi: PRIMARY not reachable within 1 minute - falling back to SAFETY.");
    }
    Serial.println("WiFi: attempting SAFETY (" + safetySsid + ")");
    WiFi.begin(safetySsid.c_str(), safetyPassword.c_str());
    wifiAttemptSlot = SLOT_SAFETY;
  }
}

// Called periodically while connected via SAFETY. Makes one short, bounded
// attempt to reconnect to PRIMARY in the background; switches back if it
// succeeds (e.g. the auto is back at the stand), otherwise reconnects to
// SAFETY so the device is never left stranded mid-check.
void checkPrimaryRecovery() {
  if (primarySsid.length() == 0) return; // no primary configured, nothing to recover to
  unsigned long now = millis();
  if (now - lastPrimaryRecheck < PRIMARY_RECHECK_MS) return;
  lastPrimaryRecheck = now;

  Serial.println("WiFi: on SAFETY network - briefly checking if PRIMARY is back in range...");
  WiFi.begin(primarySsid.c_str(), primaryPassword.c_str());
  wifiAttemptSlot = SLOT_PRIMARY;

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(200);
    server.handleClient(); // keep AP/pairing responsive during the check
  }

  if (WiFi.status() != WL_CONNECTED && safetySsid.length() > 0) {
    // Primary still unreachable - reconnect to safety so we don't sit idle.
    Serial.println("WiFi: PRIMARY still out of range - reconnecting to SAFETY.");
    WiFi.begin(safetySsid.c_str(), safetyPassword.c_str());
    wifiAttemptSlot = SLOT_SAFETY;
    unsigned long startAttempt2 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt2 < WIFI_CONNECT_TIMEOUT_MS) {
      delay(200);
      server.handleClient();
    }
  }
  // If it succeeded, handleWifiConnection() below notices the slot change
  // (wifiConnectedSlot != wifiAttemptSlot) on its very next call and reports it.
}

// Called every loop() iteration. Tracks connect/disconnect/slot-change
// transitions and drives the retry/recovery functions above.
void handleWifiConnection() {
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (!connected) {
    if (wifiConnectedSlot != SLOT_NONE) {
      Serial.println("WiFi: disconnected.");
      wifiConnectedSlot = SLOT_NONE;
      disconnectedSince = millis(); // fresh disconnect - restart the 1-minute primary-only window
      primaryGaveUp = false;
    }
    if (disconnectedSince == 0) disconnectedSince = millis(); // boot-time case, never connected yet
    unsigned long now = millis();
    if (now - lastWifiRetry > WIFI_RETRY_MS) {
      lastWifiRetry = now;
      attemptWifiReconnect();
    }
    return;
  }

  // Connected - react if the slot we're connected via has changed.
  if (wifiConnectedSlot != wifiAttemptSlot) {
    wifiConnectedSlot = wifiAttemptSlot;
    usingSafetyNetwork = (wifiConnectedSlot == SLOT_SAFETY);
    Serial.println(usingSafetyNetwork
      ? "WiFi: connected via SAFETY (driver hotspot)."
      : "WiFi: connected via PRIMARY (auto stand).");
    if (isPaired) {
      sendNtfyMessage(storedTopic, "AutoAlert Status",
        usingSafetyNetwork
          ? "Switched to backup WiFi (driver's phone) - out of auto stand WiFi range."
          : "Back on auto stand WiFi.");
    }
  }

  if (usingSafetyNetwork) {
    checkPrimaryRecovery();
  }
}

// ---------- NTFY PUSH ----------
bool sendNtfyMessage(const String &topic, const String &title, const String &message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("STA not connected - cannot reach ntfy.sh");
    return false;
  }

  // Battery rides along as a Tag, NOT appended to the visible message text -
  // appending it to the body previously broke the app's Coming/Busy action
  // buttons (their rendering logic apparently keys off the exact body text).
  // Tags are a separate ntfy field: invisible in a plain notification, but
  // present in the JSON payload for the app to read and cache.
  int batteryPercent = voltageToPercent(readBatteryVoltage());
  String batteryTag = "batt" + String(batteryPercent);

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure(); // skip cert validation - acceptable for a prototype/demo

  HTTPClient https; // requires ESP8266HTTPClient.h (now included above)
  String url = "https://ntfy.sh/" + topic;

  if (https.begin(*client, url)) {
    https.setTimeout(6000); // bound the blocking window - don't let a hung connection stall the device indefinitely
    https.addHeader("Title", title);
    https.addHeader("Tags", batteryTag);
    int httpCode = https.POST(message);
    https.end();
    Serial.printf("ntfy POST result: %d\n", httpCode);
    return httpCode >= 200 && httpCode < 300;
  }
  Serial.println("Unable to connect to ntfy.sh");
  return false;
}

// ---------- REPLY POLLING (App -> Device, via -reply topic) ----------
// The device only ever makes OUTBOUND requests (poll = pull), never accepts
// inbound internet connections, to keep the "no open attack surface" property
// from our security design intact.
void setLed(bool green, bool red) {
  digitalWrite(LED_GREEN_PIN, green ? HIGH : LOW);
  digitalWrite(LED_RED_PIN, red ? HIGH : LOW);
  if (green || red) {
    ledActive = true;
    ledLitAt = millis();
  } else {
    ledActive = false;
  }
}

void checkLedTimeout() {
  if (ledActive && (millis() - ledLitAt >= LED_ON_DURATION_MS)) {
    setLed(false, false); // auto turn-off, 10 seconds after it lit up
    Serial.println("LED auto-off after 10 seconds.");
  }
}

void pollReplyTopic() {
  if (!isPaired || WiFi.status() != WL_CONNECTED) return;
  if (!awaitingReply) return; // nothing to check for unless a request is currently open

  unsigned long now = millis();
  if (now - lastReplyPoll < REPLY_POLL_MS) return;
  lastReplyPoll = now;

  // Expire the pending request after the same 5-minute window the app uses
  if (now - lastButtonPress > REPLY_WINDOW_MS) {
    awaitingReply = false;
    setLed(false, false); // back to idle - no reply came in time
    return;
  }

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient https;
  String replyTopic = storedTopic + "-reply";
  // Use a short relative duration (ntfy supports "Ns"/"Nm" syntax) instead of a
  // raw timestamp - the ESP8266 has no synced wall-clock time, so passing
  // millis()/1000 as if it were a unix timestamp was wrong (it made ntfy think
  // "since 1970", returning the entire message history every poll). "20s" asks
  // only for messages from the last 20 seconds, which comfortably covers our
  // ~12s poll interval with a small overlap margin.
  String url = "https://ntfy.sh/" + replyTopic + "/json?poll=1&since=20s";

  if (!https.begin(*client, url)) {
    Serial.println("Unable to connect to ntfy.sh for reply poll");
    return;
  }
  https.setTimeout(6000); // bound the blocking window, same reasoning as sendNtfyMessage()

  int httpCode = https.GET();
  if (httpCode == 200) {
    String payload = https.getString();
    // Response is newline-delimited JSON (one object per line). Parse each
    // line individually so we can check each message's own "time" field -
    // this is what lets us tell a genuinely NEW reply apart from an OLD one
    // that's still inside the 20-second lookback window (see comment on
    // lastHandledReplyTime above for why this matters).
    int lineStart = 0;
    while (lineStart < payload.length()) {
      int lineEnd = payload.indexOf('\n', lineStart);
      if (lineEnd == -1) lineEnd = payload.length();
      String line = payload.substring(lineStart, lineEnd);
      lineStart = lineEnd + 1;

      if (line.length() < 2) continue; // skip blank lines

      StaticJsonDocument<512> msgDoc;
      DeserializationError err = deserializeJson(msgDoc, line);
      if (err) continue; // skip anything that isn't valid JSON (e.g. keepalive pings)

      long msgTime = msgDoc["time"] | 0L;
      String msgBody = msgDoc["message"] | "";

      // Only ever react to a reply that's newer than the last one we already
      // handled - guarantees the exact same physical reply can never be
      // reused for a different, later request.
      if (msgTime <= lastHandledReplyTime) continue;

      if (msgBody.indexOf("status:coming") != -1) {
        setLed(true, false);
        awaitingReply = false;
        lastHandledReplyTime = msgTime;
        Serial.println("Reply received: coming");
        break;
      } else if (msgBody.indexOf("status:busy") != -1) {
        setLed(false, true);
        awaitingReply = false;
        lastHandledReplyTime = msgTime;
        Serial.println("Reply received: busy");
        break;
      }
    }
  }
  https.end();
}

void routePair() {
  if (server.method() != HTTP_POST) { server.send(405, "application/json", "{\"status\":\"error\",\"message\":\"POST required\"}"); return; }
  handlePair();
}

void routeVerify() {
  if (server.method() != HTTP_POST) { server.send(405, "application/json", "{\"status\":\"error\",\"message\":\"POST required\"}"); return; }
  handleVerify();
}

void routeBattery() {
  if (server.method() != HTTP_GET) { server.send(405, "application/json", "{\"status\":\"error\",\"message\":\"GET required\"}"); return; }
  handleBattery();
}

void routePing() {
  if (server.method() != HTTP_GET) { server.send(405, "application/json", "{\"status\":\"error\",\"message\":\"GET required\"}"); return; }
  handlePing();
}

void routeWifiConfig() {
  if (server.method() != HTTP_POST) { server.send(405, "application/json", "{\"status\":\"error\",\"message\":\"POST required\"}"); return; }
  handleWifiConfig();
}

void routeStandInfo() {
  if (server.method() != HTTP_GET && server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"status\":\"error\",\"message\":\"GET or POST required\"}");
    return;
  }
  handleStandInfo();
}

void routeLinkViewer() {
  if (server.method() != HTTP_POST) { server.send(405, "application/json", "{\"status\":\"error\",\"message\":\"POST required\"}"); return; }
  handleLinkViewer();
}

void routeRevokeViewers() {
  if (server.method() != HTTP_POST) { server.send(405, "application/json", "{\"status\":\"error\",\"message\":\"POST required\"}"); return; }
  handleRevokeViewers();
}

// ---------- HTTP HANDLERS ----------

// ---------- BRUTE-FORCE PROTECTION for /verify and /pair's re-register path ----------
// Shared across both endpoints since both check a password. After 5 failed
// attempts, lock out further attempts for 30 seconds. This doesn't rely on
// the AP password being secret - it protects the actual pairing password
// even if someone is already on the local AP network.
int failedPasswordAttempts = 0;
unsigned long passwordLockoutUntil = 0;
const int    MAX_PASSWORD_ATTEMPTS = 5;
const unsigned long PASSWORD_LOCKOUT_MS = 30000;

bool isPasswordLocked() {
  return millis() < passwordLockoutUntil;
}

void recordPasswordFailure() {
  failedPasswordAttempts++;
  if (failedPasswordAttempts >= MAX_PASSWORD_ATTEMPTS) {
    passwordLockoutUntil = millis() + PASSWORD_LOCKOUT_MS;
    failedPasswordAttempts = 0;
    Serial.println("Too many failed password attempts - locked out for 30s.");
  }
}

void recordPasswordSuccess() {
  failedPasswordAttempts = 0;
}

// POST /pair  {"phone":"...","password":"..."}
void handlePair() {
  if (isPasswordLocked()) {
    server.send(429, "application/json", "{\"status\":\"error\",\"message\":\"Too many attempts, try again shortly\"}");
    return;
  }

  StaticJsonDocument<256> reqDoc;
  DeserializationError err = deserializeJson(reqDoc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Bad JSON\"}");
    return;
  }

  String phone = reqDoc["phone"] | "";
  String password = reqDoc["password"] | "";

  if (phone.length() == 0 || password.length() == 0) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Phone and password required\"}");
    return;
  }

  // If already paired, this call is being used as "re-register" after a successful
  // /verify from the app - accept the new phone label but keep requiring the SAME
  // password that's already stored (do not silently change the password here).
  if (isPaired) {
    if (simpleHash(password) != storedPasswordHash) {
      recordPasswordFailure();
      server.send(401, "application/json", "{\"status\":\"error\",\"message\":\"Incorrect password\"}");
      return;
    }
    recordPasswordSuccess();
    storedPhoneLabel = phone;
    saveConfig();

    StaticJsonDocument<320> res;
    res["status"] = "ok";
    res["topic"] = storedTopic;
    res["tiltTopic"] = tiltTopic;
    res["message"] = "Phone number updated";
    String out;
    serializeJson(res, out);
    server.send(200, "application/json", out);
    return;
  }

  // First-time pairing
  storedPasswordHash = simpleHash(password);
  storedPhoneLabel = phone;
  storedTopic = generateTopic();
  // Deliberately a SEPARATE, independently random topic - not storedTopic
  // plus a suffix - so a backup viewer who only ever learns tiltTopic can
  // never derive or guess the main topic (see tiltTopic's declaration above).
  tiltTopic = generateTopic();
  isPaired = true;
  saveConfig();

  StaticJsonDocument<320> res;
  res["status"] = "ok";
  res["topic"] = storedTopic;
  res["tiltTopic"] = tiltTopic;
  res["message"] = "Paired successfully";
  String out;
  serializeJson(res, out);
  server.send(200, "application/json", out);

  Serial.println("Device paired. Topic: " + storedTopic + " | Tilt topic: " + tiltTopic);
}

// POST /verify {"password":"..."}
void handleVerify() {
  if (!isPaired) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Device not yet paired\"}");
    return;
  }

  if (isPasswordLocked()) {
    server.send(429, "application/json", "{\"status\":\"error\",\"message\":\"Too many attempts, try again shortly\"}");
    return;
  }

  StaticJsonDocument<128> reqDoc;
  DeserializationError err = deserializeJson(reqDoc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Bad JSON\"}");
    return;
  }

  String password = reqDoc["password"] | "";
  if (simpleHash(password) == storedPasswordHash) {
    recordPasswordSuccess();
    server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Password verified\"}");
  } else {
    recordPasswordFailure();
    server.send(401, "application/json", "{\"status\":\"error\",\"message\":\"Incorrect password\"}");
  }
}

// GET /battery
void handleBattery() {
  float voltage = readBatteryVoltage();
  int percent = voltageToPercent(voltage);
  bool charging = false; // wire a charge-detect pin if your charger module exposes one

  StaticJsonDocument<128> res;
  res["battery"] = percent;
  res["voltage"] = voltage;
  res["charging"] = charging;
  String out;
  serializeJson(res, out);
  server.send(200, "application/json", out);
}

// POST /wifi-config  {"slot":"primary"|"safety" (optional, default "primary"),
//                      "ssid":"...","password":"...","currentPassword":"..."}
// Called by the app while connected to this device's own AP. Saves credentials
// into EITHER the PRIMARY slot (the fixed auto-stand WiFi) or the SAFETY slot
// (the driver's own phone hotspot, used as a fallback once PRIMARY is out of
// range) - this is how the driver sets up/changes each network, without any
// captive portal. Requires the existing pairing password once the device is
// already paired, same security posture as /pair's re-register path -
// prevents a stranger on the local AP from silently redirecting the device.
void handleWifiConfig() {
  if (isPasswordLocked()) {
    server.send(429, "application/json", "{\"status\":\"error\",\"message\":\"Too many attempts, try again shortly\"}");
    return;
  }

  StaticJsonDocument<320> reqDoc;
  DeserializationError err = deserializeJson(reqDoc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Bad JSON\"}");
    return;
  }

  String slot = reqDoc["slot"] | "primary"; // default "primary" so older app builds (no slot field) keep working unchanged
  String newSsid = reqDoc["ssid"] | "";
  String newPassword = reqDoc["password"] | "";
  String currentPassword = reqDoc["currentPassword"] | "";

  if (newSsid.length() == 0) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"WiFi name required\"}");
    return;
  }
  if (slot != "primary" && slot != "safety") {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"slot must be 'primary' or 'safety'\"}");
    return;
  }

  // If already paired, require the existing pairing password before allowing
  // a WiFi change - stops a stranger on the local AP from hijacking which
  // network the device sends data over.
  if (isPaired) {
    if (simpleHash(currentPassword) != storedPasswordHash) {
      recordPasswordFailure();
      server.send(401, "application/json", "{\"status\":\"error\",\"message\":\"Incorrect password\"}");
      return;
    }
    recordPasswordSuccess();
  }

  bool isSafetySlot = (slot == "safety");
  Serial.println("Received new " + slot + " WiFi credentials via app: " + newSsid);

  // Save into the right slot ourselves (LittleFS), rather than relying on the
  // ESP8266 SDK's single built-in credential slot - see header comment.
  if (isSafetySlot) {
    safetySsid = newSsid;
    safetyPassword = newPassword;
  } else {
    primarySsid = newSsid;
    primaryPassword = newPassword;
  }
  saveConfig();

  // Verify the slot just saved by actually connecting with it now, so the
  // app gets an immediate, real answer instead of guessing. NOTE: if the
  // device is currently connected via the OTHER slot, this briefly switches
  // the STA radio to test the new one - the normal failover logic in
  // handleWifiConnection()/checkPrimaryRecovery() will re-prefer PRIMARY
  // again within PRIMARY_RECHECK_MS, so this is a short, self-correcting
  // interruption, not a permanent downgrade.
  WiFi.begin(newSsid.c_str(), newPassword.c_str());
  wifiAttemptSlot = isSafetySlot ? SLOT_SAFETY : SLOT_PRIMARY;

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(300);
    server.handleClient(); // keep the AP/pairing side responsive during the wait
  }

  StaticJsonDocument<192> res;
  bool connected = (WiFi.status() == WL_CONNECTED);
  res["status"] = connected ? "ok" : "error";
  res["message"] = connected
    ? ("Connected successfully (" + slot + ")")
    : "Could not connect - check WiFi name and password";
  res["staIp"] = connected ? WiFi.localIP().toString() : "";
  String out;
  serializeJson(res, out);
  server.send(connected ? 200 : 400, "application/json", out);
}

// POST /link-viewer  {"password":"...","viewerPhone":"..."}
// Registers a backup viewer (e.g. a family member's phone) and hands back
// tiltTopic ONLY - never storedTopic. This is the actual security boundary
// for the backup-account feature: a viewer's app subscribes to tiltTopic
// (safety/tilt alerts only) and never learns or subscribes to the main
// topic, so it can never see regular button-press notifications or replies
// no matter what the app's own UI chooses to show. The master password is
// used once, here, to authorize the link - the caller (the viewer's phone)
// is expected to send it live and NEVER persist it locally afterward; only
// tiltTopic should be saved on that device. Requires the device to already
// be paired (nothing to link a viewer into otherwise).
void handleLinkViewer() {
  if (!isPaired) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Device not yet paired\"}");
    return;
  }
  if (isPasswordLocked()) {
    server.send(429, "application/json", "{\"status\":\"error\",\"message\":\"Too many attempts, try again shortly\"}");
    return;
  }

  StaticJsonDocument<256> reqDoc;
  DeserializationError err = deserializeJson(reqDoc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Bad JSON\"}");
    return;
  }

  String password = reqDoc["password"] | "";
  String viewerPhone = reqDoc["viewerPhone"] | "";

  if (viewerPhone.length() == 0) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Viewer phone number required\"}");
    return;
  }
  if (simpleHash(password) != storedPasswordHash) {
    recordPasswordFailure();
    server.send(401, "application/json", "{\"status\":\"error\",\"message\":\"Incorrect password\"}");
    return;
  }
  recordPasswordSuccess();

  // Dedupe + cap the list. Stored as plain comma-separated text since it's
  // only ever used for display/audit in the app, not as a security check.
  String marker = "," + viewerPhone + ",";
  String haystack = "," + viewerPhones + ",";
  bool alreadyLinked = haystack.indexOf(marker) >= 0;

  if (!alreadyLinked) {
    int currentCount = (viewerPhones.length() == 0) ? 0 : 1;
    for (unsigned int i = 0; i < viewerPhones.length(); i++) {
      if (viewerPhones[i] == ',') currentCount++;
    }
    if (currentCount >= MAX_VIEWERS) {
      server.send(400, "application/json",
        "{\"status\":\"error\",\"message\":\"Maximum " + String(MAX_VIEWERS) + " backup viewers reached - revoke access to add a new one\"}");
      return;
    }
    viewerPhones = (viewerPhones.length() == 0) ? viewerPhone : (viewerPhones + "," + viewerPhone);
    saveConfig();
  }

  StaticJsonDocument<256> res;
  res["status"] = "ok";
  res["tiltTopic"] = tiltTopic;
  res["message"] = "Linked as backup viewer (safety alerts only)";
  String out;
  serializeJson(res, out);
  server.send(200, "application/json", out);
  Serial.println("Backup viewer linked: " + viewerPhone);
}

// POST /revoke-viewers  {"password":"..."}
// Blunt, all-or-nothing revoke - this is a real limitation, not an oversight:
// ntfy topics have no per-subscriber access control, so there is no way to
// cut off ONE viewer while leaving others connected. Revoking regenerates a
// brand new tiltTopic and clears the whole viewer list; anyone who should
// keep access (including viewers you want to keep) must be re-linked via
// /link-viewer afterward with the new topic.
void handleRevokeViewers() {
  if (!isPaired) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Device not yet paired\"}");
    return;
  }
  if (isPasswordLocked()) {
    server.send(429, "application/json", "{\"status\":\"error\",\"message\":\"Too many attempts, try again shortly\"}");
    return;
  }

  StaticJsonDocument<192> reqDoc;
  DeserializationError err = deserializeJson(reqDoc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Bad JSON\"}");
    return;
  }

  String password = reqDoc["password"] | "";
  if (simpleHash(password) != storedPasswordHash) {
    recordPasswordFailure();
    server.send(401, "application/json", "{\"status\":\"error\",\"message\":\"Incorrect password\"}");
    return;
  }
  recordPasswordSuccess();

  tiltTopic = generateTopic();
  viewerPhones = "";
  saveConfig();

  StaticJsonDocument<256> res;
  res["status"] = "ok";
  res["tiltTopic"] = tiltTopic;
  res["message"] = "All viewer access revoked - re-link anyone who should keep access";
  String out;
  serializeJson(res, out);
  server.send(200, "application/json", out);
  Serial.println("Viewer access revoked. New tilt topic: " + tiltTopic);
}
// A free-text name/address for the auto stand (e.g. "Vytilla Junction Auto
// Stand"), set once by whoever configures the device. This project has no
// GPS and isn't adding a paid/free geolocation lookup service (out of scope
// for now) - so this is the honest, zero-cost substitute for location: since
// we already know which WiFi slot is active (see PRIMARY/SAFETY design
// above), we know whether the vehicle is AT the stand or AWAY from it. See
// locationContext() below for how this gets turned into alert text.
// POST requires the pairing password once paired, same posture as /wifi-config.
void handleStandInfo() {
  if (server.method() == HTTP_GET) {
    StaticJsonDocument<192> res;
    res["standName"] = standLabel;
    String out;
    serializeJson(res, out);
    server.send(200, "application/json", out);
    return;
  }

  // POST
  if (isPasswordLocked()) {
    server.send(429, "application/json", "{\"status\":\"error\",\"message\":\"Too many attempts, try again shortly\"}");
    return;
  }

  StaticJsonDocument<256> reqDoc;
  DeserializationError err = deserializeJson(reqDoc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Bad JSON\"}");
    return;
  }

  String newStandName = reqDoc["standName"] | "";
  String currentPassword = reqDoc["currentPassword"] | "";

  if (isPaired) {
    if (simpleHash(currentPassword) != storedPasswordHash) {
      recordPasswordFailure();
      server.send(401, "application/json", "{\"status\":\"error\",\"message\":\"Incorrect password\"}");
      return;
    }
    recordPasswordSuccess();
  }

  standLabel = newStandName;
  saveConfig();

  StaticJsonDocument<192> res;
  res["status"] = "ok";
  res["standName"] = standLabel;
  String out;
  serializeJson(res, out);
  server.send(200, "application/json", out);
}

// Turns "which WiFi slot is active right now" into a plain-English location
// hint for alert messages - the closest thing to a location this project has
// without GPS or a paid/free geolocation API (deliberately out of scope).
String locationContext() {
  String name = (standLabel.length() > 0) ? standLabel : "the auto stand";
  if (usingSafetyNetwork) {
    return "away from " + name + " - exact location unknown";
  }
  return "at " + name;
}
void handlePing() {
  StaticJsonDocument<192> res;
  res["status"] = "ok";
  res["message"] = "AutoAlert device online";
  // Current internet-WiFi IP, so the app can learn it automatically without
  // the person needing to read it off a Serial Monitor. Empty string if STA
  // isn't connected right now.
  res["staIp"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
  String out;
  serializeJson(res, out);
  server.send(200, "application/json", out);
}

void handleNotFound() {
  server.send(404, "application/json", "{\"status\":\"error\",\"message\":\"Unknown endpoint\"}");
}

// ---------- BUTTON + LOW BATTERY LOGIC ----------
// Interrupt-driven instead of polling digitalRead() every loop() cycle.
// Why this matters: sendNtfyMessage() and pollReplyTopic() make BLOCKING
// network calls - while the chip is waiting on ntfy.sh's response, it can't
// run any other code, including checking the button pin. With plain polling,
// a press occurring during that blocking window was completely lost, not
// just delayed. A hardware interrupt fires the instant the pin changes,
// regardless of what else the chip is doing at that moment - the ISR just
// sets a flag; checkButton() picks it up as soon as the current blocking
// call finishes. This guarantees every physical press is captured, even if
// its processing is briefly delayed.
volatile bool buttonInterruptFlag = false;

void IRAM_ATTR buttonISR() {
  buttonInterruptFlag = true;
}

void checkButton() {
  if (!buttonInterruptFlag) return;
  buttonInterruptFlag = false;

  unsigned long now = millis();
  if (now - lastButtonPress > DEBOUNCE_MS) {
    lastButtonPress = now;

    if (isPaired) {
      bool ok = sendNtfyMessage(storedTopic, "Service Required", "Passenger requesting service at auto stand.");
      Serial.println(ok ? "Service request sent." : "Failed to send service request.");
      if (ok) {
        setLed(false, false);       // clear any previous Coming/Busy state
        awaitingReply = true;        // start listening for a reply on the -reply topic
      }
    } else {
      Serial.println("Button pressed but device not paired yet - ignoring.");
    }
  } else {
    Serial.println("Button pressed during cooldown - request already active, ignoring.");
  }
}

void checkLowBattery() {
  unsigned long now = millis();
  if (now - lastBatteryCheck > BATTERY_CHECK_MS) {
    lastBatteryCheck = now;

    int percent = voltageToPercent(readBatteryVoltage());
    if (percent < LOW_BATTERY_THRESHOLD) {
      if (!lowBatteryAlertSent && isPaired) {
        sendNtfyMessage(storedTopic, "Low Battery Warning", "AutoAlert device battery is below 20%. Please charge soon.");
        lowBatteryAlertSent = true; // one-time alert until it goes back above threshold
      }
    } else {
      lowBatteryAlertSent = false; // reset once charged back up
    }
  }
}

// ---------- SETUP / LOOP ----------
void setup() {
  Serial.begin(115200);
  delay(200); // give the USB-serial link a moment to settle before we print anything
  // Diagnostic: print WHY the chip just started, so a brownout/exception reset
  // during WiFi association (a known risk when powering only from a phone's
  // USB-OTG port, which often can't sustain the ~200-300mA WiFi join spike)
  // shows up clearly on the very next boot instead of looking like silence.
  Serial.println("Reset reason: " + ESP.getResetReason() + " | " + ESP.getResetInfo());

  // Initialize and calibrate the MPU before starting the network services.
  // The vehicle must be stationary in its normal position during boot.
  setupMpu();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_RED_PIN, OUTPUT);
  setLed(false, false);

  loadConfig();

  String apName = String(AP_SSID_PREFIX) + String(ESP.getChipId(), HEX);

  // Physical reset: hold the button down while powering on to force-forget
  // the saved WiFi. No captive portal involved - the driver reconfigures
  // WiFi afterward through the app's WiFi setup screen (POST /wifi-config),
  // while connected to this device's own AP below, same as normal pairing.
  bool forceResetWifi = (digitalRead(BUTTON_PIN) == LOW);
  if (forceResetWifi) {
    Serial.println("Button held at boot - clearing saved WiFi (both primary and safety).");
    primarySsid = ""; primaryPassword = "";
    safetySsid = "";  safetyPassword = "";
    saveConfig();
    WiFi.disconnect(true);
  }

  // --- Everything below ALWAYS runs, on every boot, regardless of whether
  // WiFi is connected. The device's own local network, HTTP server, and
  // mDNS never wait on internet WiFi at all. ---

  WiFi.mode(WIFI_AP_STA);
  // We manage our own PRIMARY/SAFETY credentials in LittleFS (loaded above by
  // loadConfig()), rather than the ESP8266 SDK's single built-in credential
  // slot, so disable the SDK's own flash-persisted WiFi (also avoids extra
  // flash wear from the frequent WiFi.begin() calls the failover logic makes).
  WiFi.persistent(false);
  WiFi.softAP(apName.c_str(), AP_PASSWORD);
  Serial.println("AP started: " + apName + "  IP: " + WiFi.softAPIP().toString());

  // Attempt STA using whichever saved credentials we have (PRIMARY first,
  // falling back to SAFETY if only that's configured) - fired once here,
  // non-blocking; loop() below (handleWifiConnection) polls the connection
  // status and keeps retrying/failing-over in the background forever,
  // without ever pausing local functionality.
  attemptWifiReconnect();

  // Routes
  server.on("/pair", routePair);
  server.on("/verify", routeVerify);
  server.on("/battery", routeBattery);
  server.on("/ping", routePing);
  server.on("/wifi-config", routeWifiConfig);
  server.on("/stand-info", routeStandInfo);
  server.on("/link-viewer", routeLinkViewer);
  server.on("/revoke-viewers", routeRevokeViewers);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started.");

  // mDNS: lets the app find this device as "autoalert.local" instead of
  // needing to know its numeric IP.
  if (MDNS.begin("autoalert")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS responder started: http://autoalert.local");
  } else {
    Serial.println("mDNS responder failed to start.");
  }
}

// ---------- USB SERIAL WIFI CONFIG ----------
// Lets the app send WiFi credentials over a wired USB connection instead of
// any WiFi-based setup flow - sidesteps every Android WiFi/AP reliability
// quirk we've hit, since no radio is involved in this step at all. This is
// the PRIMARY way both credential sets get onto the device: the driver
// plugs in via USB-OTG once, sends primary AND safety, reads the assigned
// staIp back off the terminal, then unplugs - no USB needed again after
// that (pairing afterward happens over the network, using that IP).
// Protocol: app sends one line per credential set -
//   "CONFIG_WIFI:{\"ssid\":\"...\",\"password\":\"...\",\"slot\":\"primary\"|\"safety\"}\n"
// ("slot" is optional, defaults to "primary" - matches /wifi-config's default,
// so older app builds that never sent "slot" keep working unchanged). Sent
// twice in a row (once per slot) to load both credential sets in one visit.
// Device replies with "WIFI_RESULT:{\"status\":\"ok\",\"staIp\":\"...\"}\n" or
// "WIFI_RESULT:{\"status\":\"error\",\"message\":\"...\"}\n" so the app can parse
// a clear answer out of the same stream that also carries normal debug prints.
// No password gate here (unlike /wifi-config) - physical possession of the
// USB cable is already a stronger trust boundary than local AP/WiFi range.
String serialLineBuffer = "";

void checkSerialWifiConfig() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n') {
      if (serialLineBuffer.startsWith("CONFIG_WIFI:")) {
        String jsonPart = serialLineBuffer.substring(12); // after "CONFIG_WIFI:"
        StaticJsonDocument<320> reqDoc;
        DeserializationError err = deserializeJson(reqDoc, jsonPart);

        if (err) {
          Serial.println("WIFI_RESULT:{\"status\":\"error\",\"message\":\"Bad JSON\"}");
        } else {
          String newSsid = reqDoc["ssid"] | "";
          String newPassword = reqDoc["password"] | "";
          String slot = reqDoc["slot"] | "primary"; // default "primary" - matches /wifi-config

          if (newSsid.length() == 0) {
            Serial.println("WIFI_RESULT:{\"status\":\"error\",\"message\":\"WiFi name required\"}");
          } else if (slot != "primary" && slot != "safety") {
            Serial.println("WIFI_RESULT:{\"status\":\"error\",\"message\":\"slot must be 'primary' or 'safety'\"}");
          } else {
            bool isSafetySlot = (slot == "safety");
            if (isSafetySlot) {
              safetySsid = newSsid;
              safetyPassword = newPassword;
            } else {
              primarySsid = newSsid;
              primaryPassword = newPassword;
            }
            saveConfig();
            WiFi.begin(newSsid.c_str(), newPassword.c_str());
            wifiAttemptSlot = isSafetySlot ? SLOT_SAFETY : SLOT_PRIMARY;
            unsigned long startAttempt = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
              delay(300);
            }

            StaticJsonDocument<192> res;
            bool connected = (WiFi.status() == WL_CONNECTED);
            res["status"] = connected ? "ok" : "error";
            res["slot"] = slot;
            if (connected) {
              res["staIp"] = WiFi.localIP().toString();
            } else {
              res["message"] = "Could not connect - check WiFi name and password";
            }
            String out;
            serializeJson(res, out);
            Serial.println("WIFI_RESULT:" + out);
          }
        }
      }
      serialLineBuffer = "";
    } else if (c != '\r') {
      serialLineBuffer += c;
      // Safety cap so a runaway/garbled line can't grow forever
      if (serialLineBuffer.length() > 400) serialLineBuffer = "";
    }
  }
}

void loop() {
  server.handleClient();
  MDNS.update();
  checkSerialWifiConfig();

  // Dual-WiFi (primary/safety) connect, retry and failover - never blocks
  // anything else in loop() except during its own short bounded checks.
  handleWifiConnection();

  checkMpuTilt();
  checkButton();
  checkLowBattery();
  pollReplyTopic();
  checkLedTimeout();
}
