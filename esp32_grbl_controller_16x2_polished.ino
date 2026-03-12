
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <SD.h>
#include <EEPROM.h>

#define LCD_ADDR        0x27
#define LCD_COLS        16
#define LCD_ROWS        2

#define BTN_UP          32
#define BTN_DOWN        33
#define BTN_SELECT      25
#define BTN_BACK        26
#define BUZZER_PIN      27

#define SD_CS_PIN       5
#define SD_MOSI_PIN     23
#define SD_MISO_PIN     19
#define SD_SCK_PIN      18

#define GRBL_RX_PIN     16
#define GRBL_TX_PIN     17
#define GRBL_BAUD       115200

#define WIFI_AP_SSID    "ESP32-CNC"
#define WIFI_AP_PASS    "12345678"

#define STATUS_INTERVAL 300
#define UI_REFRESH_MS   140
#define DEBOUNCE_MS     110
#define MAX_FILES       50
#define MAX_NAME_LEN    40
#define GCODE_LINE_LEN  96
#define JOG_FEED        800
#define EEPROM_SIZE     32

#define EE_BUZZER       0
#define EE_SCROLL       1

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
WebServer server(80);
HardwareSerial GRBL(2);
SPIClass SDSPI(VSPI);

struct Button {
  uint8_t pin;
  bool lastRaw;
  bool stable;
  unsigned long changedAt;
};

Button btnUp     = {BTN_UP, HIGH, HIGH, 0};
Button btnDown   = {BTN_DOWN, HIGH, HIGH, 0};
Button btnSelect = {BTN_SELECT, HIGH, HIGH, 0};
Button btnBack   = {BTN_BACK, HIGH, HIGH, 0};

enum ButtonEvent { EV_NONE, EV_UP, EV_DOWN, EV_SELECT, EV_BACK };
enum ScreenState {
  SCR_BOOT,
  SCR_MAIN,
  SCR_CONTROL,
  SCR_MOVE_STEP,
  SCR_MOVE_AXIS,
  SCR_JOG,
  SCR_STATUS,
  SCR_SD_MENU,
  SCR_FILE_LIST,
  SCR_WIFI,
  SCR_RUN,
  SCR_SETTINGS
};

ScreenState screen = SCR_BOOT;
bool forceRedraw = true;
unsigned long lastUiMs = 0;
unsigned long lastScrollMs = 0;
uint8_t scrollOffset = 0;

int mainIndex = 0;      // Control, Status, SD, WiFi, Settings
int controlIndex = 0;   // Home, Unlock, Move, Zero, Origin, Reset
int stepIndex = 0;      // Back, 10, 1, 0.1
int axisIndex = 0;      // Back, X, Y, Z
int sdIndex = 0;        // Back, Refresh, Files
int fileIndex = 0;
int settingsIndex = 0;  // Buzzer, Scroll

char jogAxis = 'X';
float jogStep = 10.0f;

bool sdReady = false;
char fileNames[MAX_FILES][MAX_NAME_LEN];
int fileCount = 0;

File runFile;
bool fileRunning = false;
bool waitingOk = false;
bool runPaused = false;
unsigned long lastLineSentMs = 0;
char currentRunFile[MAX_NAME_LEN] = "";
uint32_t runFileSize = 0;

char grblState[16] = "Idle";
char posX[16] = "0.000";
char posY[16] = "0.000";
char posZ[16] = "0.000";
char fsFeed[12] = "0";
char fsSpindle[12] = "0";
char serialLine[128];
int serialPos = 0;
unsigned long lastStatusMs = 0;

