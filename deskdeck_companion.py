"""
DeskDeck companion app (Windows)

Runs in the background, finds the DeskDeck over Bluetooth automatically, pushes
volume / system / now-playing data, and relays VLC + media + power commands.

Key behaviours
  * Picks the CORRECT Bluetooth COM port with a PING/PONG handshake (Windows
    creates two ports per SPP device; only the outgoing one answers).
  * Detects a dropped link with a keep-alive and re-scans automatically, so you
    never have to relaunch it when the ESP32 disconnects or reboots.
  * Survives unexpected errors without exiting (logs to deskdeck.log).

Run once to add it to Windows startup (hidden, via pythonw):
    python deskdeck_companion.py --install
Remove it from startup:
    python deskdeck_companion.py --uninstall
Run normally / debug:
    python deskdeck_companion.py
"""

import os
import sys
import time
import ctypes
import subprocess
import xml.etree.ElementTree as ET

import serial
import serial.tools.list_ports
import requests
import psutil
from ctypes import cast, POINTER
from comtypes import CLSCTX_ALL
import comtypes
from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume

try:
    import GPUtil
except Exception:
    GPUtil = None

# ----------------------- CONFIG -----------------------
BAUD          = 115200
VLC_PORT      = 8080
VLC_PASSWORD  = "vlc"             # The Lua HTTP password configured in VLC
PUSH_INTERVAL = 1.0               # seconds between volume / stats / now-playing pushes
PING_INTERVAL = 3.0               # seconds between keep-alive PINGs
LINK_TIMEOUT  = 9.0               # no reply for this long  ->  assume disconnect & rescan
HANDSHAKE_WAIT = 1.5              # seconds to wait for PONG when probing a port

LAUNCHERS = [                     # (Label shown on device, absolute path or URL)
    ("VLC",      r"C:\Program Files\VideoLAN\VLC\vlc.exe"),
    ("Chrome",   r"C:\Program Files\Google\Chrome\Application\chrome.exe"),
    ("YouTube",  "https://youtube.com"),
    ("Notepad",  "notepad.exe"),
]

APP_NAME = "DeskDeck"
RUN_KEY  = r"Software\Microsoft\Windows\CurrentVersion\Run"
LOG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "deskdeck.log")
# ------------------------------------------------------

# Virtual-key codes for system media keys
VK = {"playpause": 0xB3, "next": 0xB0, "prev": 0xB1, "mute": 0xAD}


# ----------------------- LOGGING ----------------------
def log(*args):
    """Print if a console exists, and always append to the log file. Never raises."""
    msg = " ".join(str(a) for a in args)
    line = time.strftime("%H:%M:%S ") + msg
    try:
        print(line)
    except Exception:
        pass
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


# --------------- STARTUP (auto-run) INSTALL -----------
def _pythonw_path():
    """pythonw.exe runs with no console window; fall back to python.exe."""
    pyw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    return pyw if os.path.exists(pyw) else sys.executable


def install_startup():
    import winreg
    script = os.path.abspath(__file__)
    cmd = f'"{_pythonw_path()}" "{script}"'
    key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE)
    winreg.SetValueEx(key, APP_NAME, 0, winreg.REG_SZ, cmd)
    winreg.CloseKey(key)
    log(f"Installed to startup: {cmd}")
    print("\nDone. DeskDeck will now start automatically and silently when you log in.")
    print("It also reconnects on its own, so you won't need to run it again.")


def uninstall_startup():
    import winreg
    try:
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE)
        winreg.DeleteValue(key, APP_NAME)
        winreg.CloseKey(key)
        log("Removed from startup.")
        print("Removed DeskDeck from Windows startup.")
    except FileNotFoundError:
        print("DeskDeck was not in startup.")


# ----------------------- PORT DISCOVERY (handshake) ----
def candidate_ports():
    """All COM ports that look like a Bluetooth serial link."""
    out = []
    for p in serial.tools.list_ports.comports():
        hwid = (p.hwid or "").upper()
        desc = (p.description or "").lower()
        if "BTHENUM" in hwid or "bluetooth" in desc:
            out.append(p.device)
    return out


