/* Robust Weekly Timer - DS3231 + NTP fallback (PKT / UTC+5*)
  *This code was written and tested in Pakistan (UTC+5) timezone.
  Adjust according to your local time by altering here
  const long gmtOffset_sec = UTC_OFFSET * 3600; // UTC offset in hours
  const int daylightOffset_sec = 0; // daylight adjustment offset in seconds

  Set your desired AP credentials here to connect to webui and edit schedule,  override, set time etc.
  const char* AP_SSID = "your_AP_SSID";
  const char* AP_PASS = "your_AP_PASS";

  Set your WiFi router credentials in setDefaults() to connect to ntp server if RTC fails
  strcpy(ntpSSID, "your_ntp_SSID");
  strcpy(ntpPassword, "your_ntp_PASS");
*/


#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include <sys/time.h>
#include <time.h>
#include <Wire.h>

#define RELAY_PIN 27
#define LED_PIN 2
#define RELAY_ACTIVE_LOW true // set true if your relay module is active low
#include <DNSServer.h>

DNSServer dnsServer;  // Global object (place near top with other globals)

WebServer server(80);

RTC_DS3231 rtc;
// WiFi (AP fallback)
const char* AP_SSID = "your_AP_SSID";
const char* AP_PASS = "your_AP_PASS";

char ntpSSID[32] = "";
char ntpPassword[64] = "";

// Config stored in LittleFS /config.json
struct Config {
  bool overrideActive;
  bool overrideStateOn;      // true => forced ON, false => forced OFF
  uint32_t overrideUntil;    // unix sec UTC, 0 = indefinite / none
  int schedule[7][8][2];     // [day][interval][0=start_min,1=stop_min] minutes from midnight, -1 = disabled //####//
} cfg;

const char* CONFIG_PATH = "/config.json";

const long gmtOffset_sec = 5 * 3600; // PKT offset, adjust according to your need
const int daylightOffset_sec = 0;

// forward declarations
bool syncRTCfromNTP();
bool rtcAvailable();
void updateAllRTCs(const DateTime& dt);
DateTime getLocalDateTimeFromSystem();
bool isValidDateTime(const DateTime& t);
uint32_t nowUnix();
DateTime nowDT(); // local DateTime (PKT)
DateTime getCurrentTime();

// --- Utility helpers ---
String minuteToHHMM(int mins) {
  if (mins < 0) return String("--:--");
  int h = mins / 60;
  int m = mins % 60;
  char b[6]; snprintf(b, sizeof(b), "%02d:%02d", h, m);
  return String(b);
}

// Centralized update: dt is **local** (PKT). This function writes that to DS3231
// and sets ESP32 system time correctly (settimeofday expects UTC epoch)
// dt is local PKT DateTime
void updateAllRTCs(const DateTime& dt) {
  if (!isValidDateTime(dt)) {
    Serial.println("[updateAllRTCs] Rejecting invalid DateTime");
    return;
  }

  // Update DS3231 (stores PKT fields)
  if (rtcAvailable() && rtc.begin()) {
    rtc.adjust(dt);
    Serial.println("[RTC] DS3231 adjusted (local PKT fields)");
  }

  // Update ESP32 system (expects UTC epoch!)
  timeval tv;
  tv.tv_sec = dt.unixtime() - gmtOffset_sec; // convert local→UTC
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);

  Serial.printf("[RTC] ESP32 clock updated (UTC=%lu, PKT=%lu)\n",
  (unsigned long)tv.tv_sec, (unsigned long)dt.unixtime());
}

// Get a DateTime representing local PKT time, prefer DS3231 if valid else system
DateTime getCurrentTime() {
  if (rtcAvailable() && rtc.begin()) {
    DateTime now = rtc.now();
    if (isValidDateTime(now)) {
      return now; // DS3231 stores local PKT fields
    }
    Serial.println("[WARN] RTC returned invalid date/time; falling back to system");
  }

  // fallback: system local time (TZ must be set with setenv/tzset)
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    return DateTime(
      timeinfo.tm_year + 1900,
      timeinfo.tm_mon + 1,
      timeinfo.tm_mday,
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec
    );
  }

  // last resort
  Serial.println("[ERROR] No valid time source available!");
  return DateTime(2025, 1, 1, 0, 0, 0);
}

// Validity check for a DateTime (full field checks)
bool isValidDateTime(const DateTime& t) {
  return (
    t.year()   >= 2020 && t.year() <= 2099 &&
    t.month()  >= 1    && t.month() <= 12 &&
    t.day()    >= 1    && t.day()   <= 31 &&
    t.hour()   >= 0    && t.hour()  <= 23 &&
    t.minute() >= 0    && t.minute()<= 59 &&
    t.second() >= 0    && t.second()<= 59
  );
}

// --- Periodic RTC watchdog ---
unsigned long lastRTCcheck = 0;
const unsigned long rtcCheckInterval = 900000UL; // 15 min

