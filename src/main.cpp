#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>

#include "bluetooth_web.h"

#define LED_PIN 2
#define RESET_BUTTON_PIN 0

static const byte DNS_PORT = 53;
static const char SETUP_AP_PASSWORD[] = "esp32setup";
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;
static const unsigned long RESET_BUTTON_HOLD_MS = 5000;

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

bool setupMode = false;
String setupReason;

static bool otaUpdateOk = false;
static unsigned long resetButtonPressedAt = 0;
static bool resetButtonHandled = false;

String htmlEscape(const String& value) {
  String result;
  result.reserve(value.length());

  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    switch (c) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '"': result += "&quot;"; break;
      case '\'': result += "&#39;"; break;
      default: result += c; break;
    }
  }

  return result;
}

String css() {
  return R"rawliteral(
    body { font-family: system-ui, sans-serif; margin: 0; padding: 32px; background: #f5f5f5; color: #222; }
    .card { max-width: 760px; margin: 0 auto; padding: 24px; border-radius: 16px; background: white; box-shadow: 0 4px 24px rgba(0,0,0,0.08); }
    label { display: block; margin: 16px 0 6px; font-weight: 600; }
    input { width: 100%; box-sizing: border-box; padding: 12px; border: 1px solid #bbb; border-radius: 10px; font-size: 16px; }
    button, .button { display: inline-block; margin-top: 18px; padding: 12px 16px; border: 0; border-radius: 10px; background: #222; color: white; font-size: 16px; text-decoration: none; cursor: pointer; }
    .secondary { background: #eee; color: #222; }
    code { background: #eee; padding: 2px 6px; border-radius: 6px; }
    .muted { color: #666; }
    .warning { color: #9a4b00; }
  )rawliteral";
}

String makeSetupApName() {
  uint64_t mac = ESP.getEfuseMac();
  uint16_t suffix = (uint16_t)(mac & 0xFFFF);
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "ESP32-Setup-%04X", suffix);
  return String(buffer);
}

void blinkLed(int count, int delayMs) {
  for (int i = 0; i < count; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(delayMs);
    digitalWrite(LED_PIN, LOW);
    delay(delayMs);
  }
}

bool loadWiFiCredentials(String& ssid, String& pass) {
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  pass = preferences.getString("password", "");
  preferences.end();
  return ssid.length() > 0;
}

void saveWiFiCredentials(const String& ssid, const String& pass) {
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", pass);
  preferences.end();
}

void clearWiFiCredentials() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();
}

String pageWrap(const String& title, const String& body) {
  String html = "<!doctype html><html lang=\"ru\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>";
  html += title;
  html += "</title><style>" + css() + "</style></head><body><div class=\"card\">" + body + "</div></body></html>";
  return html;
}

String makeSetupPage() {
  String body = "<h1>Настройка Wi-Fi для ESP32</h1>";
  body += "<p class=\"muted\">Причина запуска режима настройки: <code>" + htmlEscape(setupReason) + "</code></p>";
  body += "<p>Введите имя и пароль Wi-Fi сети. После сохранения ESP32 перезагрузится и попробует подключиться к этой сети.</p>";
  body += "<form method=\"POST\" action=\"/save\"><label for=\"ssid\">SSID / имя Wi-Fi сети</label><input id=\"ssid\" name=\"ssid\" required autocomplete=\"off\">";
  body += "<label for=\"password\">Пароль Wi-Fi</label><input id=\"password\" name=\"password\" type=\"password\" autocomplete=\"current-password\">";
  body += "<button type=\"submit\">Сохранить и перезагрузить</button></form>";
  body += "<p class=\"muted\">ESP32 подключается только к 2.4 GHz Wi-Fi.</p><p><a class=\"button secondary\" href=\"/status\">Статус</a></p>";
  return pageWrap("ESP32 Wi-Fi Setup", body);
}

String makeMainPage() {
  String body = "<h1>ESP32 работает</h1><p>Устройство подключено к Wi-Fi и отдаёт эту страницу из локальной сети.</p>";
  body += "<p><b>IP ESP32:</b> <code>" + WiFi.localIP().toString() + "</code></p>";
  body += "<p><b>RSSI:</b> <code>" + String(WiFi.RSSI()) + " dBm</code></p>";
  body += "<p><b>Uptime:</b> <code>" + String(millis() / 1000) + " s</code></p>";
  body += "<p><a class=\"button secondary\" href=\"/status\">JSON статус</a></p>";
  body += "<p><a class=\"button secondary\" href=\"/ota\">OTA обновление прошивки</a></p>";
  body += "<p><a class=\"button secondary\" href=\"/bluetooth\">Bluetooth / BLE console</a></p>";
  body += "<p><a class=\"button secondary\" href=\"/reset-wifi\" onclick=\"return confirm('Стереть Wi-Fi настройки и перезагрузить ESP32?')\">Стереть Wi-Fi настройки</a></p>";
  body += "<p class=\"muted\">Для сброса настроек также можно удерживать IO0/BOOT около 5 секунд после обычного запуска.</p>";
  return pageWrap("ESP32 Web Server", body);
}

String makeOtaPage() {
  String body = "<h1>OTA обновление ESP32</h1><p>Выбери файл <code>firmware.bin</code>, собранный GitHub Actions.</p>";
  body += "<p class=\"warning\">Не загружай сюда <code>merged-flash.bin</code>. Он нужен для полной прошивки через USB/UART.</p>";
  body += "<form method=\"POST\" action=\"/update\" enctype=\"multipart/form-data\"><label for=\"firmware\">Файл прошивки</label><input id=\"firmware\" name=\"firmware\" type=\"file\" accept=\".bin\" required><button type=\"submit\">Загрузить и установить</button></form>";
  body += "<p><a class=\"button secondary\" href=\"/\">Назад</a></p>";
  return pageWrap("ESP32 OTA Update", body);
}

void handleSetupRoot() { server.send(200, "text/html; charset=utf-8", makeSetupPage()); }

void handleSaveWiFi() {
  String ssid = server.arg("ssid");
  String pass = server.arg("password");
  ssid.trim();
  if (ssid.length() == 0) {
    server.send(400, "text/plain; charset=utf-8", "SSID is empty");
    return;
  }
  saveWiFiCredentials(ssid, pass);
  server.send(200, "text/html; charset=utf-8", pageWrap("Saved", "<h1>Wi-Fi настройки сохранены</h1><p>ESP32 перезагрузится через несколько секунд.</p>"));
  delay(1500);
  ESP.restart();
}

void handleMainRoot() {
  digitalWrite(LED_PIN, HIGH);
  server.send(200, "text/html; charset=utf-8", makeMainPage());
  delay(50);
  digitalWrite(LED_PIN, LOW);
}

void handleOtaPage() {
  if (setupMode) {
    server.send(403, "text/plain; charset=utf-8", "OTA update is disabled in setup mode");
    return;
  }
  server.send(200, "text/html; charset=utf-8", makeOtaPage());
}

void handleOtaUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaUpdateOk = false;
    digitalWrite(LED_PIN, HIGH);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!Update.hasError()) {
      size_t written = Update.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize) Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    otaUpdateOk = Update.end(true);
    if (!otaUpdateOk) Update.printError(Serial);
    digitalWrite(LED_PIN, LOW);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    otaUpdateOk = false;
    Update.abort();
    digitalWrite(LED_PIN, LOW);
  }
}

void handleOtaFinished() {
  server.sendHeader("Connection", "close");
  if (otaUpdateOk) {
    server.send(200, "text/html; charset=utf-8", pageWrap("OTA OK", "<h1>OTA обновление успешно</h1><p>ESP32 перезагрузится через несколько секунд.</p>"));
    delay(1500);
    ESP.restart();
  } else {
    server.send(500, "text/html; charset=utf-8", pageWrap("OTA failed", "<h1>OTA обновление не удалось</h1><p>Проверь Serial Monitor. Загружать нужно firmware.bin.</p><p><a href=\"/ota\">Назад</a></p>"));
  }
}

void handleStatus() {
  String json = "{";
  json += "\"mode\":\"" + String(setupMode ? "setup" : "normal") + "\",";
  json += "\"wifi_status\":" + String((int)WiFi.status()) + ",";
  json += "\"local_ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"soft_ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"sketch_size\":" + String(ESP.getSketchSize()) + ",";
  json += "\"free_sketch_space\":" + String(ESP.getFreeSketchSpace()) + ",";
  json += "\"uptime_ms\":" + String(millis());
  json += "}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleResetWiFi() {
  server.send(200, "text/plain; charset=utf-8", "Wi-Fi credentials cleared. Restarting ESP32...");
  clearWiFiCredentials();
  delay(1000);
  ESP.restart();
}

void handleNotFound() {
  if (setupMode) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain; charset=utf-8", "Redirecting to setup portal");
  } else {
    server.send(404, "text/plain; charset=utf-8", "404 Not Found");
  }
}

