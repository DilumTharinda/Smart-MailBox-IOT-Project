#include <Arduino.h>

#ifndef PARCEL_DOOR_H
#define PARCEL_DOOR_H

// ============================================================
//  PIN DEFINITIONS — HC-SR04 #2 (Parcel Sensor)
// ============================================================
#define PARCEL_TRIG_PIN         25
#define PARCEL_ECHO_PIN         35

// ============================================================
//  PIN DEFINITIONS — L298N Motor Controller
// ============================================================
#define MOTOR_IN1               26    // Direction A
#define MOTOR_IN2               27    // Direction B
#define MOTOR_ENA               14    // PWM Speed

// ============================================================
//  MOTOR SETTINGS
// ============================================================
#define MOTOR_SPEED             200   // 0-255 PWM (~78% speed)
#define DOOR_OPEN_TIME_MS       2500  // Time for drawer to fully open
#define DOOR_CLOSE_TIME_MS      2500  // Time for drawer to fully close

// ============================================================
//  DETECTION SETTINGS
// ============================================================
#define PARCEL_DISTANCE_CM      8     // Parcel detected when dist <= 8cm
#define PARCEL_DETECT_WAIT_MS   2000  // Wait 2s after detect before msg+open
#define PARCEL_FALL_WAIT_MS     2000  // Wait 2s after parcel falls before close
#define PARCEL_TIMEOUT_MS       30000 // Safety close if parcel never falls
#define COOLDOWN_MS             5000  // Wait after close before re-arming

// ============================================================
//  FLOW:
//
//  [CLOSED]
//    sensor reads <= 8cm  (parcel in front of sensor)
//    ↓
//  [PARCEL_WAIT]  wait 2000ms to confirm parcel is real
//    ↓
//  → return 1  ←  WiFiTask sends "📦 Parcel Detected!" msg
//  [OPENING]  motor pushes drawer OUT for 2500ms
//    ↓
//  [OPEN_WAITING]  wait for parcel to fall (dist goes > 8cm)
//    ↓
//  [FALL_WAIT]  wait 2000ms after fall confirmed
//    ↓
//  [CLOSING]  motor pulls drawer IN for 2500ms
//    ↓
//  → return 2  ←  WiFiTask sends "✅ Parcel stored in safe box!" msg
//  [COOLDOWN]  wait 5000ms
//    ↓
//  [CLOSED]  re-armed for next delivery
// ============================================================
#define DOOR_CLOSED             0
#define DOOR_PARCEL_WAIT        1
#define DOOR_OPENING            2
#define DOOR_OPEN_WAITING       3
#define DOOR_FALL_WAIT          4
#define DOOR_CLOSING            5
#define DOOR_COOLDOWN           6

// ============================================================
//  STATE VARIABLES  (volatile = safe across both cores)
// ============================================================
static volatile int           doorState      = DOOR_CLOSED;
static volatile unsigned long doorActionTime = 0;
static volatile unsigned long parcelWaitTime = 0;
static volatile unsigned long fallWaitTime   = 0;
static volatile unsigned long cooldownTime   = 0;

// ============================================================
//  initDoor()
//  Call once in setup() after initSensor()
// ============================================================
void initDoor() {
  pinMode(PARCEL_TRIG_PIN, OUTPUT);
  pinMode(PARCEL_ECHO_PIN, INPUT);
  digitalWrite(PARCEL_TRIG_PIN, LOW);

  pinMode(MOTOR_IN1, OUTPUT);
  pinMode(MOTOR_IN2, OUTPUT);
  pinMode(MOTOR_ENA, OUTPUT);

  // Motor starts stopped
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, LOW);
  analogWrite(MOTOR_ENA, 0);

  delay(200);
  Serial.println("[Door] Initialized. State: CLOSED");
}

// ============================================================
//  measureParcelDistance()
//  Single HC-SR04 reading — fast, used inside state machine
//  Returns distance in cm, or -1 if invalid
// ============================================================
float measureParcelDistance() {
  digitalWrite(PARCEL_TRIG_PIN, LOW);
  delayMicroseconds(4);
  digitalWrite(PARCEL_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(PARCEL_TRIG_PIN, LOW);

  long duration = pulseIn(PARCEL_ECHO_PIN, HIGH, 15000);
  if (duration == 0) return -1;

  float distance = (duration * 0.0343f) / 2.0f;
  if (distance <= 0 || distance > 200) return -1;

  return distance;
}

// ============================================================
//  getReliableParcelDistance()
//  Median of 3 readings — used for "door" status command only
//  NOT used inside state machine (too slow for 100ms loop)
// ============================================================
float getReliableParcelDistance() {
  float r[3];
  int   valid = 0;

  for (int i = 0; i < 3; i++) {
    r[i] = measureParcelDistance();
    if (r[i] > 0) valid++;
    delayMicroseconds(30000);
  }

  if (valid == 0) return -1;
  if (valid == 1) {
    for (int i = 0; i < 3; i++) if (r[i] > 0) return r[i];
  }

  // Sort ascending → return median
  for (int i = 0; i < 2; i++)
    for (int j = i+1; j < 3; j++)
      if (r[i] > r[j]) { float t = r[i]; r[i] = r[j]; r[j] = t; }

  return r[1] > 0 ? r[1] : r[0];
}

// ============================================================
//  Motor helpers
// ============================================================
void motorOpen() {
  analogWrite(MOTOR_ENA, MOTOR_SPEED);
  digitalWrite(MOTOR_IN1, HIGH);
  digitalWrite(MOTOR_IN2, LOW);
  Serial.println("[Door] Motor → OPEN");
}

void motorClose() {
  analogWrite(MOTOR_ENA, MOTOR_SPEED);
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, HIGH);
  Serial.println("[Door] Motor → CLOSE");
}