void checkRTCs() {
  DateTime now = getCurrentTime();
  if (!isValidDateTime(now)) {
    Serial.println("[WATCHDOG] Invalid time detected, attempting NTP resync...");
    if (syncRTCfromNTP()) {
      DateTime refreshed = getCurrentTime();
      if (isValidDateTime(refreshed)) {
        updateAllRTCs(refreshed);
        Serial.println("[WATCHDOG] Resync success");
      } else {
        Serial.println("[WATCHDOG] Resync returned invalid time");
      }
    } else {
      Serial.println("[WATCHDOG] NTP resync failed");
    }
  } else {
    // Optional: print periodic heartbeat
    Serial.printf("[WATCHDOG] Time OK: %04d-%02d-%02d %02d:%02d:%02d\n",
      now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  }
}


// --- Internal: try to get NTP and update RTC ---
// This function switches to STA, connects, sets TZ, fetches local time and writes to RTC & system.
bool syncRTCfromNTP() {
  Serial.println("[NTP] Attempting NTP sync (switching to STA) ...");
  Serial.print ("SSID : ") ; Serial.println (ntpSSID);
  Serial.print ("PWD : ") ; Serial.println (ntpPassword);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ntpSSID, ntpPassword);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000UL) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[NTP] WiFi connect failed for NTP");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    return false;
  }

  Serial.println("\n[NTP] WiFi connected, requesting NTP time...");

  // Always request UTC from NTP (no TZ yet)
  configTime(0, 0, "pool.ntp.org");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("[NTP] getLocalTime timeout");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    return false;
  }

  // Convert system UTC → PKT (UTC+5)
  time_t raw = mktime(&timeinfo);   // raw = UTC epoch
  DateTime dtLocal(raw + gmtOffset_sec);

  Serial.printf("[NTP] Got PKT local time: %04d-%02d-%02d %02d:%02d:%02d\n",
    dtLocal.year(), dtLocal.month(), dtLocal.day(),
    dtLocal.hour(), dtLocal.minute(), dtLocal.second());

  // Update external RTC and ESP32 internal clock
  updateAllRTCs(dtLocal);

  Serial.println("[NTP] RTC + system updated from NTP (PKT)");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  return true;
}

// --- helpers ---
bool rtcAvailable() {
  Wire.beginTransmission(0x68);
  return (Wire.endTransmission() == 0);
}

uint32_t nowUnix() {
  if (rtcAvailable() && rtc.begin()) {
    DateTime t = rtc.now(); // DS3231 stores local fields
    if (isValidDateTime(t)) {
      return t.unixtime() - gmtOffset_sec; // convert local→UTC
    }
  }
  return (uint32_t)time(nullptr); // system clock is UTC already
}

// nowDT returns local PKT DateTime (use for UI and schedule comparisons)
// always returns local PKT DateTime
DateTime nowDT() {
  if (rtcAvailable() && rtc.begin()) {
    DateTime t = rtc.now();
    if (isValidDateTime(t)) return t; // DS3231 holds local PKT
  }
  // fallback: system UTC → add offset
  time_t sys = time(nullptr);
  return DateTime(sys + gmtOffset_sec);
}

DateTime getLocalDateTimeFromSystem() {
  return nowDT();
}

// ---- Filesystem and persistence ----
void saveConfig() {
  DynamicJsonDocument doc(4096);
  doc["overrideActive"] = cfg.overrideActive;
  doc["overrideStateOn"] = cfg.overrideStateOn;
  doc["overrideUntil"] = cfg.overrideUntil;
   doc["ntpSSID"] = ntpSSID;
  doc["ntpPassword"] = ntpPassword;
  JsonArray sched = doc.createNestedArray("schedule");
  for (int d=0; d<7; d++) {
    JsonArray day = sched.createNestedArray();
    for (int i=0;i<8;i++) {           //####//
      JsonArray interval = day.createNestedArray();
      interval.add(cfg.schedule[d][i][0]);
      interval.add(cfg.schedule[d][i][1]);
    }
  }
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) { Serial.println("Failed to open config for writing"); return; }
  serializeJson(doc, f);
  f.close();
}

void setDefaults() {
  cfg.overrideActive = false;
  cfg.overrideStateOn = false;
  cfg.overrideUntil = 0;
  for (int d=0; d<7; d++) for (int i=0;i<8;i++) { cfg.schedule[d][i][0] = -1; cfg.schedule[d][i][1] = -1; }     //####//
  strcpy(ntpSSID, "your_ntp_SSID");
  strcpy(ntpPassword, "your_ntp_PASS");
  saveConfig();
}

void loadConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) {
    Serial.println("No config; creating defaults");
    setDefaults();
    return;
  }
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) { Serial.println("Cannot open config file"); setDefaults(); return; }
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.println("Config parse failed; using defaults"); setDefaults(); return; }
  cfg.overrideActive = doc["overrideActive"] | false;
  cfg.overrideStateOn = doc["overrideStateOn"] | false;
  cfg.overrideUntil = doc["overrideUntil"] | 0;

   //  Load WiFi credentials if present
  const char* ssid = doc["ntpSSID"] | "";
  const char* pass = doc["ntpPassword"] | "";
  strncpy(ntpSSID, ssid, sizeof(ntpSSID) - 1);
  strncpy(ntpPassword, pass, sizeof(ntpPassword) - 1);
  ntpSSID[sizeof(ntpSSID) - 1] = '\0';
  ntpPassword[sizeof(ntpPassword) - 1] = '\0';

  JsonArray sched = doc["schedule"].as<JsonArray>();
  if (!sched) { setDefaults(); return; }
  for (int d=0; d<7; d++) {
    JsonArray day = sched[d].as<JsonArray>();
    for (int i=0;i<8;i++) {  
      JsonArray inter = day[i].as<JsonArray>();
      cfg.schedule[d][i][0] = inter[0] | -1;
      cfg.schedule[d][i][1] = inter[1] | -1;
    }
  }
}

