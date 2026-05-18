#include <Arduino.h>

#ifndef SENSOR_H
#define SENSOR_H

// ============================================================
//  PIN DEFINITIONS
// ============================================================
#define TRIG_PIN              13
#define ECHO_PIN              34

// ============================================================
//  DETECTION SETTINGS
// ============================================================
#define MAIL_DISTANCE_CM      7       // Below this = letter detected
#define DUPLICATE_TIME_MS     1000    // Ignore duplicate within 1000ms

// ============================================================
//  SENSOR STATE
//  volatile keyword required for dual core
//  Tells compiler: this variable can change from another core
//  Without volatile, compiler caches value and misses updates
// ============================================================
static volatile bool          notificationSent  = false;
static volatile unsigned long firstDetectedTime = 0;

// ============================================================
//  initSensor()
//  FIX: 500ms settle time for voltage divider RC capacitance
// ============================================================
void initSensor() {
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
  delay(500);
  Serial.println("[Sensor] Ready.");
}

// ============================================================
//  measureDistance()
//  Single ultrasonic reading
//  timeout 15000us = safe for voltage divider RC delay
// ============================================================
float measureDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(4);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 15000);

  if (duration == 0) return -1;

  float distance = (duration * 0.0343f) / 2.0f;

  if (distance <= 0 || distance > 200) return -1;

  return distance;
}

// ============================================================
//  getReliableDistance()
//  Median of 3 readings - filters voltage divider noise
//  Called only from SensorTask on Core 1
// ============================================================
float getReliableDistance() {
  float r[3];
  int   valid = 0;

  for (int i = 0; i < 3; i++) {
    r[i] = measureDistance();
    if (r[i] > 0) valid++;
    delayMicroseconds(30000);   // 30ms between readings
  }

  if (valid == 0) return -1;
  if (valid == 1) {
    for (int i = 0; i < 3; i++) if (r[i] > 0) return r[i];
  }

  // Sort ascending - return median
  for (int i = 0; i < 2; i++)
    for (int j = i+1; j < 3; j++)
      if (r[i] > r[j]) { float t = r[i]; r[i] = r[j]; r[j] = t; }

  return r[1] > 0 ? r[1] : r[0];
}

// ============================================================
//  calibrateSensor()
// ============================================================
void calibrateSensor() {
  Serial.println("[Sensor] Threshold: " + String(MAIL_DISTANCE_CM) + "cm");
}

// ============================================================
//  checkForMail()
//  Called ONLY from SensorTask (Core 1)
//  Returns true once per new mail detection
//  notificationSent reset is done by WiFiTask after sending
//  via resetMailFlag() below
// ============================================================
bool checkForMail() {
  float distance = getReliableDistance();

  if (distance == -1) return false;

  // ── Letter detected ────────────────────────────────────
  if (distance <= MAIL_DISTANCE_CM) {
    if (!notificationSent) {
      notificationSent  = true;
      firstDetectedTime = millis();
      Serial.print("[Sensor] Mail! Distance: ");
      Serial.print(distance);
      Serial.println("cm");
      return true;
    }
    return false;   // already sent
  }

  // ── No letter - reset for next delivery ────────────────
  if (notificationSent) {
    Serial.println("[Sensor] Cleared - ready.");
  }
  notificationSent  = false;
  firstDetectedTime = 0;
  return false;
}

// ============================================================
//  getSensorStatus()
//  Called from WiFiTask (Core 0) when user sends "sensor"
//  Safe to call from Core 0 - only reads hardware pins
//  Does not touch notificationSent flag
// ============================================================
String getSensorStatus() {
  float distance = getReliableDistance();

  if (distance == -1) {
    return "📏 Sensor Error!\nCheck:\n• Wiring\n• Voltage divider (2x 1kΩ)\n• VCC = 5V";
  }

  bool letterInside = (distance <= MAIL_DISTANCE_CM);
  String status  = "📏 Distance: "    + String(distance, 1) + " cm\n";
  status        += "📬 Mail inside: " + String(letterInside ? "✅ Yes" : "❌ No") + "\n";
  status        += "🎯 Threshold: "   + String(MAIL_DISTANCE_CM) + " cm";
  return status;
}

#endif // SENSOR_H