String apIP = "0.0.0.0";
bool buzzerEnabled = true;
bool betterScroll = true;

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 CNC Controller</title>
<style>
body{font-family:Arial;background:#111;color:#eee;margin:0;padding:16px}
.card{background:#1c1c1c;border-radius:14px;padding:16px;margin-bottom:14px}
button{padding:12px 14px;border:none;border-radius:10px;margin:4px;background:#2e7d32;color:#fff;font-size:16px}
button.stop{background:#c62828} button.warn{background:#ef6c00}
pre{white-space:pre-wrap;word-break:break-word;background:#000;padding:12px;border-radius:10px}
.row{display:flex;gap:8px;flex-wrap:wrap}
</style></head><body>
<div class="card"><h2>Status</h2><div id="status">Loading...</div></div>
<div class="card"><h2>Machine</h2><div class="row">
<button onclick="cmd('$H')">Home</button>
<button onclick="cmd('$X')">Unlock</button>
<button onclick="cmd('G92 X0 Y0 Z0')">Set Zero</button>
<button onclick="cmd('G90 G0 X0 Y0 Z0')">Origin</button>
<button class="stop" onclick="cmd('!')">Pause</button>
<button onclick="cmd('~')">Resume</button>
<button class="stop" onclick="cmd(String.fromCharCode(0x18))">Reset</button>
</div></div>
<div class="card"><h2>Jog</h2><div class="row">
<button onclick="jog('X',10)">X+10</button><button onclick="jog('X',-10)">X-10</button>
<button onclick="jog('Y',10)">Y+10</button><button onclick="jog('Y',-10)">Y-10</button>
<button onclick="jog('Z',1)">Z+1</button><button onclick="jog('Z',-1)">Z-1</button>
</div></div>
<div class="card"><h2>Files</h2><div id="files">Loading...</div></div>
<div class="card"><h2>Upload</h2><input type="file" id="f"><button onclick="up()">Upload</button><div id="u"></div></div>
<pre id="log">Ready</pre>
<script>
async function status(){let r=await fetch('/api/status');let j=await r.json();document.getElementById('status').innerHTML=
'State:<b>'+j.state+'</b><br>X:'+j.x+' Y:'+j.y+' Z:'+j.z+'<br>Feed:'+j.feed+' Spindle:'+j.spindle+
'<br>Files:'+j.fileCount+'<br>Running:'+(j.running?j.runningFile:'No')+'<br>Progress:'+j.progress+'%';}
async function files(){let r=await fetch('/api/files');let j=await r.json();let h='';(j.files||[]).forEach(f=>h+='<div class="row"><button onclick="runf(\''+f+'\')">Run</button><div>'+f+'</div></div>');document.getElementById('files').innerHTML=h||'No files';}
async function cmd(c){let r=await fetch('/api/cmd?c='+encodeURIComponent(c));document.getElementById('log').textContent=await r.text();setTimeout(status,300);}
async function jog(a,s){let r=await fetch('/api/jog?axis='+a+'&step='+s);document.getElementById('log').textContent=await r.text();}
async function runf(n){let r=await fetch('/api/run?name='+encodeURIComponent(n));document.getElementById('log').textContent=await r.text();setTimeout(status,300);}
async function up(){let f=document.getElementById('f').files[0];if(!f)return;let d=new FormData();d.append('file',f,f.name);document.getElementById('u').textContent='Uploading...';let r=await fetch('/upload',{method:'POST',body:d});document.getElementById('u').textContent=await r.text();files();status();}
setInterval(status,1200);setInterval(files,3000);status();files();
</script></body></html>
)rawliteral";

void saveSettings() {
  EEPROM.write(EE_BUZZER, buzzerEnabled ? 1 : 0);
  EEPROM.write(EE_SCROLL, betterScroll ? 1 : 0);
  EEPROM.commit();
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t b = EEPROM.read(EE_BUZZER);
  uint8_t s = EEPROM.read(EE_SCROLL);
  buzzerEnabled = (b == 0 || b == 1) ? b : 1;
  betterScroll = (s == 0 || s == 1) ? s : 1;
}

void beep(uint16_t ms = 30, uint16_t freq = 2500) {
  if (!buzzerEnabled) return;
  tone(BUZZER_PIN, freq, ms);
}

void beepOk() { beep(35, 2600); }
void beepWarn() { beep(90, 1800); }
void beepDone() { beep(45, 3000); delay(55); beep(60, 3400); }

bool checkPress(Button &b) {
  bool raw = digitalRead(b.pin);
  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.changedAt = millis();
  }
  if ((millis() - b.changedAt) > DEBOUNCE_MS && raw != b.stable) {
    b.stable = raw;
    if (b.stable == LOW) return true;
  }
  return false;
}

ButtonEvent readButtons() {
  if (checkPress(btnUp)) return EV_UP;
  if (checkPress(btnDown)) return EV_DOWN;
  if (checkPress(btnSelect)) return EV_SELECT;
  if (checkPress(btnBack)) return EV_BACK;
  return EV_NONE;
}

String fit16(String s) {
  if (s.length() > 16) return s.substring(0, 16);
  while (s.length() < 16) s += ' ';
  return s;
}

String scrollText(String s, bool reset = false) {
  if (reset) {
    scrollOffset = 0;
    lastScrollMs = millis();
  }
  if (!betterScroll || s.length() <= 16) return fit16(s);

  if (millis() - lastScrollMs > 300) {
    lastScrollMs = millis();
    scrollOffset++;
    if (scrollOffset > s.length()) scrollOffset = 0;
  }

  String padded = s + "   " + s;
  return padded.substring(scrollOffset, scrollOffset + 16);
}

void lcd2(const String &l1, const String &l2, bool resetScroll = false) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(fit16(l1));
  lcd.setCursor(0, 1);
  lcd.print(scrollText(l2, resetScroll));
}

void grblSend(const String &cmd) {
  if (cmd.length() == 1 && cmd[0] == 0x18) GRBL.write(0x18);
  else GRBL.println(cmd);
}

void requestStatus() { GRBL.print("?"); }

void parseMPos(const char *src) {
  const char *m = strstr(src, "MPos:");
  if (!m) return;
  m += 5;
  const char *c1 = strchr(m, ',');
  if (!c1) return;
  const char *c2 = strchr(c1 + 1, ',');
  if (!c2) return;
  const char *end = strchr(c2 + 1, '|');
  if (!end) end = strchr(c2 + 1, '>');
  if (!end) return;

  size_t lx = min((int)(c1 - m), 15);
  size_t ly = min((int)(c2 - (c1 + 1)), 15);
  size_t lz = min((int)(end - (c2 + 1)), 15);

  strncpy(posX, m, lx); posX[lx] = 0;
  strncpy(posY, c1 + 1, ly); posY[ly] = 0;
  strncpy(posZ, c2 + 1, lz); posZ[lz] = 0;
}

void parseFS(const char *src) {
  const char *m = strstr(src, "FS:");
  if (!m) return;
  m += 3;
  const char *c1 = strchr(m, ',');
  if (!c1) return;
  const char *end = strchr(c1 + 1, '|');
  if (!end) end = strchr(c1 + 1, '>');
  if (!end) return;

  size_t lf = min((int)(c1 - m), 11);
  size_t ls = min((int)(end - (c1 + 1)), 11);

  strncpy(fsFeed, m, lf); fsFeed[lf] = 0;
  strncpy(fsSpindle, c1 + 1, ls); fsSpindle[ls] = 0;
}

void parseStatusLine(char *line) {
  char *p1 = strchr(line, '<');
  char *p2 = strchr(line, '|');
  if (p1 && p2 && p2 > p1) {
    size_t len = min((int)(p2 - p1 - 1), 15);
    strncpy(grblState, p1 + 1, len);
    grblState[len] = 0;
  }
  parseMPos(line);
  parseFS(line);
}

void handleGrblSerial() {
  while (GRBL.available()) {
    char c = GRBL.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (serialPos > 0) {
        serialLine[serialPos] = 0;
        if (serialLine[0] == '<') parseStatusLine(serialLine);
        else if (strcmp(serialLine, "ok") == 0) waitingOk = false;
        serialPos = 0;
      }
    } else if (serialPos < (int)sizeof(serialLine) - 1) {
      serialLine[serialPos++] = c;
    } else {
      serialPos = 0;
    }
  }
}

void refreshFiles() {
  fileCount = 0;
  fileIndex = 0;
  sdReady = SD.begin(SD_CS_PIN, SDSPI);
  if (!sdReady) return;

  File root = SD.open("/");
  if (!root) return;

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      String upper = name;
      upper.toUpperCase();
      if (upper.endsWith(".NC") || upper.endsWith(".GCO") || upper.endsWith(".GCODE") || upper.endsWith(".TXT")) {
        if (fileCount < MAX_FILES) {
          name.toCharArray(fileNames[fileCount], MAX_NAME_LEN);
          fileCount++;
        }
      }
    }
    entry.close();
  }
  root.close();
}

