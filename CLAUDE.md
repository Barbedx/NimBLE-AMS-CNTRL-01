# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP32 firmware (Arduino framework via PlatformIO) that connects to an iPhone over BLE and consumes Apple services **as a GATT client**:
- **AMS** (Apple Media Service) — now-playing metadata + playback control;
- **ANCS** (Apple Notification Center Service) — incoming notifications (messages/calls/app alerts), kept as a small live history;
- **CTS** (Current Time Service) — sets the ESP32 RTC.

Built on `h2zero/NimBLE-Arduino`. It also runs Wi-Fi (station with AP fallback) + a **PsychicHttp** web dashboard for live status and control.

Role model (important): the ESP32 advertises as a **peripheral** named `CTRL 01`; the **iPhone connects to it** (user taps it in iOS Settings → Bluetooth). On connect the ESP32 grabs a GATT *client* over that phone-initiated connection (`NimBLEServer::getClient`) and reads AMS/ANCS/CTS, which the iPhone hosts. Bonding makes every later reconnect automatic and silent.

This is a sandbox/proof-of-concept intended to be ported into a larger car head-unit project; modules are kept self-contained for that reason.

## Commands

PlatformIO. Shared options live in the `[env]` base; two boards inherit it. **`esp32dev-mini` (ESP32‑C3 SuperMini) is the primary board.**

```bash
pio run -e esp32dev-mini                                 # build
pio run -e esp32dev-mini -t upload --upload-port COMx    # flash (C3 auto-resets over native USB)
pio device monitor -p COMx -b 115200 --filter direct     # serial console
```

No tests. The serial monitor + the web dashboard are the observation/debug interfaces.

- **C3 SuperMini** (`esp32dev-mini`): native USB-Serial/JTAG, upload auto-resets — no buttons.
- **Classic `esp32dev`**: no working auto-reset — enter download mode manually (hold BOOT, tap EN, release BOOT) before uploading.

### Interfaces
- **Pairing:** iPhone → Settings → Bluetooth → tap `CTRL 01` → Pair. Then reconnection is automatic.
- **Web dashboard:** in STA mode reachable at `http://ctrl01.local` (mDNS) or the DHCP IP; in AP fallback at `http://192.168.4.1` (SSID `CTRL 01 Setup`, pw `ctrl0101`). Shows clock, now-playing (with a live-advancing progress bar), notification history, media controls, and a Wi-Fi config form (network dropdown from a live scan + manual entry).
- **Serial commands:** `help`, `clearbonds`, media commands `play pause toggle next prev vol+ vol- ...`. See `pollSerial()` in [src/main.cpp](src/main.cpp).

## Architecture

Each module is a `namespace` (not classes) with a header in `include/` and impl in `src/`. They are deliberately decoupled for porting:

- **[src/main.cpp](src/main.cpp)** — wiring only: `Bluetooth::Begin` → `WiFiManager::Begin` → `WebPortal::Begin`; `loop()` drives `Bluetooth::Service()` + `WiFiManager::Handle()` + serial.
- **[src/bluetooth.cpp](src/bluetooth.cpp)** (`Bluetooth::`) — the BLE peripheral link: advertising, bonding, connection lifecycle. On connect it secures the link then brings up AMS + ANCS + CTS; drives `AppleNotificationService::Process()` while connected.
- **[src/apple_media_service.cpp](src/apple_media_service.cpp)** (`AppleMediaService::`) — AMS Entity Update decode → `MediaInformation`; single-byte Remote Commands. `CurrentElapsedTime()` extrapolates the playback position between AMS updates (AMS only reports it on state changes).
- **[src/apple_notification_service.cpp](src/apple_notification_service.cpp)** (`AppleNotificationService::`) — ANCS. Keeps a mutex-guarded history (newest first, capped) keyed by UID; prunes on Removed events. `GetRecent()` for the UI.
- **[src/current_time_service.cpp](src/current_time_service.cpp)** (`CurrentTimeService::`) — CTS → `settimeofday()`.
- **[src/wifi_manager.cpp](src/wifi_manager.cpp)** (`WiFiManager::`) — STA with NVS-persisted credentials → AP fallback + captive DNS; mDNS; async network scan; optional static IP. Owns all networking.
- **[src/web_portal.cpp](src/web_portal.cpp)** (`WebPortal::`) — PsychicHttp dashboard + `/api/*`. Pure presentation: reads the service modules and WiFiManager. Knows nothing about how the network is set up. Runs in its own task (no loop `Handle()`).

### Threading invariant (do not break)
**BLE notification callbacks never block and never do a GATT read/write-with-response.** Such a call from the NimBLE host task deadlocks the host (incoming ACL buffers exhaust → `Failed to allocate buffer, retrying`). Pattern: the callback parses + stores + enqueues; the *loop task* performs outbound writes — e.g. ANCS queues a UID in the callback and `Process()` (loop) fetches its attributes.

### Connection / pairing flow
iOS only exposes AMS/ANCS to a **bonded** peer, and you can't tell which scanned RPA is "my iPhone". The peripheral model sidesteps both: the human picks the *ESP32* by name in Settings.

1. `Begin()`: `setSecurityAuth(bonding=true, mitm=false, sc=true)`, IO cap `NO_INPUT_OUTPUT` (Just Works), `setMTU(247)`; create `NimBLEServer`, `advertiseOnDisconnect(true)`.
2. **Advertising**: device name in the *primary* packet (scan-response-only name is hidden by iOS Settings) + AMS UUID as a **solicited** service (AD type `0x15`; `addData()` does not prepend the AD length byte, so it is built as `[0x11][0x15][16 UUID LE]`). Once bonded, iOS exposes both AMS and ANCS even though only AMS is solicited.
3. `onConnect` → `Client = pServer->getClient(connInfo)`.
4. `Service()` (loop): `secureConnection()` first, then start AMS → ANCS → CTS; retried ~every 800ms until it succeeds (issue ordering matters — NimBLE issue #1033).

### BLE GATT identifiers
- **AMS** `89D3502B-0F36-433A-8EF4-C502AD55F8DC` (Remote Command / Entity Update / Entity Attribute). Entity IDs Player=0/Queue=1/Track=2. Entity Update notifications: `[entityID][attrID][flags][UTF-8 value]`.
- **ANCS** `7905F431-B5CE-4E99-A40F-4B1E122D00D0` (Notification Source / Control Point / Data Source). Notification Source = `[EventID][Flags][CategoryID][Count][UID×4]`; Data Source response = `[CmdID][UID×4]` then `[AttrID][len×2 LE][value]` tuples.

## Notes for editing

- `build_flags` are in the `[env]` base; per-board flags append via `${env.build_flags}`. `CORE_DEBUG_LEVEL=3` keeps serial readable (5 buries it in WebServer/HCI dumps).
- Dependencies: `NimBLE-Arduino`, `hoeken/PsychicHttp` (pulls ArduinoJson + UrlEncode). `lib_ignore = BLE` keeps the old Bluedroid lib out.
- Code mixes English and Ukrainian comments — both are normal here.
- WiFi + BLE coexist on a single-core C3; keep the loop responsive. A Wi-Fi scan briefly disrupts the AP (single radio).
- The HTML/JS dashboard is an inline `PROGMEM` raw-string in [src/web_portal.cpp](src/web_portal.cpp); `/api/status` is hand-built JSON (escaped via `jsonEscape`).
- We host **no** GATT services (we're a client of the phone). A scanner that connects and "performs GATT discovery" forever is expected. Add a server only if another device must read this data (the head-unit project).