def probe_port(dev):
    """Open a port, send PING, return the open Serial if it answers PONG (else None)."""
    try:
        s = serial.Serial(dev, BAUD, timeout=0.3)
    except Exception:
        return None  # locked / wrong-direction port
    try:
        time.sleep(0.2)
        s.reset_input_buffer()
        s.write(b"PING\n")
        s.flush()
        deadline = time.time() + HANDSHAKE_WAIT
        buf = ""
        while time.time() < deadline:
            chunk = s.read(64).decode(errors="ignore")
            if chunk:
                buf += chunk
                if "PONG" in buf:
                    return s          # keep this one OPEN
        s.close()
        return None
    except Exception:
        try:
            s.close()
        except Exception:
            pass
        return None


def find_deskdeck():
    """Return an open Serial to the DeskDeck, or None if not found this round."""
    for dev in candidate_ports():
        s = probe_port(dev)
        if s:
            log(f"DeskDeck answered on {dev}")
            return s
    return None


# ----------------------- AUDIO MANAGEMENT -----------------------
def master_endpoint():
    try:
        spk = AudioUtilities.GetSpeakers()
        if hasattr(spk, "Activate"):
            iface = spk.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
            return cast(iface, POINTER(IAudioEndpointVolume))
        return spk.EndpointVolume
    except Exception as e:
        log(f"[Audio] master endpoint error: {e}")
        return None


def build_audio():
    """Return (display_list, handles). Index 0 is Master, then active app sessions."""
    display, handles = [], []
    try:
        m = master_endpoint()
        if m:
            display.append(("Master", round(m.GetMasterVolumeLevelScalar() * 100), bool(m.GetMute())))
            handles.append(("master", m))
    except Exception as e:
        log(f"[Audio] master read failed: {e}")

    try:
        for s in AudioUtilities.GetAllSessions():
            if not s.Process:
                continue
            name = s.Process.name().replace(".exe", "")
            try:
                v = s.SimpleAudioVolume
                display.append((name[:15], round(v.GetMasterVolume() * 100), bool(v.GetMute())))
                handles.append(("app", v))
            except Exception as e:
                log(f"[Audio] session read failed for {name}: {e}")
                continue
            if len(handles) >= 8:
                break
    except Exception as e:
        log(f"[Audio] iterating sessions failed: {e}")

    return display, handles


def set_volume(handles, i, pct):
    if 0 <= i < len(handles):
        kind, h = handles[i]
        scal = max(0.0, min(1.0, pct / 100.0))
        try:
            if kind == "master":
                h.SetMasterVolumeLevelScalar(scal, None)
            else:
                h.SetMasterVolume(scal, None)
        except Exception as e:
            log(f"[Audio] set volume failed @ {i}: {e}")


def toggle_mute(handles, i):
    if 0 <= i < len(handles):
        kind, h = handles[i]
        try:
            cur = h.GetMute()
            h.SetMute(0 if cur else 1, None)
        except Exception as e:
            log(f"[Audio] mute toggle failed @ {i}: {e}")


def nudge_master_volume(delta):
    """Relative change to the system master volume (delta in 0..1 scale)."""
    m = master_endpoint()
    if not m:
        return
    try:
        cur = m.GetMasterVolumeLevelScalar()
        m.SetMasterVolumeLevelScalar(max(0.0, min(1.0, cur + delta)), None)
    except Exception as e:
        log(f"[Audio] master volume nudge failed: {e}")


# ----------------------- OS & APPLICATIONS -----------------------
def media_key(action):
    vk = VK.get(action)
    if vk:
        ctypes.windll.user32.keybd_event(vk, 0, 0, 0)
        ctypes.windll.user32.keybd_event(vk, 0, 2, 0)


