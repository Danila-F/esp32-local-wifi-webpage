#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

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

unsigned long resetButtonPressedAt = 0;
bool resetButtonHandled = false;

String htmlEscape(const String& value) {
  String result;

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

bool loadWiFiCredentials(String& ssid, String& password) {
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  preferences.end();

  return ssid.length() > 0;
}

void saveWiFiCredentials(const String& ssid, const String& password) {
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
}

void clearWiFiCredentials() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();
}

String css() {
  return R"rawliteral(
    body {
      font-family: system-ui, sans-serif;
      margin: 0;
      padding: 32px;
      background: #f5f5f5;
      color: #222;
    }
    .card {
      max-width: 720px;
      margin: 0 auto;
      padding: 24px;
      border-radius: 16px;
      background: white;
      box-shadow: 0 4px 24px rgba(0,0,0,0.08);
    }
    label {
      display: block;
      margin: 16px 0 6px;
      font-weight: 600;
    }
    input {
      width: 100%;
      box-sizing: border-box;
      padding: 12px;
      border: 1px solid #bbb;
      border-radius: 10px;
      font-size: 16px;
    }
    button, .button {
      display: inline-block;
      margin-top: 18px;
      padding: 12px 16px;
      border: 0;
      border-radius: 10px;
      background: #222;
      color: white;
      font-size: 16px;
      text-decoration: none;
      cursor: pointer;
    }
    .secondary {
      background: #eee;
      color: #222;
    }
    code {
      background: #eee;
      padding: 2px 6px;
      border-radius: 6px;
    }
    .muted {
      color: #666;
    }
  )rawliteral";
}

String makeSetupPage() {
  String html = R"rawliteral(
<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Wi-Fi Setup</title>
  <style>
)rawliteral";

  html += css();

  html += R"rawliteral(
  </style>
</head>
<body>
  <div class="card">
    <h1>Настройка Wi-Fi для ESP32</h1>
)rawliteral";

  html += "<p class=\"muted\">Причина запуска режима настройки: <code>";
  html += htmlEscape(setupReason);
  html += "</code></p>";

  html += R"rawliteral(
    <p>Введите имя и пароль Wi-Fi сети. После сохранения ESP32 перезагрузится и попробует подключиться к этой сети.</p>

    <form method="POST" action="/save">
      <label for="ssid">SSID / имя Wi-Fi сети</label>
      <input id="ssid" name="ssid" required autocomplete="off">

      <label for="password">Пароль Wi-Fi</label>
      <input id="password" name="password" type="password" autocomplete="current-password">

      <button type="submit">Сохранить и перезагрузить</button>
    </form>

    <p class="muted">ESP32 подключается только к 2.4 GHz Wi-Fi. К 5 GHz-only сети она не подключится.</p>
    <p><a class="button secondary" href="/status">Статус</a></p>
  </div>
</body>
</html>
)rawliteral";

  return html;
}

String makeMainPage() {
  String html = R"rawliteral(
<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Web Server</title>
  <style>
)rawliteral";

  html += css();

  html += R"rawliteral(
  </style>
</head>
<body>
  <div class="card">
    <h1>ESP32 работает</h1>
    <p>Устройство подключено к Wi-Fi и отдаёт эту страницу из локальной сети.</p>
)rawliteral";

  html += "<p><b>IP ESP32:</b> <code>" + WiFi.localIP().toString() + "</code></p>";
  html += "<p><b>RSSI:</b> <code>" + String(WiFi.RSSI()) + " dBm</code></p>";
  html += "<p><b>Uptime:</b> <code>" + String(millis() / 1000) + " s</code></p>";

  html += R"rawliteral(
    <p><a class="button secondary" href="/status">JSON статус</a></p>
    <p><a class="button secondary" href="/reset-wifi" onclick="return confirm('Стереть Wi-Fi настройки и перезагрузить ESP32?')">Стереть Wi-Fi настройки</a></p>

    <p class="muted">
      Также можно стереть настройки, удерживая кнопку IO0/BOOT около 5 секунд после обычного запуска ESP32.
      Не держи IO0 во время нажатия EN, иначе плата уйдёт в bootloader mode.
    </p>
  </div>
</body>
</html>
)rawliteral";

  return html;
}

void handleSetupRoot() {
  server.send(200, "text/html; charset=utf-8", makeSetupPage());
}

void handleSaveWiFi() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain; charset=utf-8", "Missing ssid");
    return;
  }

  String ssid = server.arg("ssid");
  String password = server.arg("password");

  ssid.trim();

  if (ssid.length() == 0) {
    server.send(400, "text/plain; charset=utf-8", "SSID is empty");
    return;
  }

  saveWiFiCredentials(ssid, password);

  server.send(
    200,
    "text/html; charset=utf-8",
    "<!doctype html><html lang=\"ru\"><meta charset=\"utf-8\">"
    "<h1>Wi-Fi настройки сохранены</h1>"
    "<p>ESP32 перезагрузится через несколько секунд.</p>"
    "</html>"
  );

  Serial.println("Wi-Fi credentials saved. Restarting...");
  delay(1500);
  ESP.restart();
}

