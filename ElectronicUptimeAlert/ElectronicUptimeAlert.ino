#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <FirebaseESP8266.h>

// ======================== FIREBASE CONFIG ========================
#define WEB_API_KEY "AIzaSyCwgPIXYmb1X265MAMnblvhuLH-F397HuY"
#define DATABASE_URL "https://thiefdetectorapp-default-rtdb.asia-southeast1.firebasedatabase.app"
#define USER_EMAIL "YOUR_FIREBASE_EMAIL"
#define USER_PASS "YOUR_FIREBASE_PASSWORD"

// Firebase root path (separate from ThiefAlarm)
#define ROOT_PATH "uptimeAlert"

// Config paths
#define CFG_WIFI_SSID     ROOT_PATH "/config/wifi_ssid"
#define CFG_WIFI_PASS     ROOT_PATH "/config/wifi_pass"
#define CFG_WA_SENDER     ROOT_PATH "/config/wa_sender"
#define CFG_WA_GROUP      ROOT_PATH "/config/wa_group"
#define CFG_WA_TOKEN      ROOT_PATH "/config/wa_token"
#define CFG_WA_URL        ROOT_PATH "/config/wa_url"
#define CFG_TG_TOKEN      ROOT_PATH "/config/telegram_bot_token"
#define CFG_TG_CHAT_ID    ROOT_PATH "/config/telegram_chat_id"
#define CFG_NOTIFY_METHOD ROOT_PATH "/config/notification_method"
#define CFG_ALERT_ENABLED ROOT_PATH "/config/alert_enabled"
#define CFG_FIRST_ALERT   ROOT_PATH "/config/first_alert_minutes"
#define CFG_ESCALATION    ROOT_PATH "/config/escalation_intervals"
#define CFG_DEVICE_NAME   ROOT_PATH "/config/device_name"
#define CFG_HEARTBEAT_INT ROOT_PATH "/config/heartbeat_interval"
#define CFG_ALERT_ON_BOOT ROOT_PATH "/config/alert_on_power_on"
#define CFG_RESTART       ROOT_PATH "/config/restart_request"

// Status paths
#define STS_UPTIME        ROOT_PATH "/status/uptime_seconds"
#define STS_LAST_BOOT     ROOT_PATH "/status/last_boot"
#define STS_WIFI_RSSI     ROOT_PATH "/status/wifi_rssi"
#define STS_WIFI_SSID     ROOT_PATH "/status/wifi_ssid"
#define STS_HEARTBEAT     ROOT_PATH "/status/last_heartbeat"
#define STS_ALERTS_SENT   ROOT_PATH "/status/alerts_sent"
#define STS_LAST_ALERT    ROOT_PATH "/status/last_alert_time"
#define STS_IS_ONLINE     ROOT_PATH "/status/is_online"
#define STS_CURRENT_INTERVAL ROOT_PATH "/status/current_interval_minutes"

// ======================== DEFAULT CONFIG ========================
const char* DEFAULT_WIFI_SSID[] = {"iconnet"};
const char* DEFAULT_WIFI_PASS[] = {"BlackPanther"};
const int DEFAULT_WIFI_COUNT = 1;

String waSender = "";
String waGroup = "";
String waToken = "";
String waUrl = "https://waservices.brahmayasa.com:8000/send-message";
String telegramBotToken = "";
String telegramChatID = "";
int notificationMethod = 0;
bool alertEnabled = true;
int firstAlertMinutes = 30;
String escalationIntervals = "20,15,10,5";
String deviceName = "UptimeAlert";
int heartbeatInterval = 60;
bool alertOnPowerOn = false;
bool restartRequest = false;

// Runtime state
unsigned long bootTime = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastAlertTime = 0;
int alertsSent = 0;
int currentEscalationIndex = 0;
int currentIntervalMinutes = 0;
bool firstAlertSent = false;
unsigned long lastPollTime = 0;
const unsigned long pollInterval = 10000;

// WiFi polling
String pollWifiSsid = "";
String pollWifiPass = "";
bool pollRestartRequest = false;
bool pollWifiSsidChanged = false;

// ======================== FIREBASE OBJECTS ========================
FirebaseAuth auth;
FirebaseConfig config;
FirebaseData fbdo;
FirebaseData fbdoRead;

