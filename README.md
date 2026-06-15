# ESP32 ⇄ iPhone — AMS + ANCS + CTS over NimBLE (peripheral model)

ESP32 firmware that connects to an iPhone over Bluetooth Low Energy and consumes Apple's
on-device services **as a GATT client**:

- **AMS** (Apple Media Service) — now-playing metadata (title/artist/album/position/volume/queue/shuffle/repeat) and playback control (play/pause/next/prev/volume).
- **ANCS** (Apple Notification Center Service) — incoming notifications (messages, calls, app alerts), kept as a small live history.
- **CTS** (Current Time Service) — sets the ESP32 clock from the phone.

It also runs Wi-Fi (joins your network, or falls back to its own access point) and serves a
live **web dashboard** (PsychicHttp) for status and control.

Built on [`h2zero/NimBLE-Arduino`](https://github.com/h2zero/NimBLE-Arduino) 2.x. Tested on an
**ESP32‑C3 SuperMini**; a classic ESP32 target is also configured.

> This is a focused, working reference for the genuinely fiddly problem of talking to Apple
> BLE services from an ESP32. If you've been fighting "the device won't pair / iOS won't show
> AMS / it re-pairs every time / the stack deadlocks", the [Key learnings](#key-learnings-the-fiddly-bits) section is for you.

---

## The problem this solves

iOS is hostile to naive approaches:

1. **iOS only exposes AMS/ANCS to a *bonded* peer.** Without bonding you get ATT
   `Insufficient Authentication` (0x05) on every read.
2. **You can't tell which device is "my iPhone" from advertising.** iOS uses a rotating
   Resolvable Private Address and advertises no name; every nearby Apple device looks the same.
3. A common older trick — scan as a **central** and filter on the Spotify service UUID — is
   fragile and **breaks when the app stops advertising that UUID** (which is exactly what happened).

### The approach that works
Flip the roles: the **ESP32 advertises as a peripheral** named `CTRL 01`; **the iPhone connects to it**
(you tap it in iOS Settings → Bluetooth). The ESP32 then takes a GATT **client** over that
phone-initiated connection (`NimBLEServer::getClient`) and reads AMS/ANCS/CTS, which the iPhone
hosts. **Bonding** makes every later reconnect automatic and silent. The human picks the *ESP32*
by name — so there is nothing to "guess".

This is the same pattern as NimBLE-Arduino's `examples/ANCS`, extended to AMS + CTS + a history.

---

## Hardware

- **ESP32‑C3 SuperMini** (primary, env `esp32dev-mini`) — native USB-Serial/JTAG, flashes with auto-reset.
- **Classic ESP32 dev board** (env `esp32dev`) — works, but on boards without an auto-reset circuit
  you must enter download mode manually (hold **BOOT**, tap **EN/RST**, release **BOOT**) before flashing.

No extra wiring — BLE + Wi-Fi are on-chip.

## Build & flash (PlatformIO)

```bash
pio run -e esp32dev-mini                                  # build
pio run -e esp32dev-mini -t upload --upload-port COMx     # flash
pio device monitor -p COMx -b 115200 --filter direct      # serial console @115200
```

## First-time pairing

1. Flash and power the board.
2. On the iPhone: **Settings → Bluetooth** → under *Other Devices* tap **CTRL 01** → **Pair**.
3. Done. From then on it reconnects automatically whenever the phone is near — no prompts.

To re-pair from scratch: `clearbonds` on the serial console (and "Forget This Device" on the iPhone).

## Web dashboard

Connect to Wi-Fi (see below) and open the dashboard. It shows the clock, now-playing with a
**live-advancing** progress bar, the **notification history**, media control buttons, and a
**Wi-Fi config** form (network dropdown from a live scan + manual entry).

- **Station mode** (joined your network): `http://ctrl01.local` (mDNS) or the DHCP IP.
- **AP fallback**: connect to Wi-Fi `CTRL 01 Setup` (password `ctrl0101`) → `http://192.168.4.1`.

## Serial commands

`help`, `clearbonds`, and media commands: `play pause toggle next prev vol+ vol- ff rew rep shuf like dislike star`.

---

## Architecture

Each module is a `namespace` with a header in `include/` and impl in `src/`, deliberately decoupled:

| Module | Responsibility |
|---|---|
| `main.cpp` | Wiring only — `Bluetooth::Begin` → `WiFiManager::Begin` → `WebPortal::Begin`; drives `Service()`/`Handle()` in `loop()`. |
| `Bluetooth` | BLE peripheral link: advertising, bonding, connection lifecycle. On connect, secures the link and brings up AMS + ANCS + CTS. |
| `AppleMediaService` | AMS Entity Update decode → `MediaInformation`; single-byte Remote Commands; live playback-position extrapolation. |
| `AppleNotificationService` | ANCS: mutex-guarded notification history keyed by UID; prunes on *Removed* events. |
| `CurrentTimeService` | CTS → `settimeofday()`. |
| `WiFiManager` | STA with NVS-persisted credentials → AP fallback + captive DNS; mDNS; async network scan; optional static IP. |
| `WebPortal` | PsychicHttp dashboard + `/api/*`. Pure presentation; runs in its own task. |

Dependencies: `NimBLE-Arduino`, `hoeken/PsychicHttp` (pulls ArduinoJson + UrlEncode).

### BLE GATT identifiers
- **AMS** `89D3502B-0F36-433A-8EF4-C502AD55F8DC` — Remote Command / Entity Update / Entity Attribute.
- **ANCS** `7905F431-B5CE-4E99-A40F-4B1E122D00D0` — Notification Source / Control Point / Data Source.
- **CTS** `0x1805` — Current Time `0x2A2B`.

---

## Key learnings (the fiddly bits)

These cost real debugging; they're the reason this repo exists.

1. **Advertise the device name in the *primary* advertising packet, not the scan response.**
   iOS Settings → Bluetooth hides devices whose name is only in the scan response.

2. **Advertise the AMS UUID as a *solicited* service (AD type `0x15`, 128-bit), not a normal service UUID.**
   In NimBLE-Arduino, `NimBLEAdvertisementData::addData()` does **not** prepend the AD length byte —
   build the field manually as `[0x11][0x15][16 UUID bytes little-endian]`. (See NimBLE issue #1033.)
   Once bonded, iOS exposes **both** AMS and ANCS even though only AMS is solicited.

3. **Order matters: `secureConnection()` *before* service discovery.** AMS/ANCS characteristics are
   gated behind encryption; discover/subscribe before the bond completes and you get ATT 0x05.
   Retry the bring-up until it succeeds.

4. **A BLE notification callback must NEVER block or do a GATT write-with-response.** The callback
   runs on the NimBLE host task; a blocking write waits for a response that the *same task* must
   process → deadlock → incoming ACL buffers exhaust → `Failed to allocate buffer, retrying` forever,
   and BLE + the web server wedge. **Pattern:** the callback parses + stores + enqueues; the *loop
   task* performs outbound writes. (ANCS queues a notification UID; the loop fetches its attributes.)

5. **AMS reports the playback position only on state changes**, not continuously. Extrapolate it:
   `elapsed + (now − lastUpdate) × rate` while playing — otherwise the progress bar looks frozen.

6. **`setMTU(247)`** so ANCS message bodies fit in a single notification.

7. **We host no GATT services** — we are a *client* of the phone. A BLE scanner that connects to the
   ESP32 and "performs GATT discovery" forever is expected behaviour (there's nothing to discover).

---

## References & credits

- Apple — [AMS](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleMediaService_Reference/Specification/Specification.html) and [ANCS](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/) specifications.
- [`h2zero/NimBLE-Arduino`](https://github.com/h2zero/NimBLE-Arduino) — `examples/ANCS` is the seed of the peripheral approach; issue #1033 on solicitation advertising.
- [Adafruit — Now Playing: Bluetooth Apple Media Service](https://learn.adafruit.com/now-playing-bluetooth-apple-media-service-display) and its CircuitPython AMS library.
- Prior ESP32 AMS work this descends from: [err4o4/ctrl01](https://github.com/err4o4/ctrl01), [Marcus10110/DelSolClock](https://github.com/Marcus10110/DelSolClock), [Smartphone-Companions/ESP32-ANCS-Notifications](https://github.com/Smartphone-Companions/ESP32-ANCS-Notifications).
