#include <Arduino.h>

#ifndef MAILBOX_CAPACITY_H
#define MAILBOX_CAPACITY_H

// ============================================================
//  PIN DEFINITIONS — HC-SR04 #3 (Capacity Sensor)
// ============================================================
#define CAP_TRIG_PIN            32
#define CAP_ECHO_PIN            33

// ============================================================
//  MAILBOX DIMENSIONS
//
//  CAP_BOX_HEIGHT_CM:
//  Total internal height of mailbox from floor to sensor
//  Measure with ruler before deploying
//  Example: sensor mounted at top, box floor is 20cm below
//  → CAP_BOX_HEIGHT_CM = 20
//
//  HOW CAPACITY IS CALCULATED:
//  Empty box  → sensor reads ~20cm (full height, nothing inside)
//  Half full  → sensor reads ~10cm (mail pile halfway up)
//  Full box   → sensor reads ~2cm  (mail almost at sensor)
//
//  capacity% = ((boxHeight - distance) / boxHeight) × 100
//
//  Example:
//  boxHeight = 20cm, distance = 5cm
//  filled    = 20 - 5 = 15cm of mail
//  capacity  = (15 / 20) × 100 = 75%
// ============================================================
#define CAP_BOX_HEIGHT_CM       20.0f   // ← MAILBOX height
#define CAP_FULL_THRESHOLD      90      // Warn when capacity >= 90%
#define CAP_NEAR_FULL_THRESHOLD 70      // Near-full warning at 70%

// ============================================================
//  SENSOR SETTINGS
// ============================================================
#define CAP_MIN_DISTANCE_CM     2.0f    // Sensor dead zone (too close)
#define CAP_MAX_DISTANCE_CM     (CAP_BOX_HEIGHT_CM + 5.0f)  // Beyond box = empty

// ============================================================
//  STATE
// ============================================================
static bool capFullAlertSent     = false;   // Track if full alert already sent
static bool capNearFullAlertSent = false;   // Track if near-full alert sent

// ============================================================
//  initCapacitySensor()
//  Call once in setup() after initDoor()
// ============================================================
void initCapacitySensor() {
  pinMode(CAP_TRIG_PIN, OUTPUT);
  pinMode(CAP_ECHO_PIN, INPUT);
  digitalWrite(CAP_TRIG_PIN, LOW);
  delay(200);
  Serial.println("[Capacity] Sensor initialized.");
}

