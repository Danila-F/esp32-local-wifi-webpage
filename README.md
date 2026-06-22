# ESP32 GPIO2 Blink Firmware

Проект для сборки тестовой прошивки под плату NodeMCU-32S / ESP-32S / ESP32-WROOM-32 через GitHub Actions.

Прошивка мигает светодиодом на GPIO2 и пишет диагностические сообщения в UART Serial Monitor на скорости 115200.

## Что делает прошивка

- настраивает GPIO2 как выход;
- мигает: 500 мс ON, 500 мс OFF;
- пишет в Serial Monitor:
  - `ESP32 GPIO2 blink firmware started`
  - `blink: ON`
  - `blink: OFF`

## Как собрать через GitHub Actions

1. Создай новый репозиторий на GitHub.
2. Загрузи в него все файлы из этого архива.
3. Открой вкладку **Actions**.
4. Выбери workflow **Build ESP32 firmware**.
5. Нажми **Run workflow**.
6. Дождись успешного завершения.
7. Внизу страницы run скачай artifact **esp32-gpio2-blink-firmware**.

## Что будет в artifact

Обычно внутри будут:

- `firmware.bin`
- `bootloader.bin`
- `partitions.bin`
- `boot_app0.bin`
- `merged-flash.bin`
- `FLASH_ADDRESSES.txt`
- `SHA256SUMS.txt`

## Как прошивать через Android flasher

Сначала попробуй один файл:

```text
firmware.bin -> 0x10000
```

Если прошивка не запускается, прошей полный набор:

```text
bootloader.bin -> 0x1000
partitions.bin -> 0x8000
boot_app0.bin  -> 0xe000
firmware.bin   -> 0x10000
```

Если приложение поддерживает merged image, можно прошить один файл:

```text
merged-flash.bin -> 0x0
```

## Если светодиод не мигает

Открой Serial Monitor на 115200 и нажми EN. Если сообщения `blink: ON/OFF` идут, прошивка работает, но светодиод на конкретной плате может быть подключён не к GPIO2 или иметь другую обвязку.

Для active-low светодиода измени в `platformio.ini`:

```ini
-D LED_ACTIVE_LOW=1
```

и запусти Actions повторно.