// ======================== WEB SERVER ========================
ESP8266WebServer webServer(80);

// ======================== NTP ========================
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

// ======================== FORWARD DECLARATIONS ========================
void streamDataCallback(StreamData data);
void streamTimeoutCallback(bool timeout);
void loadConfigFromFirebase();
void pollFirebase();
void checkAlerts();
void sendAlert();
bool sendTelegramNotification(String message);
bool sendWhatsAppNotification(String message);
bool sendNotification(String message);
String formatUptime(unsigned long seconds);
String getTimestamp();
int getNextIntervalMinutes();
bool connectToWiFi();
void setupOTA();
void handleRoot();
void handleRestart();
void writeBootStatus();
void writeHeartbeat();

// ======================== SETUP ========================
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ElectronicUptimeAlert ===");
  Serial.println("Booting...");

  bootTime = millis();

  WiFi.mode(WIFI_STA);
  WiFi.hostname("uptime-alert-" + String(ESP.getChipId(), HEX));
  if (!connectToWiFi()) {
    Serial.println("WiFi failed, restarting in 30s...");
    delay(30000);
    ESP.restart();
  }

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Waiting for NTP sync...");
  int retries = 0;
  while (time(nullptr) < 100000 && retries < 40) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  Serial.println();

  config.api_key = WEB_API_KEY;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASS;
  config.database_url = DATABASE_URL;
  config.timeout.serverResponse = 10 * 1000;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Waiting for Firebase auth...");
  unsigned long fbStart = millis();
  while (!Firebase.ready() && millis() - fbStart < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (Firebase.ready()) {
    Serial.println("Firebase connected!");

    Firebase.setStreamCallback(fbdo, streamDataCallback, streamTimeoutCallback);
    if (!Firebase.beginStream(fbdo, ROOT_PATH "/config")) {
      Serial.printf("Stream begin failed: %s\n", fbdo.errorReason().c_str());
    }

    loadConfigFromFirebase();
    writeBootStatus();
  } else {
    Serial.println("Firebase auth failed, running with defaults");
  }

  setupOTA();

  webServer.on("/", handleRoot);
  webServer.on("/restart", HTTP_POST, handleRestart);
  webServer.begin();
  Serial.println("Web server started on port 80");

  Serial.println("=== Setup Complete ===");
  Serial.printf("Device: %s\n", deviceName.c_str());
  Serial.printf("First alert: %d minutes\n", firstAlertMinutes);
  Serial.printf("Escalation: %s\n", escalationIntervals.c_str());
  Serial.printf("Alert enabled: %s\n", alertEnabled ? "YES" : "NO");
}

// ======================== MAIN LOOP ========================
void loop() {
  ArduinoOTA.handle();
  webServer.handleClient();

  unsigned long now = millis();

  if (now - lastPollTime >= pollInterval) {
    lastPollTime = now;
    if (Firebase.ready()) {
      pollFirebase();
    }
  }

  if (Firebase.ready() && (now - lastHeartbeat >= (unsigned long)heartbeatInterval * 1000)) {
    lastHeartbeat = now;
    writeHeartbeat();
  }

  if (alertEnabled && Firebase.ready()) {
    checkAlerts();
  }

  if (restartRequest) {
    Serial.println("Restart requested from dashboard!");
    delay(500);
    ESP.restart();
  }

  static unsigned long lastWifiCheck = 0;
  if (now - lastWifiCheck >= 30000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected, reconnecting...");
      connectToWiFi();
    }
  }

  delay(10);
}

// ======================== FIREBASE STREAM ========================
void streamDataCallback(StreamData data) {
  if (data.dataType() == "json") {
    Serial.println("Stream data:");
    Serial.println(data.jsonString());
  }
}

void streamTimeoutCallback(bool timeout) {
  if (timeout) {
    Serial.println("Stream timeout, reconnecting...");
    Firebase.beginStream(fbdo, ROOT_PATH "/config");
  }
}