// ============================================================
//  measureCapDistance()
//  Single HC-SR04 reading
//  Returns distance in cm, or -1 if invalid
// ============================================================
float measureCapDistance() {
  digitalWrite(CAP_TRIG_PIN, LOW);
  delayMicroseconds(4);
  digitalWrite(CAP_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(CAP_TRIG_PIN, LOW);

  long duration = pulseIn(CAP_ECHO_PIN, HIGH, 15000);
  if (duration == 0) return -1;

  float distance = (duration * 0.0343f) / 2.0f;
  if (distance < CAP_MIN_DISTANCE_CM || distance > CAP_MAX_DISTANCE_CM) return -1;

  return distance;
}

// ============================================================
//  getReliableCapDistance()
//  Median of 3 readings for accuracy
// ============================================================
float getReliableCapDistance() {
  float r[3];
  int   valid = 0;

  for (int i = 0; i < 3; i++) {
    r[i] = measureCapDistance();
    if (r[i] > 0) valid++;
    delayMicroseconds(30000);
  }

  if (valid == 0) return -1;
  if (valid == 1) {
    for (int i = 0; i < 3; i++) if (r[i] > 0) return r[i];
  }

  // Sort ascending → return median
  for (int i = 0; i < 2; i++)
    for (int j = i + 1; j < 3; j++)
      if (r[i] > r[j]) { float t = r[i]; r[i] = r[j]; r[j] = t; }

  return r[1] > 0 ? r[1] : r[0];
}

// ============================================================
//  getCapacityPercent()
//  Returns 0–100 (%), or -1 if sensor error
//
//  0%   = completely empty
//  100% = completely full (mail at sensor level)
// ============================================================
int getCapacityPercent() {
  float dist = getReliableCapDistance();
  if (dist == -1) return -1;

  // Clamp distance to box height range
  if (dist > CAP_BOX_HEIGHT_CM) dist = CAP_BOX_HEIGHT_CM;

  float filled   = CAP_BOX_HEIGHT_CM - dist;
  int   capacity = (int)((filled / CAP_BOX_HEIGHT_CM) * 100.0f);

  // Clamp to 0-100
  if (capacity < 0)   capacity = 0;
  if (capacity > 100) capacity = 100;

  return capacity;
}

// ============================================================
//  getCapacityBar()
// ============================================================
String getCapacityBar(int percent) {
  int filled = percent / 10;   // 0–10 blocks
  String bar = "[";
  for (int i = 0; i < 10; i++) {
    bar += (i < filled) ? "█" : "░";
  }
  bar += "] " + String(percent) + "%";
  return bar;
}

// ============================================================
//  checkCapacityAlert()
//  Call from SensorTask — returns alert level:
//
//  0 = normal, no alert
//  1 = near full (>= 70%) — send warning once
//  2 = full     (>= 90%) — send urgent alert once
//
//  Alerts reset when capacity drops below threshold
//  (user collected mail)
// ============================================================
int checkCapacityAlert() {
  int capacity = getCapacityPercent();
  if (capacity == -1) return 0;   // Sensor error - skip

  // ── Reset alerts when mail collected ──────────────────
  if (capacity < CAP_NEAR_FULL_THRESHOLD) {
    capFullAlertSent     = false;
    capNearFullAlertSent = false;
    return 0;
  }

  // ── Full alert (>= 90%) ────────────────────────────────
  if (capacity >= CAP_FULL_THRESHOLD && !capFullAlertSent) {
    capFullAlertSent = true;
    Serial.print("[Capacity] FULL alert: ");
    Serial.print(capacity);
    Serial.println("%");
    return 2;
  }

  // ── Near full alert (>= 70%) ──────────────────────────
  if (capacity >= CAP_NEAR_FULL_THRESHOLD && !capNearFullAlertSent) {
    capNearFullAlertSent = true;
    Serial.print("[Capacity] NEAR FULL alert: ");
    Serial.print(capacity);
    Serial.println("%");
    return 1;
  }

  return 0;
}

// ============================================================
//  getCapacityStatus()
//  For Telegram "capacity" command — full status string
// ============================================================
String getCapacityStatus() {
  int   capacity = getCapacityPercent();
  float dist     = getReliableCapDistance();

  if (capacity == -1) {
    return "📦 *Mailbox Capacity*\n\n❌ Sensor Error - check wiring!\n_Pins: TRIG=" + String(CAP_TRIG_PIN) + " ECHO=" + String(CAP_ECHO_PIN) + "_";
  }

  String emoji;
  String label;
  if      (capacity >= CAP_FULL_THRESHOLD)     { emoji = "🔴"; label = "FULL - Please collect mail!"; }
  else if (capacity >= CAP_NEAR_FULL_THRESHOLD) { emoji = "🟡"; label = "Nearly Full";                }
  else if (capacity >= 30)                      { emoji = "🟢"; label = "Moderate";                    }
  else                                          { emoji = "🟢"; label = "Empty / Low";                 }

  String status  = "📦 *Mailbox Capacity*\n\n";
  status        += emoji + " " + getCapacityBar(capacity) + "\n\n";
  status        += "📊 Filled: *" + String(capacity) + "%*\n";
  status        += "📏 Distance to mail: " + (dist == -1 ? "N/A" : String(dist, 1) + " cm") + "\n";
  status        += "📐 Box height: " + String((int)CAP_BOX_HEIGHT_CM) + " cm\n";
  status        += "💬 Status: " + label;
  return status;
}

#endif // MAILBOX_CAPACITY_H