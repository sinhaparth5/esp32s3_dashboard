# ESP32-S3 Dashboard

An ESP32-S3 joins your Wi-Fi, serves a small React dashboard from its own flash, and pushes live telemetry to it over a WebSocket once a second. The dashboard can also send commands back to the chip.

What you see in the browser:

- Die temperature, free heap (with a memory bar), uptime and Wi-Fi RSSI, each with a sparkline of the last 60 samples
- Connection status that notices a pulled plug and reconnects on its own
- Ping with round-trip time, a Wi-Fi scan table, and a colour picker for the onboard RGB LED

The status LED shows red with no Wi-Fi, green once connected, and flashes blue each time telemetry goes out.

## Layout

```
firmware/   ESP-IDF app in C (Wi-Fi, HTTP server, WebSocket, telemetry)
frontend/   React + Vite; the gzipped build is embedded in the firmware
```

The dashboard is compiled into the app binary with `EMBED_FILES`, so there is no filesystem partition or mount code. The gzipped build is about 71 KB.

## Requirements

- ESP32-S3 board with 16 MB flash (for example a DevKitC-1)
- ESP-IDF 5.x (developed on 5.3)
- Node.js (developed on Node 24)

## Build and flash

Build the frontend first. The firmware embeds `frontend/dist/*.gz` and won't build without it.

```bash
cd frontend
npm install
npm run build

cd ../firmware
. $HOME/esp/esp-idf/export.sh
idf.py menuconfig                       # Dashboard Wi-Fi: SSID, password, LED GPIO
idf.py -p /dev/ttyACM0 flash monitor    # Ctrl+] exits the monitor
```

The monitor prints `Got IP: 192.168.x.x`. Open that address in a browser on the same network.

Wi-Fi credentials live in `firmware/sdkconfig`, which is gitignored. The LED pin defaults to GPIO 48. Some DevKitC-1 v1.1 boards use GPIO 38, so check the silkscreen next to the LED.

Any frontend change needs `npm run build` and a reflash to show up on the chip.

## Frontend development

```bash
cd frontend
npm run dev
```

Open `http://localhost:5173/?ip=<ESP_IP>`. In dev mode the dashboard connects to the IP you pass in. When served from the chip it uses the page's own host.

## WebSocket protocol

Everything goes through `ws://<ESP_IP>/ws` as JSON text frames.

Telemetry, sent to every client once a second:

```json
{"temp":38.5,"heap":245120,"heap_min":230000,"heap_total":330000,"uptime":120,"rssi":-52}
```

Commands from the client:

| Send | Reply |
|------|-------|
| `{"cmd":"ping","t":123}` | `{"type":"pong","t":123}` |
| `{"cmd":"scan"}` | `{"type":"scan","aps":[{"ssid":"...","rssi":-60,"ch":6,"open":false}]}`, broadcast to all clients when the scan finishes |
| `{"cmd":"led","rgb":"#ff8800"}` | Sets the LED colour. `"rgb":null` hands it back to the status colours |

Bad input gets `{"type":"error","msg":"..."}`.

To watch the raw stream without the dashboard (Node 24 has WebSocket built in):

```bash
node -e 'new WebSocket("ws://<ESP_IP>/ws").onmessage = e => console.log(e.data)'
```

## Troubleshooting

- **Disconnect reason 201:** the SSID wasn't found. iPhone hotspot names use a curly apostrophe (`’`), not `'`.
- **Reason 15 or 204:** wrong password.
- **Permission denied on the serial port:** run `sudo usermod -aG dialout $USER`, then log out and back in.
- **Telemetry stops for a few seconds during a scan:** this is expected. The radio leaves the channel while it scans for about 3 s. The dashboard waits 6 s before treating the link as dead.
