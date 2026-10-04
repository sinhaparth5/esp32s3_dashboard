# ESP32-S3 Dashboard — Roadmap

ESP32-S3 serves live telemetry over WebSocket to a React dashboard.
Each phase ends with something you can see working. Don't start the next phase until the current "Done when" passes.

```
esp32s3_dashboard/
├── ROADMAP.md
├── firmware/          ESP-IDF app (C)
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── main/
│       ├── CMakeLists.txt
│       ├── Kconfig.projbuild   Wi-Fi SSID/password (set in menuconfig)
│       └── main.c
└── frontend/          React + Vite; `npm run build` output is embedded in the firmware
```

---

## Using one ESP-IDF for many projects

`~/esp/esp-idf` is an SDK + toolchain, not a project. Projects can live anywhere; nothing gets written into the IDF folder.
Each project has its own `build/` and `sdkconfig`, so projects never step on each other.

```bash
# once: add to ~/.bashrc
alias get_idf='. $HOME/esp/esp-idf/export.sh'

# every new terminal you want to build in:
get_idf
cd ~/Documents/esp32s3/esp32s3_dashboard/firmware
idf.py build
```

Rules:
- Never edit files inside `~/esp/esp-idf`. To change an IDF component, copy it into `firmware/components/<name>/` and the project copy wins.
- Extra libraries (mDNS, LittleFS, ...) go in via `idf.py add-dependency`, which writes to *this* project's `main/idf_component.yml` and `managed_components/`.
- Don't `git pull` / switch branches in `~/esp/esp-idf` without thinking about your other projects. If one ever needs a different IDF version, clone a second copy (e.g. `~/esp/esp-idf-v5.4`) and source that one's `export.sh` instead.

---

## Phase 0 — Environment ✅
- [x] ESP-IDF v5.3 installed at `~/esp/esp-idf` (shared)
- [x] Node 24 for the frontend
- [ ] Add `get_idf` alias to `~/.bashrc`
- [ ] Find your serial port: plug in the board, `ls /dev/ttyACM* /dev/ttyUSB*`
- [ ] If permission denied on the port: `sudo usermod -aG dialout $USER`, then log out/in

**Done when:** `get_idf && idf.py --version` works in a fresh terminal.

## Phase 1 — Wi-Fi connect (firmware) ✅
- [x] Project skeleton, target pinned to esp32s3 via `sdkconfig.defaults`
- [x] Wi-Fi station mode with auto-reconnect
- [x] SSID/password via `idf.py menuconfig` → "Dashboard Wi-Fi" (stays in `sdkconfig`, not in code)
- [x] Flash and see the IP in the monitor

Gotcha: iPhone hotspot names use a curly apostrophe `’`, not `'`. Disconnect reason 201 = SSID not found.

```bash
cd firmware
idf.py menuconfig             # Dashboard Wi-Fi → set SSID + password
idf.py -p /dev/ttyACM0 flash monitor   # Ctrl+] to exit monitor
```

**Done when:** monitor prints `Got IP: 192.168.x.x` and the board answers `ping 192.168.x.x`.

## Phase 2 — HTTP + WebSocket telemetry (firmware) ✅
- [x] Enable `CONFIG_HTTPD_WS_SUPPORT`, start `esp_http_server` on port 80
- [x] `/ws` endpoint, track connected client fds
- [x] Telemetry task, 1 Hz: die temp (`driver/temperature_sensor.h`), free heap, min free heap, uptime, RSSI
- [x] Broadcast JSON: `{"temp":38.5,"heap":245120,"heap_min":230000,"heap_total":330000,"uptime":120,"rssi":-52}`
- [x] Status LED: red = no Wi-Fi, green = connected, blue flash = telemetry sent (pin in menuconfig, default GPIO 48)

**Done when:** in any browser devtools console:
`new WebSocket("ws://<ESP_IP>/ws").onmessage = e => console.log(e.data)` prints one JSON line per second.
Or from a terminal (Node 24 has WebSocket built in):
`node -e 'new WebSocket("ws://<ESP_IP>/ws").onmessage = e => console.log(e.data)'`
The laptop must be on the same network as the ESP.

## Phase 3 — React dashboard, dev mode (frontend) ✅ verified live (reflash for memory bar)
- [x] `npm create vite@latest frontend -- --template react`
- [x] WebSocket hook with auto-reconnect (retry every 2 s; 3 s silence = dead link, since a pulled plug never sends a close)
- [x] ESP IP from `?ip=` query param / input box (no rebuild to change it)
- [x] Cards: temperature (blue → green → orange → red), heap bar, uptime, RSSI, connection status
- [x] Rolling sparklines (last 60 samples), plain SVG, no chart lib
- [x] Firmware sends `heap_total` for the memory bar

**Done when:** `npm run dev`, open `http://localhost:5173/?ip=<ESP_IP>`, numbers update live; unplugging the board shows "offline", replugging recovers on its own.

## Phase 4 — Commands dashboard → chip ✅ code written
- [x] Client sends JSON over the same socket: `{"cmd":"ping","t":…}`, `{"cmd":"scan"}`, `{"cmd":"led","rgb":"#ff8800"}` / `"rgb":null`
- [x] Firmware (cJSON): `ping` → `pong` echoing `t`; `scan` is non-blocking, results broadcast on `SCAN_DONE`; `led` overrides the status LED until "auto"
- [x] Controls card: Ping (round-trip ms), Scan Wi-Fi (table), native colour picker + LED auto

**Done when:** clicking each button produces a visible effect / result.

Gotcha: the system event task has a tiny stack (2304 B default). Big arrays there = `Stack canary watchpoint triggered (sys_evt)` reboot. Raised to 4096 and scan records go on the heap.

## Phase 5 — Serve the dashboard from the chip ✅ verified live
Went with `EMBED_FILES` instead of SPIFFS: no filesystem, no mount code, no extra partition. Gzipped dashboard is ~71 KB inside the app binary.
- [x] Flash size 16 MB + IDF's built-in "single app large" partition table (1.5 MB app, 38% free)
- [x] Vite emits fixed names (`assets/app.js`, `assets/app.css`); `npm run build` also gzips them
- [x] `firmware/main/CMakeLists.txt` embeds `frontend/dist/*.gz`; one handler serves them with `Content-Encoding: gzip`
- [x] Frontend talks to `location.host` when not on localhost, so no `?ip=` needed

**Update the dashboard on the chip** (frontend changes need a reflash):
```bash
cd frontend && npm run build
cd ../firmware && idf.py -p /dev/ttyACM0 flash
```

**Done when:** `http://192.168.1.182/` shows the dashboard with no laptop server running. ✅

Gotcha: a Wi-Fi scan pauses telemetry for ~3 s while the radio hops channels. The dashboard watchdog was 3 s, so it killed its own socket mid-scan and lost the result. Now 6 s, and a dropped socket clears "Scanning…".

## Phase 6 — Nice-to-haves (only if wanted)
- [ ] mDNS → `http://esp32.local/` (`idf.py add-dependency espressif/mdns`)
- [ ] SoftAP fallback when the home Wi-Fi isn't reachable
- [ ] Wi-Fi provisioning from the browser instead of menuconfig
- [ ] OTA firmware update from the dashboard