// ---- LittleFS init with auto-format on failure ----
void initFS() {
  Serial.print("Mounting LittleFS... ");
  if (LittleFS.begin()) {
    Serial.println("OK");
    return;
  }
  Serial.println("FAILED - attempting format...");
  if (LittleFS.begin(true)) {
    Serial.println("Formatted and mounted LittleFS");
    setDefaults();
    return;
  }
  Serial.println("Format attempt failed. Try reinstalling LittleFS library.");
}

// ---- Validation: Stop>Start & no overlaps for each day ----
bool validateDayIntervals(int day, String &errMsg) {
  int lastStop = -1;
  for (int i=0;i<8;i++) {   
    int s = cfg.schedule[day][i][0];
    int e = cfg.schedule[day][i][1];
    if (s < 0 && e < 0) continue; // empty interval allowed
    if (s < 0 || e < 0) {
      errMsg = "Interval " + String(i+1) + " incomplete (start/stop required)";
      return false;
    }
    if (e <= s) {
      errMsg = "Interval " + String(i+1) + ": Stop must be > Start";
      return false;
    }
    if (s < lastStop) {
      errMsg = "Interval " + String(i+1) + " overlaps previous interval";
      return false;
    }
    lastStop = e;
  }
  return true;
}

// ---- Relay control helper ----
void setRelay(bool on) {
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(RELAY_PIN, on ? LOW : HIGH);
    digitalWrite(LED_PIN,   on ? HIGH : LOW);
  } else {
    digitalWrite(RELAY_PIN, on ? HIGH : LOW);
    digitalWrite(LED_PIN,   on ? LOW : HIGH);
  }
}

// ---- Apply logic: check override expiry then schedule ----
void applyLogicOnce() {
  DateTime now = nowDT();   // nowDT returns local PKT datetime
  uint32_t nowu = nowUnix(); // UTC epoch used for expiry comparisons

  // check override expiry
  if (cfg.overrideActive && cfg.overrideUntil != 0) {
    if (nowu >= cfg.overrideUntil) {
      cfg.overrideActive = false;
      cfg.overrideUntil = 0;
      saveConfig();
    }
  }

  // if override active, force state
  if (cfg.overrideActive) {
    setRelay(cfg.overrideStateOn);
    return;
  }

  // schedule check (use local minutes and local dow)
  int dow = now.dayOfTheWeek(); // 0 = Sunday .. 6 = Saturday
  int mins = now.hour() * 60 + now.minute();
  bool on = false;
  for (int i=0;i<8;i++) { 
    int s = cfg.schedule[dow][i][0];
    int e = cfg.schedule[dow][i][1];
    if (s >= 0 && e >= 0) {
      if (s <= mins && mins < e) { on = true; break; }
    }
  }
  setRelay(on);
}

void handleRoot() {
  DateTime now = nowDT(); // Local PKT time
  String html;

  html  = "<!doctype html><html lang='en'><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Smart Interval Controller</title>";
  html += "<style>"
          "body {margin:0; font-family:'Segoe UI',sans-serif; background:#f4f6f8; color:#333; text-align:center;}"
          "header {padding:15px; background:#fff; box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
          "h1 {margin:0; font-size:22px; color:#0066cc;}"
          ".container {padding:20px;}"
          ".status-box {background:#fff; margin:20px auto; padding:15px; border-radius:10px; max-width:320px; box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
          ".button {display:block; width:85%; margin:10px auto; padding:14px; font-size:16px; background-color:#4CAF50; color:white;"
          "border:none; border-radius:8px; cursor:pointer; transition:background-color 0.2s;}"
          ".button:hover {background-color:#45a049;}"
          ".small {font-size:14px; color:#666; margin-top:10px;}"
          "button.grey {padding:8px 14px; font-size:14px; margin-top:10px; background:#eee; color:#333; border:1px solid #ccc; border-radius:6px;}"
          "</style></head><body>";

  html += "<header><h1>Weekly Smart Timer</h1></header><div class='container'>";
  html += "<div class='status-box'>";

  char tb[64];
  snprintf(tb, sizeof(tb), "%04d-%02d-%02d %02d:%02d:%02d",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());
  html += "<div><strong>Time:</strong> " + String(tb) + "</div>";

  bool currentRelay = (digitalRead(RELAY_PIN) == (RELAY_ACTIVE_LOW ? LOW : HIGH));
  html += "<div><strong>Status:</strong> " + String(currentRelay ? "ON" : "OFF") + "</div>";

  if (cfg.overrideActive) {
    uint32_t rem = 0;
    uint32_t nowu = nowUnix();
    if (cfg.overrideUntil > nowu) rem = (cfg.overrideUntil - nowu) / 60;
    html += "<div><strong>Override:</strong> ACTIVE, forcing " +
            String(cfg.overrideStateOn ? "ON" : "OFF") +
            (cfg.overrideUntil ? (", expires in " + String(rem) + " min")
                               : ", indefinite") + "</div>";
  } else {
    html += "<div><strong>Mode:</strong> Schedule</div>";
  }

  // RTC temperature
  if (rtcAvailable() && rtc.begin()) {
  float rtcTemp = rtc.getTemperature();
   html += "<div class='small'><strong>Temperature:</strong> " + String(rtcTemp, 1) + " &deg;C</div>";
   html += "</div>"; // end status box
  } else {
     html += "<div class='small'><strong>Temperature: N/A</strong> </div>";
   html += "</div>"; // end status box
  }
  
    // Navigation buttons
  html += "<button class='button' onclick=\"location.href='/schedule'\">Edit Schedule</button>";
  html += "<button class='button' onclick=\"location.href='/override'\">Manual Override</button>";
  html += "<button class='button' onclick=\"location.href='/wifi'\">Wi-Fi Setup</button>";
  html += "<button class='button' onclick='setTimeFromBrowser()'>Set Time from Browser</button>";

  // Inline JavaScript for time sync
  html += "<script>"
          "function setTimeFromBrowser(){"
          "var d=new Date();"
          "var qs='?y='+d.getFullYear()+'&M='+(d.getMonth()+1)+'&D='+d.getDate()+'&h='+d.getHours()+'&m='+d.getMinutes()+'&s='+d.getSeconds();"
          "fetch('/updatetime'+qs).then(r=>{if(r.ok)alert('RTC updated');else alert('Update failed');});}"
          "</script>";

  // Footer
  html += "<div class='small' style='margin-top:20px;'>Connect to AP SSID: <b>" + String(AP_SSID) +
          "</b> | Password: <b>" + String(AP_PASS) + "</b></div>";

  html += "</div></body></html>";

  server.send(200, "text/html", html);
}