// ======================== LOAD CONFIG ========================
void loadConfigFromFirebase() {
  Serial.println("Loading config from Firebase...");

  Firebase.getString(fbdoRead, CFG_WIFI_SSID);
  if (fbdoRead.stringData().length() > 0) {
    pollWifiSsid = fbdoRead.stringData();
    Serial.printf("WiFi SSID: %s\n", pollWifiSsid.c_str());
  }

  Firebase.getString(fbdoRead, CFG_WIFI_PASS);
  if (fbdoRead.stringData().length() > 0) pollWifiPass = fbdoRead.stringData();

  Firebase.getString(fbdoRead, CFG_WA_SENDER);
  if (fbdoRead.stringData().length() > 0) waSender = fbdoRead.stringData();

  Firebase.getString(fbdoRead, CFG_WA_GROUP);
  if (fbdoRead.stringData().length() > 0) waGroup = fbdoRead.stringData();

  Firebase.getString(fbdoRead, CFG_WA_TOKEN);
  if (fbdoRead.stringData().length() > 0) waToken = fbdoRead.stringData();

  Firebase.getString(fbdoRead, CFG_WA_URL);
  if (fbdoRead.stringData().length() > 0) waUrl = fbdoRead.stringData();

  Firebase.getString(fbdoRead, CFG_TG_TOKEN);
  if (fbdoRead.stringData().length() > 0) telegramBotToken = fbdoRead.stringData();

  Firebase.getString(fbdoRead, CFG_TG_CHAT_ID);
  if (fbdoRead.stringData().length() > 0) telegramChatID = fbdoRead.stringData();

  Firebase.getInt(fbdoRead, CFG_NOTIFY_METHOD);
  notificationMethod = fbdoRead.intData();

  Firebase.getBool(fbdoRead, CFG_ALERT_ENABLED);
  if (fbdoRead.boolData() != 0) alertEnabled = fbdoRead.boolData();

  Firebase.getInt(fbdoRead, CFG_FIRST_ALERT);
  if (fbdoRead.intData() > 0) firstAlertMinutes = fbdoRead.intData();

  Firebase.getString(fbdoRead, CFG_ESCALATION);
  if (fbdoRead.stringData().length() > 0) escalationIntervals = fbdoRead.stringData();

  Firebase.getString(fbdoRead, CFG_DEVICE_NAME);
  if (fbdoRead.stringData().length() > 0) deviceName = fbdoRead.stringData();

  Firebase.getInt(fbdoRead, CFG_HEARTBEAT_INT);
  if (fbdoRead.intData() > 0) heartbeatInterval = fbdoRead.intData();

  Firebase.getBool(fbdoRead, CFG_ALERT_ON_BOOT);
  if (fbdoRead.boolData() != 0) alertOnPowerOn = fbdoRead.boolData();

  Firebase.getBool(fbdoRead, CFG_RESTART);
  if (fbdoRead.boolData()) restartRequest = true;

  Serial.println("Config loaded successfully");
}

// ======================== POLL FIREBASE ========================
void pollFirebase() {
  Firebase.getString(fbdoRead, CFG_WIFI_SSID);
  String newSsid = fbdoRead.stringData();
  if (newSsid.length() > 0 && newSsid != pollWifiSsid) {
    pollWifiSsid = newSsid;
    pollWifiSsidChanged = true;
    Serial.printf("WiFi SSID changed: %s\n", pollWifiSsid.c_str());
  }

  Firebase.getString(fbdoRead, CFG_WIFI_PASS);
  String newPass = fbdoRead.stringData();
  if (newPass.length() > 0 && newPass != pollWifiPass) {
    pollWifiPass = newPass;
  }

  Firebase.getBool(fbdoRead, CFG_RESTART);
  if (fbdoRead.boolData()) pollRestartRequest = true;

  if (pollWifiSsidChanged && pollWifiSsid.length() > 0) {
    Serial.printf("Connecting to new WiFi: %s\n", pollWifiSsid.c_str());
    WiFi.disconnect();
    delay(500);
    WiFi.begin(pollWifiSsid.c_str(), pollWifiPass.c_str());
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected to new WiFi!");
      Firebase.setString(fbdo, STS_WIFI_SSID, WiFi.SSID());
      Firebase.setBool(fbdo, ROOT_PATH "/config/wifi_connect_result/success", true);
    } else {
      Serial.println("\nFailed to connect to new WiFi");
      Firebase.setBool(fbdo, ROOT_PATH "/config/wifi_connect_result/success", false);
    }
    Firebase.setBool(fbdo, CFG_RESTART, false);
    pollWifiSsidChanged = false;
  }

  if (pollRestartRequest) {
    Serial.println("Restart from Firebase!");
    delay(500);
    ESP.restart();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Firebase.setInt(fbdo, STS_WIFI_RSSI, WiFi.RSSI());
  }
}

