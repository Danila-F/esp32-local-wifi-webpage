# ESP32 Wi-Fi Web Server

ESP32 NodeMCU-32S / ESP32-WROOM-32 test firmware.

GitHub Actions generates `src/wifi_secrets.h` from repository secrets:

- `WIFI_SSID`
- `WIFI_PASSWORD`

The firmware connects to Wi-Fi, starts an HTTP server on port 80, and uploads firmware binaries as a GitHub Actions artifact.

Flash:

```text
merged-flash.bin -> 0x0
```

Serial Monitor:

```text
115200
```