void handleSchedule(); // you'll use your existing schedule handler
void handleOverridePage(); // your existing override page
void handleUpdateTime(); // existing endpoint for JS-set

void handleWiFiPage() {
  if (server.method() == HTTP_POST) {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");

    Serial.printf("Testing WiFi connection to: %s\n", ssid.c_str());

    WiFi.mode(WIFI_AP_STA);  // Keep AP active while testing
    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long start = millis();
    bool connected = false;
    while (millis() - start < 8000) {
      if (WiFi.status() == WL_CONNECTED) { connected = true; break; }
      delay(500);
    }

    String msgColor = "#dc3545";  // red by default
    String msgText = "Wi-Fi connection failed!";
    String resultScript;

    if (connected) {
      Serial.println("Connected! Testing NTP...");
      configTime(0, 0, "pool.ntp.org");
      struct tm tinfo;
      bool ntpOK = getLocalTime(&tinfo, 5000);

      if (ntpOK) {
        strncpy(ntpSSID, ssid.c_str(), sizeof(ntpSSID));
        strncpy(ntpPassword, pass.c_str(), sizeof(ntpPassword));
        saveConfig();

        msgColor = "#28a745";  // green
        msgText = "Wi-Fi and NTP test successful!";
      } else {
        msgColor = "#ffc107";  // amber
        msgText = "Wi-Fi connected, but NTP failed!";
      }
    }

    // Refined result overlay (modern message box)
    resultScript =
      "<script>"
      "let overlay=document.createElement('div');"
      "overlay.style.position='fixed';"
      "overlay.style.top='0';overlay.style.left='0';"
      "overlay.style.width='100%';overlay.style.height='100%';"
      "overlay.style.background='rgba(0,0,0,0.6)';"
      "overlay.style.display='flex';overlay.style.justifyContent='center';overlay.style.alignItems='center';"
      "overlay.style.zIndex='9999';"
      "let box=document.createElement('div');"
      "box.style.background='" + msgColor + "';"
      "box.style.color='white';"
      "box.style.padding='30px 60px';"
      "box.style.borderRadius='16px';"
      "box.style.fontSize='24px';"
      "box.style.fontFamily='Segoe UI, sans-serif';"
      "box.style.boxShadow='0 8px 30px rgba(0,0,0,0.5)';"
      "box.style.textAlign='center';"
      "box.innerHTML='<b>" + msgText + "</b>';"
      "overlay.appendChild(box);"
      "document.body.appendChild(overlay);"
      "setTimeout(()=>{overlay.style.transition='opacity 0.8s';overlay.style.opacity='0';setTimeout(()=>{location='/'},1000);},2500);"
      "</script>";

    server.send(200, "text/html",
      "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>body{font-family:'Segoe UI',sans-serif;text-align:center;margin-top:60px;font-size:20px;color:#333;}</style>"
      "</head><body>"
      "<div id='overlay' style='position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.5);display:flex;align-items:center;justify-content:center;color:#fff;font-size:24px;'>"
      "<div>Testing Wi-Fi... please wait</div></div>"
      + resultScript +
      "</body></html>");
    return;
  }

  // ---------- GET: Render WiFi setup page ----------
  String html;
  html.reserve(4000);

  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
 

  html += "<title>Wi-Fi Setup</title><style>";
  html += "body{margin:0;font-family:'Segoe UI',sans-serif;background:#f4f6f8;color:#333;text-align:center;}";
  html += "header{background:#fff;padding:20px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}";
  html += "h1{margin:0;font-size:22px;color:#0066cc;}";
  html += ".container{padding:20px;}";
  html += "input{padding:10px;width:80%;max-width:320px;margin:10px;border:1px solid #ccc;border-radius:8px;font-size:16px;}";
  html += "button,a{display:block;width:80%;max-width:320px;margin:10px auto;padding:12px;border:none;border-radius:8px;font-size:16px;font-family:'Segoe UI',sans-serif;text-decoration:none;}";
  html += "button{background:#4CAF50;color:white;transition:background-color 0.2s;}button:hover{background:#45a049;cursor:pointer;}";
  html += "a{background:#6c757d;color:white;}a:hover{background:#5a6268;}";
  html += "#overlay{position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.6);display:none;justify-content:center;align-items:center;color:#fff;font-size:24px;}";
  html += "</style></head><body>";

  html += "<header><h1>Wi-Fi Setup & Test</h1></header>";
  html += "<div class='container'>";
  html += "<form method='POST' onsubmit='showOverlay()'>";
  html += "<input name='ssid' placeholder='Router SSID' value='" + String(ntpSSID) + "' required>";
  html += "<input name='pass' type='password' placeholder='Router Password' value='" + String(ntpPassword) + "' required>";
  html += "<button type='submit'>Test Connection</button>";
  html += "<a href='/'>Back to Dashboard</a>";
  html += "</form>";
  html += "<div id='overlay'><div>Connecting...</div></div>";
  html += "<script>function showOverlay(){document.getElementById('overlay').style.display='flex';}</script>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void handleSchedule() {
  if (server.method() == HTTP_POST) {
    // parse posted values into cfg.schedule, then validate per day
    for (int d=0; d<7; d++) {
      for (int i=0; i<8; i++) {
        String sname = "s" + String(d) + "_" + String(i);
        String ename = "e" + String(d) + "_" + String(i);
        if (server.hasArg(sname) && server.hasArg(ename)) {
          cfg.schedule[d][i][0] = server.arg(sname).toInt();
          cfg.schedule[d][i][1] = server.arg(ename).toInt();
        } else {
          cfg.schedule[d][i][0] = -1;
          cfg.schedule[d][i][1] = -1;
        }
      }
    }

    // validate per-day
    String errAll = "";
    bool ok = true;
    for (int d=0; d<7; d++) {
      String msg;
      if (!validateDayIntervals(d, msg)) {
        ok = false;
        static const char* daynames[7] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
        errAll += String(daynames[d]) + ": " + msg + "<br>";
      }
    }
    if (!ok) {
      String out =
      "<!doctype html><html><head>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>"
      "body{font-family:Arial,sans-serif;margin:20px;}"
      "h3{color:#cc0000;}"
      "p{font-size:16px;}"
      ".error{color:red;margin-bottom:12px;}"
      ".box{border:1px solid #ccc;padding:10px;border-radius:6px;background:#f9f9f9;}"
      ".btn{display:inline-block;padding:10px 16px;margin-top:16px;"
      "background:#0066cc;color:#fff;text-decoration:none;border-radius:6px;"
      "font-size:16px;}"
      "</style></head><body>";

      out += "<h3>Schedule Error</h3>";
      out += "<p class='error'>One or more days have invalid or overlapping intervals:</p>";
      out += "<div class='box'>" + errAll + "</div>";
      out += "<a href='/schedule' class='btn'>Back to Schedule</a>";
      out += "</body></html>";

      server.send(200, "text/html", out);
      return;
    }

    // save stable config
    saveConfig();

    server.send(200, "text/html",
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<script>setTimeout(function(){window.location.href='/'},1000);</script>"
    "<body style='font-family:Arial,sans-serif;text-align:center;margin-top:40px;'>"
    "<div style='display:inline-block;padding:10px 20px;background:#dff0d8;color:#3c763d;"
    "border:1px solid #3c763d;border-radius:6px;'>Data Saved!</div><br><br>"
    "<a href='/' style='display:inline-block;padding:10px 20px;background:#0066cc;color:#fff;"
    "text-decoration:none;border-radius:6px;'>Home</a>"
    "</body></html>");
    return;
  }

  // GET -> stream HTML table
  static const char* days[7] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", ""); // start

  server.sendContent("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  server.sendContent("<style>");
  server.sendContent("body{font-family:Arial,sans-serif;font-size:16px;margin:10px;}");
  server.sendContent("h2{text-align:center;color:#0066cc;}");
  server.sendContent("table{border-collapse:collapse;width:100%;overflow-x:auto;display:block;}");
  server.sendContent("th,td{border:1px solid #ccc;padding:6px;text-align:left;}");
  server.sendContent("th{background:#f0f0f0;}");
  server.sendContent("tr:nth-child(4n+2){background:#caffa2;}"); 
  server.sendContent("tr:nth-child(4n+3){background:#caffa2;}"); 
  server.sendContent("tr:nth-child(4n+4){background:#92bffc;}"); 
  server.sendContent("tr:nth-child(4n+5){background:#92bffc;}"); 
  
  server.sendContent("select{font-size:14px;padding:3px;}");
  server.sendContent("button,input[type=submit]{background:#0066cc;color:white;border:none;border-radius:5px;padding:8px 14px;margin:6px;font-size:16px;}");
  server.sendContent("button:hover,input[type=submit]:hover{background:#004c99;cursor:pointer;}");
  server.sendContent(".container{max-width:100%;overflow-x:auto;}");
  server.sendContent("</style>");
  server.sendContent("<title>Schedule</title></head><body>");
  server.sendContent("<h3>Weekly Schedule (8 intervals/day)</h3>");
  server.sendContent("<form method='POST'><table border='1' cellspacing='0' cellpadding='4'><tr><th>Timer</th>"); //the first blank corner
  for (int d=0; d<7; d++) server.sendContent("<th>" + String(days[d]) + "</th>");
  server.sendContent("</tr>");

  // Build rows with disable option
  for (int row=0; row<8; row++) {  
    // Start row
    server.sendContent("<tr><td>ON </td>");
    for (int d=0; d<7; d++) {
      String sname = "s" + String(d) + "_" + String(row);
      server.sendContent("<td><select name='" + sname + "' id='" + sname + "' onchange='disableStop(this,\"e" + String(d) + "_" + String(row) + "\")'>");
      // Disabled option
      server.sendContent("<option value='-1'");
      if (cfg.schedule[d][row][0] < 0) server.sendContent(" selected");
      server.sendContent(">--</option>");
      // Time options
      String buf; buf.reserve(1024);
      for (int h=0; h<24; h++) {
        for (int m=0; m<60; m+=30) {
          int val = h*60 + m;
          char tb[8]; snprintf(tb, sizeof(tb), "%02d:%02d", h, m);
          buf += "<option value='" + String(val) + "'";
          if (val == cfg.schedule[d][row][0]) buf += " selected";
          buf += ">" + String(tb) + "</option>";
          if (buf.length() > 900) { server.sendContent(buf); buf = ""; }
        }
      }
      if (buf.length()) server.sendContent(buf);
      server.sendContent("</select></td>");
    }
    server.sendContent("</tr>");

    // Stop row with 24:00 added
    //server.sendContent("<tr><td>OFF " + String(row+1) + "</td>");
    server.sendContent("<tr><td>OFF </td>");
    for (int d=0; d<7; d++) {
      String ename = "e" + String(d) + "_" + String(row);
      server.sendContent("<td><select name='" + ename + "' id='" + ename + "'>");
      // Disabled option
      server.sendContent("<option value='-1'");
      if (cfg.schedule[d][row][1] < 0 || cfg.schedule[d][row][0] < 0) server.sendContent(" selected");
      server.sendContent(">--</option>");
      
      // Time options
      String buf; buf.reserve(1024);
      for (int h=0; h<24; h++) {
        for (int m=0; m<60; m+=30) {
          int val = h*60 + m;
          char tb[8]; snprintf(tb, sizeof(tb), "%02d:%02d", h, m);
          buf += "<option value='" + String(val) + "'";
          if (val == cfg.schedule[d][row][1]) buf += " selected";
          buf += ">" + String(tb) + "</option>";
          if (buf.length() > 900) { server.sendContent(buf); buf = ""; }
        }
      }
      // Add 24:00 at the end
      buf += "<option value='1440'";
      if (cfg.schedule[d][row][1] == 1440) buf += " selected";
      buf += ">24:00</option>";

      if (buf.length()) server.sendContent(buf);
      server.sendContent("</select></td>");
    }
    server.sendContent("</tr>");
  }

  // Save + Home buttons
  server.sendContent(
    "</table><br><div style='text-align:center;'>"
    "<input type='submit' value='Save' "
    "style='display:inline-block;padding:10px 16px;background:#0066cc;color:#fff;"
    "border:none;border-radius:6px;font-family:Arial,sans-serif;font-size:16px;margin-right:10px;'>"
    "<a href='/' style='display:inline-block;padding:10px 16px;background:#0066cc;color:#fff;"
    "text-decoration:none;border-radius:6px;font-family:Arial,sans-serif;font-size:16px;'>Home</a>"
    "</div></form>"
  );

  // Copy Sunday button
  server.sendContent(
    "<div style='text-align:center;margin:15px 0;'>"
    "<button type='button' onclick='copySunday()' "
    "style='padding:10px 16px;background:#0066cc;color:#fff;border:none;"
    "border-radius:6px;font-size:16px;'>Copy Sunday to All Days</button>"
    "</div>"
  );

  server.sendContent("<script>");
  server.sendContent("function copySunday(){");
  server.sendContent(" for (var row=0; row<8; row++){"); //####//
  server.sendContent("   var startSun=document.querySelector(\"select[name='s0_\"+row+\"']\");");
  server.sendContent("   var stopSun=document.querySelector(\"select[name='e0_\"+row+\"']\");");
  server.sendContent("   if(startSun && stopSun){");
  server.sendContent("     var valStart=startSun.value;");
  server.sendContent("     var valStop=stopSun.value;");
  server.sendContent("     for (var d=1; d<7; d++){");
  server.sendContent("       var tgtStart=document.querySelector(\"select[name='s\"+d+\"_\"+row+\"']\");");
  server.sendContent("       var tgtStop=document.querySelector(\"select[name='e\"+d+\"_\"+row+\"']\");");
  server.sendContent("       if(tgtStart){ tgtStart.value = valStart; }");
  server.sendContent("       if(tgtStop){ tgtStop.value = valStop; }");
  server.sendContent("       if(tgtStart){ disableStop(tgtStart, 'e'+d+'_'+row); }"); 
  server.sendContent("     }");
  server.sendContent("   }");
  server.sendContent(" }");
  server.sendContent("}");
  server.sendContent("</script>");

  // Disable Stop when Start is —
  server.sendContent("<script>");
  server.sendContent("function disableStop(startSel, stopId){");
  server.sendContent(" var stopSel=document.getElementById(stopId);");
  server.sendContent(" if(startSel.value=='-1'){ stopSel.disabled=true; stopSel.value='-1'; } else { stopSel.disabled=false; }");
  server.sendContent("}");
  server.sendContent("window.onload=function(){");
  server.sendContent(" var allStarts=document.querySelectorAll('select[name^=s]');");
  server.sendContent(" for(var i=0;i<allStarts.length;i++){");
  server.sendContent("   var sel=allStarts[i];");
  server.sendContent("   var stopId=sel.name.replace('s','e');");
  server.sendContent("   disableStop(sel,stopId);");
  server.sendContent(" }");
  server.sendContent("};");
  server.sendContent("</script></body></html>");
}


void handleOverridePage() { 
  // Handle POST submission
  if (server.method() == HTTP_POST) {
    String state = server.arg("state");
    float hours = server.arg("hours").toFloat();
    int mins = (int)(hours * 60);

    if (state == "on") {
      cfg.overrideActive = true; cfg.overrideStateOn = true;
      cfg.overrideUntil = (mins <= 0) ? 0 : nowUnix() + (uint32_t)mins * 60;
    } else if (state == "off") {
      cfg.overrideActive = true; cfg.overrideStateOn = false;
      cfg.overrideUntil = (mins <= 0) ? 0 : nowUnix() + (uint32_t)mins * 60;
    } else if (state == "clear") {
      cfg.overrideActive = false; cfg.overrideUntil = 0;
    }
    saveConfig();

    server.send(200, "text/html",
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<script>setTimeout(function(){window.location.href='/'},1200);</script>"
      "<style>"
      "body{font-family:'Segoe UI',sans-serif;text-align:center;margin-top:40px;background:#f4f6f8;}"
      ".msg{display:inline-block;padding:12px 24px;background:#dff0d8;"
      "color:#3c763d;border:1px solid #3c763d;border-radius:10px;"
      "box-shadow:0 2px 4px rgba(0,0,0,0.1);font-size:17px;}"
      "a{display:inline-block;margin-top:20px;padding:8px 14px;background:#6c757d;"
      "color:white;text-decoration:none;border-radius:8px;font-size:15px;"
      "transition:all 0.15s ease;}"
      "a:hover{background:#5a6268;transform:scale(0.97);}"
      "</style></head><body>"
      "<div class='msg'>Override Updated!</div><br>"
      "<a href='/'> Dashboard </a>"
      "</body></html>"
    );
    return;
  }

  // Compute Current Override Status
  String statusText, color;
  if (cfg.overrideActive) {
    time_t nowT = nowUnix();
    if (cfg.overrideUntil > nowT) {
      uint32_t remaining = cfg.overrideUntil - nowT;
      uint16_t minsLeft = remaining / 60;
      uint8_t hrs = minsLeft / 60;
      uint8_t mins = minsLeft % 60;
      String timeLeft = String(hrs) + "h " + String(mins) + "m";
      statusText = String("ACTIVE — Forced ") + (cfg.overrideStateOn ? "ON" : "OFF") +
                   " (" + timeLeft + " remaining)";
      color = cfg.overrideStateOn ? "#28a745" : "#dc3545";  // green/red
    } else {
      statusText = "Expired — Override no longer active";
      color = "#6c757d";
    }
  } else {
    statusText = "Inactive";
    color = "#6c757d";
  }

  // Serve page with compact layout
  String html =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Manual Override</title>"
    "<style>"
    "body{margin:0;font-family:'Segoe UI',sans-serif;background:#f4f6f8;"
    "color:#333;text-align:center;}"
    "header{background:#fff;padding:18px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
    "h1{margin:0;font-size:22px;color:#0066cc;}"
    ".container{padding:20px;}"
    "select{padding:10px;width:80%;max-width:320px;margin:10px auto;border:1px solid #ccc;"
    "border-radius:8px;font-size:16px;display:block;}"
    "button,a{display:block;width:80%;max-width:320px;margin:10px auto;padding:12px;"
    "border:none;border-radius:8px;font-size:16px;font-family:'Segoe UI',sans-serif;"
    "text-decoration:none;transition:all 0.15s ease;}"
    "button{background:#4CAF50;color:white;}button:hover{background:#45a049;transform:scale(0.97);cursor:pointer;}"
    "button[value='clear']{background:#888;}button[value='clear']:hover{background:#777;}"
    "a.back{width:auto;display:inline-block;padding:8px 14px;margin-top:20px;background:#6c757d;color:white;"
    "border-radius:8px;font-size:15px;}a.back:hover{background:#5a6268;transform:scale(0.97);}"
    ".status{margin-top:25px;font-size:17px;font-weight:bold;color:" + color + ";}"
    "</style></head><body>"
    "<header><h1>Manual Override</h1></header>"
    "<div class='container'>"
    "<form method='POST'>"
    "<label for='hours'>Duration (hours):</label>"
    "<select name='hours' id='hours'>"
    "<option value='0.5'>0.5</option>"
    "<option value='1.0'>1.0</option>"
    "<option value='1.5'>1.5</option>"
    "<option value='2.0'>2.0</option>"
    "<option value='2.5'>2.5</option>"
    "<option value='3.0'>3.0</option>"
    "<option value='3.5'>3.5</option>"
    "<option value='4.0'>4.0</option>"
    "</select>"
    "<button name='state' value='on'>Force ON</button>"
    "<button name='state' value='off'>Force OFF</button>"
    "<button name='state' value='clear'>Clear Override</button>"
    "</form>"
    "<a class='back' href='/'>Back to Dashboard</a>"
    "<div class='status'>" + statusText + "</div>"
    "</div></body></html>";

  server.send(200, "text/html", html);
}



void handleUpdateTime() {
  if (!(server.hasArg("y") && server.hasArg("M") && server.hasArg("D") &&
        server.hasArg("h") && server.hasArg("m") && server.hasArg("s"))) {
    server.send(400, "text/plain", "missing params");
    return;
  }

  int Y = server.arg("y").toInt();
  int M = server.arg("M").toInt();
  int D = server.arg("D").toInt();
  int h = server.arg("h").toInt();
  int m = server.arg("m").toInt();
  int s = server.arg("s").toInt();

  DateTime dt(Y,M,D,h,m,s);
  updateAllRTCs(dt); // dt is local PKT
  server.send(200, "text/plain", "ok");
}

// ---- Setup / Loop ----
void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  if (RELAY_ACTIVE_LOW) { digitalWrite(RELAY_PIN, HIGH); digitalWrite(LED_PIN, HIGH); }
  else { digitalWrite(RELAY_PIN, LOW); digitalWrite(LED_PIN, LOW); }
  initFS();
  loadConfig();

  // Ensure TZ is set for local PKT conversions (even before NTP)
  setenv("TZ", "PKT-5", 1);
  tzset();

  // WiFi: AP (we keep AP active). We'll switch to STA temporarily for NTP when needed.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
 
IPAddress apIP = WiFi.softAPIP();
Serial.print("Access Point IP: ");
Serial.println(apIP);

// --- DNS Server: respond only for smarttimer.net ---
dnsServer.start(53, "smarttimer.home", apIP);
dnsServer.start(53, "www.smarttimer.home", apIP);

Serial.println("Local DNS resolver active for smarttimer.net");
Serial.println("Access via: http://smarttimer.home or http://" + apIP.toString());
  //***************************************

  // Init RTC I2C
  Wire.begin();

  // If RTC present and sane, align system clock from RTC (convert local to UTC epoch)
  if (rtcAvailable() && rtc.begin()) {
    Serial.println("RTC found and initialized");
    DateTime rnow = rtc.now();
    if (isValidDateTime(rnow)) {
      Serial.printf("RTC time: %04d-%02d-%02d %02d:%02d:%02d\n",
        rnow.year(), rnow.month(), rnow.day(), rnow.hour(), rnow.minute(), rnow.second());
        timeval tv;
      tv.tv_sec = rnow.unixtime();  // Already UTC epoch
      tv.tv_usec = 0;
      settimeofday(&tv, nullptr);
      Serial.println("System clock set from RTC.");
    } else {
      Serial.println("RTC not running or invalid; trying NTP...");
      if (syncRTCfromNTP()) { Serial.println("Synced from NTP"); }
      else Serial.println("NTP sync failed; system time unchanged.");
    }
  } else {
    Serial.println("RTC not found, trying NTP...");
    if (syncRTCfromNTP()) {
      Serial.println("NTP sync success");
    } else {
      Serial.println("No RTC, and NTP sync failed. System time may be incorrect until set manually.");
    }
  }

  // Register endpoints
  server.on("/", HTTP_GET, handleRoot);
  server.on("/schedule", HTTP_GET, handleSchedule);
  server.on("/schedule", HTTP_POST, handleSchedule);
  server.on("/override", HTTP_GET, handleOverridePage);
  server.on("/override", HTTP_POST, handleOverridePage);
  server.on("/wifi", handleWiFiPage);
  server.on("/updatetime", HTTP_GET, handleUpdateTime);

  server.begin();
  Serial.println("HTTP server started");
}

unsigned long lastLogic = 0;
void loop() {
  dnsServer.processNextRequest();  // Keep DNS server alive
  server.handleClient();

  unsigned long nowms = millis();
  if (nowms - lastLogic >= 1000UL) {
    lastLogic = nowms;
    applyLogicOnce();
  }

  if (millis() - lastRTCcheck > rtcCheckInterval) {
    lastRTCcheck = millis();
    checkRTCs();
  }
}


