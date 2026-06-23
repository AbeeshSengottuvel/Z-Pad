/*
  ESP32 Desk Deck  -  Bluetooth + WiFi VLC remote / system controller
  Display : 1.3" OLED (SH1106, 128x64, I2C)
  Input   : analog joystick (X, Y) + push button
  PC link : Bluetooth Classic (SPP) to a companion app

  CONTROLS
    up / down     move the highlight
    left          back (up one level)
    right         open / enter / fire
    press         select; on a value (volume, brightness, seek) press to
                  START editing, joystick changes it, press again to finish
                  while editing a volume row, up/down toggles mute

  FIRST-TIME WiFi
    On first boot (no saved networks) the device starts a hotspot
    "DeskDeck-Setup" (password below). Join it from a phone and open
    192.168.4.1 to add your home network. After that it auto-connects and
    the web page is reachable at the LAN IP shown in Settings > WiFi setup.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "BluetoothSerial.h"
#include "esp_gap_bt_api.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <time.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error "Enable Bluetooth (use an ESP32 with Bluetooth Classic)."
#endif

// ---------- OLED ----------
#define i2c_Address   0x3C
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------- Joystick ----------
#define JOY_X_PIN    34
#define JOY_Y_PIN    35
#define JOY_BTN_PIN 32
const bool INVERT_X = false, INVERT_Y = false;

// ---------- Tuning ----------
const int           DEADZONE        = 700;
const unsigned long NAV_DELAY_MS    = 350, NAV_RATE_MS = 150;
const unsigned long EDIT_DELAY_MS   = 220, EDIT_RATE_MS = 70;
const int           VOL_STEP        = 4, BRI_STEP = 5;
const unsigned long BTN_DEBOUNCE_MS = 160;
const unsigned long SLEEP_MS        = 60000;

// ---------- Identity / config ----------
const char* BT_NAME = "DeskDeck";
const char* AP_SSID = "DeskDeck-Setup";
const char* AP_PASS = "deskdeck123";          // >= 8 chars
const long  GMT_OFFSET_SEC = 19800;            // IST UTC+5:30
const int   DST_OFFSET_SEC = 0;
uint8_t     PC_MAC[6] = {0xDE,0xAD,0xBE,0xEF,0x12,0x34};  // <-- your PC's MAC (Wake-on-LAN)
const char* OTA_HOST = "deskdeck";
const char* OTA_PASS = "deskdeck-ota";

// ---------- Objects ----------
BluetoothSerial SerialBT;
WiFiMulti wifiMulti;
WiFiUDP   udp;
WebServer server(80);
Preferences prefs;

// ---------- Settings state ----------
bool btOn = true, wifiOn = true, sleepOn = false, rot = false, subOn = false;
int  brightness = 80;

// ---------- WiFi data ----------
#define MAX_NETS 6
struct Net { String ssid; String pass; };
Net savedNets[MAX_NETS]; int savedCount = 0;
struct Scan { char ssid[33]; int rssi; bool saved; };
Scan scanList[12]; int scanCount = 0; int scanState = 0;   // 0 idle, 1 scanning
bool wifiUp = false; String webIP = "off";
bool connecting = false; unsigned long connectUntil = 0; String connectSSID = "";
unsigned long nextWifiCheck = 0, nextClock = 0;
char clockStr[6] = "--:--";

// ---------- PC data ----------
struct Session { char name[16]; uint8_t vol; bool muted; };
Session sessions[8]; int sessionCount = 0;
struct Stats { int cpu, ram, gpu, temp; float net; bool valid; } stats = {0,0,0,0,0,false};
char launchers[8][16]; int launcherCount = 0;

// ---------- Menu data ----------
enum { P_HOME, P_VLC, P_VOLUME, P_MEDIA, P_SYSTEM, P_LAUNCHER, P_SETTINGS, P_POWER, P_WIFI, P_CALIBRATE, P_ABOUT, P_N };
int page = P_HOME;
int selByPage[P_N] = {0};

const char* HOME_L[] = {"VLC","Volume mixer","Media","System","Launcher","Settings","Power","Calibrate"};
const int   HOME_T[] = {P_VLC,P_VOLUME,P_MEDIA,P_SYSTEM,P_LAUNCHER,P_SETTINGS,P_POWER,P_CALIBRATE};
const int   HOME_N   = 8;
const char* MEDIA_L[] = {"Play / Pause","Next track","Previous","Mute"};
const char* MEDIA_C[] = {"playpause","next","prev","mute"};
const char* POWER_L[] = {"Shutdown","Restart","Sleep","Lock","Log off"};
const char* POWER_C[] = {"shutdown","restart","sleep","lock","logoff"};
const char* STITLE[]  = {"Menu","VLC","Volume","Media","System","Apps","Settings","Power","WiFi","Calib","About"};

// ---------- Modes / confirm / sleep timer ----------
bool editing = false;
bool cfActive = false; int confirmSel = 0; int cfType = 0; String cfArg = ""; String cfTitle = "";
enum { CF_PWR, CF_RESTART, CF_POWEROFF };
int sleepTimerMin = 0; unsigned long sleepTimerEnd = 0; int stIdx = 0;
const int ST_OPTS[] = {0,15,30,45,60,90};
bool asleep = false, booting = false; unsigned long bootUntil = 0;
String toastMsg = ""; unsigned long toastUntil = 0;
int centerX = 2048, centerY = 2048;

// ---------- Input state ----------
int prevVd = 0, prevHd = 0; unsigned long vNext = 0, hNext = 0;
bool btnPrev = false; unsigned long btnLock = 0;
int lastX = 2048, lastY = 2048; unsigned long lastInput = 0;

// ---------- Animation ----------
float hlY = 12; bool snapHL = true; float slideX = 0;

// ---------- Serial ----------
String rxBuf = "";

// ---------- Row model ----------
#define K_GO 0
#define K_ACT 1
#define K_TG 2
#define K_VAL 3
#define K_LVL 4
#define K_SCRUB 5
#define K_CYC 6
#define K_NET 7
#define K_WEB 8
struct Row { String label; int kind; int i; String code; int v; bool m; bool saved; bool on; int str; };
Row rowList[16]; int rowN = 0;

// ---------- Prototypes ----------
void send(const String&); void readBT(); void parseLine(String);
void parseVolume(String); void parseStats(String); void parseLaunchers(String);
void handleInput(); void fireV(int); void fireH(int); void onButton(); void rightPress();
void navMove(int); void doRow(bool); void doToggle(String); void doAct(String,int);
void wifiRowAction(Row&); void enterPage(int); void goHome(); void goBack(); int parentPage(int);
bool isDisplay(int); int wrapi(int,int); void toast(const String&); String stLabel();
void cfBegin(String,int,String); void resolveConfirm(); void adjustH(int); void editV();
void cycleSleepTimer(); void checkSleepTimer(); void deepSleep(); void clearBonds();
void applyBrightness(); void sendWOL(); void updateClock();
void loadNets(); void saveNets(); void addOrUpdateNet(String,String); void forgetNet(String);
bool isSaved(String); String passFor(String); void applyMulti();
void wifiStart(); void wifiStop(); void updateWebIP(); void startScan(); void pollScan(); int rssiBars(int);
void buildRows();
void render(); void drawStatusBar(); void invertRect(int,int,int,int);
void drawList(); void drawSystem(); void drawCalibrate(); void drawAbout();
void drawConfirm(); void drawToast();
void handleRoot(); void handleSave(); void handleForget(); String buildPage();

// ==========================================================================
void setup() {
  Serial.begin(115200);
  pinMode(JOY_BTN_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  delay(250);
  if (!display.begin(i2c_Address, true)) { Serial.println(F("OLED not found")); for(;;); }
  Wire.setClock(400000);            // 400 kHz I2C -> ~4x faster screen refresh
  display.setTextWrap(false);
  display.setRotation(rot ? 2 : 0);
  applyBrightness();
  if (btOn) SerialBT.begin(BT_NAME);
  loadNets();
  if (wifiOn) wifiStart();
  booting = true; bootUntil = millis() + 900; lastInput = millis();
  goHome();
}

void loop() {
  if (booting && millis() > bootUntil) { booting = false; snapHL = true; goHome(); }
  if (btOn) readBT();
  if (wifiOn) {
    server.handleClient();
    ArduinoOTA.handle();
    pollScan();
    if (millis() > nextWifiCheck) { nextWifiCheck = millis() + 4000; updateWebIP(); }
  }
  if (connecting && (WiFi.status() == WL_CONNECTED || millis() > connectUntil)) {
    connecting = false; updateWebIP();
    toast(WiFi.status() == WL_CONNECTED ? "Connected" : "Failed");
  }
  handleInput();
  updateClock();
  checkSleepTimer();
  if (!asleep && sleepOn && !booting && (millis() - lastInput > SLEEP_MS)) asleep = true;
  render();
  delay(2);
}

// ==========================================================================
// Bluetooth I/O
// ==========================================================================
void send(const String& s) { if (btOn) SerialBT.println(s); Serial.println(s); }
void readBT() {
  while (SerialBT.available()) { char c = SerialBT.read();
    if (c == '\n') { parseLine(rxBuf); rxBuf = ""; }
    else if (c != '\r') { rxBuf += c; if (rxBuf.length() > 320) rxBuf = ""; } }
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
      sessions[sessionCount].vol = constrain(it.substring(c1+1,c2).toInt(),0,100);
      sessions[sessionCount].muted = it.substring(c2+1).toInt()!=0; sessionCount++; }
    if (sc==-1) break; s = sc+1; }
}
void parseStats(String b) {
  int s = 0;
  while (s < (int)b.length()) {
    int sc = b.indexOf(';', s); String kv = (sc==-1)?b.substring(s):b.substring(s,sc);
    int eq = kv.indexOf('='); if (eq>0){ String k=kv.substring(0,eq),v=kv.substring(eq+1);
      if(k=="cpu")stats.cpu=v.toInt(); else if(k=="ram")stats.ram=v.toInt();
      else if(k=="gpu")stats.gpu=v.toInt(); else if(k=="temp")stats.temp=v.toInt();
      else if(k=="net")stats.net=v.toFloat(); }
    if (sc==-1) break; s = sc+1; }
  stats.valid = true;
}
void parseLaunchers(String b) {
  launcherCount = 0; int s = 0;
  while (s < (int)b.length() && launcherCount < 8) {
    int sc = b.indexOf(';', s); String it = (sc==-1)?b.substring(s):b.substring(s,sc);
    it.toCharArray(launchers[launcherCount], 16); launcherCount++;
    if (sc==-1) break; s = sc+1; }
}

// ==========================================================================
// Input
// ==========================================================================
int wrapi(int v, int n) { if (n<=0) return 0; while (v<0) v+=n; return v%n; }

void handleInput() {
  unsigned long now = millis();
  int xv = analogRead(JOY_X_PIN), yv = analogRead(JOY_Y_PIN);
  lastX = xv; lastY = yv;
  int dx = xv - centerX; if (INVERT_X) dx = -dx;
  int dy = yv - centerY; if (INVERT_Y) dy = -dy;
  bool pressed = (digitalRead(JOY_BTN_PIN) == LOW);
  bool any = (abs(dx) > DEADZONE) || (abs(dy) > DEADZONE) || pressed;

  if (asleep) { if (any) { asleep = false; lastInput = now; } btnPrev = pressed; return; }
  if (any) lastInput = now;
  if (booting) { btnPrev = pressed; return; }

  bool horiz = abs(dx) >= abs(dy);
  int hd = (horiz && dx>DEADZONE)? +1 : (horiz && dx<-DEADZONE)? -1 : 0;
  int vd = (!horiz && dy>DEADZONE)? +1 : (!horiz && dy<-DEADZONE)? -1 : 0;   // +1 = up

  if (vd != 0) {
    bool rep = !editing;
    if (vd != prevVd) { fireV(vd); vNext = now + NAV_DELAY_MS; }
    else if (rep && now >= vNext) { fireV(vd); vNext = now + NAV_RATE_MS; }
  }
  prevVd = vd;

  if (hd != 0) {
    bool rep = editing;
    if (hd != prevHd) { fireH(hd); hNext = now + EDIT_DELAY_MS; }
    else if (rep && now >= hNext) { fireH(hd); hNext = now + EDIT_RATE_MS; }
  }
  prevHd = hd;

  if (pressed && !btnPrev && now > btnLock) { btnLock = now + BTN_DEBOUNCE_MS; onButton(); }
  btnPrev = pressed;
}

void fireV(int vd) {
  if (cfActive) { confirmSel = 1 - confirmSel; return; }
  if (editing)  { editV(); return; }
  if (page == P_CALIBRATE) return;
  navMove(vd > 0 ? -1 : +1);     // up = move selection up
}
void fireH(int hd) {
  if (cfActive) { confirmSel = 1 - confirmSel; return; }
  if (editing)  { adjustH(hd); return; }
  if (page == P_CALIBRATE) return;
  if (hd > 0) rightPress(); else goBack();
}
void onButton() {
  if (asleep)   { asleep = false; return; }
  if (booting)  return;
  if (cfActive) { resolveConfirm(); return; }
  if (editing)  { editing = false; toast("Saved"); return; }
  if (page == P_CALIBRATE) { centerX = lastX; centerY = lastY; toast("Center set"); goBack(); return; }
  if (isDisplay(page)) { goBack(); return; }
  doRow(true);
}
void rightPress() { if (isDisplay(page)) return; doRow(false); }

void navMove(int dir) { if (isDisplay(page)) return; buildRows(); if (rowN==0) return;
  selByPage[page] = wrapi(selByPage[page] + dir, rowN); }

void doRow(bool allowEdit) {
  buildRows(); if (rowN == 0) return; Row& r = rowList[selByPage[page]];
  switch (r.kind) {
    case K_GO:    enterPage(r.i); break;
    case K_TG:    doToggle(r.code); break;
    case K_CYC:   cycleSleepTimer(); break;
    case K_NET:   wifiRowAction(r); break;
    case K_WEB:   toast(String("Open ") + webIP); break;
    case K_ACT:   doAct(r.code, r.i); break;
    case K_VAL: case K_LVL: case K_SCRUB: if (allowEdit) { editing = true; toast("Editing"); } break;
  }
}

void doToggle(String c) {
  if (c=="wifi") { wifiOn = !wifiOn; if (wifiOn) wifiStart(); else wifiStop(); toast(String("WiFi ")+(wifiOn?"ON":"OFF")); }
  else if (c=="bt") { btOn = !btOn; if (btOn) SerialBT.begin(BT_NAME); else SerialBT.end(); toast(String("Bluetooth ")+(btOn?"ON":"OFF")); }
  else if (c=="sub") { subOn = !subOn; send("VLC subtitle"); toast(String("Subtitles ")+(subOn?"ON":"OFF")); }
  else if (c=="sleep") { sleepOn = !sleepOn; toast(String("Auto-sleep ")+(sleepOn?"ON":"OFF")); }
  else if (c=="rot") { rot = !rot; display.setRotation(rot?2:0); toast("Rotated"); }
}
void doAct(String c, int i) {
  if (c=="vlc_pp") { send("VLC playpause"); toast("Play / Pause"); }
  else if (c=="vlc_audio") { send("VLC audiotrack"); toast("Audio track"); }
  else if (c=="vlc_full")  { send("VLC fullscreen"); toast("Fullscreen"); }
  else if (c.startsWith("media:")) { send(String("MEDIA ")+c.substring(6)); toast("Media"); }
  else if (c=="launch") { send(String("LAUNCH ")+i); toast(String("Launch ")+launchers[i]); }
  else if (c=="wol") { sendWOL(); }
  else if (c.startsWith("pwr:")) { String cmd=c.substring(4); cfBegin(cmd+"?", CF_PWR, cmd); }
  else if (c=="repair") { clearBonds(); toast("Re-paired"); }
  else if (c=="restart") { cfBegin("Restart ESP32?", CF_RESTART, ""); }
  else if (c=="poweroff") { cfBegin("Power off?", CF_POWEROFF, ""); }
}
void wifiRowAction(Row& r) {
  if (r.on) { toast("Already on"); return; }
  if (r.saved) { String ss = scanList[r.i].ssid; connectSSID = ss; connecting = true;
    connectUntil = millis()+9000; WiFi.begin(ss.c_str(), passFor(ss).c_str()); toast("Connecting..."); }
  else { toast(String("Add in browser ") + webIP); }
}

void cfBegin(String title, int type, String arg) { cfActive=true; confirmSel=0; cfType=type; cfArg=arg; cfTitle=title; }
void resolveConfirm() {
  bool yes = (confirmSel == 1); cfActive = false; if (!yes) return;
  if (cfType==CF_PWR) { send(String("PWR ")+cfArg); toast("Sent"); goHome(); }
  else if (cfType==CF_RESTART) { ESP.restart(); }
  else if (cfType==CF_POWEROFF) { deepSleep(); }
}
void adjustH(int dir) {
  buildRows(); Row& r = rowList[selByPage[page]];
  if (r.kind==K_VAL) { int i=r.i; sessions[i].vol = constrain(sessions[i].vol+dir*VOL_STEP,0,100); send(String("SET ")+i+" "+sessions[i].vol); }
  else if (r.kind==K_LVL) { 
    brightness = constrain(brightness+dir*BRI_STEP,5,100); 
    applyBrightness(); 
    send(String("BRI ") + brightness); // Updated to target modern PC display structures over serial pipeline
  }
  else if (r.kind==K_SCRUB) { send(String("VLC seek ")+(dir>0?"10":"-10")); toast(dir>0?"+10s":"-10s"); }
}
void editV() {
  buildRows(); Row& r = rowList[selByPage[page]];
  if (r.kind==K_VAL) { int i=r.i; sessions[i].muted=!sessions[i].muted; send(String("MUTE ")+i); toast(sessions[i].muted?"Muted":"Unmuted"); }
}

void enterPage(int p) { page=p; editing=false; cfActive=false; snapHL=true; slideX=14; selByPage[p]=0; if (p==P_WIFI) startScan(); }
void goHome() { enterPage(P_HOME); }
void goBack() { int p = parentPage(page); if (p>=0) { page=p; editing=false; cfActive=false; snapHL=true; slideX=14; } }
int  parentPage(int p) { if (p==P_ABOUT || p==P_WIFI) return P_SETTINGS; if (p==P_HOME) return -1; return P_HOME; }
bool isDisplay(int p) { return p==P_SYSTEM || p==P_CALIBRATE || p==P_ABOUT; }

void toast(const String& m) { toastMsg = m; toastUntil = millis() + 1100; }
void applyBrightness() { display.setContrast(map(brightness, 0, 100, 0, 255)); }
String stLabel() { if (sleepTimerMin==0) return "Sleep timer: OFF";
  long rem = (long)(sleepTimerEnd - millis())/60000 + 1; return String("Sleep timer: ")+rem+"m"; }
void cycleSleepTimer() {
  stIdx = (stIdx+1) % (int)(sizeof(ST_OPTS)/sizeof(ST_OPTS[0]));
  sleepTimerMin = ST_OPTS[stIdx];
  if (sleepTimerMin==0) { sleepTimerEnd=0; toast("Timer off"); }
  else { sleepTimerEnd = millis()+(unsigned long)sleepTimerMin*60000UL; toast(String(sleepTimerMin)+"m timer"); }
}
void checkSleepTimer() {
  if (sleepTimerMin==0 || sleepTimerEnd==0) return;
  if (millis() >= sleepTimerEnd) { send("VLC pause"); sleepTimerMin=0; sleepTimerEnd=0; stIdx=0;
    toast("Paused (timer)"); if (sleepOn) asleep = true; }
}

void clearBonds() {
  int n = esp_bt_gap_get_bond_device_num(); if (n<=0) return;
  esp_bd_addr_t* list = (esp_bd_addr_t*) malloc(sizeof(esp_bd_addr_t)*n); if (!list) return;
  if (esp_bt_gap_get_bond_device_list(&n, list)==ESP_OK)
    for (int i=0;i<n;i++) esp_bt_gap_remove_bond_device(list[i]);
  free(list);
}
void deepSleep() {
  display.clearDisplay(); display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  display.setCursor(28,28); display.print("Powered off"); display.display(); delay(500);
  display.oled_command(0xAE);
  rtc_gpio_pullup_en((gpio_num_t)JOY_BTN_PIN); rtc_gpio_pulldown_dis((gpio_num_t)JOY_BTN_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)JOY_BTN_PIN, 0);
  esp_deep_sleep_start();
}
void sendWOL() {
  if (WiFi.status()!=WL_CONNECTED) { toast("No WiFi"); return; }
  uint8_t pkt[102]; for (int i=0;i<6;i++) pkt[i]=0xFF;
  for (int i=0;i<16;i++) memcpy(pkt+6+i*6, PC_MAC, 6);
  udp.beginPacket(IPAddress(255,255,255,255), 9); udp.write(pkt, sizeof(pkt)); udp.endPacket();
  toast("Wake sent");
}
void updateClock() {
  if (!wifiOn || WiFi.status()!=WL_CONNECTED || millis()<nextClock) return;
  nextClock = millis()+1000; struct tm ti;
  if (getLocalTime(&ti, 5)) strftime(clockStr, sizeof(clockStr), "%H:%M", &ti);
}

// ==========================================================================
// WiFi credentials (NVS) + multi + scan + web server
// ==========================================================================
void loadNets() {
  prefs.begin("wifi", true); savedCount = prefs.getInt("n", 0);
  if (savedCount > MAX_NETS) savedCount = MAX_NETS;
  for (int i=0;i<savedCount;i++) {
    savedNets[i].ssid = prefs.getString(("s"+String(i)).c_str(), "");
    savedNets[i].pass = prefs.getString(("p"+String(i)).c_str(), ""); }
  prefs.end();
}
void saveNets() {
  prefs.begin("wifi", false); prefs.putInt("n", savedCount);
  for (int i=0;i<savedCount;i++) { prefs.putString(("s"+String(i)).c_str(), savedNets[i].ssid);
    prefs.putString(("p"+String(i)).c_str(), savedNets[i].pass); }
  prefs.end();
}
bool isSaved(String s) { for (int i=0;i<savedCount;i++) if (savedNets[i].ssid==s) return true; return false; }
String passFor(String s) { for (int i=0;i<savedCount;i++) if (savedNets[i].ssid==s) return savedNets[i].pass; return ""; }
void applyMulti() { for (int i=0;i<savedCount;i++) wifiMulti.addAP(savedNets[i].ssid.c_str(), savedNets[i].pass.c_str()); }
void addOrUpdateNet(String ss, String pw) {
  for (int i=0;i<savedCount;i++) if (savedNets[i].ssid==ss) { savedNets[i].pass=pw; saveNets(); applyMulti(); return; }
  if (savedCount<MAX_NETS) { savedNets[savedCount].ssid=ss; savedNets[savedCount].pass=pw; savedCount++; saveNets(); applyMulti(); }
}
void forgetNet(String ss) {
  int j=0; for (int i=0;i<savedCount;i++) if (savedNets[i].ssid!=ss) savedNets[j++]=savedNets[i];
  savedCount=j; saveNets();
}
int rssiBars(int r) { if (r>-60) return 3; if (r>-72) return 2; return 1; }
void startScan() { scanCount=0; scanState=1; WiFi.scanNetworks(true, false); }
void pollScan() {
  if (scanState!=1) return; int n = WiFi.scanComplete(); if (n<0) return;
  scanCount=0; for (int i=0;i<n && scanCount<12;i++) { String s=WiFi.SSID(i);
    s.toCharArray(scanList[scanCount].ssid,33); scanList[scanCount].rssi=WiFi.RSSI(i);
    scanList[scanCount].saved=isSaved(s); scanCount++; }
  WiFi.scanDelete(); scanState=0;
}
void updateWebIP() { if (WiFi.status()==WL_CONNECTED) { wifiUp=true; webIP=WiFi.localIP().toString(); }
  else { wifiUp=false; webIP=WiFi.softAPIP().toString(); } }
void wifiStart() {
  WiFi.mode(WIFI_AP_STA); WiFi.softAP(AP_SSID, AP_PASS);
  applyMulti(); WiFi.setAutoReconnect(true); wifiMulti.run(4000);
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  server.on("/", handleRoot); server.on("/save", HTTP_POST, handleSave); server.on("/forget", HTTP_POST, handleForget);
  server.begin();
  ArduinoOTA.setHostname(OTA_HOST); ArduinoOTA.setPassword(OTA_PASS); ArduinoOTA.begin();
  updateWebIP();
}
void wifiStop() { server.stop(); WiFi.softAPdisconnect(true); WiFi.disconnect(true); WiFi.mode(WIFI_OFF); wifiUp=false; webIP="off"; }

String buildPage() {
  int n = WiFi.scanNetworks();
  String h = "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>";
  h += "<style>body{font-family:sans-serif;max-width:480px;margin:14px auto;padding:0 12px;color:#222}";
  h += "h2{font-weight:500}.n{display:flex;align-items:center;gap:8px;border:1px solid #ddd;border-radius:8px;padding:9px;margin:6px 0}";
  h += ".n b{flex:1}input{padding:6px;border:1px solid #ccc;border-radius:6px}button{padding:6px 12px;border:1px solid #bbb;border-radius:6px;background:#f5f5f5}</style></head><body>";
  h += "<h2>DeskDeck WiFi setup</h2>";
  if (WiFi.status()==WL_CONNECTED) h += "<p>Connected: " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")</p>";
  else h += "<p>Setup mode at " + WiFi.softAPIP().toString() + "</p>";
  h += "<h3>Saved</h3>";
  for (int i=0;i<savedCount;i++)
    h += "<div class=n><b>"+savedNets[i].ssid+"</b><form method=POST action=/forget><input type=hidden name=ssid value='"+savedNets[i].ssid+"'><button>Forget</button></form></div>";
  h += "<h3>Available</h3>";
  for (int i=0;i<n;i++) { String s = WiFi.SSID(i);
    h += "<div class=n><b>"+s+"</b><form method=POST action=/save><input type=hidden name=ssid value='"+s+"'><input name=pass type=password placeholder=password><button>Save</button></form></div>"; }
  WiFi.scanDelete();
  h += "<h3>Add manually</h3><form method=POST action=/save><input name=ssid placeholder=SSID> <input name=pass type=password placeholder=password> <button>Save</button></form>";
  h += "</body></html>"; return h;
}
void handleRoot()   { server.send(200, "text/html", buildPage()); }
void handleSave()   { String ss=server.arg("ssid"), pw=server.arg("pass"); if (ss.length()) addOrUpdateNet(ss,pw);
                      server.sendHeader("Location","/"); server.send(303); }
void handleForget() { String ss=server.arg("ssid"); if (ss.length()) forgetNet(ss);
                      server.sendHeader("Location","/"); server.send(303); }

// ==========================================================================
// Rows
// ==========================================================================
void addR(String l, int k, int i, String code) {
  rowList[rowN].label=l; rowList[rowN].kind=k; rowList[rowN].i=i; rowList[rowN].code=code;
  rowList[rowN].v=0; rowList[rowN].m=false; rowList[rowN].saved=false; rowList[rowN].on=false; rowList[rowN].str=0; rowN++;
}
void buildRows() {
  rowN = 0;
  if (page==P_HOME) { for (int i=0;i<HOME_N;i++) addR(HOME_L[i], K_GO, HOME_T[i], ""); }
  else if (page==P_VLC) {
    addR("Play / Pause", K_ACT, 0, "vlc_pp");
    addR(String("Subtitles: ")+(subOn?"ON":"OFF"), K_TG, 0, "sub");
    addR("Audio track", K_ACT, 0, "vlc_audio");
    addR("Fullscreen", K_ACT, 0, "vlc_full");
    addR("Seek", K_SCRUB, 0, "");
    addR(stLabel(), K_CYC, 0, "");
  } else if (page==P_VOLUME) {
    for (int i=0;i<sessionCount;i++) { addR(sessions[i].name, K_VAL, i, "");
      rowList[rowN-1].v=sessions[i].vol; rowList[rowN-1].m=sessions[i].muted; }
  } else if (page==P_MEDIA) {
    for (int i=0;i<4;i++) addR(MEDIA_L[i], K_ACT, i, String("media:")+MEDIA_C[i]);
  } else if (page==P_LAUNCHER) {
    for (int i=0;i<launcherCount;i++) addR(launchers[i], K_ACT, i, "launch");
  } else if (page==P_POWER) {
    for (int i=0;i<5;i++) addR(POWER_L[i], K_ACT, i, String("pwr:")+POWER_C[i]);
    addR("Wake PC", K_ACT, 0, "wol");
  } else if (page==P_SETTINGS) {
    addR(String("WiFi: ")+(wifiOn?"ON":"OFF"), K_TG, 0, "wifi");
    addR(String("Bluetooth: ")+(btOn?"ON":"OFF"), K_TG, 0, "bt");
    addR("Brightness", K_LVL, 0, ""); rowList[rowN-1].v=brightness;
    addR(String("Auto-sleep: ")+(sleepOn?"ON":"OFF"), K_TG, 0, "sleep");
    addR(String("Rotate 180: ")+(rot?"ON":"OFF"), K_TG, 0, "rot");
    addR("WiFi setup", K_GO, P_WIFI, "");
    addR("Re-pair Bluetooth", K_ACT, 0, "repair");
    addR("Restart ESP32", K_ACT, 0, "restart");
    addR("Power off", K_ACT, 0, "poweroff");
    addR("About device", K_GO, P_ABOUT, "");
  } else if (page==P_WIFI) {
    for (int i=0;i<scanCount && rowN<14;i++) {
      addR(scanList[i].ssid, K_NET, i, "");
      rowList[rowN-1].str = rssiBars(scanList[i].rssi);
      rowList[rowN-1].saved = scanList[i].saved;
      rowList[rowN-1].on = (WiFi.status()==WL_CONNECTED && WiFi.SSID()==String(scanList[i].ssid));
    }
    if (scanState==1 && scanCount==0) addR("scanning...", K_WEB, 0, "");
    addR(String("Add/edit  ")+webIP, K_WEB, 0, "");
  }
}

// ==========================================================================
// Rendering
// ==========================================================================
void invertRect(int x, int y, int w, int h) {
  uint8_t* buf = display.getBuffer();
  for (int yy=y; yy<y+h; yy++) { if (yy<0||yy>=SCREEN_HEIGHT) continue;
    int pg = yy>>3; uint8_t bit = 1<<(yy&7);
    for (int xx=x; xx<x+w; xx++) { if (xx<0||xx>=SCREEN_WIDTH) continue; buf[xx+pg*SCREEN_WIDTH] ^= bit; } }
}

void render() {
  display.clearDisplay();
  if (asleep) { display.display(); return; }
  if (booting) {
    display.setTextColor(SH110X_WHITE); display.setTextSize(2); display.setCursor(28,16); display.print("ESP32");
    display.setTextSize(1); display.setCursor(40,44); display.print("starting"); display.display(); return;
  }
  drawStatusBar();
  if      (page==P_CALIBRATE) drawCalibrate();
  else if (page==P_SYSTEM)    drawSystem();
  else if (page==P_ABOUT)     drawAbout();
  else                        drawList();
  if (cfActive) drawConfirm();
  if (millis() < toastUntil) drawToast();
  display.display();
}

void drawStatusBar() {
  display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  if (btOn) { display.setCursor(0,1); display.print("BT"); }
  if (wifiOn) {
    int bars = (WiFi.status()==WL_CONNECTED) ? rssiBars(WiFi.RSSI()) : 0;
    for (int b=0;b<3;b++) { int bh=2+b*2, x=16+b*3;
      if (b<bars) display.fillRect(x, 8-bh, 2, bh, SH110X_WHITE); else display.drawRect(x, 8-bh, 2, bh, SH110X_WHITE); }
  }
  int16_t bx,by; uint16_t bw,bh;
  display.getTextBounds(clockStr,0,0,&bx,&by,&bw,&bh);
  display.setCursor(SCREEN_WIDTH-bw-1, 1); display.print(clockStr);
  const char* t = STITLE[page];
  display.getTextBounds(t,0,0,&bx,&by,&bw,&bh);
  display.setCursor((SCREEN_WIDTH-bw)/2, 1); display.print(t);
  display.drawLine(0,10,SCREEN_WIDTH,10,SH110X_WHITE);
}

void drawList() {
  buildRows();
  const int rowH=12, top=12, vis=4;
  int sel = selByPage[page];
  if (sel >= rowN) sel = max(0, rowN-1);
  int start = sel>=vis ? sel-vis+1 : 0; start = min(start, max(0, rowN-vis));
  if (slideX>0.5) slideX*=0.6; else slideX=0; int sx=(int)slideX;

  display.setTextSize(1);
  for (int i=0;i<vis && start+i<rowN;i++) {
    Row& r = rowList[start+i]; int y = top + i*rowH;
    display.setTextColor(SH110X_WHITE);
    int maxc = (r.kind==K_VAL||r.kind==K_LVL)?9 : (r.kind==K_NET?13:20);
    String lbl = r.label; if (lbl.length()>maxc) lbl = lbl.substring(0,maxc);
    display.setCursor(4+sx, y+2); display.print(lbl);

    if (r.kind==K_VAL || r.kind==K_LVL) {
      int val = (r.kind==K_VAL)? r.v : brightness; bool muted = (r.kind==K_VAL && r.m);
      int bxx=70+sx, bw=34, byy=y+3, bh=6; display.drawRect(bxx,byy,bw,bh,SH110X_WHITE);
      if (muted) { display.drawLine(bxx,byy+bh,bxx+bw,byy,SH110X_WHITE); display.setCursor(108+sx,y+2); display.print("M"); }
      else { int fw=(bw-2)*val/100; if (fw>0) display.fillRect(bxx+1,byy+1,fw,bh-2,SH110X_WHITE); display.setCursor(108+sx,y+2); display.print(val); }
    } else if (r.kind==K_NET) {
      int bxx=96+sx;
      for (int b=0;b<3;b++) { int bh=2+b*2; if (b<r.str) display.fillRect(bxx+b*3, y+9-bh, 2, bh, SH110X_WHITE);
        else display.drawRect(bxx+b*3, y+9-bh, 2, bh, SH110X_WHITE); }
      if (r.on) display.fillRect(112+sx, y+4, 5, 5, SH110X_WHITE);
      else if (!r.saved) { display.drawRect(112+sx, y+5, 5, 4, SH110X_WHITE);
        display.drawLine(113+sx,y+5,113+sx,y+3,SH110X_WHITE); display.drawLine(116+sx,y+5,116+sx,y+3,SH110X_WHITE);
        display.drawLine(113+sx,y+3,116+sx,y+3,SH110X_WHITE); }
    }
  }
  if (rowN > vis) { int h=48; display.drawLine(125,12,125,12+h,SH110X_WHITE);
    int bh=max(4, h*vis/rowN); int by=12+(h-bh)*start/max(1,rowN-vis); display.fillRect(124,by,3,bh,SH110X_WHITE); }

  // back hint chevron (any non-home page)
  if (page != P_HOME) { display.setTextColor(SH110X_WHITE); display.setCursor(0, 34); display.print("\x11"); }

  // animated highlight (XOR); blinks while editing
  int hlTarget = top + (sel-start)*rowH;
  if (snapHL) { hlY = hlTarget; snapHL = false; } else hlY += (hlTarget - hlY) * 0.4;
  bool blink = editing ? (((millis()/400)%2)==0) : true;
  if (blink) invertRect(0, (int)(hlY+0.5), SCREEN_WIDTH, rowH);
}

void drawSystem() {
  display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  if (!stats.valid) { display.setCursor(4,28); display.print("Waiting for PC..."); }
  else {
    display.setCursor(4,16); display.print("CPU "); display.print(stats.cpu); display.print("%  RAM "); display.print(stats.ram); display.print("%");
    display.setCursor(4,30); display.print("GPU "); display.print(stats.gpu); display.print("%  "); display.print(stats.temp); display.print("C");
    display.setCursor(4,44); display.print("NET "); display.print(stats.net,1); display.print(" MB/s");
  }
  display.setCursor(0,34); display.print("\x11");
}
void drawCalibrate() {
  int dx=lastX-centerX, dy=lastY-centerY; bool ok = abs(dx)<220 && abs(dy)<220;
  int bx=4, by=12, bs=48;
  display.drawRect(bx,by,bs,bs,SH110X_WHITE);
  display.drawLine(bx+bs/2,by,bx+bs/2,by+bs,SH110X_WHITE); display.drawLine(bx,by+bs/2,bx+bs,by+bs/2,SH110X_WHITE);
  int px=bx+(int)((long)lastX*(bs-4)/4095), py=by+(int)((4095-(long)lastY)*(bs-4)/4095);
  display.fillCircle(px+2,py+2,2,SH110X_WHITE);
  display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  display.setCursor(60,14); display.print("X "); display.print(lastX);
  display.setCursor(60,26); display.print("Y "); display.print(lastY);
  display.setCursor(60,38); display.print(ok?"centered":"off-center");
  display.setCursor(4,54);  display.print("Press = set center");
}
void drawAbout() {
  display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  display.setCursor(4,14); display.print("Desk Deck v1.0");
  display.setCursor(4,26);
  if (wifiOn && WiFi.status()==WL_CONNECTED) { display.print("ip "); display.print(WiFi.localIP()); }
  else if (wifiOn) { display.print("setup "); display.print(WiFi.softAPIP()); }
  else display.print("WiFi off");
  display.setCursor(4,38); display.print("BT "); display.print(SerialBT.hasClient()?"linked":"waiting");
  display.print(wifiOn?"  OTA on":"");
  display.setCursor(0,34); display.print("\x11");
}
void drawConfirm() {
  display.fillRect(14,16,100,34,SH110X_BLACK); display.drawRect(14,16,100,34,SH110X_WHITE);
  display.setTextColor(SH110X_WHITE); display.setTextSize(1);
  display.setCursor(18,20); display.print(cfTitle);
  display.drawRect(22,36,34,12,SH110X_WHITE); display.setCursor(32,38); display.print("No");
  display.drawRect(72,36,34,12,SH110X_WHITE); display.setCursor(80,38); display.print("Yes");
  if (confirmSel==0) invertRect(22,36,34,12); else invertRect(72,36,34,12);
}
void drawToast() {
  int w = toastMsg.length()*6 + 10; int x = (SCREEN_WIDTH-w)/2; if (x<0) x=0;
  display.fillRoundRect(x,48,w,14,3,SH110X_WHITE);
  display.setTextColor(SH110X_BLACK); display.setTextSize(1);
  display.setCursor(x+5,51); display.print(toastMsg);
}