void motorStop() {
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, LOW);
  analogWrite(MOTOR_ENA, 0);
  Serial.println("[Door] Motor → STOP");
}

// ============================================================
//  getDoorStatusString()
//  Called from WiFiTask when user sends "door" command
// ============================================================
String getDoorStatusString() {
  float dist = getReliableParcelDistance();

  String stateStr;
  switch (doorState) {
    case DOOR_CLOSED:       stateStr = "🔒 Closed - waiting for parcel";   break;
    case DOOR_PARCEL_WAIT:  stateStr = "📦 Parcel detected - opening soon"; break;
    case DOOR_OPENING:      stateStr = "🔓 Opening...";                     break;
    case DOOR_OPEN_WAITING: stateStr = "🟡 Open - waiting parcel to drop";  break;
    case DOOR_FALL_WAIT:    stateStr = "⬇️ Parcel dropped - closing soon";  break;
    case DOOR_CLOSING:      stateStr = "🔒 Closing...";                     break;
    case DOOR_COOLDOWN:     stateStr = "⏳ Cooldown - ready soon";           break;
    default:                stateStr = "❓ Unknown";
  }

  String status  = "🚪 *Door Status*\n\n";
  status        += "State: " + stateStr + "\n";
  status        += "📏 Sensor distance: ";
  status        += (dist == -1) ? "Sensor Error" : (String(dist, 1) + " cm");
  return status;
}

// ============================================================
//  checkDoorStateMachine()
//  NON-BLOCKING — called from SensorTask every 100ms (Core 1)
//
//  RETURNS:
//  0 = nothing to report
//  1 = parcel detected + door opening → send "📦 Parcel Detected!"
//  2 = door closed after parcel drop  → send "✅ Parcel Stored!"
// ============================================================
int checkDoorStateMachine() {
  unsigned long now = millis();

  switch (doorState) {

    // ── CLOSED: scan for parcel ───────────────────────────
    case DOOR_CLOSED: {
      float dist = measureParcelDistance();

      if (dist > 0 && dist <= PARCEL_DISTANCE_CM) {
        // Parcel detected → start 2s confirmation wait
        doorState     = DOOR_PARCEL_WAIT;
        parcelWaitTime = now;
        Serial.print("[Door] Parcel detected at ");
        Serial.print(dist);
        Serial.println("cm - waiting 2s to confirm...");
      }
      break;
    }

    // ── PARCEL_WAIT: confirm parcel still there after 2s ──
    case DOOR_PARCEL_WAIT: {
      float dist = measureParcelDistance();

      // Parcel removed before 2s? Cancel and go back
      if (dist == -1 || dist > PARCEL_DISTANCE_CM) {
        doorState = DOOR_CLOSED;
        Serial.println("[Door] Parcel removed during wait - cancelled.");
        break;
      }

      if (now - parcelWaitTime >= PARCEL_DETECT_WAIT_MS) {
        // 2s confirmed → send alert then open door
        motorOpen();
        doorState     = DOOR_OPENING;
        doorActionTime = now;
        Serial.println("[Door] Confirmed! Sending alert + opening door.");
        return 1;   // ← WiFiTask sends "📦 Parcel Detected!" msg
      }
      break;
    }

    // ── OPENING: wait for drawer to fully extend ──────────
    case DOOR_OPENING:
      if (now - doorActionTime >= DOOR_OPEN_TIME_MS) {
        motorStop();
        doorState     = DOOR_OPEN_WAITING;
        doorActionTime = now;   // reset for timeout tracking
        Serial.println("[Door] Fully open - waiting for parcel to fall.");
      }
      break;

    // ── OPEN_WAITING: wait for parcel to fall/drop off ────
    //  When drawer opens, parcel slides/falls to ground floor
    //  Sensor reads > 8cm once parcel is gone from sensor view
    case DOOR_OPEN_WAITING: {
      float dist = measureParcelDistance();

      if (dist == -1 || dist > PARCEL_DISTANCE_CM) {
        // Parcel has fallen → start 2s wait before closing
        doorState    = DOOR_FALL_WAIT;
        fallWaitTime = now;
        Serial.println("[Door] Parcel fell to ground! Waiting 2s...");
      }

      // Safety timeout: force close if parcel never falls
      if (now - doorActionTime >= PARCEL_TIMEOUT_MS) {
        Serial.println("[Door] Timeout - force closing.");
        motorClose();
        doorState     = DOOR_CLOSING;
        doorActionTime = now;
      }
      break;
    }

    // ── FALL_WAIT: 2s pause after parcel drops ────────────
    case DOOR_FALL_WAIT:
      if (now - fallWaitTime >= PARCEL_FALL_WAIT_MS) {
        motorClose();
        doorState     = DOOR_CLOSING;
        doorActionTime = now;
        Serial.println("[Door] Closing door.");
      }
      break;

    // ── CLOSING: wait for drawer to fully retract ─────────
    case DOOR_CLOSING:
      if (now - doorActionTime >= DOOR_CLOSE_TIME_MS) {
        motorStop();
        doorState    = DOOR_COOLDOWN;
        cooldownTime = now;
        Serial.println("[Door] Closed - parcel in safe box.");
        return 2;   // ← WiFiTask sends "✅ Parcel stored in safe box!" msg
      }
      break;

    // ── COOLDOWN: brief pause before re-arming ────────────
    case DOOR_COOLDOWN:
      if (now - cooldownTime >= COOLDOWN_MS) {
        doorState = DOOR_CLOSED;
        Serial.println("[Door] Armed - ready for next delivery.");
      }
      break;

    default:
      break;
  }

  return 0;
}

#endif // PARCEL_DOOR_H