void handleMainRoot() {
  digitalWrite(LED_PIN, HIGH);
  server.send(200, "text/html; charset=utf-8", makeMainPage());
  delay(50);
  digitalWrite(LED_PIN, LOW);
}

void handleStatus() {
  String json = "{";
  json += "\"mode\":\"";
  json += setupMode ? "setup" : "normal";
  json += "\",";
  json += "\"wifi_status\":" + String((int)WiFi.status()) + ",";
  json += "\"local_ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"soft_ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime_ms\":" + String(millis());
  json += "}";

  server.send(200, "application/json; charset=utf-8", json);
}

void handleResetWiFi() {
  server.send(200, "text/plain; charset=utf-8", "Wi-Fi credentials cleared. Restarting ESP32...");
  Serial.println("Clearing Wi-Fi credentials by HTTP request...");
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

bool connectToWiFi(const String& ssid, const String& password, unsigned long timeoutMs) {
  Serial.println();
  Serial.println("Connecting to saved Wi-Fi...");
  Serial.print("SSID: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-web-test");
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startTime < timeoutMs) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    Serial.print(".");
    delay(500);
  }

  digitalWrite(LED_PIN, LOW);
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    return true;
  }

  Serial.println("Wi-Fi connection failed");
  return false;
}

void startMainServer() {
  setupMode = false;

  if (MDNS.begin("esp32-test")) {
    Serial.println("mDNS started: http://esp32-test.local/");
  } else {
    Serial.println("mDNS failed");
  }

  server.on("/", HTTP_GET, handleMainRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/reset-wifi", HTTP_GET, handleResetWiFi);
  server.onNotFound(handleNotFound);

  server.begin();

  Serial.println("HTTP server started");
  Serial.print("Open in browser: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
}

void startSetupPortal(const String& reason) {
  setupMode = true;
  setupReason = reason;

  WiFi.disconnect();
  delay(200);

  WiFi.mode(WIFI_AP);

  String apSsid = makeSetupApName();

  bool apStarted;

  if (strlen(SETUP_AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(apSsid.c_str(), SETUP_AP_PASSWORD);
  } else {
    apStarted = WiFi.softAP(apSsid.c_str());
  }

  if (!apStarted) {
    Serial.println("Failed to start setup AP. Restarting...");
    delay(1000);
    ESP.restart();
  }

  IPAddress apIp = WiFi.softAPIP();

  dnsServer.start(DNS_PORT, "*", apIp);

  server.on("/", HTTP_GET, handleSetupRoot);
  server.on("/save", HTTP_POST, handleSaveWiFi);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/reset-wifi", HTTP_GET, handleResetWiFi);
  server.onNotFound(handleNotFound);

  server.begin();

  Serial.println();
  Serial.println("Setup portal started");
  Serial.print("Reason: ");
  Serial.println(setupReason);
  Serial.print("AP SSID: ");
  Serial.println(apSsid);

  if (strlen(SETUP_AP_PASSWORD) >= 8) {
    Serial.print("AP password: ");
    Serial.println(SETUP_AP_PASSWORD);
  } else {
    Serial.println("AP password: <open network>");
  }

  Serial.print("Setup URL: http://");
  Serial.print(apIp);
  Serial.println("/");
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

      Serial.println();
      Serial.println("BOOT/IO0 long press detected. Clearing Wi-Fi credentials...");
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

  Serial.println();
  Serial.println("ESP32 Wi-Fi provisioning web server started");

  String ssid;
  String password;

  if (loadWiFiCredentials(ssid, password)) {
    if (connectToWiFi(ssid, password, WIFI_CONNECT_TIMEOUT_MS)) {
      startMainServer();
    } else {
      startSetupPortal("Saved Wi-Fi credentials failed");
    }
  } else {
    startSetupPortal("No saved Wi-Fi credentials");
  }
}

void loop() {
  if (setupMode) {
    dnsServer.processNextRequest();
  }

  server.handleClient();
  checkResetButton();

  static unsigned long lastWiFiCheck = 0;
  static unsigned long disconnectedSince = 0;

  if (!setupMode && millis() - lastWiFiCheck > 5000) {
    lastWiFiCheck = millis();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi disconnected. Trying to reconnect...");

      if (disconnectedSince == 0) {
        disconnectedSince = millis();
      }

      WiFi.reconnect();

      if (millis() - disconnectedSince > 60000) {
        Serial.println("Wi-Fi was disconnected for too long. Restarting...");
        delay(1000);
        ESP.restart();
      }
    } else {
      disconnectedSince = 0;
    }
  }
}
