# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP32 firmware (Arduino framework via PlatformIO) acting as a **BLE central/client** for iOS. It scans, connects to an iPhone, and consumes Apple Media Service (AMS) for now-playing metadata + playback control, and Current Time Service (CTS) to set the ESP32 RTC. It does **not** run a GATT server — pairing disambiguation is solved with a WiFi setup page + bonding, not by advertising. Built on `h2zero/NimBLE-Arduino` (the old Bluedroid `BLE` lib is explicitly ignored).

It also runs a **WiFi access point + web portal** (always on) for first-time device selection, live status, and remote control.

## Commands

PlatformIO project, single env `esp32dev` (board `esp32dev`, 115200 monitor baud).

```bash
pio run                 # build
pio run -t upload       # flash over serial
pio device monitor      # serial console @115200 (also issues runtime commands, see below)
pio run -t clean
```

There are no tests. The serial monitor is the primary debugging/control interface.

### Setup / control interfaces
- **WiFi portal:** on boot the device starts an AP `CTRL 01 Setup` (password `ctrl0101`, `192.168.4.1`). The page lists nearby BLE devices by RSSI, shows now-playing status, and has play/pause/next/prev buttons. See [src/setup_portal.cpp](src/setup_portal.cpp).
- **Runtime serial commands:** `help`, `clearbonds`, and media commands `play pause toggle next prev vol+ vol- ff rew rep shuf like dislike star`. Media commands only work while connected. See `pollSerial()` in [src/main.cpp](src/main.cpp).

## Architecture

Four modules + entry point, each a `namespace` (not classes) with a public header in `include/` and impl in `src/`:

- **[src/main.cpp](src/main.cpp)** — `setup()` calls `Bluetooth::Begin()`, registers the AMS notification callback, and `SetupPortal::Begin()`; `loop()` drives `Bluetooth::Service()` + `SetupPortal::Handle()`, prints RTC time every 10s, polls serial.
- **[src/bluetooth.cpp](src/bluetooth.cpp)** (`Bluetooth::`) — owns the single `NimBLEClient`, scan lifecycle, bonding, and the connection state machine. On connect it secures/bonds, then calls `AppleMediaService::StartMediaService()` and `CurrentTimeService::StartTimeService()`.
- **[src/setup_portal.cpp](src/setup_portal.cpp)** (`SetupPortal::`) — WiFi AP + `WebServer` + captive `DNSServer`. Reads `Bluetooth::GetCandidates()`/status and calls `Bluetooth::RequestConnect()` / `AppleMediaService::SendRemoteCommand()`. No extra PlatformIO deps (`WiFi`/`WebServer`/`DNSServer` are framework built-ins).
- **[src/apple_media_service.cpp](src/apple_media_service.cpp)** (`AppleMediaService::`) — subscribes to the AMS Entity Update characteristic, decodes Player/Queue/Track attributes into `MediaInformation`; sends remote commands.
- **[src/current_time_service.cpp](src/current_time_service.cpp)** (`CurrentTimeService::`) — reads CTS and converts to `time_t` for `settimeofday()`.

### Connection / pairing model (the key flow)
The hard problem: iOS only exposes AMS to a **bonded** peer, and from advertising alone there's no stable way to tell which Apple device is "my iPhone" (rotating RPA, no name). Solution = bonding + human-in-the-loop selection:

1. `Begin()` sets `setSecurityAuth(bonding=true, mitm=false, sc=true)` + IO cap `NO_INPUT_OUTPUT` (iOS "Just Works", keys persisted in NVS), and starts a forever active scan. **Bonding being ON is what makes reconnection silent** — the previous version used `bonding=false`, which re-prompted every connect.
2. `AmsScanCallbacks::onResult` records every connectable advertiser into a candidate list (address/RSSI/name/Apple-flag/bonded-flag) for the portal. If a candidate `NimBLEDevice::isBonded()`, it auto-requests reconnect.
3. **First pairing:** the user opens the WiFi page, picks the strongest Apple entry → `RequestConnect()`. The iOS pairing prompt is the disambiguator — accepting it designates "my phone" and stores the bond + the phone's identity (IRK).
4. **Reconnect:** once bonded, `Service()` reconnects automatically — via the scan auto-connect, plus a belt-and-suspenders direct `connect(getBondedAddress(0))` every 8s in case the rotating address isn't resolved during scan.
5. `connectTo()` does connect → `secureConnection()` → AMS → CTS. On `secureConnection()` failure of a bonded peer it `deleteBond()`s (stale-bond recovery) so the next attempt re-pairs cleanly.

Shared state (candidate list + pending-connect request) is touched by both the NimBLE host task (scan callbacks) and the loop task (portal handlers), so it's guarded by `StateMutex` via the `Lock` RAII helper.

### BLE GATT identifiers
AMS service `89D3502B-0F36-433A-8EF4-C502AD55F8DC` with Remote Command / Entity Update / Entity Attribute characteristics (UUIDs `#define`d at the top of [src/apple_media_service.cpp](src/apple_media_service.cpp)). Entity IDs: Player=0, Queue=1, Track=2. Remote commands are single bytes (`RemoteCommandID` in [include/apple_media_service.h](include/apple_media_service.h)); Entity Update notifications parse as `[entityID][attrID][flags][UTF-8 value]`.

## Notes for editing

- `build_flags` in [platformio.ini](platformio.ini) crank NimBLE + ESP debug logging to verbose; expect heavy serial output. Adjust there, not in source.
- Code mixes English and Ukrainian comments — both are normal here.
- `.vscode/c_cpp_properties.json` is auto-generated by PlatformIO; do not hand-edit.
- WiFi + BLE run concurrently (coexistence). Flash usage is ~86% of the default `esp32dev` partition — large additions may need a custom partition table.
- The HTML/JS portal page is an inline `PROGMEM` raw-string literal in [src/setup_portal.cpp](src/setup_portal.cpp).