uint8_t progressPercent() {
  if (!fileRunning || !runFileSize || !runFile) return 0;
  uint32_t pos = runFile.position();
  return (uint8_t)((pos * 100UL) / runFileSize);
}

bool startRunFileByName(const char *name) {
  if (!sdReady) refreshFiles();
  if (!sdReady) return false;
  if (runFile) runFile.close();

  runFile = SD.open(String("/") + name, FILE_READ);
  if (!runFile) {
    runFile = SD.open(name, FILE_READ);
    if (!runFile) return false;
  }

  strncpy(currentRunFile, name, MAX_NAME_LEN - 1);
  currentRunFile[MAX_NAME_LEN - 1] = 0;
  fileRunning = true;
  waitingOk = false;
  runPaused = false;
  lastLineSentMs = 0;
  runFileSize = runFile.size();
  screen = SCR_RUN;
  forceRedraw = true;
  beepOk();
  return true;
}

void stopRunFile() {
  fileRunning = false;
  waitingOk = false;
  runPaused = false;
  if (runFile) runFile.close();
}

void processRunFile() {
  if (!fileRunning || !runFile || waitingOk || runPaused) return;
  if (millis() - lastLineSentMs < 8) return;

  char line[GCODE_LINE_LEN];
  size_t p = 0;

  while (runFile.available()) {
    char c = runFile.read();
    if (c == '\r') continue;
    if (c == '\n') break;

    if (c == ';') {
      while (runFile.available()) {
        c = runFile.read();
        if (c == '\n') break;
      }
      break;
    }

    if (p < sizeof(line) - 1) line[p++] = c;
  }

  line[p] = 0;
  while (p > 0 && (line[p - 1] == ' ' || line[p - 1] == '\t')) {
    line[p - 1] = 0;
    p--;
  }

  if (p > 0) {
    grblSend(String(line));
    waitingOk = true;
    lastLineSentMs = millis();
    return;
  }

  if (!runFile.available()) {
    stopRunFile();
    screen = SCR_STATUS;
    forceRedraw = true;
    beepDone();
  }
}

