#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

#if __has_include("wifi_secrets.h")
  #include "wifi_secrets.h"
#else
  #error "Missing src/wifi_secrets.h. GitHub Actions generates it from WIFI_SSID and WIFI_PASSWORD repository secrets."
#endif

#define LED_PIN 2

WebServer server(80);

String makeHtmlPage() {
  String ip = WiFi.localIP().toString();
  String rssi = String(WiFi.RSSI());

  String html = R"rawliteral(
<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Web Server</title>
  <style>
    body {
      font-family: system-ui, sans-serif;
      margin: 40px;
      background: #f5f5f5;
      color: #222;
    }
    .card {
      max-width: 680px;
      padding: 24px;
      border-radius: 16px;
      background: #fff;
      box-shadow: 0 4px 24px rgba(0,0,0,0.08);
    }
    h1 { margin-top: 0; }
    code {
      background: #eee;
      padding: 2px 6px;
      border-radius: 6px;
    }
  </style>
</head>
<body>
  <div class="card">
    <h1>ESP32 работает</h1>
    <p>Эта HTML-страница отдана HTTP-сервером, запущенным на ESP32.</p>
    <p>Wi-Fi SSID и пароль были переданы в прошивку через GitHub Actions Secrets.</p>
)rawliteral";

  html += "<p><b>IP ESP32:</b> <code>" + ip + "</code></p>";
  html += "<p><b>Wi-Fi RSSI:</b> <code>" + rssi + " dBm</code></p>";

  html += R"rawliteral(
    <p>Проверочный JSON endpoint: <a href="/status">/status</a></p>
  </div>
</body>
</html>
)rawliteral";

  return html;
}

void handleRoot() {
  digitalWrite(LED_PIN, HIGH);
  server.send(200, "text/html; charset=utf-8", makeHtmlPage());
  delay(50);
  digitalWrite(LED_PIN, LOW);
}

void handleStatus() {
  String json = "{";
  json += "\"status\":\"ok\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime_ms\":" + String(millis());
  json += "}";

  server.send(200, "application/json; charset=utf-8", json);
}

void handleNotFound() {
  server.send(404, "text/plain; charset=utf-8", "404 Not Found");
}

void connectToWiFi() {
  Serial.println();
  Serial.println("Connecting to Wi-Fi...");
  Serial.print("SSID: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-web-test");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(500);
    Serial.print(".");

    if (millis() - startTime > 30000) {
      Serial.println();
      Serial.println("Wi-Fi connection timeout. Restarting...");
      delay(1000);
      ESP.restart();
    }
  }

  digitalWrite(LED_PIN, LOW);

  Serial.println();
  Serial.println("Wi-Fi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println();
  Serial.println("ESP32 Wi-Fi Web Server test started");

  connectToWiFi();

  if (MDNS.begin("esp32-test")) {
    Serial.println("mDNS started: http://esp32-test.local/");
  } else {
    Serial.println("mDNS failed");
  }

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.onNotFound(handleNotFound);

  server.begin();

  Serial.println("HTTP server started");
  Serial.print("Open in browser: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
}

void loop() {
  server.handleClient();

  static unsigned long lastWiFiCheck = 0;

  if (millis() - lastWiFiCheck > 5000) {
    lastWiFiCheck = millis();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi disconnected. Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
}