def power(action):
    if action == "shutdown":
        subprocess.run("shutdown /s /t 0", shell=True)
    elif action == "restart":
        subprocess.run("shutdown /r /t 0", shell=True)
    elif action == "logoff":
        subprocess.run("shutdown /l", shell=True)
    elif action == "lock":
        ctypes.windll.user32.LockWorkStation()
    elif action == "sleep":
        ctypes.windll.PowrProf.SetSuspendState(0, 1, 0)


def launch(i):
    if 0 <= i < len(LAUNCHERS):
        target = LAUNCHERS[i][1]
        try:
            os.startfile(target)
        except Exception:
            try:
                subprocess.Popen(target)
            except Exception:
                pass


def vlc(command, val=None):
    params = {"command": command}
    if val is not None:
        params["val"] = val
    try:
        requests.get(f"http://127.0.0.1:{VLC_PORT}/requests/status.xml",
                     params=params, auth=("", VLC_PASSWORD), timeout=1)
    except Exception:
        pass


def do_vlc(parts):
    sub = parts[1] if len(parts) > 1 else ""
    if sub == "playpause":    vlc("pl_pause")
    elif sub == "pause":      vlc("pl_forcepause")
    elif sub == "fullscreen": vlc("fullscreen")
    elif sub == "subtitle":   vlc("key", "subtitle-track")
    elif sub == "audiotrack": vlc("key", "audio-track")
    elif sub == "seek":
        amt = parts[2] if len(parts) > 2 else "10"
        if not amt.startswith("-"):
            amt = "+" + amt
        vlc("seek", amt)


# ----------------------- NOW PLAYING --------------------
def vlc_now_playing():
    """Return (state, pos_sec, len_sec, title). state: 0 stop / 1 play / 2 pause."""
    try:
        r = requests.get(f"http://127.0.0.1:{VLC_PORT}/requests/status.xml",
                         auth=("", VLC_PASSWORD), timeout=1)
        root = ET.fromstring(r.content)
    except Exception:
        return 0, 0, 0, ""

    raw = (root.findtext("state") or "stopped").lower()
    state = 1 if raw == "playing" else 2 if raw == "paused" else 0

    def as_int(tag):
        try:
            return int(float(root.findtext(tag) or 0))
        except Exception:
            return 0

    pos = as_int("time")
    length = as_int("length")

    # Title: prefer the meta "title", fall back to "filename".
    title, filename = "", ""
    for cat in root.iter("category"):
        if cat.get("name") == "meta":
            for info in cat.iter("info"):
                nm = info.get("name")
                if nm == "title" and info.text:
                    title = info.text
                elif nm == "filename" and info.text:
                    filename = info.text
    name = (title or filename or "").strip()
    name = name.replace("\n", " ").replace("\r", " ")[:46]
    return state, pos, length, name


# ----------------------- METRICS & TELEMETRY -----------------------
_net_prev = None
_net_t = None


def net_mbs():
    global _net_prev, _net_t
    now = time.time()
    cur = psutil.net_io_counters()
    total = cur.bytes_sent + cur.bytes_recv
    if _net_prev is None:
        _net_prev, _net_t = total, now
        return 0.0
    dt = max(0.001, now - _net_t)
    rate = (total - _net_prev) / dt / 1_000_000.0
    _net_prev, _net_t = total, now
    return max(0.0, rate)


def gpu_stats():
    if GPUtil:
        try:
            g = GPUtil.getGPUs()
            if g:
                return int(g[0].load * 100), int(g[0].temperature)
        except Exception:
            pass
    return 0, 0


def push_state(ser, handles):
    disp, h = build_audio()
    handles[:] = h

    v = ";".join(f"{n},{vol},{1 if m else 0}" for (n, vol, m) in disp[:8])
    ser.write(("V|" + v + "\n").encode())

    gpu, temp = gpu_stats()
    s = (f"cpu={int(psutil.cpu_percent())};ram={int(psutil.virtual_memory().percent)};"
         f"gpu={gpu};temp={temp};net={net_mbs():.1f}")
    ser.write(("S|" + s + "\n").encode())

    state, pos, length, title = vlc_now_playing()
    ser.write((f"N|{state};{pos};{length};{title}\n").encode())

    ser.flush()