// ======================== ALERT LOGIC ========================
void checkAlerts() {
  unsigned long uptimeSeconds = (millis() - bootTime) / 1000;

  static unsigned long lastUptimeWrite = 0;
  if (millis() - lastUptimeWrite >= 10000) {
    lastUptimeWrite = millis();
    Firebase.setInt(fbdo, STS_UPTIME, uptimeSeconds);
  }

  unsigned long uptimeMinutes = uptimeSeconds / 60;

  if (!firstAlertSent && uptimeMinutes >= (unsigned long)firstAlertMinutes) {
    firstAlertSent = true;
    currentEscalationIndex = 0;
    currentIntervalMinutes = getNextIntervalMinutes();
    Firebase.setInt(fbdo, STS_CURRENT_INTERVAL, currentIntervalMinutes);
    sendAlert();
    return;
  }

  if (firstAlertSent && currentIntervalMinutes > 0) {
    unsigned long timeSinceLastAlert = (millis() - lastAlertTime) / 1000 / 60;
    if (timeSinceLastAlert >= (unsigned long)currentIntervalMinutes) {
      currentEscalationIndex++;
      currentIntervalMinutes = getNextIntervalMinutes();
      Firebase.setInt(fbdo, STS_CURRENT_INTERVAL, currentIntervalMinutes);
      sendAlert();
    }
  }
}

int getNextIntervalMinutes() {
  int intervals[10];
  int count = 0;

  String temp = escalationIntervals;
  while (temp.length() > 0 && count < 10) {
    int commaIdx = temp.indexOf(',');
    if (commaIdx >= 0) {
      intervals[count++] = temp.substring(0, commaIdx).toInt();
      temp = temp.substring(commaIdx + 1);
    } else {
      intervals[count++] = temp.toInt();
      break;
    }
  }

  if (count == 0) return 5;

  if (currentEscalationIndex < count) {
    return intervals[currentEscalationIndex];
  }
  return intervals[count - 1];
}

void sendAlert() {
  unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
  String uptimeStr = formatUptime(uptimeSeconds);
  String timestamp = getTimestamp();

  String message = "[" + deviceName + "] Uptime Alert!\n";
  message += "Device has been running for: " + uptimeStr + "\n";
  message += "Alert #" + String(alertsSent + 1) + "\n";
  message += "Time: " + timestamp + "\n";
  message += "Next alert in: " + String(getNextIntervalMinutes()) + " minutes";

  Serial.println("=== SENDING ALERT ===");
  Serial.println(message);
  Serial.println("=====================");

  bool success = sendNotification(message);

  if (success) {
    alertsSent++;
    lastAlertTime = millis();
    firstAlertSent = true;
    Firebase.setInt(fbdo, STS_ALERTS_SENT, alertsSent);
    Firebase.setString(fbdo, STS_LAST_ALERT, timestamp);
    Serial.printf("Alert sent! Total: %d\n", alertsSent);
  } else {
    Serial.println("Alert send FAILED");
  }
}

// ======================== NOTIFICATION ========================
bool sendNotification(String message) {
  if (notificationMethod == 1) return sendTelegramNotification(message);
  return sendWhatsAppNotification(message);
}

