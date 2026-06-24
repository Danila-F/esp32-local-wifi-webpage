#include <Arduino.h>
#include <WebServer.h>
#include <vector>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>

#include "bluetooth_web.h"

extern WebServer server;
extern bool setupMode;

struct BleDeviceInfo {
  String address;
  String name;
  int rssi;
  String serviceUuid;
};

static const int BLE_SCAN_SECONDS = 5;
static const char DEFAULT_SERVICE_UUID[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char DEFAULT_CHAR_UUID[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";

static bool bleInitialized = false;
static bool bleScanning = false;
static std::vector<BleDeviceInfo> bleDevices;

static BLEClient* bleClient = nullptr;
static BLERemoteCharacteristic* bleCharacteristic = nullptr;

static String bleAddress;
static String bleServiceUuid;
static String bleCharacteristicUuid;
static String bleLastError;
static String bleLastRead;
static String bleLastWrite;

static String htmlEscape(const String& value) {
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

static String pageCss() {
  return R"rawliteral(
    body { font-family: system-ui, sans-serif; margin: 0; padding: 32px; background: #f5f5f5; color: #222; }
    .card { max-width: 880px; margin: 0 auto 18px; padding: 24px; border-radius: 16px; background: white; box-shadow: 0 4px 24px rgba(0,0,0,0.08); }
    .row { display: flex; gap: 12px; flex-wrap: wrap; align-items: center; }
    label { display: block; margin: 14px 0 6px; font-weight: 600; }
    input, textarea { width: 100%; box-sizing: border-box; padding: 12px; border: 1px solid #bbb; border-radius: 10px; font-size: 16px; }
    textarea { min-height: 110px; font-family: ui-monospace, SFMono-Regular, Consolas, monospace; }
    button, .button { display: inline-block; margin-top: 14px; padding: 12px 16px; border: 0; border-radius: 10px; background: #222; color: white; font-size: 16px; text-decoration: none; cursor: pointer; }
    .secondary { background: #eee; color: #222; }
    .danger { background: #8a1f1f; }
    code { background: #eee; padding: 2px 6px; border-radius: 6px; }
    table { width: 100%; border-collapse: collapse; margin-top: 12px; }
    th, td { text-align: left; border-bottom: 1px solid #eee; padding: 8px; vertical-align: top; }
    .muted { color: #666; }
    .warning { color: #9a4b00; }
    .ok { color: #0b6b2b; }
    .err { color: #9a1b1b; }
  )rawliteral";
}

static void ensureBleInitialized() {
  if (bleInitialized) {
    return;
  }

  BLEDevice::init("ESP32-Web-BLE");
  bleInitialized = true;
}

static bool isBleConnected() {
  return bleClient != nullptr && bleClient->isConnected() && bleCharacteristic != nullptr;
}

static void disconnectBle() {
  if (bleClient != nullptr && bleClient->isConnected()) {
    bleClient->disconnect();
  }

  bleCharacteristic = nullptr;
  bleAddress = "";
  bleServiceUuid = "";
  bleCharacteristicUuid = "";
}

class WebBleAdvertisedCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    BleDeviceInfo info;
    info.address = String(advertisedDevice.getAddress().toString().c_str());
    info.name = advertisedDevice.haveName() ? String(advertisedDevice.getName().c_str()) : String("<без имени>");
    info.rssi = advertisedDevice.getRSSI();
    info.serviceUuid = advertisedDevice.haveServiceUUID() ? String(advertisedDevice.getServiceUUID().toString().c_str()) : String("");

    for (const auto& existing : bleDevices) {
      if (existing.address == info.address) {
        return;
      }
    }

    bleDevices.push_back(info);
  }
};

static WebBleAdvertisedCallbacks scanCallbacks;

static void notifyCallback(BLERemoteCharacteristic* characteristic, uint8_t* data, size_t length, bool isNotify) {
  String value;
  value.reserve(length);

  for (size_t i = 0; i < length; i++) {
    char c = (char)data[i];
    if (c >= 32 && c <= 126) {
      value += c;
    } else {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\x%02X", data[i]);
      value += buf;
    }
  }

  bleLastRead = value;
  Serial.print("BLE notify/read data: ");
  Serial.println(bleLastRead);
}

static bool scanBleDevices() {
  ensureBleInitialized();

  bleScanning = true;
  bleLastError = "";
  bleDevices.clear();

  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCallbacks, true);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);

  Serial.println("BLE scan started");
  scan->start(BLE_SCAN_SECONDS, false);
  scan->clearResults();
  Serial.print("BLE scan finished. Devices: ");
  Serial.println((int)bleDevices.size());

  bleScanning = false;
  return true;
}

static bool connectBle(const String& address, const String& serviceUuid, const String& characteristicUuid) {
  ensureBleInitialized();

  bleLastError = "";
  bleLastRead = "";
  bleLastWrite = "";

  if (address.length() == 0 || serviceUuid.length() == 0 || characteristicUuid.length() == 0) {
    bleLastError = "Address, service UUID and characteristic UUID are required";
    return false;
  }

  disconnectBle();

  if (bleClient == nullptr) {
    bleClient = BLEDevice::createClient();
  }

  BLEAddress bleAddr(address.c_str());

  Serial.print("Connecting to BLE device: ");
  Serial.println(address);

  if (!bleClient->connect(bleAddr)) {
    bleLastError = "BLE connection failed";
    disconnectBle();
    return false;
  }

  BLERemoteService* service = bleClient->getService(BLEUUID(serviceUuid.c_str()));
  if (service == nullptr) {
    bleLastError = "Service UUID not found on selected BLE device";
    disconnectBle();
    return false;
  }

  bleCharacteristic = service->getCharacteristic(BLEUUID(characteristicUuid.c_str()));
  if (bleCharacteristic == nullptr) {
    bleLastError = "Characteristic UUID not found in selected service";
    disconnectBle();
    return false;
  }

  if (bleCharacteristic->canNotify()) {
    bleCharacteristic->registerForNotify(notifyCallback);
  }

  bleAddress = address;
  bleServiceUuid = serviceUuid;
  bleCharacteristicUuid = characteristicUuid;

  Serial.println("BLE connected and characteristic selected");
  return true;
}

static bool parseHex(const String& input, std::vector<uint8_t>& out) {
  out.clear();
  String clean;

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (isxdigit((unsigned char)c)) {
      clean += c;
    }
  }

  if (clean.length() == 0 || clean.length() % 2 != 0) {
    return false;
  }

  for (size_t i = 0; i < clean.length(); i += 2) {
    char buf[3] = { clean[i], clean[i + 1], 0 };
    out.push_back((uint8_t)strtoul(buf, nullptr, 16));
  }

  return true;
}

static bool writeBle(const String& data, bool hexMode) {
  if (!isBleConnected()) {
    bleLastError = "No BLE characteristic is connected";
    return false;
  }

  if (!bleCharacteristic->canWrite() && !bleCharacteristic->canWriteNoResponse()) {
    bleLastError = "Selected characteristic is not writable";
    return false;
  }

  if (hexMode) {
    std::vector<uint8_t> bytes;
    if (!parseHex(data, bytes)) {
      bleLastError = "Invalid HEX string";
      return false;
    }

    bleCharacteristic->writeValue(bytes.data(), bytes.size(), true);
    bleLastWrite = "HEX bytes sent: " + String(bytes.size());
  } else {
    bleCharacteristic->writeValue((uint8_t*)data.c_str(), data.length(), true);
    bleLastWrite = data;
  }

  bleLastError = "";
  return true;
}

static bool readBle() {
  if (!isBleConnected()) {
    bleLastError = "No BLE characteristic is connected";
    return false;
  }

  if (!bleCharacteristic->canRead()) {
    bleLastError = "Selected characteristic is not readable";
    return false;
  }

  String value = String(bleCharacteristic->readValue().c_str());
  bleLastRead = value;
  bleLastError = "";
  return true;
}

static String makeBluetoothPage() {
  String html = R"rawliteral(
<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Bluetooth Console</title>
  <style>
)rawliteral";

  html += pageCss();

  html += R"rawliteral(
  </style>
</head>
<body>
  <div class="card">
    <h1>Bluetooth / BLE console</h1>
    <p class="warning">
      Эта страница работает с BLE GATT-устройствами. Для отправки данных нужно знать Service UUID и Characteristic UUID.
      Обычные Bluetooth-аудиоустройства, клавиатуры, мыши и произвольные Classic Bluetooth устройства так управляться не будут.
    </p>
    <p class="muted">
      Примеры UUID: Nordic UART Service — service <code>6e400001-b5a3-f393-e0a9-e50e24dcca9e</code>, write characteristic <code>6e400002-b5a3-f393-e0a9-e50e24dcca9e</code>.
      HM-10-подобные BLE UART часто используют service <code>0000ffe0-0000-1000-8000-00805f9b34fb</code>, characteristic <code>0000ffe1-0000-1000-8000-00805f9b34fb</code>.
    </p>
    <div class="row">
      <form method="GET" action="/bt-scan"><button type="submit">Сканировать BLE устройства</button></form>
      <form method="POST" action="/bt-disconnect"><button class="danger" type="submit">Отключиться</button></form>
      <a class="button secondary" href="/">Назад</a>
    </div>
  </div>
)rawliteral";

  html += "<div class=\"card\"><h2>Статус</h2>";
  html += "<p><b>BLE:</b> ";
  html += bleInitialized ? "<code>initialized</code>" : "<code>not initialized</code>";
  html += "</p>";

  html += "<p><b>Connection:</b> ";
  html += isBleConnected() ? "<span class=\"ok\">connected</span>" : "<span class=\"muted\">not connected</span>";
  html += "</p>";

  if (isBleConnected()) {
    html += "<p><b>Address:</b> <code>" + htmlEscape(bleAddress) + "</code></p>";
    html += "<p><b>Service UUID:</b> <code>" + htmlEscape(bleServiceUuid) + "</code></p>";
    html += "<p><b>Characteristic UUID:</b> <code>" + htmlEscape(bleCharacteristicUuid) + "</code></p>";
  }

  if (bleLastError.length() > 0) {
    html += "<p class=\"err\"><b>Last error:</b> " + htmlEscape(bleLastError) + "</p>";
  }

  if (bleLastWrite.length() > 0) {
    html += "<p><b>Last write:</b> <code>" + htmlEscape(bleLastWrite) + "</code></p>";
  }

  if (bleLastRead.length() > 0) {
    html += "<p><b>Last read/notify:</b> <code>" + htmlEscape(bleLastRead) + "</code></p>";
  }

  html += "</div>";

  if (isBleConnected()) {
    html += R"rawliteral(
  <div class="card">
    <h2>Отправка данных</h2>
    <form method="POST" action="/bt-send">
      <label for="data">Данные</label>
      <textarea id="data" name="data" placeholder="Текст или HEX-строка"></textarea>
      <label><input style="width:auto" type="checkbox" name="hex" value="1"> Отправить как HEX, например <code>48656C6C6F0A</code></label>
      <button type="submit">Отправить</button>
    </form>
    <p><a class="button secondary" href="/bt-read">Прочитать characteristic</a></p>
  </div>
)rawliteral";
  }

  html += "<div class=\"card\"><h2>Найденные устройства</h2>";

  if (bleScanning) {
    html += "<p>Идёт сканирование...</p>";
  } else if (bleDevices.empty()) {
    html += "<p class=\"muted\">Список пуст. Нажми кнопку сканирования.</p>";
  } else {
    html += "<table><tr><th>Device</th><th>RSSI</th><th>Advertised service</th><th>Connect</th></tr>";

    for (const auto& device : bleDevices) {
      html += "<tr><td><b>" + htmlEscape(device.name) + "</b><br><code>" + htmlEscape(device.address) + "</code></td>";
      html += "<td>" + String(device.rssi) + " dBm</td>";
      html += "<td><code>" + htmlEscape(device.serviceUuid) + "</code></td>";
      html += "<td>";
      html += "<form method=\"POST\" action=\"/bt-connect\">";
      html += "<input type=\"hidden\" name=\"addr\" value=\"" + htmlEscape(device.address) + "\">";
      html += "<label>Service UUID</label><input name=\"service\" value=\"" + String(DEFAULT_SERVICE_UUID) + "\">";
      html += "<label>Characteristic UUID</label><input name=\"char\" value=\"" + String(DEFAULT_CHAR_UUID) + "\">";
      html += "<button type=\"submit\">Подключиться</button>";
      html += "</form>";
      html += "</td></tr>";
    }

    html += "</table>";
  }

  html += R"rawliteral(
  </div>
</body>
</html>
)rawliteral";

  return html;
}

static void handleBluetoothPage() {
  if (setupMode) {
    server.send(403, "text/plain; charset=utf-8", "Bluetooth console is disabled in setup mode");
    return;
  }

  server.send(200, "text/html; charset=utf-8", makeBluetoothPage());
}

static void handleBluetoothScan() {
  if (setupMode) {
    server.send(403, "text/plain; charset=utf-8", "Bluetooth scan is disabled in setup mode");
    return;
  }

  scanBleDevices();
  server.send(200, "text/html; charset=utf-8", makeBluetoothPage());
}

static void handleBluetoothConnect() {
  if (setupMode) {
    server.send(403, "text/plain; charset=utf-8", "Bluetooth connect is disabled in setup mode");
    return;
  }

  String address = server.arg("addr");
  String serviceUuid = server.arg("service");
  String characteristicUuid = server.arg("char");

  address.trim();
  serviceUuid.trim();
  characteristicUuid.trim();

  connectBle(address, serviceUuid, characteristicUuid);
  server.send(200, "text/html; charset=utf-8", makeBluetoothPage());
}

static void handleBluetoothDisconnect() {
  disconnectBle();
  bleLastError = "";
  bleLastRead = "";
  bleLastWrite = "";
  server.send(200, "text/html; charset=utf-8", makeBluetoothPage());
}

static void handleBluetoothSend() {
  String data = server.arg("data");
  bool hexMode = server.hasArg("hex");

  writeBle(data, hexMode);
  server.send(200, "text/html; charset=utf-8", makeBluetoothPage());
}

static void handleBluetoothRead() {
  readBle();
  server.send(200, "text/html; charset=utf-8", makeBluetoothPage());
}

void registerBluetoothRoutes() {
  server.on("/bluetooth", HTTP_GET, handleBluetoothPage);
  server.on("/bt-scan", HTTP_GET, handleBluetoothScan);
  server.on("/bt-connect", HTTP_POST, handleBluetoothConnect);
  server.on("/bt-disconnect", HTTP_POST, handleBluetoothDisconnect);
  server.on("/bt-send", HTTP_POST, handleBluetoothSend);
  server.on("/bt-read", HTTP_GET, handleBluetoothRead);
}
