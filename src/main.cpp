#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <ctype.h>

#define LED_PIN 2
#define RESET_BUTTON_PIN 0

static const byte DNS_PORT = 53;

static const char SETUP_AP_PASSWORD[] = "esp32setup";

static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;
static const unsigned long RESET_BUTTON_HOLD_MS = 5000;

// UART connection for the NEC-compatible IR UART module.
// Wiring:
//   ESP32 GPIO17 / TX2 -> IR module RXD
//   ESP32 GPIO16 / RX2 <- IR module TXD through a level shifter or divider if the module is powered from 5V
//   ESP32 GND          -> IR module GND
//   ESP32 5V or 3V3    -> IR module VCC, depending on the hardware variant and level matching
static const int IR_RX_PIN = 16;
static const int IR_TX_PIN = 17;
static const unsigned long IR_BAUD_RATE = 9600;
static const uint8_t IR_MODULE_ADDRESS = 0xA1;
static const uint8_t IR_SEND_COMMAND = 0xF1;
static const unsigned long IR_ACK_TIMEOUT_MS = 500;
static const unsigned long IR_RX_FRAME_GAP_MS = 100;

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;
HardwareSerial IrSerial(2);

bool setupMode = false;
String setupReason;
bool otaUpdateOk = false;

unsigned long resetButtonPressedAt = 0;
bool resetButtonHandled = false;

uint8_t irPendingBytes[3] = {0, 0, 0};
uint8_t irPendingIndex = 0;
unsigned long irLastByteAt = 0;

uint8_t irLastCode[3] = {0, 0, 0};
bool irLastCodeValid = false;
unsigned long irLastCodeAt = 0;
unsigned long irReceivedFrameCount = 0;

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

String byteToHex(uint8_t value) {
  char buffer[3];
  snprintf(buffer, sizeof(buffer), "%02X", value);
  return String(buffer);
}

String makeIrCodeString(const uint8_t* data, size_t length) {
  String result;

  for (size_t i = 0; i < length; i++) {
    if (i > 0) {
      result += " ";
    }

    result += byteToHex(data[i]);
  }

  return result;
}

bool parseHexByte(String value, uint8_t& output) {
  value.trim();
  value.toUpperCase();

  if (value.startsWith("0X")) {
    value = value.substring(2);
  }

  if (value.length() == 0 || value.length() > 2) {
    return false;
  }

  for (size_t i = 0; i < value.length(); i++) {
    if (!isxdigit((unsigned char)value[i])) {
      return false;
    }
  }

  output = (uint8_t)strtoul(value.c_str(), nullptr, 16);
  return true;
}

void rememberIrCode(const uint8_t* data) {
  memcpy(irLastCode, data, sizeof(irLastCode));
  irLastCodeValid = true;
  irLastCodeAt = millis();
  irReceivedFrameCount++;

  Serial.print("IR received: ");
  Serial.print(makeIrCodeString(irLastCode, 3));
  Serial.print(" ");
  Serial.println(byteToHex((uint8_t)~irLastCode[2]));
}

void processIrByte(uint8_t value) {
  unsigned long now = millis();

  if (irPendingIndex > 0 && now - irLastByteAt > IR_RX_FRAME_GAP_MS) {
    irPendingIndex = 0;
  }

  irPendingBytes[irPendingIndex++] = value;
  irLastByteAt = now;

  if (irPendingIndex >= sizeof(irPendingBytes)) {
    rememberIrCode(irPendingBytes);
    irPendingIndex = 0;
  }
}

void processIrSerial() {
  while (IrSerial.available()) {
    uint8_t value = (uint8_t)IrSerial.read();
    processIrByte(value);
  }
}

void clearIrState() {
  irPendingIndex = 0;
  irLastCodeValid = false;
  irLastCodeAt = 0;
  irReceivedFrameCount = 0;

  while (IrSerial.available()) {
    IrSerial.read();
  }
}