bool sendTelegramNotification(String message) {
  if (telegramBotToken.length() == 0 || telegramChatID.length() == 0) {
    Serial.println("Telegram credentials not set!");
    return false;
  }

  Serial.println("Sending Telegram...");

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000);

  IPAddress ip;
  if (WiFi.hostByName("api.telegram.org", ip)) {
    Serial.printf("api.telegram.org -> %s\n", ip.toString().c_str());
  }

  StaticJsonDocument<512> doc;
  doc["chat_id"] = telegramChatID;
  doc["text"] = message;
  String payload;
  serializeJson(doc, payload);

  String hosts[] = {"api.telegram.org", "149.154.166.110", "149.154.167.220"};
  for (int i = 0; i < 3; i++) {
    Serial.printf("Trying %s:443... ", hosts[i].c_str());
    if (client.connect(hosts[i].c_str(), 443)) {
      String httpReq = "POST /bot" + telegramBotToken + "/sendMessage HTTP/1.1\r\n";
      httpReq += "Host: api.telegram.org\r\n";
      httpReq += "Content-Type: application/json\r\n";
      httpReq += "Connection: close\r\n";
      httpReq += "Content-Length: " + String(payload.length()) + "\r\n";
      httpReq += "\r\n" + payload;

      client.print(httpReq);

      unsigned long timeout = millis() + 10000;
      bool success = false;
      while (millis() < timeout) {
        if (client.available()) {
          String line = client.readStringUntil('\n');
          if (line.startsWith("HTTP/")) {
            int sp1 = line.indexOf(' ');
            int sp2 = line.indexOf(' ', sp1 + 1);
            if (sp1 > 0 && sp2 > sp1) {
              int code = line.substring(sp1 + 1, sp2).toInt();
              if (code == 200) { success = true; Serial.println("Sent!"); }
            }
          }
          if (success) break;
        }
      }
      client.stop();
      if (success) return true;
    } else {
      Serial.println("Failed");
    }
    delay(100);
  }
  Serial.println("All Telegram hosts failed!");
  return false;
}

bool sendWhatsAppNotification(String message) {
  if (waSender.length() == 0 || waGroup.length() == 0 || waToken.length() == 0) {
    Serial.println("WhatsApp credentials not set!");
    return false;
  }

  Serial.println("Sending WhatsApp...");

  HTTPClient http;
  WiFiClient waClient;
  http.begin(waClient, waUrl);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setTimeout(20000);

  String postData = "sender=" + waSender + "&number=" + waGroup + "&is_group_target=1&message=" + message + "&token_auth=" + waToken;
  int code = http.POST(postData);

  if (code > 0) {
    Serial.printf("WA Response: %d\n", code);
    Serial.println(http.getString());
    http.end();
    return true;
  }
  Serial.printf("WA Error: %d\n", code);
  http.end();
  return false;
}

// ======================== UTILITY ========================
String formatUptime(unsigned long seconds) {
  unsigned long d = seconds / 86400;
  unsigned long h = (seconds % 86400) / 3600;
  unsigned long m = (seconds % 3600) / 60;
  unsigned long s = seconds % 60;
  String r = "";
  if (d > 0) r += String(d) + "d ";
  r += String(h) + "h " + String(m) + "m " + String(s) + "s";
  return r;
}

String getTimestamp() {
  time_t now = time(nullptr);
  if (now < 100000) return "NTP not synced";
  struct tm* t = localtime(&now);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
  return String(buf);
}

// ======================== FIREBASE WRITES ========================
void writeBootStatus() {
  String ts = getTimestamp();
  Firebase.setString(fbdo, STS_LAST_BOOT, ts);
  Firebase.setString(fbdo, STS_HEARTBEAT, ts);
  Firebase.setBool(fbdo, STS_IS_ONLINE, true);
  if (WiFi.status() == WL_CONNECTED) {
    Firebase.setString(fbdo, STS_WIFI_SSID, WiFi.SSID());
    Firebase.setInt(fbdo, STS_WIFI_RSSI, WiFi.RSSI());
  }
  if (pollWifiSsid.length() == 0) {
    Firebase.setString(fbdo, CFG_WIFI_SSID, DEFAULT_WIFI_SSID[0]);
  }
  if (deviceName.length() == 0) {
    Firebase.setString(fbdo, CFG_DEVICE_NAME, "UptimeAlert");
  }
  Firebase.setInt(fbdo, CFG_FIRST_ALERT, firstAlertMinutes);
  Firebase.setString(fbdo, CFG_ESCALATION, escalationIntervals);
  Firebase.setBool(fbdo, CFG_ALERT_ENABLED, alertEnabled);
  Firebase.setInt(fbdo, CFG_NOTIFY_METHOD, notificationMethod);
  Serial.println("Boot status written to Firebase");
}