void sendJog(char axis, float step) {
  String cmd = "$J=G91G21";
  cmd += axis;
  cmd += String(step, 3);
  cmd += "F";
  cmd += String(JOG_FEED);
  grblSend(cmd);
}

String shortName(const char *src, uint8_t maxLen = 15) {
  String s = String(src);
  if (s.length() > maxLen) return s.substring(0, maxLen);
  return s;
}

void drawLogo() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  DIY CNC CTRL  ");
  lcd.setCursor(0, 1);
  lcd.print("   ESP32 GRBL   ");
  beep(45, 2400);
  delay(500);
  beep(45, 2900);
  delay(700);
}

void drawUI() {
  if (!forceRedraw && millis() - lastUiMs < UI_REFRESH_MS) return;
  lastUiMs = millis();
  forceRedraw = false;

  switch (screen) {
    case SCR_BOOT:
      lcd2("DIY CNC CTRL", "Booting...", true);
      break;

    case SCR_MAIN: {
      const char *items[] = {"Control", "Status", "SD Card", "WiFi", "Settings"};
      lcd2("Main Menu", String(">") + items[mainIndex], true);
      break;
    }

    case SCR_CONTROL: {
      const char *items[] = {"Auto Home", "Unlock", "Move Axis", "Set Zero", "Go Origin", "Soft Reset"};
      lcd2("Control", String(">") + items[controlIndex], true);
      break;
    }

    case SCR_MOVE_STEP: {
      const char *items[] = {"Back", "10mm", "1mm", "0.1mm"};
      lcd2("Step Size", String(">") + items[stepIndex], true);
      break;
    }

    case SCR_MOVE_AXIS: {
      const char *items[] = {"Back", "Move X", "Move Y", "Move Z"};
      lcd2("Select Axis", String(">") + items[axisIndex], true);
      break;
    }

    case SCR_JOG:
      lcd2(String(jogAxis) + " " + String(jogStep, jogStep < 1.0 ? 1 : 0) + "mm", "UP+ DN- BACK", true);
      break;

    case SCR_STATUS: {
      String l1 = String(grblState) + " X" + String(posX);
      String l2 = "Y" + String(posY) + " Z" + String(posZ);
      lcd2(l1, l2, false);
      break;
    }

    case SCR_SD_MENU: {
      const char *items[] = {"Back", "Refresh", "Files"};
      String l2 = String(">") + items[sdIndex];
      if (sdIndex == 2) l2 += ":" + String(fileCount);
      lcd2("SD Menu", l2, true);
      break;
    }

    case SCR_FILE_LIST:
    refreshFiles();
    if (!sdReady) lcd2("SD Not Ready", "BACK", true);
    else if (fileCount == 0) lcd2("No GCode Files", "BACK", true);
    else lcd2("File " + String(fileIndex + 1) + "/" + String(fileCount), ">" + shortName(fileNames[fileIndex], 28), false);
    break;

    case SCR_WIFI:
      lcd2("WiFi AP", String("IP:") + apIP, false);
      break;

    case SCR_RUN: {
      String l1 = "RUN " + shortName(currentRunFile, 22);
      String l2;
      if (runPaused) l2 = "Paused " + String(progressPercent()) + "%";
      else l2 = String(progressPercent()) + "% SEL:Pause";
      lcd2(l1, l2, false);
      break;
    }

    case SCR_SETTINGS: {
      String l2;
      if (settingsIndex == 0) l2 = String(">Buzzer:") + (buzzerEnabled ? "ON" : "OFF");
      else l2 = String(">Scroll:") + (betterScroll ? "ON" : "OFF");
      lcd2("Settings", l2, true);
      break;
    }
  }
}