bool sendIrNecCommand(uint8_t data1, uint8_t data2, uint8_t data3, bool& ackReceived, uint8_t& firstResponseByte) {
  uint8_t packet[5] = {
    IR_MODULE_ADDRESS,
    IR_SEND_COMMAND,
    data1,
    data2,
    data3
  };

  processIrSerial();
  irPendingIndex = 0;

  ackReceived = false;
  firstResponseByte = 0;

  IrSerial.write(packet, sizeof(packet));
  IrSerial.flush();

  unsigned long start = millis();

  while (millis() - start < IR_ACK_TIMEOUT_MS) {
    if (IrSerial.available()) {
      uint8_t value = (uint8_t)IrSerial.read();

      if (firstResponseByte == 0) {
        firstResponseByte = value;
      }

      if (value == IR_SEND_COMMAND) {
        ackReceived = true;
        return true;
      }

      processIrByte(value);
    }

    delay(1);
  }

  return false;
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
    .warning {
      color: #9a4b00;
    }
    .success {
      color: #206a20;
    }
    .danger {
      color: #8a1f11;
    }
    .row {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 12px;
    }
    @media (max-width: 600px) {
      .row {
        grid-template-columns: 1fr;
      }
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
  html += "<p><b>IR UART:</b> <code>RX2 GPIO" + String(IR_RX_PIN) + ", TX2 GPIO" + String(IR_TX_PIN) + ", " + String(IR_BAUD_RATE) + " baud</code></p>";

  if (irLastCodeValid) {
    html += "<p><b>Последний принятый ИК-код:</b> <code>";
    html += makeIrCodeString(irLastCode, 3);
    html += " ";
    html += byteToHex((uint8_t)~irLastCode[2]);
    html += "</code></p>";
  } else {
    html += "<p><b>Последний принятый ИК-код:</b> <span class=\"muted\">ещё не получен</span></p>";
  }

  html += R"rawliteral(
    <p><a class="button" href="/ir">Управление ИК-модулем</a></p>
    <p><a class="button secondary" href="/status">JSON статус</a></p>
    <p><a class="button secondary" href="/ota">OTA обновление прошивки</a></p>
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

String makeIrPage(const String& message = "", bool success = true) {
  String html = R"rawliteral(
<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 IR UART</title>
  <style>
)rawliteral";

  html += css();

  html += R"rawliteral(
  </style>
</head>
<body>
  <div class="card">
    <h1>Управление ИК UART-модулем</h1>
)rawliteral";

  if (message.length() > 0) {
    html += "<p class=\"";
    html += success ? "success" : "danger";
    html += "\">";
    html += htmlEscape(message);
    html += "</p>";
  }

  html += "<p><b>Подключение:</b> <code>GPIO";
  html += String(IR_TX_PIN);
  html += " / TX2 → RXD модуля</code>, <code>GPIO";
  html += String(IR_RX_PIN);
  html += " / RX2 ← TXD модуля</code>, скорость <code>";
  html += String(IR_BAUD_RATE);
  html += " 8N1</code>.</p>";

  html += R"rawliteral(
    <p class="warning">
      Если модуль питается от 5V и на его TXD около 4.5–5V, подключай TXD модуля к RX2 ESP32 только через level shifter или делитель.
      При питании модуля от 3V3 сначала измерь уровни TXD/RXD мультиметром.
    </p>

    <h2>Отправить NEC-команду</h2>
    <p class="muted">
      Для YS-IRTM-подобного модуля отправляется UART-пакет <code>A1 F1 data1 data2 data3</code>.
      В ИК обычно уходит <code>data1 data2 data3 ~data3</code>.
    </p>

    <form method="POST" action="/ir/send">
      <div class="row">
        <div>
          <label for="data1">data1, hex</label>
          <input id="data1" name="data1" value="00" required maxlength="4" autocomplete="off">
        </div>
        <div>
          <label for="data2">data2, hex</label>
          <input id="data2" name="data2" value="FF" required maxlength="4" autocomplete="off">
        </div>
        <div>
          <label for="data3">data3, hex</label>
          <input id="data3" name="data3" value="45" required maxlength="4" autocomplete="off">
        </div>
      </div>

      <button type="submit">Отправить ИК-команду</button>
    </form>

    <h2>Чтение</h2>
)rawliteral";

  if (irLastCodeValid) {
    html += "<p><b>Последний принятый код:</b> <code>";
    html += makeIrCodeString(irLastCode, 3);
    html += " ";
    html += byteToHex((uint8_t)~irLastCode[2]);
    html += "</code></p>";
    html += "<p><b>Первые 3 байта для повторной отправки:</b> <code>";
    html += makeIrCodeString(irLastCode, 3);
    html += "</code></p>";
    html += "<p><b>Получено кадров после запуска/очистки:</b> <code>";
    html += String(irReceivedFrameCount);
    html += "</code></p>";
    html += "<p><b>Возраст последнего кода:</b> <code>";
    html += String((millis() - irLastCodeAt) / 1000);
    html += " s</code></p>";
  } else {
    html += "<p class=\"muted\">Пока не принято ни одного ИК-кода.</p>";
  }

  html += R"rawliteral(
    <p>
      <a class="button secondary" href="/ir">Обновить чтение</a>
      <a class="button secondary" href="/ir/read">JSON чтение</a>
    </p>

    <form method="POST" action="/ir/clear">
      <button class="secondary" type="submit">Очистить принятый код</button>
    </form>

    <p><a class="button secondary" href="/">Назад</a></p>
  </div>
