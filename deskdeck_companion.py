"""
Desk Deck - Windows companion app
==================================
Talks to the ESP32 over the Bluetooth (SPP) serial port. It receives the
remote's commands and acts on Windows, and streams audio/stats/launcher
state back so the device's screens stay in sync.

SETUP
-----
1) Install Python 3, then the libraries:
       pip install pyserial pycaw comtypes psutil requests
   (GPU load/temp is optional NVIDIA-only:  pip install gputil )

2) Pair the ESP32 ("DeskDeck") in Windows Bluetooth settings. Then open
   "More Bluetooth settings" -> COM Ports tab and note the *Outgoing*
   "DeskDeck 'SPP'" port (e.g. COM5). Put it in COM_PORT below.
   (Tip: to list ports run:  python -m serial.tools.list_ports )

3) For VLC control: VLC -> Preferences -> show All -> Interface ->
   Main interfaces -> tick "Web". Under its sub-entry "Lua HTTP" set a
   password, and put the same password in VLC_PASSWORD below. Restart VLC.

4) Edit LAUNCHERS for your shortcuts, then run:
       python deskdeck_companion.py

PROTOCOL
--------
ESP32 -> PC : SET i v | MUTE i | PWR <a> | MEDIA <a> | LAUNCH i |
              VLC playpause|seek 10|seek -10|subtitle|audiotrack|fullscreen|pause
PC -> ESP32 : V|name,vol,muted;...   S|cpu=..;ram=..;gpu=..;temp=..;net=..   L|a;b;c
"""

import os, time, ctypes, subprocess

# ----------------------- CONFIG -----------------------
COM_PORT     = "COM5"            # the OUTGOING Bluetooth SPP port for DeskDeck
BAUD         = 115200
VLC_PORT     = 8080
VLC_PASSWORD = "vlc"             # the Lua HTTP password you set in VLC
LAUNCHERS = [                    # (label shown on device, what to open)
    ("VLC",      r"C:\Program Files\VideoLAN\VLC\vlc.exe"),
    ("Chrome",   r"C:\Program Files\Google\Chrome\Application\chrome.exe"),
    ("YouTube",  "https://youtube.com"),
    ("Notepad",  "notepad.exe"),
]
# ------------------------------------------------------

import serial
import requests
import psutil
from ctypes import cast, POINTER
from comtypes import CLSCTX_ALL
from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume

try:
    import GPUtil
except Exception:
    GPUtil = None

# Virtual-key codes for media keys
VK = {"playpause": 0xB3, "next": 0xB0, "prev": 0xB1, "mute": 0xAD}


# ----------------------- audio (pycaw) -----------------------
def master_endpoint():
    spk = AudioUtilities.GetSpeakers()
    iface = spk.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
    return cast(iface, POINTER(IAudioEndpointVolume))


def build_audio():
    """Return (display_list, handles). Index 0 is Master, then app sessions."""
    display, handles = [], []
    try:
        m = master_endpoint()
        display.append(("Master", round(m.GetMasterVolumeLevelScalar() * 100), bool(m.GetMute())))
        handles.append(("master", m))
    except Exception:
        pass
    try:
        for s in AudioUtilities.GetAllSessions():
            if not s.Process:
                continue
            try:
                v = s.SimpleAudioVolume
                name = s.Process.name().replace(".exe", "")
                display.append((name[:15], round(v.GetMasterVolume() * 100), bool(v.GetMute())))
                handles.append(("app", v))
            except Exception:
                continue
            if len(handles) >= 8:
                break
    except Exception:
        pass
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
        except Exception:
            pass


def toggle_mute(handles, i):
    if 0 <= i < len(handles):
        kind, h = handles[i]
        try:
            cur = h.GetMute()
            h.SetMute(0 if cur else 1, None)
        except Exception:
            pass


# ----------------------- other actions -----------------------
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
    if sub == "playpause":   vlc("pl_pause")
    elif sub == "pause":     vlc("pl_forcepause")
    elif sub == "fullscreen":vlc("fullscreen")
    elif sub == "subtitle":  vlc("key", "subtitle-track")   # cycles incl. Off
    elif sub == "audiotrack":vlc("key", "audio-track")
    elif sub == "seek":
        amt = parts[2] if len(parts) > 2 else "10"
        if not amt.startswith("-"):
            amt = "+" + amt
        vlc("seek", amt)


# ----------------------- state -> ESP32 -----------------------
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
    s = f"cpu={int(psutil.cpu_percent())};ram={int(psutil.virtual_memory().percent)};gpu={gpu};temp={temp};net={net_mbs():.1f}"
    ser.write(("S|" + s + "\n").encode())


def push_launchers(ser):
    ser.write(("L|" + ";".join(l[0] for l in LAUNCHERS) + "\n").encode())


# ----------------------- command handling -----------------------
def handle(line, ser, handles):
    parts = line.split()
    if not parts:
        return
    cmd = parts[0]
    try:
        if cmd == "SET" and len(parts) >= 3:
            set_volume(handles, int(parts[1]), int(parts[2]))
        elif cmd == "MUTE" and len(parts) >= 2:
            toggle_mute(handles, int(parts[1]))
        elif cmd == "MEDIA" and len(parts) >= 2:
            media_key(parts[1])
        elif cmd == "PWR" and len(parts) >= 2:
            power(parts[1])
        elif cmd == "LAUNCH" and len(parts) >= 2:
            launch(int(parts[1]))
        elif cmd == "VLC":
            do_vlc(parts)
    except Exception as e:
        print("cmd error:", line, e)


# ----------------------- main loop -----------------------
def run():
    handles = []
    while True:
        try:
            ser = serial.Serial(COM_PORT, BAUD, timeout=0.05)
        except Exception as e:
            print(f"Waiting for {COM_PORT} ... ({e})")
            time.sleep(3)
            continue
        print(f"Connected on {COM_PORT}")
        push_launchers(ser)
        last_push, last_launch = 0, time.time()
        try:
            while True:
                line = ser.readline().decode(errors="ignore").strip()
                if line:
                    handle(line, ser, handles)
                now = time.time()
                if now - last_push >= 1.0:
                    push_state(ser, handles)
                    last_push = now
                if now - last_launch >= 10.0:
                    push_launchers(ser)
                    last_launch = now
        except Exception as e:
            print("Link lost, reconnecting...", e)
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(2)


if __name__ == "__main__":
    run()