String jsonEscape(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

void handleStatusApi() {
  String json = "{";
  json += "\"state\":\"" + jsonEscape(String(grblState)) + "\",";
  json += "\"x\":\"" + jsonEscape(String(posX)) + "\",";
  json += "\"y\":\"" + jsonEscape(String(posY)) + "\",";
  json += "\"z\":\"" + jsonEscape(String(posZ)) + "\",";
  json += "\"feed\":\"" + jsonEscape(String(fsFeed)) + "\",";
  json += "\"spindle\":\"" + jsonEscape(String(fsSpindle)) + "\",";
  json += "\"fileCount\":" + String(fileCount) + ",";
  json += "\"running\":" + String(fileRunning ? "true" : "false") + ",";
  json += "\"runningFile\":\"" + jsonEscape(String(currentRunFile)) + "\",";
  json += "\"progress\":" + String(progressPercent());
  json += "}";
  server.send(200, "application/json", json);
}

void handleFilesApi() {
  refreshFiles();
  String json = "{\"files\":[";
  for (int i = 0; i < fileCount; i++) {
    if (i) json += ",";
    json += "\"" + jsonEscape(String(fileNames[i])) + "\"";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleCmdApi() {
  if (!server.hasArg("c")) {
    server.send(400, "text/plain", "Missing c");
    return;
  }
  String c = server.arg("c");
  if (c == "!") { runPaused = true; }
  if (c == "~") { runPaused = false; }
  if (c.length() == 1 && c[0] == 0x18) GRBL.write(0x18);
  else grblSend(c);
  server.send(200, "text/plain", "Command sent: " + c);
}

void handleJogApi() {
  if (!server.hasArg("axis") || !server.hasArg("step")) {
    server.send(400, "text/plain", "Missing axis or step");
    return;
  }
  sendJog(server.arg("axis")[0], server.arg("step").toFloat());
  server.send(200, "text/plain", "Jog sent");
}

void handleRunApi() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "Missing file name");
    return;
  }
  String name = server.arg("name");
  if (startRunFileByName(name.c_str())) server.send(200, "text/plain", "Running: " + name);
  else server.send(500, "text/plain", "Failed to run file");
}