</body>
</html>
)rawliteral";

  return html;
}

String makeOtaPage() {
  String html = R"rawliteral(
<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 OTA Update</title>
  <style>
)rawliteral";

  html += css();

  html += R"rawliteral(
  </style>
</head>
<body>
  <div class="card">
    <h1>OTA обновление ESP32</h1>

    <p>Выбери файл <code>firmware.bin</code>, собранный GitHub Actions.</p>

    <p class="warning">
      Не загружай сюда <code>merged-flash.bin</code>. Он предназначен для полной прошивки через USB/UART по адресу <code>0x0</code>.
    </p>

    <form method="POST" action="/update" enctype="multipart/form-data">
      <label for="firmware">Файл прошивки</label>
      <input id="firmware" name="firmware" type="file" accept=".bin" required>

      <button type="submit">Загрузить и установить</button>
    </form>

    <p><a class="button secondary" href="/">Назад</a></p>
  </div>
</body>
</html>
)rawliteral";

  return html;
}

String makeIrStatusJson() {
  String json = "{";
  json += "\"uart\":\"Serial2\",";
  json += "\"baud\":" + String(IR_BAUD_RATE) + ",";
  json += "\"rx_pin\":" + String(IR_RX_PIN) + ",";
  json += "\"tx_pin\":" + String(IR_TX_PIN) + ",";
  json += "\"module_address\":\"" + byteToHex(IR_MODULE_ADDRESS) + "\",";
  json += "\"send_command\":\"" + byteToHex(IR_SEND_COMMAND) + "\",";
  json += "\"received_frame_count\":" + String(irReceivedFrameCount) + ",";
  json += "\"has_last_code\":";
  json += irLastCodeValid ? "true" : "false";
  json += ",";
  json += "\"last_code_3_bytes\":";

  if (irLastCodeValid) {
    json += "\"";
    json += makeIrCodeString(irLastCode, 3);
    json += "\"";
  } else {
    json += "null";
  }

  json += ",";
  json += "\"last_code_full_nec\":";

  if (irLastCodeValid) {
    json += "\"";
    json += makeIrCodeString(irLastCode, 3);
    json += " ";
    json += byteToHex((uint8_t)~irLastCode[2]);
    json += "\"";
  } else {
    json += "null";
  }

  json += ",";
  json += "\"last_code_age_ms\":";
  json += irLastCodeValid ? String(millis() - irLastCodeAt) : "null";
  json += "}";

  return json;
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

void handleIrPage() {
  processIrSerial();
  server.send(200, "text/html; charset=utf-8", makeIrPage());
}

void handleIrSend() {
  uint8_t data1;
  uint8_t data2;
  uint8_t data3;

  if (!server.hasArg("data1") || !server.hasArg("data2") || !server.hasArg("data3")) {
    server.send(400, "text/html; charset=utf-8", makeIrPage("Не переданы параметры data1, data2, data3.", false));
    return;
  }

  if (!parseHexByte(server.arg("data1"), data1) ||
      !parseHexByte(server.arg("data2"), data2) ||
      !parseHexByte(server.arg("data3"), data3)) {
    server.send(400, "text/html; charset=utf-8", makeIrPage("Каждый байт должен быть hex-значением от 00 до FF.", false));
    return;
  }

  bool ackReceived = false;
  uint8_t firstResponseByte = 0;
  bool ok = sendIrNecCommand(data1, data2, data3, ackReceived, firstResponseByte);

  Serial.print("IR send requested: ");
  Serial.print(byteToHex(data1));
  Serial.print(" ");
  Serial.print(byteToHex(data2));
  Serial.print(" ");
  Serial.println(byteToHex(data3));

  String message = "Отправлен UART-пакет A1 F1 ";
  uint8_t data[3] = {data1, data2, data3};
  message += makeIrCodeString(data, 3);
  message += ". ИК-код должен быть ";
  message += makeIrCodeString(data, 3);
  message += " ";
  message += byteToHex((uint8_t)~data3);
  message += ". ";

  if (ok && ackReceived) {
    message += "Модуль ответил ACK F1.";
  } else if (firstResponseByte != 0) {
    message += "ACK F1 не получен, первый ответ модуля: ";
    message += byteToHex(firstResponseByte);
    message += ".";
  } else {
    message += "ACK F1 не получен за время ожидания. Передача могла всё равно сработать, если линия TXD модуля не подключена к RX2 ESP32.";
  }

  server.send(200, "text/html; charset=utf-8", makeIrPage(message, ok));
}

void handleIrClear() {
  clearIrState();
  server.send(200, "text/html; charset=utf-8", makeIrPage("Принятый ИК-код очищен.", true));
}

void handleIrReadJson() {
  processIrSerial();
  server.send(200, "application/json; charset=utf-8", makeIrStatusJson());
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

    Serial.println();
    Serial.print("OTA update started. File: ");
    Serial.println(upload.filename);

    digitalWrite(LED_PIN, HIGH);

    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      Serial.println("Update.begin() failed");
      Update.printError(Serial);
    }
  }

  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!Update.hasError()) {
      size_t written = Update.write(upload.buf, upload.currentSize);

      if (written != upload.currentSize) {
        Serial.println("Update.write() failed");
        Update.printError(Serial);
      }
    }
  }

  else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      otaUpdateOk = true;

      Serial.println();
      Serial.print("OTA update finished. Size: ");
      Serial.print(upload.totalSize);
      Serial.println(" bytes");
    } else {
      otaUpdateOk = false;

      Serial.println();
      Serial.println("Update.end() failed");
      Update.printError(Serial);
    }

    digitalWrite(LED_PIN, LOW);
  }

  else if (upload.status == UPLOAD_FILE_ABORTED) {
    otaUpdateOk = false;
    Update.abort();

    Serial.println();
    Serial.println("OTA update aborted");

    digitalWrite(LED_PIN, LOW);
  }
}

