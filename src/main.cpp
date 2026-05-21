/*
 * ============================================================
 *  SMART DROP MailBox - DUAL CORE VERSION
 *
 *  CORE ASSIGNMENT:
 *  Core 1 → SensorTask  (HC-SR04, runs every 100ms, never blocked)
 *  Core 0 → WiFiTask    (Telegram, WiFi, commands, every 3000ms)
 *
 *  COMMUNICATION:
 *  Core 1 detects mail → sends event to mailQueue
 *  Core 0 reads mailQueue → sends Telegram notification
 *
 *  PROTECTION:
 *  usersMutex  → protects users[] and userCount
 *  modeMutex   → protects currentMode
 *
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "Mail_Detected.h"
#include "Parcel_Door.h"
#include "Mailbox_Capacity.h"  

// Forward declarations
bool sendTelegramMessage(String chatId, String message);
void sendMailNotification();
void sendParcelNotification();
void sendApproachNotification();
void sendCapacityAlert(int level);
void checkTelegramCommands();
void saveUsersToStorage();
void handleNewUser(String chatId, String text, int userIndex);
void handleLoggedInUser(String chatId, String text, int userIndex);

// ============================================================
//  USER SETTINGS
// ============================================================
const char* WIFI_SSID        = "PASINDU";
const char* WIFI_PASSWORD    = "12345678";
const char* BOT_TOKEN        = "1589610753:AAEY3Gpdv_e-Vu37U-H5Jvh0OmO5xHPb1dg";
const char* MAILBOX_PASSWORD = "1234";
const int   MAX_USERS        = 5;

// ============================================================
//  TIMING
// ============================================================
#define SENSOR_CHECK_MS       100    // Core 1 sensor interval
#define TELEGRAM_CHECK_MS     3000   // Core 0 Telegram poll interval
#define SLEEP_DURATION_MS     8000   // Sleep mode WiFi off duration

// ============================================================
//  CONVERSATION STATES
// ============================================================
#define STATE_NEW        0
#define STATE_WAIT_FNAME 1
#define STATE_WAIT_LNAME 2
#define STATE_WAIT_PWD   3
#define STATE_LOGGED_IN  4
#define STATE_LOGGED_OUT 5

// ============================================================
//  USER ACCOUNT STRUCTURE
// ============================================================
struct UserAccount {
  String chatId;
  String firstName;
  String lastName;
  bool   isLoggedIn;
  int    state;
  String tempFirstName;
  String tempLastName;
};

// ============================================================
//  FREERTOS OBJECTS
//
//  mailQueue   - Core 1 sends "1" when mail detected
//                Core 0 reads and sends Telegram
//                Queue size 5 - handles burst detections safely
//
//  usersMutex  - protects users[] array and userCount
//                Both cores may access users[] simultaneously
//                Mutex ensures only one core accesses at a time
//
//  modeMutex   - protects currentMode variable
//                Core 0 writes (from Telegram command)
//                Core 1 reads (to decide sleep/active)
// ============================================================
QueueHandle_t     mailQueue;
QueueHandle_t     parcelQueue;
QueueHandle_t     capacityQueue;
SemaphoreHandle_t usersMutex;
SemaphoreHandle_t modeMutex;

// ============================================================
//  TASK HANDLES
//  Used to monitor and control tasks if needed
// ============================================================
TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t wifiTaskHandle   = NULL;

// ============================================================
//  GLOBAL VARIABLES
// ============================================================
Preferences              prefs;
UserAccount              users[MAX_USERS];
int                      userCount    = 0;
int                      currentMode  = 1;   // 0=Sleep 1=Active
WiFiClientSecure         secured_client;
UniversalTelegramBot     bot(BOT_TOKEN, secured_client);

// ============================================================
//  STORAGE FUNCTIONS
// ============================================================
void saveUsersToStorage() {
  prefs.begin("smartdrop", false);
  prefs.putInt("user_count", userCount);
  for (int i = 0; i < MAX_USERS; i++) {
    String prefix = "u" + String(i) + "_";
    prefs.putString((prefix + "cid").c_str(), users[i].chatId);
    prefs.putString((prefix + "fn").c_str(),  users[i].firstName);
    prefs.putString((prefix + "ln").c_str(),  users[i].lastName);
    prefs.putBool((prefix + "li").c_str(),    users[i].isLoggedIn);
    prefs.putInt((prefix + "st").c_str(),     users[i].state);
  }
  prefs.end();
}

void loadUsersFromStorage() {
  prefs.begin("smartdrop", true);
  userCount = prefs.getInt("user_count", 0);
  for (int i = 0; i < MAX_USERS; i++) {
    String prefix = "u" + String(i) + "_";
    users[i].chatId        = prefs.getString((prefix + "cid").c_str(), "");
    users[i].firstName     = prefs.getString((prefix + "fn").c_str(),  "");
    users[i].lastName      = prefs.getString((prefix + "ln").c_str(),  "");
    users[i].isLoggedIn    = prefs.getBool((prefix + "li").c_str(),    false);
    users[i].state         = prefs.getInt((prefix + "st").c_str(),     STATE_NEW);
    users[i].tempFirstName = "";
    users[i].tempLastName  = "";
  }
  prefs.end();
  Serial.print("[Storage] Loaded ");
  Serial.print(userCount);
  Serial.println(" user(s).");
}

// ============================================================
//  USER MANAGEMENT
//  All functions lock usersMutex before accessing users[]
// ============================================================
int findUser(String chatId) {
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i].chatId == chatId) return i;
  }
  return -1;
}

int createUser(String chatId) {
  if (userCount >= MAX_USERS) return -1;
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i].chatId == "") {
      users[i].chatId        = chatId;
      users[i].firstName     = "";
      users[i].lastName      = "";
      users[i].isLoggedIn    = false;
      users[i].state         = STATE_WAIT_FNAME;
      users[i].tempFirstName = "";
      users[i].tempLastName  = "";
      userCount++;
      return i;
    }
  }
  return -1;
}

void deleteUser(int index) {
  users[index].chatId        = "";
  users[index].firstName     = "";
  users[index].lastName      = "";
  users[index].isLoggedIn    = false;
  users[index].state         = STATE_NEW;
  users[index].tempFirstName = "";
  users[index].tempLastName  = "";
  userCount--;
  saveUsersToStorage();
}

// ============================================================
//  REGISTRATION FLOW
// ============================================================
void handleNewUser(String chatId, String text, int userIndex) {
  int state = users[userIndex].state;

  if (state == STATE_WAIT_FNAME) {
    users[userIndex].tempFirstName = text;
    users[userIndex].state         = STATE_WAIT_LNAME;
    saveUsersToStorage();
    bot.sendMessage(chatId, "👍 Got it!\n\nNow enter your *Last Name*:", "Markdown");
    return;
  }

  if (state == STATE_WAIT_LNAME) {
    users[userIndex].tempLastName = text;
    users[userIndex].state        = STATE_WAIT_PWD;
    saveUsersToStorage();
    bot.sendMessage(chatId, "🔐 Almost done!\n\nEnter the *Mailbox Password*\n_(printed on your mailbox)_:", "Markdown");
    return;
  }

  if (state == STATE_WAIT_PWD) {
    if (text == String(MAILBOX_PASSWORD)) {
      users[userIndex].firstName  = users[userIndex].tempFirstName;
      users[userIndex].lastName   = users[userIndex].tempLastName;
      users[userIndex].isLoggedIn = true;
      users[userIndex].state      = STATE_LOGGED_IN;
      saveUsersToStorage();

      String welcome  = "✅ *Registration Successful!*\n\n";
      welcome        += "👤 Name: " + users[userIndex].firstName + " " + users[userIndex].lastName + "\n";
      welcome        += "🆔 Account: " + chatId + "\n\n";
      welcome        += "🔔 You will receive all mail notifications!\n\n";
      welcome        += "*Available commands:*\n";
      welcome        += "• `active` - Switch to Active Mode\n";
      welcome        += "• `sleep` - Switch to Sleep Mode\n";
      welcome        += "• `status` - Check current status\n";
      welcome        += "• `sensor` - Check mail sensor reading\n";
      welcome        += "• `door` - Check parcel door status\n";
      welcome        += "• `logout` - Logout & delete account";
      bot.sendMessage(chatId, welcome, "Markdown");

    } else {
      deleteUser(userIndex);
      bot.sendMessage(chatId, "❌ *Wrong Password!*\n\nSend any message to try again.", "Markdown");
    }
    return;
  }
}

// ============================================================
//  LOGGED-IN COMMANDS
// ============================================================
void handleLoggedInUser(String chatId, String text, int userIndex) {

  if (text == "/active") {
    // Lock modeMutex before writing currentMode
    if (xSemaphoreTake(modeMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      currentMode = 1;
      xSemaphoreGive(modeMutex);
    }
    bot.sendMessage(chatId, "✅ Switched to *Active Mode*\nWi-Fi stays ON always.", "Markdown");
    return;
  }

  if (text == "/sleep") {
    if (xSemaphoreTake(modeMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      currentMode = 0;
      xSemaphoreGive(modeMutex);
    }
    bot.sendMessage(chatId, "✅ Switched to *Sleep Mode*\nWi-Fi turns OFF between sends.", "Markdown");
    return;
  }

  if (text == "/status") {
    int mode = 1;
    if (xSemaphoreTake(modeMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      mode = currentMode;
      xSemaphoreGive(modeMutex);
    }
    String modeStr = (mode == 1) ? "ACTIVE" : "SLEEP";
    String msg  = "📊 *System Status*\n\n";
    msg += "⚙️ Mode: *" + modeStr + "*\n";
    msg += "🔔 Notifications: ✅ All logged-in users receive alerts\n";
    msg += "👤 Logged in as: " + users[userIndex].firstName + " " + users[userIndex].lastName;
    bot.sendMessage(chatId, msg, "Markdown");
    return;
  }

  if (text == "/sensor") {
    // getSensorStatus() reads GPIO directly - safe from Core 0
    String sensorInfo = getSensorStatus();
    bot.sendMessage(chatId, sensorInfo, "Markdown");
    return;
  }

  if (text == "/door") {
    String doorInfo = getDoorStatusString();
    bot.sendMessage(chatId, doorInfo, "Markdown");
    return;
  }

   if (text == "/capacity") {
    String capInfo = getCapacityStatus();
    bot.sendMessage(chatId, capInfo, "Markdown");
    return;
  }

  if (text == "/logout") {
    deleteUser(userIndex);
    bot.sendMessage(chatId, "👋 *Logged out successfully!*\n\nYour account has been deleted.\nSend any message to register again.", "Markdown");
    return;
  }

  String help  = "❓ Unknown command.\n\n*Available commands:*\n";
  help        += "• `/active` - Switch to Active Mode\n";
  help        += "• `/sleep` - Switch to Sleep Mode\n";
  help        += "• `/status` - Check current status\n";
  help        += "• `/sensor` - Check mail sensor reading\n";
  help        += "• `/door` - Check parcel door status\n";
  help += "• `/capacity` - Check mailbox capacity\n";
  help        += "• `/logout` - Logout & delete account";
  bot.sendMessage(chatId, help, "Markdown");
}

// ============================================================
//  TELEGRAM COMMAND HANDLER
//  Runs only on Core 0 (WiFiTask)
// ============================================================
void checkTelegramCommands() {
  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

  for (int i = 0; i < numNewMessages; i++) {
    String chatId = bot.messages[i].chat_id;
    String text   = bot.messages[i].text;
    text.toLowerCase();
    text.trim();

    // Lock users array before accessing
    if (xSemaphoreTake(usersMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      Serial.println("[WiFi] Could not lock usersMutex");
      return;
    }

    int userIndex = findUser(chatId);

    if (userIndex == -1) {
      if (userCount >= MAX_USERS) {
        xSemaphoreGive(usersMutex);
        bot.sendMessage(chatId, "⚠️ *Mailbox is full!*\n\nMaximum users reached.", "Markdown");
        return;
      }
      createUser(chatId);
      saveUsersToStorage();
      xSemaphoreGive(usersMutex);

      String welcome  = "👋 *Welcome to Smart Drop!*\n\n";
      welcome        += "🔐 This mailbox requires registration.\n\n";
      welcome        += "Please enter your *First Name*:";
      bot.sendMessage(chatId, welcome, "Markdown");
      return;
    }

    int state = users[userIndex].state;
    xSemaphoreGive(usersMutex);   // Unlock before sending messages

    if (state == STATE_WAIT_FNAME ||
        state == STATE_WAIT_LNAME ||
        state == STATE_WAIT_PWD) {
      handleNewUser(chatId, text, userIndex);
      return;
    }

    if (state == STATE_LOGGED_IN) {
      handleLoggedInUser(chatId, text, userIndex);
      return;
    }
  }
}

// ============================================================
//  MAIL NOTIFICATION
//  Runs only on Core 0 (WiFiTask)
//  Locks usersMutex to safely read users[]
// ============================================================
void sendMailNotification() {

  // Lock users array
  if (xSemaphoreTake(usersMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    Serial.println("[Notify] Could not lock usersMutex - skipping.");
    return;
  }

  if (userCount == 0) {
    xSemaphoreGive(usersMutex);
    Serial.println("[Notify] No users.");
    return;
  }

  // Copy chatIds while locked - release mutex quickly
  String chatIds[MAX_USERS];
  int    count = 0;
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i].isLoggedIn && users[i].chatId != "") {
      chatIds[count++] = users[i].chatId;
    }
  }
  xSemaphoreGive(usersMutex);   // Unlock ASAP - don't hold during network calls

  // Flash LED
  digitalWrite(2, HIGH);
  delay(1000);
  digitalWrite(2, LOW);

  // Now send Telegram without holding mutex
  String msg  = "📬 *Mail Detected!*\n\n";
  msg        += "Your mailbox has received mail.\n";
  msg        += "🕐 Please collect it soon.";

  int sentCount = 0;
  for (int i = 0; i < count; i++) {
    bool success = sendTelegramMessage(chatIds[i], msg);
    if (success) sentCount++;
    delay(300);
  }

  Serial.print("[Notify] Alerts sent: ");
  Serial.println(sentCount);
}

// ============================================================
//  PARCEL NOTIFICATION  (return 1 — parcel detected, door opening)
//  Called by WiFiTask after 2s confirmation + door starts opening
// ============================================================
void sendParcelNotification() {
  if (xSemaphoreTake(usersMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return;

  String chatIds[MAX_USERS];
  int    count = 0;
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i].isLoggedIn && users[i].chatId != "") {
      chatIds[count++] = users[i].chatId;
    }
  }
  xSemaphoreGive(usersMutex);

  String msg  = "📦 *Parcel Detected!*\n\n";
  msg        += "A parcel has been detected at your mailbox.\n";
  msg        += "🔓 Door is opening now to receive it.";

  for (int i = 0; i < count; i++) {
    sendTelegramMessage(chatIds[i], msg);
    delay(300);
  }
  Serial.println("[Notify] Parcel detected alerts sent.");
}

// ============================================================
//  PARCEL STORED NOTIFICATION  (return 2 — door closed, parcel safe)
//  Called by WiFiTask after parcel drops and door fully closes
// ============================================================
void sendApproachNotification() {
  if (xSemaphoreTake(usersMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return;

  String chatIds[MAX_USERS];
  int    count = 0;
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i].isLoggedIn && users[i].chatId != "") {
      chatIds[count++] = users[i].chatId;
    }
  }
  xSemaphoreGive(usersMutex);

  String msg  = "✅ *Parcel Stored Safely!*\n\n";
  msg        += "Your parcel has dropped into the safe box.\n";
  msg        += "🔒 Door is now locked.\n";
  msg        += "🕐 Please collect it soon.";

  for (int i = 0; i < count; i++) {
    sendTelegramMessage(chatIds[i], msg);
    delay(300);
  }
  Serial.println("[Notify] Parcel stored alerts sent.");
}

void sendCapacityAlert(int level) {
  if (xSemaphoreTake(usersMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return;

  String chatIds[MAX_USERS];
  int    count = 0;
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i].isLoggedIn && users[i].chatId != "") {
      chatIds[count++] = users[i].chatId;
    }
  }
  xSemaphoreGive(usersMutex);

  String msg;
  if (level == 2) {
    msg  = "🔴 *Mailbox is FULL!*\n\n";
    msg += "Your mailbox is at 90%+ capacity.\n";
    msg += "📬 Please collect your mail soon!\n";
    msg += "Use `capacity` command to check details.";
  } else {
    msg  = "🟡 *Mailbox Nearly Full*\n\n";
    msg += "Your mailbox is at 70%+ capacity.\n";
    msg += "📬 Consider collecting your mail soon.";
  }

  for (int i = 0; i < count; i++) {
    sendTelegramMessage(chatIds[i], msg);
    delay(300);
  }
  Serial.println("[Notify] Capacity alert sent.");
}

// ============================================================
//  NETWORK FUNCTIONS
// ============================================================
bool isInternetAvailable() {
  IPAddress ip;
  return WiFi.hostByName("api.telegram.org", ip);
}

bool connectWiFi() {
  Serial.print("[WiFi] Connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;
    if (attempts > 30) {
      Serial.println("\n[WiFi] FAILED!");
      return false;
    }
  }

  Serial.println("\n[WiFi] Connected!");
  delay(3000);
  return isInternetAvailable();
}

void disconnectWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool sendTelegramMessage(String chatId, String message) {
  int maxRetries = 3;
  int retryDelay = 2000;

  for (int attempt = 1; attempt <= maxRetries; attempt++) {
    if (!isInternetAvailable()) {
      delay(retryDelay);
      continue;
    }
    bool success = bot.sendMessage(chatId, message, "Markdown");
    if (success) return true;
    delay(retryDelay);
  }

  Serial.println("[Telegram] All retries failed.");
  return false;
}

void flushOldMessages() {
  Serial.println("[Telegram] Flushing...");
  int numMessages = bot.getUpdates(bot.last_message_received + 1);
  while (numMessages) {
    numMessages = bot.getUpdates(bot.last_message_received + 1);
  }
  Serial.println("[Telegram] Queue cleared!");
}

// ============================================================
//  SENSOR TASK — Runs on Core 1
//
//  This task NEVER touches WiFi, Telegram, or users[]
//  It only reads the HC-SR04 sensor
//  When mail detected → puts event in mailQueue
//  Core 0 reads mailQueue and handles Telegram
//
//  vTaskDelay(pdMS_TO_TICKS(100)):
//  FreeRTOS-safe delay - yields CPU to other tasks
//  Never use delay() inside FreeRTOS tasks
// ============================================================
void SensorTask(void* parameter) {
  Serial.println("[Core1] SensorTask started.");

  for (;;) {

    // ── Mail sensor check ─────────────────────────────────
    if (checkForMail()) {
      uint8_t mailEvent = 1;
      xQueueSend(mailQueue, &mailEvent, pdMS_TO_TICKS(100));
      Serial.println("[Core1] Mail event sent to queue.");
    }

    // ── Parcel door state machine ─────────────────────────
    // Returns: 0=nothing, 1=parcel loaded, 2=approach detected
    int doorResult = checkDoorStateMachine();
    if (doorResult == 1) {
      uint8_t parcelEvent = 1;
      xQueueSend(parcelQueue, &parcelEvent, pdMS_TO_TICKS(100));
      Serial.println("[Core1] Parcel event sent to queue.");
    }
    if (doorResult == 2) {
      uint8_t approachEvent = 2;
      xQueueSend(parcelQueue, &approachEvent, pdMS_TO_TICKS(100));
      Serial.println("[Core1] Approach event sent to queue.");
    }

    // ── Capacity check ────────────────────────────────────
    int capAlert = checkCapacityAlert();
    if (capAlert > 0) {
      uint8_t capEvent = (uint8_t)capAlert;
      xQueueSend(capacityQueue, &capEvent, pdMS_TO_TICKS(100));
      Serial.println("[Core1] Capacity alert event sent.");
    }

    vTaskDelay(pdMS_TO_TICKS(SENSOR_CHECK_MS));
  }
}

// ============================================================
//  WIFI TASK — Runs on Core 0
//
//  Handles everything network related:
//  - Reads mailQueue → sends notification if mail detected
//  - Polls Telegram every 3000ms for commands
//  - Reconnects WiFi if dropped
//
//  xQueueReceive(..., 0):
//  timeout = 0 means non-blocking check
//  If nothing in queue, continues immediately
//  Does NOT wait - keeps Telegram polling running
// ============================================================
void WiFiTask(void* parameter) {
  Serial.println("[Core0] WiFiTask started.");

  unsigned long lastTelegramCheck = 0;
  uint8_t       mailEvent         = 0;
  uint8_t       parcelEvent       = 0;
  uint8_t       capEvent          = 0;

  for (;;) {

    // ── Auto reconnect WiFi ──────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      delay(1000);
      connectWiFi();
    }

    // ── Check mailQueue (non-blocking) ───────────────────
    if (xQueueReceive(mailQueue, &mailEvent, 0) == pdTRUE) {
      Serial.println("[Core0] Mail event - sending notification.");
      sendMailNotification();
    }

    // ── Check parcelQueue (non-blocking) ─────────────────
    if (xQueueReceive(parcelQueue, &parcelEvent, 0) == pdTRUE) {
      if (parcelEvent == 1) {
        Serial.println("[Core0] Parcel event - sending notification.");
        sendParcelNotification();
      }
      if (parcelEvent == 2) {
        Serial.println("[Core0] Approach event - sending notification.");
        sendApproachNotification();
      }
    }

    // ── Check capacityQueue (non-blocking) ───────────────
    if (xQueueReceive(capacityQueue, &capEvent, 0) == pdTRUE) {
      Serial.println("[Core0] Capacity alert event received.");
      sendCapacityAlert(capEvent);
    }

    // ── Telegram poll every 3000ms ───────────────────────
    unsigned long now = millis();
    if (now - lastTelegramCheck >= TELEGRAM_CHECK_MS) {
      lastTelegramCheck = now;
      checkTelegramCommands();
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================
//  SETUP
//  Runs on Core 1 before tasks start
//  Creates FreeRTOS objects and pins tasks to cores
// ============================================================
void setup() {
  pinMode(2, OUTPUT);
  Serial.begin(115200);
  delay(1000);

  secured_client.setInsecure();
  secured_client.setTimeout(15);
  secured_client.setHandshakeTimeout(15);

  Serial.println("=== SMART DROP Dual Core Starting ===");

  // ── Create FreeRTOS objects ──────────────────────────
  // Queue: holds up to 5 mail events, each event = 1 byte
  mailQueue  = xQueueCreate(5, sizeof(uint8_t));
  parcelQueue = xQueueCreate(5, sizeof(uint8_t));
  capacityQueue = xQueueCreate(5, sizeof(uint8_t)); 

  usersMutex = xSemaphoreCreateMutex();
  modeMutex  = xSemaphoreCreateMutex();

  if (mailQueue == NULL || parcelQueue == NULL || capacityQueue == NULL || usersMutex == NULL || modeMutex == NULL) {
    Serial.println("[ERROR] FreeRTOS objects failed. Restarting...");
    delay(3000);
    ESP.restart();
  }

  // ── Init sensor and load users ───────────────────────
  initSensor();
  initDoor();
  initCapacitySensor(); 
  loadUsersFromStorage();

  // ── Connect WiFi ─────────────────────────────────────
  bool connected = connectWiFi();
  if (!connected) {
    Serial.println("[ERROR] No internet. Restarting...");
    delay(5000);
    ESP.restart();
  }

  delay(2000);
  flushOldMessages();
  calibrateSensor();

  // ── Create Tasks pinned to specific cores ────────────
  //
  // xTaskCreatePinnedToCore(
  //   function,     // task function
  //   name,         // task name for debugging
  //   stack size,   // bytes of RAM for this task
  //   parameter,    // passed to task function (NULL = nothing)
  //   priority,     // 0=lowest, higher=more important
  //   handle,       // reference to task (for monitoring)
  //   core          // 0 or 1
  // )

  xTaskCreatePinnedToCore(
    SensorTask,         // function
    "SensorTask",       // name
    4096,               // stack: 4KB enough for sensor math
    NULL,               // no parameter needed
    2,                  // priority 2 = HIGH (sensor is critical)
    &sensorTaskHandle,  // handle
    1                   // Core 1
  );

  xTaskCreatePinnedToCore(
    WiFiTask,           // function
    "WiFiTask",         // name
    8192,               // stack: 8KB for WiFi/SSL/JSON/Strings
    NULL,               // no parameter needed
    1,                  // priority 1 = MEDIUM
    &wifiTaskHandle,    // handle
    0                   // Core 0
  );

  Serial.println("=== Tasks Started ===");
  Serial.println("[Core1] SensorTask → 100ms interval");
  Serial.println("[Core0] WiFiTask   → 3000ms Telegram poll");
  Serial.println("=== Ready! ===");
}

// ============================================================
//  LOOP
//  Empty - all work done by FreeRTOS tasks
//  loop() itself runs on Core 1 at lowest priority
//  It will never interfere with SensorTask or WiFiTask
// ============================================================
void loop() {
  vTaskDelay(pdMS_TO_TICKS(10000));   // Sleep loop - tasks handle everything
}