def push_launchers(ser):
    ser.write(("L|" + ";".join(l[0] for l in LAUNCHERS) + "\n").encode())
    ser.flush()


# ----------------------- COMMAND ROUTER -----------------------
def handle(line, ser, handles):
    parts = line.split()
    if not parts:
        return
    cmd = parts[0]
    if cmd == "PONG":          # keep-alive reply, nothing to do
        return
    try:
        if cmd == "SET" and len(parts) >= 3:
            set_volume(handles, int(parts[1]), int(parts[2]))
        elif cmd == "MUTE" and len(parts) >= 2:
            toggle_mute(handles, int(parts[1]))
        elif cmd == "VOL" and len(parts) >= 2:
            nudge_master_volume(0.10 if parts[1] == "up" else -0.10)
        elif cmd == "BRI" and len(parts) >= 2:
            try:
                import screen_brightness_control as sbc
                sbc.set_brightness(int(parts[1]))
            except Exception as e:
                log(f"[Brightness] could not set monitor brightness: {e}")
        elif cmd == "MEDIA" and len(parts) >= 2:
            media_key(parts[1])
        elif cmd == "PWR" and len(parts) >= 2:
            power(parts[1])
        elif cmd == "LAUNCH" and len(parts) >= 2:
            launch(int(parts[1]))
        elif cmd == "VLC":
            do_vlc(parts)
    except Exception as e:
        log("command execution error:", line, e)


# ----------------------- SESSION LOOP -------------------
def serve(ser):
    """Talk to a connected DeskDeck until the link drops. Returns on disconnect."""
    handles = []
    push_launchers(ser)
    last_push = 0.0
    last_ping = 0.0
    last_rx = time.time()        # we just handshaked, so the link is alive now
    last_launch = time.time()

    while True:
        # --- read any incoming commands ---
        try:
            line = ser.readline().decode(errors="ignore").strip()
        except Exception as e:
            log("read error, dropping link:", e)
            return
        if line:
            last_rx = time.time()
            log("RX:", line)
            handle(line, ser, handles)

        now = time.time()

        # --- periodic keep-alive PING ---
        if now - last_ping >= PING_INTERVAL:
            try:
                ser.write(b"PING\n")
                ser.flush()
            except Exception as e:
                log("write error, dropping link:", e)
                return
            last_ping = now

        # --- liveness check: no reply for too long -> reconnect ---
        if now - last_rx > LINK_TIMEOUT:
            log("no reply within timeout, assuming disconnect")
            return

        # --- periodic state push ---
        if now - last_push >= PUSH_INTERVAL:
            try:
                push_state(ser, handles)
            except Exception as e:
                log("push error, dropping link:", e)
                return
            last_push = now

        if now - last_launch >= 10.0:
            try:
                push_launchers(ser)
            except Exception:
                pass
            last_launch = now


def run():
    comtypes.CoInitialize()
    log("DeskDeck companion started.")
    while True:
        try:
            ser = find_deskdeck()
            if not ser:
                log("Searching for DeskDeck Bluetooth link...")
                time.sleep(3)
                continue
            log("Connected.")
            try:
                serve(ser)            # blocks until the link drops
            finally:
                try:
                    ser.close()
                except Exception:
                    pass
            log("Link closed, rescanning...")
            time.sleep(2)
        except Exception as e:
            # Never let the background process die.
            log("unexpected error, recovering:", e)
            time.sleep(3)


if __name__ == "__main__":
    arg = sys.argv[1].lower() if len(sys.argv) > 1 else ""
    if arg == "--install":
        install_startup()
    elif arg == "--uninstall":
        uninstall_startup()
    else:
        run()