void handleOtaFinished() {
  server.sendHeader("Connection", "close");

  if (otaUpdateOk) {
    server.send(
      200,
      "text/html; charset=utf-8",
      "<!doctype html><html lang=\"ru\"><meta charset=\"utf-8\">"
      "<h1>OTA обновление успешно</h1>"
      "<p>ESP32 перезагрузится через несколько секунд.</p>"
      "</html>"
    );

    Serial.println("Restarting after OTA update...");
    delay(1500);
    ESP.restart();
  } else {
    server.send(
      500,
      "text/html; charset=utf-8",
      "<!doctype html><html lang=\"ru\"><meta charset=\"utf-8\">"
      "<h1>OTA обновление не удалось</h1>"
      "<p>Проверь Serial Monitor для подробностей.</p>"
      "<p>Убедись, что загружаешь именно firmware.bin, а не merged-flash.bin.</p>"
      "<p><a href=\"/ota\">Назад</a></p>"
      "</html>"
    );
  }
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
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"sketch_size\":" + String(ESP.getSketchSize()) + ",";
  json += "\"free_sketch_space\":" + String(ESP.getFreeSketchSpace()) + ",";
  json += "\"ir\":";
  json += makeIrStatusJson();
  json += ",";
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
  server.on("/ir", HTTP_GET, handleIrPage);
  server.on("/ir/send", HTTP_POST, handleIrSend);
  server.on("/ir/clear", HTTP_POST, handleIrClear);
  server.on("/ir/read", HTTP_GET, handleIrReadJson);
  server.on("/ota", HTTP_GET, handleOtaPage);
  server.on("/update", HTTP_POST, handleOtaFinished, handleOtaUpload);
  server.on("/reset-wifi", HTTP_GET, handleResetWiFi);
  server.onNotFound(handleNotFound);

  server.begin();

  Serial.println("HTTP server started");
  Serial.print("Open in browser: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
  Serial.print("IR page: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/ir");
  Serial.print("OTA page: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/ota");
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

  IrSerial.begin(IR_BAUD_RATE, SERIAL_8N1, IR_RX_PIN, IR_TX_PIN);

  Serial.println();
  Serial.println("ESP32 Wi-Fi provisioning web server started");
  Serial.print("IR UART initialized on RX2 GPIO");
  Serial.print(IR_RX_PIN);
  Serial.print(", TX2 GPIO");
  Serial.print(IR_TX_PIN);
  Serial.print(", baud ");
  Serial.println(IR_BAUD_RATE);

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
  processIrSerial();

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