bool connectToWiFi(const String& ssid, const String& pass, unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-web-test");
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startTime < timeoutMs) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    Serial.print(".");
    delay(500);
  }
  digitalWrite(LED_PIN, LOW);
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  return false;
}

void startMainServer() {
  setupMode = false;
  MDNS.begin("esp32-test");
  server.on("/", HTTP_GET, handleMainRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/ota", HTTP_GET, handleOtaPage);
  server.on("/update", HTTP_POST, handleOtaFinished, handleOtaUpload);
  server.on("/reset-wifi", HTTP_GET, handleResetWiFi);
  registerBluetoothRoutes();
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.print("Open in browser: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
  Serial.print("Bluetooth page: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/bluetooth");
}

void startSetupPortal(const String& reason) {
  setupMode = true;
  setupReason = reason;
  WiFi.disconnect();
  delay(200);
  WiFi.mode(WIFI_AP);
  String apSsid = makeSetupApName();
  bool apStarted = strlen(SETUP_AP_PASSWORD) >= 8 ? WiFi.softAP(apSsid.c_str(), SETUP_AP_PASSWORD) : WiFi.softAP(apSsid.c_str());
  if (!apStarted) ESP.restart();
  IPAddress apIp = WiFi.softAPIP();
  dnsServer.start(DNS_PORT, "*", apIp);
  server.on("/", HTTP_GET, handleSetupRoot);
  server.on("/save", HTTP_POST, handleSaveWiFi);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/reset-wifi", HTTP_GET, handleResetWiFi);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.print("AP SSID: "); Serial.println(apSsid);
  Serial.print("AP password: "); Serial.println(SETUP_AP_PASSWORD);
  Serial.print("Setup URL: http://"); Serial.print(apIp); Serial.println("/");
}

void checkResetButton() {
  bool pressed = digitalRead(RESET_BUTTON_PIN) == LOW;
  if (pressed) {
    if (resetButtonPressedAt == 0) {
      resetButtonPressedAt = millis();
      resetButtonHandled = false;
    }
    if (!resetButtonHandled && millis() - resetButtonPressedAt >= RESET_BUTTON_HOLD_MS) {
      resetButtonHandled = true;
      clearWiFiCredentials();
      blinkLed(5, 120);
      ESP.restart();
    }
  } else {
    resetButtonPressedAt = 0;
    resetButtonHandled = false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  Serial.println("ESP32 Wi-Fi + OTA + BLE web server started");

  String ssid;
  String pass;
  if (loadWiFiCredentials(ssid, pass)) {
    if (connectToWiFi(ssid, pass, WIFI_CONNECT_TIMEOUT_MS)) startMainServer();
    else startSetupPortal("Saved Wi-Fi credentials failed");
  } else {
    startSetupPortal("No saved Wi-Fi credentials");
  }
}

void loop() {
  if (setupMode) dnsServer.processNextRequest();
  server.handleClient();
  checkResetButton();

  static unsigned long lastWiFiCheck = 0;
  static unsigned long disconnectedSince = 0;
  if (!setupMode && millis() - lastWiFiCheck > 5000) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (disconnectedSince == 0) disconnectedSince = millis();
      WiFi.reconnect();
      if (millis() - disconnectedSince > 60000) ESP.restart();
    } else {
      disconnectedSince = 0;
    }
  }
}