void writeHeartbeat() {
  Firebase.setString(fbdo, STS_HEARTBEAT, getTimestamp());
  Firebase.setBool(fbdo, STS_IS_ONLINE, true);
  if (WiFi.status() == WL_CONNECTED) {
    Firebase.setInt(fbdo, STS_WIFI_RSSI, WiFi.RSSI());
  }
}

// ======================== WIFI ========================
bool connectToWiFi() {
  Serial.println("Connecting to WiFi...");

  if (pollWifiSsid.length() > 0) {
    Serial.printf("Trying Firebase SSID: %s\n", pollWifiSsid.c_str());
    WiFi.begin(pollWifiSsid.c_str(), pollWifiPass.c_str());
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) {
      delay(500); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nConnected to %s! IP: %s\n", pollWifiSsid.c_str(), WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("\nFirebase WiFi failed");
  }

  for (int i = 0; i < DEFAULT_WIFI_COUNT; i++) {
    Serial.printf("Trying: %s\n", DEFAULT_WIFI_SSID[i]);
    WiFi.begin(DEFAULT_WIFI_SSID[i], DEFAULT_WIFI_PASS[i]);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) {
      delay(500); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nConnected to %s! IP: %s\n", DEFAULT_WIFI_SSID[i], WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("\nFailed");
  }
  return false;
}

// ======================== OTA ========================
void setupOTA() {
  ArduinoOTA.setHostname(("uptime-alert-" + String(ESP.getChipId(), HEX)).c_str());
  ArduinoOTA.setPassword("uptime123");
  ArduinoOTA.onStart([]() { Serial.println("OTA Starting..."); });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA Complete!"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) { Serial.printf("OTA: %u%%\r", (p / (t / 100))); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA Error[%u]: ", e); });
  ArduinoOTA.begin();
  Serial.println("OTA ready");
}

// ======================== WEB SERVER ========================
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>" + deviceName + " Control</title>";
  html += "<style>";
  html += "body{font-family:Arial;background:#0f172a;color:#e2e8f0;margin:0;padding:20px}";
  html += ".c{background:#1e293b;border:1px solid #334155;border-radius:12px;padding:20px;margin:10px 0}";
  html += "h1{color:#38bdf8;text-align:center}";
  html += ".s{font-size:24px;color:#22c55e;text-align:center;margin:10px 0}";
  html += "button{background:#0ea5e9;color:#fff;border:none;padding:12px;border-radius:8px;cursor:pointer;font-size:16px;width:100%;margin:5px 0}";
  html += "button:hover{background:#0284c7}";
  html += "button.d{background:#ef4444}";
  html += "table{width:100%;border-collapse:collapse}";
  html += "td{padding:8px;border-bottom:1px solid #334155}";
  html += "td:first-child{color:#94a3b8;width:40%}";
  html += "</style></head><body>";
  html += "<h1>" + deviceName + "</h1>";
  html += "<div class='c'><div class='s'>UPTIME: " + formatUptime((millis() - bootTime) / 1000) + "</div></div>";
  html += "<div class='c'><table>";
  html += "<tr><td>Status</td><td>" + String(Firebase.ready() ? "Online" : "Offline") + "</td></tr>";
  html += "<tr><td>WiFi</td><td>" + WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)</td></tr>";
  html += "<tr><td>IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  html += "<tr><td>Alerts Sent</td><td>" + String(alertsSent) + "</td></tr>";
  html += "<tr><td>First Alert</td><td>After " + String(firstAlertMinutes) + " min</td></tr>";
  html += "<tr><td>Escalation</td><td>" + escalationIntervals + " min</td></tr>";
  html += "<tr><td>Next Alert In</td><td>" + String(currentIntervalMinutes) + " min</td></tr>";
  html += "</table></div>";
  html += "<div class='c'><form action='/restart' method='POST'><button class='d'>Restart Device</button></form></div>";
  html += "<p style='text-align:center;color:#64748b;font-size:12px'>Use dashboard to configure all settings</p>";
  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

void handleRestart() {
  webServer.send(200, "text/html", "<html><body style='background:#0f172a;color:#e2e8f0;text-align:center;padding:50px'><h1>Restarting...</h1></body></html>");
  delay(3000);
  ESP.restart();
}
