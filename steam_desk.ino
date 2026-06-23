/*
  ESP32 Desk Deck  -  Bluetooth VLC remote / system controller
  Display : 1.3" OLED (SH1106, 128x64, I2C)
  Input   : analog joystick (X, Y) + push button
  Link    : Bluetooth Classic (SPP) to a companion app on the PC

  Pages: VLC, Volume mixer, Media, System stats, Launcher, Settings, Power, Calibrate.
  Controls: up/down move, left/right = volume / VLC seek / brightness,
            button = select. Every page has an on-screen "< Back" row (no long-press).

  --------------------------------------------------------------------------
  SERIAL PROTOCOL (over Bluetooth SPP, newline-terminated)
  --------------------------------------------------------------------------
  PC -> ESP32   (push when state changes, ~1/sec)
    V|<name>,<vol>,<muted>;...        volume sessions (vol 0-100, muted 0/1)
    S|cpu=<n>;ram=<n>;gpu=<n>;temp=<n>;net=<f>
    L|<label>;<label>;...             launcher labels

  ESP32 -> PC   (on a user action)
    SET <i> <vol>   /  MUTE <i>
    PWR shutdown|restart|sleep|lock|logoff
    MEDIA playpause|next|prev|mute
    LAUNCH <i>
    VLC playpause | VLC seek 10 | VLC seek -10 | VLC subtitle | VLC audiotrack | VLC fullscreen

  Settings actions (Bluetooth toggle, brightness, restart, power off, re-pair, rotate)
  are LOCAL to the ESP32 and are NOT sent to the PC.
  --------------------------------------------------------------------------
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "BluetoothSerial.h"
#include "esp_gap_bt_api.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error "Enable Bluetooth: Tools > ... or use an ESP32 board with Bluetooth."
#endif

// ---------- OLED ----------
#define i2c_Address   0x3C
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------- Joystick ----------
#define JOY_X_PIN   34
#define JOY_Y_PIN   35
#define JOY_BTN_PIN 32
const bool INVERT_Y = false;
const bool INVERT_X = false;

// ---------- Tuning ----------
const int           DEADZONE        = 700;
const unsigned long NAV_DELAY_MS    = 350;
const unsigned long NAV_RATE_MS     = 150;
const unsigned long VOL_RATE_MS     = 70;    // volume / brightness repeat
const unsigned long SEEK_RATE_MS    = 350;   // VLC seek repeat
const int           VOL_STEP        = 4;
const unsigned long BTN_DEBOUNCE_MS = 180;
const unsigned long SLEEP_MS        = 60000; // auto-sleep idle timeout

// ---------- Bluetooth ----------
BluetoothSerial SerialBT;
const char* BT_NAME = "DeskDeck";

// ---------- Pages ----------
enum Page { PAGE_HOME, PAGE_VLC, PAGE_VOLUME, PAGE_MEDIA, PAGE_SYSTEM,
            PAGE_LAUNCHER, PAGE_SETTINGS, PAGE_POWER, PAGE_CALIBRATE, PAGE_ABOUT, PAGE_COUNT };
Page page = PAGE_HOME;

const char* HOME_ITEMS[] = {"VLC","Volume mixer","Media","System","Launcher","Settings","Power","Calibrate"};
const Page  HOME_PAGES[] = {PAGE_VLC,PAGE_VOLUME,PAGE_MEDIA,PAGE_SYSTEM,PAGE_LAUNCHER,PAGE_SETTINGS,PAGE_POWER,PAGE_CALIBRATE};
const int   HOME_COUNT   = 8;

const char* VLC_ITEMS[]  = {"Play / Pause","Subtitles","Audio track","Fullscreen"};
const char* MEDIA_ITEMS[]= {"Play / Pause","Next track","Previous","Mute"};
const char* MEDIA_CMDS[] = {"playpause","next","prev","mute"};
const char* POWER_ITEMS[]= {"Shutdown","Restart","Sleep","Lock","Log off"};
const char* POWER_CMDS[] = {"shutdown","restart","sleep","lock","logoff"};

// ---------- Live data from PC ----------
struct Session { char name[16]; uint8_t vol; bool muted; };
Session sessions[8]; int sessionCount = 0;
struct Stats { int cpu, ram, gpu, temp; float net; bool valid; } stats = {0,0,0,0,0,false};
char launchers[8][16]; int launcherCount = 0;

// ---------- Settings / state ----------
bool btOn = true, sleepOn = false, rot = false, subOn = false;
int  brightness = 80;
int  centerX = 2048, centerY = 2048;       // set by Calibrate
int  selByPage[PAGE_COUNT] = {0};
bool cfActive = false; int confirmSel = 0;  // generic confirm
int  cfKind = 0;                            // 0=PC power, 1=restart, 2=power off
bool asleep = false, booting = false;
unsigned long bootUntil = 0;
String toastMsg = ""; unsigned long toastUntil = 0;

// ---------- Input state ----------
int activeNavDir = 0; unsigned long nextNavTime = 0;
int activeSideDir = 0; unsigned long nextSideTime = 0;
bool btnPrev = false; unsigned long btnLock = 0;
int lastX = 2048, lastY = 2048;
unsigned long lastInput = 0;

// ---------- Animation ----------
float hlY = 12; int hlTarget = 12; bool hlShow = true; bool snapHL = true;
float slideX = 0;

// ---------- Serial buffer ----------
String rxBuf = "";

// ---------- Prototypes ----------
void send(const String& s);
void readBT(); void parseLine(String);
void parseVolume(String); void parseStats(String); void parseLaunchers(String);
void handleInput(); void navStep(int); void sideStep(int); void onSelect();
int  wrap(int,int); void enterPage(Page); void goHome();
void toast(const String&); void applyBrightness(); void clearBonds(); void deepSleep();
void render(); void drawStatusBar(); void invertRect(int,int,int,int);
void drawListPage(); void drawCalibrate(); void drawAbout(); void drawConfirm(); void drawToast();

struct Row { String label; uint8_t kind; int aux; int v; bool m; };
// kind: 0 nav, 1 act, 2 back, 3 vol, 4 info, 5 toggle, 6 level, 7 setact
Row rowList[16]; int rowN = 0;
void buildRows();

// ==========================================================================
void setup() {
  Serial.begin(115200);
  pinMode(JOY_BTN_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  delay(250);
  if (!display.begin(i2c_Address, true)) { Serial.println(F("OLED not found")); for(;;); }
  display.setTextWrap(false);
  display.setRotation(rot ? 2 : 0);
  applyBrightness();
  SerialBT.begin(BT_NAME);
  // boot splash
  booting = true; bootUntil = millis() + 900;
  lastInput = millis();
  goHome();
}

void loop() {
  if (booting && millis() > bootUntil) { booting = false; snapHL = true; goHome(); }
  readBT();
  handleInput();
  // auto-sleep
  if (!asleep && sleepOn && !booting && (millis() - lastInput > SLEEP_MS)) asleep = true;
  render();
  delay(15);
}

// ==========================================================================
// Bluetooth I/O
// ==========================================================================
void send(const String& s) { SerialBT.println(s); Serial.println(s); }

void readBT() {
  while (SerialBT.available()) {
    char c = SerialBT.read();
    if (c == '\n')      { parseLine(rxBuf); rxBuf = ""; }
    else if (c != '\r') { rxBuf += c; if (rxBuf.length() > 320) rxBuf = ""; }
  }
}
void parseLine(String line) {
  if (line.length() < 2 || line[1] != '|') return;
  char t = line[0]; String b = line.substring(2);
  if (t=='V') parseVolume(b); else if (t=='S') parseStats(b); else if (t=='L') parseLaunchers(b);
}
void parseVolume(String b) {
  sessionCount = 0; int s = 0;
  while (s < (int)b.length() && sessionCount < 8) {
    int sc = b.indexOf(';', s); String it = (sc==-1)?b.substring(s):b.substring(s,sc);
    int c1 = it.indexOf(','), c2 = it.indexOf(',', c1+1);
    if (c1>0 && c2>c1) {
      it.substring(0,c1).toCharArray(sessions[sessionCount].name, 16);
      sessions[sessionCount].vol   = constrain(it.substring(c1+1,c2).toInt(),0,100);
      sessions[sessionCount].muted = it.substring(c2+1).toInt() != 0;
      sessionCount++;
    }
    if (sc==-1) break; s = sc+1;
  }
  if (selByPage[PAGE_VOLUME] > sessionCount) selByPage[PAGE_VOLUME] = sessionCount; // clamp incl back row
}
void parseStats(String b) {
  int s = 0;
  while (s < (int)b.length()) {
    int sc = b.indexOf(';', s); String kv = (sc==-1)?b.substring(s):b.substring(s,sc);
    int eq = kv.indexOf('='); if (eq>0) { String k=kv.substring(0,eq),v=kv.substring(eq+1);
      if (k=="cpu") stats.cpu=v.toInt(); else if (k=="ram") stats.ram=v.toInt();
      else if (k=="gpu") stats.gpu=v.toInt(); else if (k=="temp") stats.temp=v.toInt();
      else if (k=="net") stats.net=v.toFloat(); }
    if (sc==-1) break; s = sc+1;
  }
  stats.valid = true;
}
void parseLaunchers(String b) {
  launcherCount = 0; int s = 0;
  while (s < (int)b.length() && launcherCount < 8) {
    int sc = b.indexOf(';', s); String it = (sc==-1)?b.substring(s):b.substring(s,sc);
    it.toCharArray(launchers[launcherCount], 16); launcherCount++;
    if (sc==-1) break; s = sc+1;
  }
}

// ==========================================================================
// Navigation / input
// ==========================================================================
int wrap(int v, int n) { if (n<=0) return 0; while (v<0) v+=n; return v % n; }

void buildRows() {
  rowN = 0;
  #define ADD(L,K,A) { rowList[rowN].label=L; rowList[rowN].kind=K; rowList[rowN].aux=A; rowN++; }
  if (page==PAGE_HOME) { for (int i=0;i<HOME_COUNT;i++) ADD(HOME_ITEMS[i],0,i); }
  else if (page==PAGE_VLC) {
    ADD("Play / Pause",1,0); ADD(String("Subtitles: ")+(subOn?"ON":"OFF"),1,1);
    ADD("Audio track",1,2); ADD("Fullscreen",1,3); ADD("\x11 Back",2,0);
  } else if (page==PAGE_VOLUME) {
    for (int i=0;i<sessionCount;i++){ rowList[rowN].label=sessions[i].name; rowList[rowN].kind=3;
      rowList[rowN].aux=i; rowList[rowN].v=sessions[i].vol; rowList[rowN].m=sessions[i].muted; rowN++; }
    ADD("\x11 Back",2,0);
  } else if (page==PAGE_MEDIA) {
    for (int i=0;i<4;i++) ADD(MEDIA_ITEMS[i],1,i); ADD("\x11 Back",2,0);
  } else if (page==PAGE_LAUNCHER) {
    for (int i=0;i<launcherCount;i++) ADD(launchers[i],1,i); ADD("\x11 Back",2,0);
  } else if (page==PAGE_POWER) {
    for (int i=0;i<5;i++) ADD(POWER_ITEMS[i],1,i); ADD("\x11 Back",2,0);
  } else if (page==PAGE_SETTINGS) {
    ADD(String("Bluetooth: ")+(btOn?"ON":"OFF"),5,0);
    { rowList[rowN].label="Brightness"; rowList[rowN].kind=6; rowList[rowN].v=brightness; rowN++; }
    ADD(String("Auto-sleep: ")+(sleepOn?"ON":"OFF"),5,1);
    ADD(String("Rotate 180: ")+(rot?"ON":"OFF"),5,2);
    ADD("Re-pair Bluetooth",7,0); ADD("Restart ESP32",7,1);
    ADD("Power off",7,2); ADD("About device",7,3); ADD("\x11 Back",2,0);
  } else if (page==PAGE_SYSTEM) {
    char l[22];
    snprintf(l,22,"CPU %d%%  RAM %d%%",stats.cpu,stats.ram); ADD(String(l),4,0);
    snprintf(l,22,"GPU %d%%  %dC",stats.gpu,stats.temp);     ADD(String(l),4,0);
    char nb[10]; dtostrf(stats.net,0,1,nb); snprintf(l,22,"NET %s MB/s",nb); ADD(String(l),4,0);
    ADD("\x11 Back",2,0);
  }
  #undef ADD
}

int selectableAfter(int from, int dir) {
  int n = rowN, i = from;
  for (int k=0;k<n;k++){ i = wrap(i+dir,n); if (rowList[i].kind != 4) return i; }
  return from;
}

void handleInput() {
  unsigned long now = millis();
  int xv = analogRead(JOY_X_PIN), yv = analogRead(JOY_Y_PIN);
  lastX = xv; lastY = yv;
  int dx = xv - centerX; if (INVERT_X) dx = -dx;
  int dy = yv - centerY; if (INVERT_Y) dy = -dy;
  bool pressed = (digitalRead(JOY_BTN_PIN) == LOW);
  bool active = (abs(dx) > DEADZONE) || (abs(dy) > DEADZONE) || pressed;

  // wake from auto-sleep on any input (and consume it)
  if (asleep) { if (active) { asleep = false; lastInput = now; } btnPrev = pressed; return; }
  if (active) lastInput = now;
  if (booting) { btnPrev = pressed; return; }

  bool vertical = abs(dy) >= abs(dx);

  // vertical navigation
  int navDir = 0;
  if (vertical && abs(dy) > DEADZONE) navDir = (dy > 0) ? -1 : +1;
  if (navDir != 0) {
    if (navDir != activeNavDir) { navStep(navDir); activeNavDir = navDir; nextNavTime = now + NAV_DELAY_MS; }
    else if (now >= nextNavTime) { navStep(navDir); nextNavTime = now + NAV_RATE_MS; }
  } else activeNavDir = 0;

  // horizontal (volume / seek / brightness) -- not while confirming or calibrating
  int sideDir = 0;
  if (!cfActive && page != PAGE_CALIBRATE && !vertical && abs(dx) > DEADZONE) sideDir = (dx > 0) ? +1 : -1;
  if (sideDir != 0) {
    unsigned long rate = (page == PAGE_VLC) ? SEEK_RATE_MS : VOL_RATE_MS;
    if (sideDir != activeSideDir) { sideStep(sideDir); activeSideDir = sideDir; nextSideTime = now + rate; }
    else if (now >= nextSideTime) { sideStep(sideDir); nextSideTime = now + rate; }
  } else activeSideDir = 0;

  // button (single click = select)
  if (pressed && !btnPrev && now > btnLock) { btnLock = now + BTN_DEBOUNCE_MS; onSelect(); }
  btnPrev = pressed;
}

void navStep(int dir) {
  if (cfActive) { confirmSel = 1 - confirmSel; return; }
  if (page == PAGE_CALIBRATE || page == PAGE_ABOUT) return;
  buildRows();
  selByPage[page] = selectableAfter(selByPage[page], dir);
}

void sideStep(int dir) {
  if (page == PAGE_VLC) { send(String("VLC seek ") + (dir>0?"10":"-10")); toast(dir>0?"+10s":"-10s"); return; }
  if (page == PAGE_VOLUME) {
    buildRows(); Row& r = rowList[selByPage[page]];
    if (r.kind == 3) { int i=r.aux; sessions[i].vol = constrain(sessions[i].vol + dir*VOL_STEP,0,100);
      send(String("SET ")+i+" "+sessions[i].vol); }
    return;
  }
  if (page == PAGE_SETTINGS) {
    buildRows(); Row& r = rowList[selByPage[page]];
    if (r.kind == 6) { brightness = constrain(brightness + dir*5, 5, 100); applyBrightness(); }
    return;
  }
}

void onSelect() {
  if (asleep) { asleep = false; return; }
  if (cfActive) {
    bool yes = (confirmSel == 1); cfActive = false; confirmSel = 0;
    if (!yes) return;
    if (cfKind == 0) { send(String("PWR ")+POWER_CMDS[selByPage[PAGE_POWER]]); toast("Sent"); goHome(); }
    else if (cfKind == 1) { ESP.restart(); }
    else if (cfKind == 2) { deepSleep(); }
    return;
  }
  buildRows(); Row& r = rowList[selByPage[page]];

  if (page == PAGE_HOME) { enterPage(HOME_PAGES[selByPage[page]]); return; }
  if (page == PAGE_CALIBRATE) { centerX = lastX; centerY = lastY; toast("Center set"); goHome(); return; }
  if (page == PAGE_ABOUT)    { enterPage(PAGE_SETTINGS); return; }
  if (r.kind == 2)           { goHome(); return; }            // Back

  if (page == PAGE_VLC) {
    switch (r.aux) {
      case 0: send("VLC playpause"); toast("Play/Pause"); break;
      case 1: subOn = !subOn; send("VLC subtitle"); toast(subOn?"Subs ON":"Subs OFF"); break;
      case 2: send("VLC audiotrack"); toast("Audio track"); break;
      case 3: send("VLC fullscreen"); toast("Fullscreen"); break;
    } return;
  }
  if (page == PAGE_VOLUME) { int i=r.aux; sessions[i].muted=!sessions[i].muted;
    send(String("MUTE ")+i); toast(sessions[i].muted?"Muted":"Unmuted"); return; }
  if (page == PAGE_MEDIA)  { send(String("MEDIA ")+MEDIA_CMDS[r.aux]); toast(MEDIA_ITEMS[r.aux]); return; }
  if (page == PAGE_LAUNCHER){ send(String("LAUNCH ")+r.aux); toast(String("Launch ")+launchers[r.aux]); return; }
  if (page == PAGE_POWER)  { cfActive=true; confirmSel=0; cfKind=0; return; }

  if (page == PAGE_SETTINGS) {
    if (r.kind == 5) {                 // toggles
      if (r.aux == 0) { btOn=!btOn; if (btOn) SerialBT.begin(BT_NAME); else SerialBT.end(); toast(btOn?"BT ON":"BT OFF"); }
      else if (r.aux == 1) { sleepOn=!sleepOn; toast(sleepOn?"Sleep ON":"Sleep OFF"); }
      else if (r.aux == 2) { rot=!rot; display.setRotation(rot?2:0); toast("Rotated"); }
    } else if (r.kind == 7) {          // actions
      if (r.aux == 0) { clearBonds(); toast("Re-paired"); }
      else if (r.aux == 1) { cfActive=true; confirmSel=0; cfKind=1; }   // restart
      else if (r.aux == 2) { cfActive=true; confirmSel=0; cfKind=2; }   // power off
      else if (r.aux == 3) { enterPage(PAGE_ABOUT); }
    }
    return;
  }
  if (page == PAGE_SYSTEM) { goHome(); return; }
}

void enterPage(Page p) {
  page = p; cfActive = false; confirmSel = 0; snapHL = true; slideX = 14;
  selByPage[p] = 0;
  if (p == PAGE_SYSTEM)   selByPage[p] = 3;          // land on Back
  if (p == PAGE_SETTINGS) selByPage[p] = 0;
}
void goHome() { enterPage(PAGE_HOME); }

void toast(const String& m) { toastMsg = m; toastUntil = millis() + 1100; }
void applyBrightness() { display.setContrast(map(brightness, 0, 100, 0, 255)); }

void clearBonds() {
  int n = esp_bt_gap_get_bond_device_num();
  if (n <= 0) return;
  esp_bd_addr_t* list = (esp_bd_addr_t*) malloc(sizeof(esp_bd_addr_t) * n);
  if (!list) return;
  if (esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK)
    for (int i = 0; i < n; i++) esp_bt_gap_remove_bond_device(list[i]);
  free(list);
}

void deepSleep() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  display.setCursor(28, 28); display.print("Powered off");
  display.display(); delay(500);
  display.oled_command(0xAE);                          // panel off
  rtc_gpio_pullup_en((gpio_num_t)JOY_BTN_PIN);         // keep button pin pulled high
  rtc_gpio_pulldown_dis((gpio_num_t)JOY_BTN_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)JOY_BTN_PIN, 0); // wake on button press (LOW)
  esp_deep_sleep_start();
}

// ==========================================================================
// Rendering
// ==========================================================================
void invertRect(int x, int y, int w, int h) {
  uint8_t* buf = display.getBuffer();
  for (int yy = y; yy < y + h; yy++) {
    if (yy < 0 || yy >= SCREEN_HEIGHT) continue;
    int page8 = yy >> 3; uint8_t bit = 1 << (yy & 7);
    for (int xx = x; xx < x + w; xx++) {
      if (xx < 0 || xx >= SCREEN_WIDTH) continue;
      buf[xx + page8 * SCREEN_WIDTH] ^= bit;
    }
  }
}

void render() {
  display.clearDisplay();
  if (asleep) { display.display(); return; }
  if (booting) {
    display.setTextColor(SH110X_WHITE); display.setTextSize(2);
    display.setCursor(28, 16); display.print("ESP32");
    display.setTextSize(1); display.setCursor(40, 44); display.print("starting");
    display.display(); return;
  }
  drawStatusBar();

  if      (page == PAGE_CALIBRATE) drawCalibrate();
  else if (page == PAGE_ABOUT)     drawAbout();
  else                             drawListPage();

  if (cfActive) drawConfirm();
  if (millis() < toastUntil) drawToast();
  display.display();
}

void drawStatusBar() {
  display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  // Bluetooth indicator (left)
  display.setCursor(0, 1); display.print(btOn ? "BT" : "--");
  if (btOn) {
    bool conn = SerialBT.hasClient();
    int bars = conn ? 3 : 1;               // RSSI not exposed by SPP API; show link state
    for (int i = 0; i < 3; i++) {
      int bx = 14 + i*3, bh = 2 + i*2, by = 8 - bh;
      if (i < bars) display.fillRect(bx, by, 2, bh, SH110X_WHITE);
      else          display.drawRect(bx, by, 2, bh, SH110X_WHITE);
    }
  }
  // Temperature (right)
  char t[8]; snprintf(t, 8, "%dC", stats.temp);
  int16_t bx, by; uint16_t bw, bh; display.getTextBounds(t, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor(SCREEN_WIDTH - bw - 1, 1); display.print(t);
  // Title (center)
  const char* titles[] = {"Main menu","VLC","Volume mixer","Media","System","Launcher","Settings","Power","Calibrate","About"};
  const char* ti = titles[page];
  display.getTextBounds(ti, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor((SCREEN_WIDTH - bw) / 2, 1); display.print(ti);
  display.drawLine(0, 10, SCREEN_WIDTH, 10, SH110X_WHITE);
}

void drawListPage() {
  buildRows();
  const int rowH = 12, top = 12, vis = 4;
  int sel = selByPage[page];
  int start = sel >= vis ? sel - vis + 1 : 0;
  start = min(start, max(0, rowN - vis));
  if (slideX > 0.5) slideX *= 0.6; else slideX = 0;
  int sx = (int)slideX;

  display.setTextSize(1);
  for (int i = 0; i < vis && start + i < rowN; i++) {
    Row& r = rowList[start + i];
    int y = top + i * rowH;
    display.setTextColor(SH110X_WHITE);
    display.setCursor(4 + sx, y + 2);
    String lbl = r.label; if (lbl.length() > 12) lbl = lbl.substring(0, 12);
    display.print(lbl);
    if (r.kind == 3 || r.kind == 6) {                 // volume / brightness bar
      int bx = 70 + sx, bw = 36, by = y + 3, bh = 6;
      int val = (r.kind == 3) ? r.v : brightness;
      bool muted = (r.kind == 3 && r.m);
      if (muted) {
        display.drawRect(bx, by, bw, bh, SH110X_WHITE);
        display.drawLine(bx, by + bh, bx + bw, by, SH110X_WHITE);
        display.setCursor(108 + sx, y + 2); display.print("M");
      } else {
        display.drawRect(bx, by, bw, bh, SH110X_WHITE);
        int fw = (bw - 2) * val / 100; if (fw > 0) display.fillRect(bx + 1, by + 1, fw, bh - 2, SH110X_WHITE);
        display.setCursor(108 + sx, y + 2); display.print(val);
      }
    }
  }
  // scrollbar
  if (rowN > vis) {
    int h = 48; display.drawLine(125, 12, 125, 12 + h, SH110X_WHITE);
    int bh = max(4, h * vis / rowN);
    int by = 12 + (h - bh) * start / max(1, rowN - vis);
    display.fillRect(124, by, 3, bh, SH110X_WHITE);
  }
  // animated highlight (XOR so it inverts text + bars while it slides)
  hlShow = true;
  hlTarget = top + (sel - start) * rowH;
  if (snapHL) { hlY = hlTarget; snapHL = false; }
  else hlY += (hlTarget - hlY) * 0.4;
  invertRect(0, (int)(hlY + 0.5), SCREEN_WIDTH, rowH);
}

void drawCalibrate() {
  int dx = lastX - centerX, dy = lastY - centerY;
  bool ok = abs(dx) < 220 && abs(dy) < 220;
  int bx = 4, by = 12, bs = 48;
  display.drawRect(bx, by, bs, bs, SH110X_WHITE);
  display.drawLine(bx + bs/2, by, bx + bs/2, by + bs, SH110X_WHITE);
  display.drawLine(bx, by + bs/2, bx + bs, by + bs/2, SH110X_WHITE);
  int px = bx + (int)((long)lastX * (bs - 4) / 4095);
  int py = by + (int)((4095 - (long)lastY) * (bs - 4) / 4095);
  display.fillCircle(px + 2, py + 2, 2, SH110X_WHITE);
  display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  display.setCursor(60, 14); display.print("X "); display.print(lastX);
  display.setCursor(60, 26); display.print("Y "); display.print(lastY);
  display.setCursor(60, 38); display.print(ok ? "centered" : "off-center");
  display.setCursor(4, 54);  display.print("Press = set center");
}

void drawAbout() {
  display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  display.setCursor(4, 14); display.print("Desk Deck v0.3");
  display.setCursor(4, 26); display.print("BT "); display.print(BT_NAME);
  display.setCursor(4, 38); display.print("link ");
  display.print(SerialBT.hasClient() ? "connected" : "waiting");
  display.setCursor(4, 54); display.print("Press = back");
}

void drawConfirm() {
  const char* titles[] = {"Send to PC?","Restart ESP32?","Power off?"};
  display.fillRect(14, 16, 100, 34, SH110X_BLACK);
  display.drawRect(14, 16, 100, 34, SH110X_WHITE);
  display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  display.setCursor(18, 20); display.print(titles[cfKind]);
  display.drawRect(22, 36, 34, 12, SH110X_WHITE); display.setCursor(32, 38); display.print("No");
  display.drawRect(72, 36, 34, 12, SH110X_WHITE); display.setCursor(80, 38); display.print("Yes");
  if (confirmSel == 0) invertRect(22, 36, 34, 12); else invertRect(72, 36, 34, 12);
}

void drawToast() {
  int w = toastMsg.length() * 6 + 10;
  int x = (SCREEN_WIDTH - w) / 2;
  display.fillRoundRect(x, 48, w, 14, 3, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK); display.setTextSize(1);
  display.setCursor(x + 5, 51); display.print(toastMsg);
}
