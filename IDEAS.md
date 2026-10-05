# Ideas

Things to try with the board later. Suggested order: mDNS, then OTA.

## Small additions to this dashboard

- [ ] **mDNS:** open `http://esp32.local/` instead of the IP. `idf.py add-dependency espressif/mdns`, about 5 lines of code.
- [ ] **OTA updates:** upload new firmware from the dashboard, no USB cable. Uses `esp_https_ota`; needs two app partitions (16 MB flash has room).
- [ ] **Wi-Fi setup from the browser:** if the board can't join a network, start its own access point with a page to enter SSID and password, saved to NVS instead of `menuconfig`.
- [ ] **Real sensor:** BME280 or SHT31 (temperature, humidity, pressure) over I2C, one more field in the telemetry JSON.
- [ ] **Touch buttons:** the S3 has 14 capacitive touch pins. A wire or coin could toggle something and show up on the dashboard.

## Chip features not used yet

- **Native USB:** act as a keyboard, mouse or MIDI device (TinyUSB). For example, a macro pad driven from the dashboard.
- **Bluetooth LE 5:** read nearby BLE sensors (Xiaomi thermometers are common) or control the board from a phone without Wi-Fi.
- **I2S audio:** INMP441 mic to stream audio levels, or a MAX98357 amp to play sounds.
- **Camera:** boards like the ESP32-S3-EYE or XIAO S3 Sense can stream MJPEG video to a browser.
- **AI acceleration:** ESP-SR for offline wake words and voice commands, ESP-DL for small vision models like face detection.
- **Deep sleep:** microamps while asleep. Run on a battery, wake every few minutes, send a reading, sleep again.
- **CAN bus (TWAI):** with a cheap transceiver, read car or e-bike data.

## Bigger projects

- **Home sensor hub:** several sensors, MQTT to Home Assistant, keep this dashboard as the local view.
- **Wi-Fi/BLE scanner:** build on the existing scan command. Log results over time and chart signal strength per network.
- **Voice-controlled LED:** ESP-SR commands ("turn red") driving the RGB LED that's already wired up.
- **Small display:** SPI LCD or e-paper showing the same telemetry, so the board works without a browser.