File uploadFile;
void handleUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    refreshFiles();
    if (SD.exists(filename)) SD.remove(filename);
    uploadFile = SD.open(filename, FILE_WRITE);
} else if (upload.status == UPLOAD_FILE_END) {
  if (uploadFile) uploadFile.close();
  refreshFiles();
  sdReady = true;
  forceRedraw = true;
  beepDone();
}
}

void handleUploadDone() { server.send(200, "text/plain", "Upload complete"); }

void initWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatusApi);
  server.on("/api/files", HTTP_GET, handleFilesApi);
  server.on("/api/cmd", HTTP_GET, handleCmdApi);
  server.on("/api/jog", HTTP_GET, handleJogApi);
  server.on("/api/run", HTTP_GET, handleRunApi);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
  server.begin();
}

void initWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
  apIP = WiFi.softAPIP().toString();
}

void initPins() {
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
}

void initSD() {
  SDSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  refreshFiles();
}

void initGrbl() {
  GRBL.begin(GRBL_BAUD, SERIAL_8N1, GRBL_RX_PIN, GRBL_TX_PIN);
  delay(100);
  requestStatus();
}

void handleMain(ButtonEvent ev) {
  if (ev == EV_UP) {
    mainIndex = (mainIndex - 1 + 5) % 5;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_DOWN) {
    mainIndex = (mainIndex + 1) % 5;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_SELECT) {
    if (mainIndex == 0) screen = SCR_CONTROL;
    else if (mainIndex == 1) screen = SCR_STATUS;
    else if (mainIndex == 2) screen = SCR_SD_MENU;
    else if (mainIndex == 3) screen = SCR_WIFI;
    else screen = SCR_SETTINGS;
    forceRedraw = true;
    beepOk();
  }
}

void handleControl(ButtonEvent ev) {
  if (ev == EV_UP) {
    controlIndex = (controlIndex - 1 + 6) % 6;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_DOWN) {
    controlIndex = (controlIndex + 1) % 6;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_BACK) {
    screen = SCR_MAIN;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_SELECT) {
    switch (controlIndex) {
      case 0: grblSend("$H"); screen = SCR_STATUS; break;
      case 1: grblSend("$X"); screen = SCR_STATUS; break;
      case 2: screen = SCR_MOVE_STEP; break;
      case 3: grblSend("G92 X0 Y0 Z0"); screen = SCR_STATUS; break;
      case 4: grblSend("G90 G0 X0 Y0 Z0"); screen = SCR_STATUS; break;
      case 5: GRBL.write(0x18); screen = SCR_STATUS; break;
    }
    forceRedraw = true;
    beepOk();
  }
}

void handleStep(ButtonEvent ev) {
  if (ev == EV_UP) {
    stepIndex = (stepIndex - 1 + 4) % 4;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_DOWN) {
    stepIndex = (stepIndex + 1) % 4;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_BACK) {
    screen = SCR_CONTROL;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_SELECT) {
    if (stepIndex == 0) screen = SCR_CONTROL;
    else {
      if (stepIndex == 1) jogStep = 10.0f;
      if (stepIndex == 2) jogStep = 1.0f;
      if (stepIndex == 3) jogStep = 0.1f;
      screen = SCR_MOVE_AXIS;
    }
    forceRedraw = true;
    beepOk();
  }
}

void handleAxis(ButtonEvent ev) {
  if (ev == EV_UP) {
    axisIndex = (axisIndex - 1 + 4) % 4;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_DOWN) {
    axisIndex = (axisIndex + 1) % 4;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_BACK) {
    screen = SCR_MOVE_STEP;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_SELECT) {
    if (axisIndex == 0) screen = SCR_MOVE_STEP;
    else {
      jogAxis = axisIndex == 1 ? 'X' : axisIndex == 2 ? 'Y' : 'Z';
      screen = SCR_JOG;
    }
    forceRedraw = true;
    beepOk();
  }
}

void handleJog(ButtonEvent ev) {
  if (ev == EV_UP) {
    sendJog(jogAxis, jogStep);
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_DOWN) {
    sendJog(jogAxis, -jogStep);
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_BACK) {
    screen = SCR_MOVE_AXIS;
    forceRedraw = true;
    beepOk();
  }
}

