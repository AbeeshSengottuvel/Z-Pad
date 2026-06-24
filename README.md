<div align="center">

# 🛋️ Desk Deck

### A bedside ESP32 remote for controlling your PC — and VLC — without leaving the pillow.

[![Platform](https://img.shields.io/badge/platform-ESP32-3C8DBC?logo=espressif&logoColor=white)](https://www.espressif.com/)
[![Firmware](https://img.shields.io/badge/firmware-Arduino-00979D?logo=arduino&logoColor=white)](https://www.arduino.cc/)
[![Companion](https://img.shields.io/badge/companion-Python%20·%20Windows-3776AB?logo=python&logoColor=white)](#-the-companion-app-windows)
[![Display](https://img.shields.io/badge/display-SH1106%20OLED-111)](#-hardware--bill-of-materials)
[![License](https://img.shields.io/badge/license-MIT-green)](#-license)

```
 ┌──────────────────────────────┐
 │ BT ▮▮▮              23:41 │
 ├──────────────────────────────┤
 │ Interstellar.2014.2160p.mkv  │
 │ Playing            hold=menu │
 │ ◄▮▮▮▮▮▮░░░░░░░░░░░░░░ ► │
 │ 12:14                2:48:00 │
 └──────────────────────────────┘
   ↑ / ↓  volume     ← / →  seek     tap  play/pause     hold  back
```

</div>

---

Desk Deck is a hand-held controller built around an **ESP32**, a **1.3″ OLED**, and a single **analog joystick**. It pairs to your PC over **Bluetooth** and lets you drive VLC, mix per-app volume, fire media keys, launch apps, watch system stats, and put the machine to sleep — all from one thumb. A tiny Python companion runs on the PC and does the actual work; the ESP32 is the remote.

> **New here? Jump to [⚡ Quick Start](#-quick-start).** Everything you need — including the WiFi password — is in this one file.

---

## 📑 Table of contents

- [✨ Features](#-features)
- [⚡ Quick start](#-quick-start)
- [🧰 Hardware / bill of materials](#-hardware--bill-of-materials)
- [🔌 Wiring](#-wiring)
- [💾 Flashing the firmware](#-flashing-the-firmware)
- [📶 WiFi setup (read this)](#-wifi-setup-read-this)
- [🎬 VLC setup](#-vlc-setup)
- [🖥️ The companion app (Windows)](#-the-companion-app-windows)
- [🕹️ Controls](#-controls)
- [🗺️ Menu map](#-menu-map)
- [🧪 Troubleshooting](#-troubleshooting)
- [🔬 Reference & advanced](#-reference--advanced)
- [🗒️ Roadmap](#-roadmap)
- [📄 License](#-license)

---

## ✨ Features

| | Feature | Notes |
|---|---|---|
| 🎬 | **Now Playing** | Live VLC title, progress bar, scrub, volume |
| 🔊 | **Per-app volume mixer** | Master + each running app, with mute |
| ⏯️ | **Media keys** | Play/pause, next, previous, mute |
| 🚀 | **App launcher** | Open programs/URLs you define on the PC |
| 📊 | **System monitor** | CPU · RAM · GPU · temp · network throughput |
| ⏻ | **Power menu** | Shutdown / restart / sleep / lock / log off (with confirm) |
| 🌙 | **VLC sleep timer** | Auto-pause after 15 / 30 / 45 / 60 / 90 min |
| 📶 | **WiFi captive portal** | Add networks from your phone — no hard-coded passwords |
| 🔔 | **Wake-on-LAN** | Wake the PC from across the room |
| 🔄 | **OTA updates** | Re-flash firmware over WiFi |
| 🛏️ | **Auto-sleep + deep sleep** | Press to wake; sips power when idle |
| 🧭 | **Joystick-native UI** | No on-screen back buttons; pure directional feel |

---

## ⚡ Quick start

```text
1. Wire the OLED + joystick to the ESP32        →  see “Wiring”
2. Flash DeskDeck.ino from the Arduino IDE       →  see “Flashing the firmware”
3. On first boot, join the hotspot from a phone:
        Network:  DeskDeck-Setup
        Password: deskdeck123
   A “Sign in to network” page pops up → pick your
   home WiFi, type its password, tap Connect.
4. On the PC: enable VLC’s web interface          →  see “VLC setup”
5. On the PC: run the companion                   →  python deskdeck_companion.py --install
6. Bluetooth-pair the PC to “DeskDeck”. Done. 🎉
```

The hotspot password is **`deskdeck123`** (set by `AP_PASS` in the firmware). The VLC password is **`vlc`**. Both are configurable — see below.

---

## 🧰 Hardware / bill of materials

| Qty | Part | Detail |
|----:|------|--------|
| 1 | **ESP32 dev board** | Must have **Bluetooth Classic** (classic WROOM-32 / DevKitC). *Not* the C3/S2 — those lack BT Classic. |
| 1 | **1.3″ OLED, SH1106** | 128×64, **I²C**, address `0x3C` |
| 1 | **Analog joystick module** | 2-axis (VRx/VRy) + push button (SW) |
| — | Dupont wires, a USB cable, optional LiPo + charger | |

> 💡 A 0.96″ **SSD1306** can work too, but you’ll need to swap the display constructor/library. This project targets **SH1106**.

---

## 🔌 Wiring

```text
        ESP32                         OLED (SH1106, I2C)
   ┌────────────┐                   ┌──────────────────┐
   │        3V3 ├───────────────────┤ VCC              │
   │        GND ├───────────────────┤ GND              │
   │     GPIO21 ├───────────────────┤ SDA              │
   │     GPIO22 ├───────────────────┤ SCL              │
   │            │                   └──────────────────┘
   │            │                     Joystick
   │        3V3 ├───────────────────┤ +5V / VCC        │
   │        GND ├───────────────────┤ GND              │
   │     GPIO34 ├───────────────────┤ VRx              │
   │     GPIO35 ├───────────────────┤ VRy              │
   │     GPIO32 ├───────────────────┤ SW  (button)     │
   └────────────┘                   └──────────────────┘
```

| Signal | ESP32 pin | Notes |
|--------|-----------|-------|
| OLED SDA | **GPIO21** | default hardware I²C |
| OLED SCL | **GPIO22** | default hardware I²C |
| Joystick VRx | **GPIO34** | ADC input only (input-only pin) |
| Joystick VRy | **GPIO35** | ADC input only (input-only pin) |
| Joystick SW | **GPIO32** | `INPUT_PULLUP`; also the **deep-sleep wake** pin |

> ⚠️ GPIO34/35 are **input-only** and have no internal pull-ups — that’s fine for the analog axes. The button uses GPIO32 because it must be an **RTC** pin to wake the chip from deep sleep.

<details>
<summary>🎛️ Changing the pins</summary>

Pins are defined near the top of `DeskDeck.ino`:

```cpp
#define JOY_X_PIN    34
#define JOY_Y_PIN    35
#define JOY_BTN_PIN  32   // keep this on an RTC GPIO for deep-sleep wake
const bool INVERT_X = false, INVERT_Y = false;   // flip if your stick reads backwards
```

If your joystick’s axes feel reversed, flip `INVERT_X` / `INVERT_Y`. After wiring, run **Settings → Calibrate joystick** and press to set the resting center.
</details>

---

## 💾 Flashing the firmware

### 1. Install the ESP32 board support
In the Arduino IDE: **File → Preferences → Additional Board Manager URLs**, add:

```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

Then **Tools → Board → Boards Manager → “esp32”** → Install.

### 2. Install the two external libraries
**Sketch → Include Library → Manage Libraries**, install:

- **Adafruit GFX Library**
- **Adafruit SH110X**

Everything else (`BluetoothSerial`, `WiFi`, `WiFiMulti`, `WebServer`, `DNSServer`, `Preferences`, `ArduinoOTA`) ships with the ESP32 core.

### 3. Board settings
| Setting | Value |
|---|---|
| Board | **ESP32 Dev Module** |
| Partition Scheme | **Huge APP (3MB No OTA)** *or* **Minimal SPIFFS** — BT Classic + WiFi is large |
| Upload speed | 921600 (drop to 115200 if uploads fail) |

> If you plan to use **OTA** updates, pick a partition scheme that **keeps OTA** (e.g. “Default 4MB with spiffs”). Huge-App-No-OTA disables wireless re-flashing.

### 4. Edit your config (optional but recommended)
Open `DeskDeck.ino` and adjust the block near the top:

```cpp
const char* BT_NAME = "DeskDeck";          // Bluetooth name you’ll pair to
const char* AP_SSID = "DeskDeck-Setup";    // setup hotspot name
const char* AP_PASS = "deskdeck123";       // setup hotspot password  (≥ 8 chars)
const long  GMT_OFFSET_SEC = 19800;        // clock timezone — 19800 = IST (UTC+5:30)
uint8_t     PC_MAC[6] = {0xDE,0xAD,0xBE,0xEF,0x12,0x34};  // your PC’s MAC, for Wake-on-LAN
const char* OTA_HOST = "deskdeck";         // OTA hostname
const char* OTA_PASS = "deskdeck-ota";     // OTA password
```

- **Timezone:** `GMT_OFFSET_SEC` is seconds offset from UTC. IST = `19800`. EST = `-18000`. CET = `3600`.
- **Wake-on-LAN:** set `PC_MAC` to your PC’s MAC (run `getmac` in a Windows terminal).
- You do **not** put your home WiFi password here — that’s done from your phone (next section).

### 5. Upload
Select the COM port, hit **Upload**. On boot you’ll see an `ESP32 starting` splash, then the menu.

---

## 📶 WiFi setup (read this)

Your home WiFi password is **never compiled into the firmware**. You set it once from your phone, and the ESP32 stores it permanently in flash. Here’s the whole flow:

```text
   First boot, no saved networks
              │
              ▼
   ESP32 starts a hotspot ─────────────►  📱 Join it on your phone:
        SSID:     DeskDeck-Setup                Network:  DeskDeck-Setup
        Password: deskdeck123                   Password: deskdeck123
              │
              ▼
   Phone shows “Sign in to network”  ──►  the captive portal opens automatically
   (if it doesn’t, open  http://192.168.4.1  in any browser)
              │
              ▼
   Pick your home network → type its password → Connect
              │
              ▼
   ✅ Saved to flash. Auto-reconnects on every boot, forever.
```

### Where do I change the WiFi password later?
Two ways, both permanent:

1. **On the device:** `Settings → WiFi`. Toggle WiFi, see the connected network and IP, scan, and tap a **saved** network to reconnect. To enter/replace a password, tap **Setup in browser** and use the portal.
2. **In the browser:** join `DeskDeck-Setup` (or, once it’s on your LAN, visit the IP shown in `Settings → WiFi`), then **Connect** a network (saves/updates its password) or **Forget** one.

<details>
<summary>🔐 Is the password stored permanently? (yes — here’s how)</summary>

Saved networks live in the ESP32’s **NVS** (non-volatile flash), not RAM. They survive **reboots, power loss, and ordinary re-flashes**. On every boot the firmware reads them back and auto-connects.

- Up to **6 networks** are remembered (best signal wins, via `WiFiMulti`).
- Saving a password for an SSID that already exists just **updates** it.
- The only things that wipe them: tapping **Forget**, an explicit NVS/flash erase (`esptool erase_flash` or Arduino “Erase All Flash”), or changing the partition scheme on upload.
- ⚠️ Credentials are stored in **plain text** in flash (normal for ESP32 hobby projects). Anyone with physical access + esptool could read them.
</details>

<details>
<summary>🪄 Why the captive portal matters (the bug it fixes)</summary>

Phones run a connectivity check after joining any WiFi. If the network has no internet and nothing answers that check, the phone assumes it’s dead and silently drops back to mobile data — so you’d never reach `192.168.4.1`.

Desk Deck runs a tiny **DNS server** that points every lookup at itself, plus a catch-all web route, so the phone gets the “Sign in to network” prompt and **stays connected** long enough for you to finish setup.
</details>

<details>
<summary>📡 Note on the always-on hotspot</summary>

The `DeskDeck-Setup` hotspot stays available alongside your home connection so you can always reconfigure. It’s pinned to channel 1; when the ESP32 successfully joins your home WiFi, the radio hops to that channel and the hotspot briefly drops (your phone reconnects on its own). Change `AP_PASS` if you want the hotspot itself locked down.
</details>

---

## 🎬 VLC setup

Desk Deck talks to VLC over its built-in web (Lua HTTP) interface.

1. Open VLC → **Tools → Preferences**.
2. Bottom-left, **Show settings: All**.
3. **Interface → Main interfaces** → tick **Web**.
4. **Interface → Main interfaces → Lua** → set **Lua HTTP → Password** to `vlc`.
5. Save and **restart VLC**.

The companion uses `http://127.0.0.1:8080` with password `vlc`. Change `VLC_PORT` / `VLC_PASSWORD` in `deskdeck_companion.py` if yours differ.

> The Now Playing page, seek, subtitle/audio-track toggles, fullscreen, and the sleep timer all rely on this. Volume keys use the **system master volume** and don’t need VLC.

---

## 🖥️ The companion app (Windows)

The PC side is `deskdeck_companion.py`. It reads audio sessions + system stats, relays commands, and pushes Now-Playing info — over the Bluetooth serial link.

### Install dependencies
```bash
pip install pyserial pycaw comtypes psutil requests
pip install GPUtil screen_brightness_control   # optional: GPU stats + monitor brightness
```

### Pair Bluetooth
Pair your PC with the device named **`DeskDeck`** in Windows Bluetooth settings. This creates a Bluetooth serial (COM) port — the companion finds the right one automatically.

### Run it
```bash
python deskdeck_companion.py
```

### Make it start automatically (recommended)
No more launching it by hand, and it survives the ESP32 disconnecting:

```bash
python deskdeck_companion.py --install     # runs hidden at login, self-reconnects
python deskdeck_companion.py --uninstall   # remove from startup
```

<details>
<summary>🔎 How auto-connect & auto-reconnect work</summary>

- **Right port, automatically:** Windows creates *two* COM ports per Bluetooth device and only one works. The companion sends `PING` to each candidate and keeps the one that answers `PONG` — no guessing, no editing a port number.
- **Self-healing:** a keep-alive runs every few seconds; if the link goes quiet (ESP32 reboots, sleeps, or wanders out of range) it drops and rescans on its own. You never relaunch it.
- **Never dies:** the whole loop is wrapped so an unexpected error logs to `deskdeck.log` (next to the script) and recovers instead of exiting.
</details>

<details>
<summary>🚀 Define your launcher shortcuts</summary>

Edit the `LAUNCHERS` list in `deskdeck_companion.py` — label shown on the device, then the path/URL to open:

```python
LAUNCHERS = [
    ("VLC",     r"C:\Program Files\VideoLAN\VLC\vlc.exe"),
    ("Chrome",  r"C:\Program Files\Google\Chrome\Application\chrome.exe"),
    ("YouTube", "https://youtube.com"),
    ("Notepad", "notepad.exe"),
]
```
</details>

> 🪟 **Windows-only** today (uses `pycaw` for the mixer and `ctypes`/`shutdown` for power). Linux/macOS would need those pieces swapped — open an issue if you want it.

---

## 🕹️ Controls

The whole UI is driven by one joystick. The rule everywhere is **directional**:

| Input | Action |
|-------|--------|
| **Up / Down** | Move the highlight |
| **Left** | Back (up one level) |
| **Right** | Open / enter / activate |
| **Press** | Select |

**Editing a value** (volume, brightness): press **OK** to start editing, then **Left/Right** changes it, **Up/Down** mutes (on volume rows), and **OK** again saves. A faint `◄` chevron hints the back gesture.

### Now Playing is special
Because it’s a media screen, all four directions are mapped to playback — so **back becomes a long-press** (the one screen where Left isn’t back):

| Input | Action |
|-------|--------|
| **Up / Down** | Volume up / down (PC master volume) |
| **Left / Right** | Seek −10s / +10s (hold to keep scrubbing) |
| **Press (tap)** | Play / pause |
| **Press (hold ~½s)** | Back to menu |

---

## 🗺️ Menu map

```text
HOME
├── Now Playing ............ title · progress · ↑↓ volume · ←→ seek · tap play/pause · hold back
├── VLC
│   ├── Now Playing
│   ├── Play / Pause
│   ├── Subtitles: ON/OFF
│   ├── Audio track
│   ├── Fullscreen
│   ├── Seek
│   └── Sleep timer (off→15→30→45→60→90)
├── Volume mixer ........... Master + per-app, press to edit, ↑↓ mute
├── Media .................. Play/Pause · Next · Previous · Mute
├── System ................. CPU · RAM · GPU · temp · network
├── Launcher ............... your defined apps/URLs
├── Settings
│   ├── WiFi ............... toggle · status · IP · scan/connect · Setup in browser
│   ├── Bluetooth: ON/OFF
│   ├── Brightness
│   ├── Auto-sleep: ON/OFF
│   ├── Rotate 180: ON/OFF
│   ├── Calibrate joystick
│   ├── Re-pair Bluetooth
│   ├── Restart ESP32
│   ├── Power off (deep sleep)
│   └── About device
└── Power .................. Shutdown · Restart · Sleep · Lock · Log off · Wake PC
```

The status bar (top of every screen) shows the **Bluetooth state** (slashed = off · outline = ready · filled = connected), **WiFi signal**, the page title, and the **clock**.

---

## 🧪 Troubleshooting

<details>
<summary>Display stays blank / “OLED not found” on serial</summary>

- Confirm it’s an **SH1106** at I²C address **0x3C** (some are `0x3D`).
- Check SDA→GPIO21, SCL→GPIO22, and that VCC matches your module (3.3V vs 5V).
- A long/loose I²C wire can fail at 400 kHz — shorten it.
</details>

<details>
<summary>Phone won’t load the WiFi setup page</summary>

- Make sure you joined **DeskDeck-Setup** (password `deskdeck123`), not your home WiFi.
- If the “Sign in” prompt doesn’t pop, open **`http://192.168.4.1`** manually (use `http://`, not `https`).
- Some Android skins are stubborn about captive portals; any plain `http://` address forces it.
</details>

<details>
<summary>Saved a password but it won’t connect</summary>

- Double-check the password (case-sensitive). Re-enter it via **Setup in browser**.
- 5 GHz-only networks won’t work — the ESP32 is **2.4 GHz** only.
- Watch the OLED: you’ll get a `Connected` / `Failed` toast after a save.
</details>

<details>
<summary>Companion can’t find the device / connects but nothing happens</summary>

- Pair the PC to **DeskDeck** in Windows Bluetooth first.
- Check `deskdeck.log` (next to the script) for the chosen COM port and errors.
- If it grabbed a dead port, the handshake should reject it within ~1.5s and retry — give it a moment.
</details>

<details>
<summary>Now Playing shows “Nothing playing”</summary>

- VLC’s web interface must be enabled with password `vlc` (see VLC setup) and VLC must be running with media open.
- Confirm the companion is running and the Bluetooth icon shows **filled** (connected).
</details>

<details>
<summary>Volume keys do nothing</summary>

- Volume uses the **system master volume** via the companion — make sure the companion is running.
- GPU stats showing `0%`? `GPUtil` is NVIDIA-only; AMD/Intel report zero. That’s cosmetic.
</details>

---

## 🔬 Reference & advanced

<details>
<summary>📨 Serial protocol (PC ⇄ ESP32)</summary>

One line per message, newline-terminated.

**PC → ESP32**
| Line | Meaning |
|------|---------|
| `V\|name,vol,muted;…` | volume sessions (master first) |
| `S\|cpu=..;ram=..;gpu=..;temp=..;net=..` | system stats |
| `L\|label;label;…` | launcher labels |
| `N\|state;pos;len;title` | now playing (state 0=stop 1=play 2=pause) |
| `PONG` | reply to a keep-alive |

**ESP32 → PC**
| Line | Meaning |
|------|---------|
| `SET <i> <vol>` | set session *i* volume |
| `MUTE <i>` | toggle mute on session *i* |
| `VOL up` / `VOL down` | nudge master volume ±10% |
| `BRI <n>` | set monitor brightness |
| `MEDIA <key>` | media key (playpause/next/prev/mute) |
| `PWR <action>` | shutdown/restart/sleep/lock/logoff |
| `LAUNCH <i>` | launch app *i* |
| `VLC <cmd> [arg]` | playpause/pause/fullscreen/subtitle/audiotrack/seek |
| `PING` | keep-alive / port handshake |
</details>

<details>
<summary>📡 OTA (wireless firmware updates)</summary>

With a partition scheme that keeps OTA, the device exposes an Arduino OTA target:

- Hostname: `deskdeck`
- Password: `deskdeck-ota`

In the Arduino IDE, select the network port that appears under **Tools → Port** once the device is on your LAN, then upload as usual.
</details>

<details>
<summary>🔔 Wake-on-LAN</summary>

`Power → Wake PC` (or the WoL action) broadcasts a magic packet to `PC_MAC`. Set `PC_MAC` in the firmware to your PC’s MAC (`getmac` on Windows), and enable **Wake on Magic Packet** in your PC’s network adapter + BIOS. Requires the ESP32 to be on the same LAN (WiFi connected).
</details>

<details>
<summary>🌙 Power & sleep behaviour</summary>

- **Auto-sleep** (Settings) blanks the screen after ~60s idle; any joystick input wakes it.
- **Power off** (Settings) enters **deep sleep**; press the joystick button (GPIO32) to boot back up.
- **VLC sleep timer** pauses playback after the chosen interval and can also send the device to sleep.
</details>

<details>
<summary>🎚️ Brightness note</summary>

The Brightness row dims the **OLED** *and* sends `BRI` to the PC, which adjusts your **monitor** brightness too. If you’d rather it only touch the little screen, delete the `send("BRI …")` line in `adjustH()`.
</details>

---

## 🗒️ Roadmap

- [ ] On-device “Forget network” (currently browser-only)
- [ ] Linux / macOS companion
- [ ] Multiple PC profiles
- [ ] Battery gauge in the status bar

---

## 📄 License

MIT — do whatever you like, no warranty. See `LICENSE`.

<div align="center">

---

*Built for lazy evenings. Pull requests welcome.* 🛋️

</div>