void handleStatusScreen(ButtonEvent ev) {
  if (ev == EV_BACK) {
    screen = SCR_MAIN;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_SELECT) {
    requestStatus();
    forceRedraw = true;
    beepOk();
  }
}

void handleSDMenu(ButtonEvent ev) {
  if (ev == EV_UP) {
    sdIndex = (sdIndex - 1 + 3) % 3;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_DOWN) {
    sdIndex = (sdIndex + 1) % 3;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_BACK) {
    screen = SCR_MAIN;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_SELECT) {
    if (sdIndex == 0) screen = SCR_MAIN;
else if (sdIndex == 1) {
  refreshFiles();
  forceRedraw = true;
  beepDone();
}
else {
  refreshFiles();
  screen = SCR_FILE_LIST;
  forceRedraw = true;
}    forceRedraw = true;
  }
}

void handleFileList(ButtonEvent ev) {
  if (ev == EV_BACK) {
    screen = SCR_SD_MENU;
    forceRedraw = true;
    beepOk();
    return;
  }

  if (!sdReady || fileCount == 0) return;

  if (ev == EV_UP) {
    fileIndex = (fileIndex - 1 + fileCount) % fileCount;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_DOWN) {
    fileIndex = (fileIndex + 1) % fileCount;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_SELECT) {
    startRunFileByName(fileNames[fileIndex]);
    forceRedraw = true;
  }
}

void handleWiFi(ButtonEvent ev) {
  if (ev == EV_BACK) {
    screen = SCR_MAIN;
    forceRedraw = true;
    beepOk();
  }
}

void handleRun(ButtonEvent ev) {
  if (ev == EV_BACK) {
    grblSend("!");
    stopRunFile();
    screen = SCR_STATUS;
    forceRedraw = true;
    beepWarn();
  } else if (ev == EV_SELECT) {
    if (!runPaused) {
      grblSend("!");
      runPaused = true;
      beepWarn();
    } else {
      grblSend("~");
      runPaused = false;
      beepOk();
    }
    forceRedraw = true;
  }
}

void handleSettings(ButtonEvent ev) {
  if (ev == EV_UP || ev == EV_DOWN) {
    settingsIndex = settingsIndex == 0 ? 1 : 0;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_BACK) {
    screen = SCR_MAIN;
    forceRedraw = true;
    beepOk();
  } else if (ev == EV_SELECT) {
    if (settingsIndex == 0) buzzerEnabled = !buzzerEnabled;
    else betterScroll = !betterScroll;
    saveSettings();
    forceRedraw = true;
    beepDone();
  }
}

void setup() {
  initPins();
  loadSettings();

  lcd.init();
  lcd.backlight();
  drawLogo();

  initWiFi();
  initSD();
  initGrbl();
  initWeb();

  String l1 = "WiFi:" + apIP;
  String l2 = sdReady ? "SD OK Files:" + String(fileCount) : "SD NOT READY";
  lcd2(l1, l2, true);
  delay(1800);

  screen = SCR_MAIN;
  forceRedraw = true;
  beepDone();
}

void loop() {
  server.handleClient();
  handleGrblSerial();
  processRunFile();

  if (millis() - lastStatusMs >= STATUS_INTERVAL) {
    lastStatusMs = millis();
    requestStatus();
    if (screen == SCR_STATUS || screen == SCR_RUN) forceRedraw = true;
  }

  ButtonEvent ev = readButtons();
  if (ev != EV_NONE) {
    switch (screen) {
      case SCR_MAIN: handleMain(ev); break;
      case SCR_CONTROL: handleControl(ev); break;
      case SCR_MOVE_STEP: handleStep(ev); break;
      case SCR_MOVE_AXIS: handleAxis(ev); break;
      case SCR_JOG: handleJog(ev); break;
      case SCR_STATUS: handleStatusScreen(ev); break;
      case SCR_SD_MENU: handleSDMenu(ev); break;
      case SCR_FILE_LIST: handleFileList(ev); break;
      case SCR_WIFI: handleWiFi(ev); break;
      case SCR_RUN: handleRun(ev); break;
      case SCR_SETTINGS: handleSettings(ev); break;
      default: break;
    }
  }

  drawUI();
}
