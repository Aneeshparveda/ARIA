/* NOTE: This public copy is sanitized. Fill in your own Wi-Fi / Telegram / location values locally and never commit real credentials. See secrets.h.example. */
/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  ARIA v4.0.4 — Autonomous Risk Identification & Alert System         ║
 * ║  TRANSMITTER UNIT (ESP32) — PRODUCTION RELEASE                       ║
 * ║                                                                      ║
 * ║  🆕 FIXES IN v4.0.4:                                                 ║
 * ║   ✅ Telegram queue fixed — was corrupting String objects (heap crash)║
 * ║   ✅ Queue now uses char* with strdup/free (safe ownership transfer) ║
 * ║   ✅ Added null checks on queue receive                              ║
 * ║   ✅ Telegram task stack increased to 10240 for stability            ║
 * ║                                                                      ║
 * ║  🔧 VERIFIED FIXES FROM v4.0.3:                                      ║
 * ║   ✅ Telegram uses POST + JSON (no more HTTP errors)                 ║
 * ║   ✅ Removed UrlEncode dependency                                    ║
 * ║   ✅ WebServer no longer stopped/started around Telegram calls       ║
 * ║   ✅ Separate WiFiClientSecure instance per request                  ║
 * ║   ✅ LCD shows which sensors triggered CAUTION                       ║
 * ║   ✅ Web dashboard with 5-hour weather forecast                      ║
 * ║   ✅ Open-Meteo API (free, no key required)                          ║
 * ║   ✅ Flame sensor inverted logic (active-low detection)              ║
 * ║   ✅ Emergency code: 0b1101 (matches receiver)                       ║
 * ║   ✅ 5-bit parity Li-Fi protocol                                     ║
 * ║   ✅ Instant flame detection (debounce=1)                            ║
 * ║                                                                      ║
 * ║  Author: Aneesh | Version: 4.0.4 | Date: January 2025               ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

/* ═══════════════════════  PIN DEFINITIONS  ═══════════════════════════ */
constexpr int DHT_PIN              = 4;
constexpr int FLAME_PIN            = 18;
constexpr int MQ135_DO_PIN         = 13;
constexpr int MQ135_AO_PIN         = 32;
constexpr int MQ2_DO_PIN           = 14;
constexpr int MQ2_AO_PIN           = 33;
constexpr int LED_GREEN            = 2;
constexpr int LED_YELLOW           = 15;
constexpr int LED_RED              = 26;
constexpr int BUZZER_PIN           = 25;
constexpr int LASER_PIN            = 12;
constexpr int EMERGENCY_BUTTON_PIN = 16;
constexpr int RESET_BUTTON_PIN     = 17;

/* ═══════════════════════  WI-FI & TELEGRAM  ═════════════════════════ */
constexpr const char* WIFI_KEYWORD1 = "YOUR_SSID_KEYWORD_1";
constexpr const char* WIFI_KEYWORD2 = "YOUR_SSID_KEYWORD_2";
constexpr const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
constexpr const char* TG_BOT_TOKEN = "YOUR_TELEGRAM_BOT_TOKEN";
constexpr const char* TG_CHAT_ID = "YOUR_TELEGRAM_CHAT_ID";
constexpr const char* ALERT_LOCATION = "YOUR_LOCATION";

/* ═══════════════════════  WEATHER API CONFIG  ═══════════════════════ */
constexpr const char* WEATHER_LAT = "YOUR_LATITUDE";
constexpr const char* WEATHER_LON = "YOUR_LONGITUDE";
constexpr unsigned long WEATHER_UPDATE_MS = 1800000UL;

/* ═══════════════════════  TIMING & THRESHOLDS  ══════════════════════ */
constexpr unsigned long CALIBRATION_TOTAL_MS      = 180000UL;
constexpr unsigned long MQ_WARMUP_MS              =  60000UL;
constexpr unsigned long LOOP_INTERVAL_MS          =   2000UL;
constexpr unsigned long CAUTION_TIMEOUT_MS        =  60000UL;
constexpr unsigned long POST_CALIBRATION_GRACE_MS =  45000UL;
constexpr unsigned long TELEGRAM_COOLDOWN_MS      =  60000UL;
constexpr unsigned long TELEGRAM_TIMEOUT_MS       =  30000UL;
constexpr unsigned long HEARTBEAT_INTERVAL_MS     = 300000UL;
constexpr unsigned long WIFI_RECONNECT_MS         =  15000UL;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS   =  15000UL;
constexpr unsigned long LCD_SCROLL_MS             =   3000UL;

constexpr float TEMP_THRESHOLD_C = 40.0f;
constexpr float DEV_ELEVATED     = 50.0f;
constexpr float DEV_CRITICAL     = 120.0f;
constexpr float RATE_DANGEROUS   = 4.0f;
constexpr int   FLAME_DEBOUNCE   = 1;

/* ═══════════════════════  LI-FI PROTOCOL (5-BIT)  ═══════════════════ */
constexpr unsigned long LIFI_CYCLE_MS   = 1500UL;
constexpr unsigned long LIFI_START_MS   =  200UL;
constexpr unsigned long LIFI_BIT_MS     =  100UL;
constexpr uint8_t CODE_HEARTBEAT        = 0b1000;
constexpr uint8_t CODE_WARNING          = 0b1010;
constexpr uint8_t CODE_EMERGENCY        = 0b1101;
constexpr uint8_t CODE_ALLCLEAR         = 0b1001;
constexpr uint8_t LIFI_NUM_BITS         = 5;
constexpr int     ALL_CLEAR_BURST_COUNT = 5;

/* ═══════════════════════  ENUMS & STRUCTS  ══════════════════════════ */
enum ThreatLevel : uint8_t { THREAT_NORMAL, THREAT_CAUTION, THREAT_EMERGENCY };
enum LiFiPhase   : uint8_t { LIFI_IDLE, LIFI_START_PULSE, LIFI_TRANSMITTING };

struct GasSensor {
    float reading = 0.0f, prevReading = 0.0f, baseline = 500.0f;
    float deviation = 0.0f, ratePerSec = 0.0f;
    bool  doTriggered = false, calibrated = false;
};

struct FlameState {
    bool detected = false, pending = false;
    int count = 0;
};

struct EventLogEntry {
    unsigned long ts;
    String type;
};

struct EventLog {
    EventLogEntry buf[10];
    uint8_t idx = 0, count = 0;
};

struct SensorStatus {
    bool mq135Active = false;
    bool mq2Active   = false;
    bool tempActive  = false;
    bool flameActive = false;

    String getActiveList() {
        String list = "";
        if (flameActive) list += "FLAME ";
        if (mq135Active) list += "MQ135 ";
        if (mq2Active)   list += "MQ2 ";
        if (tempActive)  list += "TEMP ";
        return list.isEmpty() ? "None" : list;
    }

    int getActiveCount() {
        return (flameActive ? 1 : 0) + (mq135Active ? 1 : 0) +
               (mq2Active   ? 1 : 0) + (tempActive  ? 1 : 0);
    }
};

struct WeatherData {
    String timestamp   = "";
    float  temp        = 0.0f;
    int    humidity    = 0;
    String description = "";
    bool   valid       = false;
};

struct WeatherForecast {
    WeatherData   hourly[5];
    unsigned long lastUpdate = 0;
    bool          available  = false;
};

struct SystemState {
    float tempC = 25.0f, tempBaseline = 25.0f;
    float humidity = 50.0f, humBaseline = 50.0f;
    bool  dhtValid = false;
    GasSensor    mq135, mq2;
    FlameState   flame;
    SensorStatus activeSensors;
    ThreatLevel  currentThreat  = THREAT_NORMAL;
    ThreatLevel  previousThreat = THREAT_NORMAL;
    int  confScore       = 1;
    bool cautionActive   = false;
    bool manualEmergency = false;
    unsigned long calibDoneMs = 0, lastLoopMs = 0, cautionStartMs = 0;
    bool          resetActive    = false;
    unsigned long resetHoldStart = 0;
    EventLog log;
    unsigned long lcdScrollTimer = 0;
    uint8_t       lcdScrollIndex = 0;
};

struct LiFiState {
    LiFiPhase phase        = LIFI_IDLE;
    uint8_t   code5bit     = 0;
    int8_t    bitIndex     = 4;
    uint8_t   allClearBurst = 0;
    unsigned long cycleStartMs = 0, phaseStartMs = 0;
};

/* ═══════════════════════  GLOBALS  ═══════════════════════════════════ */
SystemState     sys;
LiFiState       lifi;
WeatherForecast weather;
DHT                  dht(DHT_PIN, DHT22);
LiquidCrystal_I2C    lcd(0x27, 16, 2);
WebServer            server(80);
Preferences          prefs;

/*
 * ╔═══════════════════════════════════════════════════════════════════╗
 * ║  TELEGRAM QUEUE FIX (v4.0.4)                                      ║
 * ║                                                                    ║
 * ║  OLD (BROKEN):                                                     ║
 * ║    xQueueCreate(5, sizeof(String))                                 ║
 * ║    → FreeRTOS does memcpy of String objects                        ║
 * ║    → String destructor frees internal buffer                       ║
 * ║    → Queue holds dangling pointer → HEAP CORRUPTION               ║
 * ║                                                                    ║
 * ║  NEW (FIXED):                                                      ║
 * ║    xQueueCreate(5, sizeof(char*))                                  ║
 * ║    → Queue holds plain pointers to strdup'd C-strings              ║
 * ║    → Sender: strdup() allocates copy on heap                       ║
 * ║    → Receiver: free() releases after use                           ║
 * ║    → No String objects cross task boundaries                       ║
 * ╚═══════════════════════════════════════════════════════════════════╝
 */
QueueHandle_t telegramQueue;

bool          wifiAvailable    = false;
String        connectedSSID    = "";
unsigned long lastTelegramSent = 0, lastHeartbeat = 0;
unsigned long blinkLastMs = 0, buzzerLastMs = 0;
bool          blinkState = false, buzzerState = false, lcdDirty = true;

/* ═══════════════════════  HELPER FUNCTIONS  ══════════════════════════ */
inline bool validTemp(float t) { return t >= -40.0f && t <= 85.0f && !isnan(t); }
inline bool validHum(float h)  { return h >= 0.0f && h <= 100.0f && !isnan(h); }
inline bool validADC(int v)    { return v >= 0 && v <= 4095; }

int readMQFiltered(int pin) {
    static int     buffers[2][8] = {{0}, {0}};
    static uint8_t indices[2]    = {0, 0};
    int channel = (pin == MQ135_AO_PIN) ? 0 : 1;
    buffers[channel][indices[channel]] = analogRead(pin);
    indices[channel] = (indices[channel] + 1) & 7;
    long sum = 0;
    for (int i = 0; i < 8; i++) sum += buffers[channel][i];
    return sum >> 3;
}

float compensateMQ(float rawADC, float tempC, float humPercent) {
    const float ALPHA = 0.02f, BETA = 0.01f, T_REF = 20.0f, H_REF = 65.0f;
    float cf = 1.0f + ALPHA * (tempC - T_REF) + BETA * (humPercent - H_REF);
    return rawADC / cf;
}

uint8_t encodeLiFi(uint8_t code4bit) {
    uint8_t parity = 0;
    for (int i = 0; i < 4; i++) parity ^= (code4bit >> i) & 1;
    return (code4bit << 1) | parity;
}

void logEvent(const String& type) {
    sys.log.buf[sys.log.idx] = { millis(), type };
    sys.log.idx = (sys.log.idx + 1) % 10;
    if (sys.log.count < 10) sys.log.count++;
    Serial.print(F("[EVENT] ")); Serial.print(millis() / 1000);
    Serial.print(F("s - ")); Serial.println(type);
}

void testBuzzer() {
    for (int i = 0; i < 2; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(100);
        digitalWrite(BUZZER_PIN, LOW);  delay(100);
    }
}

/* ═══════════════════════  TELEGRAM — CORE SEND FUNCTION  ════════════ */
bool sendTelegramDirect(const char* message) {
    if (!wifiAvailable || WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[TG] Not connected - skipping"));
        return false;
    }

    StaticJsonDocument<1024> doc;
    doc["chat_id"]    = TG_CHAT_ID;
    doc["text"]       = message;
    doc["parse_mode"] = "";

    String jsonBody;
    jsonBody.reserve(512);
    serializeJson(doc, jsonBody);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);

    HTTPClient http;

    String url = "https://api.telegram.org/bot";
    url += TG_BOT_TOKEN;
    url += "/sendMessage";

    Serial.println(F("[TG] Sending message..."));

    if (!http.begin(client, url)) {
        Serial.println(F("[TG] http.begin() failed"));
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection",   "close");
    http.setTimeout(TELEGRAM_TIMEOUT_MS);

    int httpCode = http.POST(jsonBody);

    bool success = false;
    if (httpCode == 200) {
        Serial.println(F("[TG] Sent OK (HTTP 200)"));
        success = true;
        String response = http.getString();
        Serial.print(F("[TG] Response: "));
        Serial.println(response.substring(0, 80));
    } else if (httpCode > 0) {
        Serial.printf("[TG] HTTP error: %d\n", httpCode);
        String resp = http.getString();
        Serial.print(F("[TG] Response: "));
        Serial.println(resp.substring(0, 120));
    } else {
        Serial.printf("[TG] Connection error: %d (%s)\n",
                      httpCode, http.errorToString(httpCode).c_str());
    }

    http.end();
    return success;
}

/* ═══════════════════════  TELEGRAM TASK (CORE 0)  ═══════════════════ */
/*
 * FIXED in v4.0.4:
 * Queue now holds char* pointers (not String objects).
 * - Sender calls strdup() to allocate a heap copy
 * - This task calls free() after sending
 * - No String objects cross FreeRTOS task boundaries
 * - No memcpy of complex C++ objects through the queue
 */
void telegramTask(void* parameter) {
    char* msgPtr = NULL;

    for (;;) {
        if (xQueueReceive(telegramQueue, &msgPtr, portMAX_DELAY) == pdTRUE) {

            // Safety check — strdup can return NULL if heap is exhausted
            if (msgPtr == NULL) {
                Serial.println(F("[TG] NULL message pointer - skipping"));
                continue;
            }

            // Check WiFi
            if (!wifiAvailable || WiFi.status() != WL_CONNECTED) {
                Serial.println(F("[TG] Offline - message dropped"));
                logEvent("TG_SKIP_OFFLINE");
                free(msgPtr);       // MUST free even if not sending
                msgPtr = NULL;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            // Enforce cooldown
            unsigned long now = millis();
            if (now - lastTelegramSent < TELEGRAM_COOLDOWN_MS) {
                Serial.println(F("[TG] Cooldown active - message dropped"));
                free(msgPtr);
                msgPtr = NULL;
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            // Send
            bool ok = sendTelegramDirect(msgPtr);

            // Free the allocated string AFTER sending
            free(msgPtr);
            msgPtr = NULL;

            if (ok) {
                lastTelegramSent = millis();
                logEvent("TG_SENT_OK");
            } else {
                logEvent("TG_SEND_FAIL");
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
    }
}

/*
 * Public API — called from main loop (Core 1)
 *
 * strdup() creates a heap-allocated copy of the message.
 * The pointer is passed through the queue to the Telegram task.
 * The task takes ownership and calls free() after sending.
 *
 * If strdup() fails (out of memory) or the queue is full,
 * the message is dropped gracefully.
 */
void sendTelegram(const String& msg) {
    char* copy = strdup(msg.c_str());
    if (copy == NULL) {
        Serial.println(F("[TG] strdup failed (out of memory)"));
        return;
    }

    if (xQueueSend(telegramQueue, &copy, 0) != pdTRUE) {
        Serial.println(F("[TG] Queue full - message dropped"));
        free(copy);     // Queue didn't take it, so we must free it
    }
}

/* ═══════════════════════  WI-FI  ════════════════════════════════════ */
bool connectToWiFi() {
    Serial.println(F("\n[WiFi] Scanning..."));
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    int n = WiFi.scanNetworks();
    if (n == 0) { Serial.println(F("[WiFi] No networks")); return false; }

    String targetSSID = "";
    int    bestRSSI   = -999;
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        int    rssi = WiFi.RSSI(i);
        Serial.printf("  %d. \"%s\" %ddBm\n", i + 1, ssid.c_str(), rssi);
        if ((ssid.indexOf(WIFI_KEYWORD1) >= 0 || ssid.indexOf(WIFI_KEYWORD2) >= 0)
            && rssi > bestRSSI) {
            targetSSID = ssid;
            bestRSSI   = rssi;
        }
    }

    if (targetSSID.isEmpty()) { Serial.println(F("[WiFi] Target not found")); return false; }

    Serial.printf("[WiFi] Connecting to \"%s\"...\n", targetSSID.c_str());
    connectedSSID = targetSSID;
    WiFi.begin(targetSSID.c_str(), WIFI_PASSWORD);

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
        delay(300); Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("[WiFi] IP: ")); Serial.println(WiFi.localIP());
        return true;
    }
    Serial.println(F("[WiFi] Failed"));
    return false;
}

/* ═══════════════════════  WEATHER HELPERS  ═══════════════════════════ */
String getWeatherDescription(int code) {
    if (code == 0)                      return "Clear sky";
    if (code == 1)                      return "Mainly clear";
    if (code == 2)                      return "Partly cloudy";
    if (code == 3)                      return "Overcast";
    if (code >= 45 && code <= 48)       return "Fog";
    if (code >= 51 && code <= 55)       return "Drizzle";
    if (code >= 61 && code <= 65)       return "Rain";
    if (code >= 71 && code <= 75)       return "Snow";
    if (code >= 80 && code <= 82)       return "Rain showers";
    if (code == 95)                     return "Thunderstorm";
    if (code >= 96)                     return "Thunderstorm + hail";
    return "Unknown";
}

void updateWeatherForecast() {
    if (!wifiAvailable || WiFi.status() != WL_CONNECTED) return;

    unsigned long now = millis();
    if (weather.available && (now - weather.lastUpdate < WEATHER_UPDATE_MS)) return;

    Serial.println(F("[WEATHER] Fetching forecast..."));

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);

    String url = "https://api.open-meteo.com/v1/forecast?latitude=";
    url += WEATHER_LAT;
    url += "&longitude=";
    url += WEATHER_LON;
    url += "&hourly=temperature_2m,relative_humidity_2m,weather_code&forecast_days=1&timezone=auto";

    HTTPClient http;
    if (!http.begin(client, url)) { Serial.println(F("[WEATHER] begin() failed")); return; }
    http.setTimeout(15000);

    int httpCode = http.GET();
    if (httpCode == 200) {
        String payload = http.getString();
        DynamicJsonDocument doc(8192);
        DeserializationError err = deserializeJson(doc, payload);

        if (!err) {
            JsonArray times  = doc["hourly"]["time"];
            JsonArray temps  = doc["hourly"]["temperature_2m"];
            JsonArray humids = doc["hourly"]["relative_humidity_2m"];
            JsonArray codes  = doc["hourly"]["weather_code"];

            int size = min(5, (int)times.size());
            for (int i = 0; i < size; i++) {
                weather.hourly[i].timestamp   = times[i].as<String>();
                weather.hourly[i].temp        = temps[i].as<float>();
                weather.hourly[i].humidity    = humids[i].as<int>();
                weather.hourly[i].description = getWeatherDescription(codes[i].as<int>());
                weather.hourly[i].valid       = true;
            }
            weather.available  = true;
            weather.lastUpdate = now;
            Serial.println(F("[WEATHER] Updated OK"));
        } else {
            Serial.printf("[WEATHER] JSON error: %s\n", err.c_str());
        }
    } else {
        Serial.printf("[WEATHER] HTTP %d\n", httpCode);
    }
    http.end();
}

String getWeatherHTML() {
    if (!weather.available)
        return F("<div class='card'><h3>Weather Forecast</h3><p>Fetching from Open-Meteo (refresh in 30s)</p></div>");

    String html = F("<div class='card'><h3>5-Hour Weather Forecast</h3>");
    html += F("<p style='font-size:0.85em;color:#888;margin:5px 0'>Open-Meteo API (free, no key)</p>");
    html += F("<table><tr><th>Time</th><th>Temp</th><th>Humidity</th><th>Condition</th></tr>");

    for (int i = 0; i < 5; i++) {
        if (weather.hourly[i].valid) {
            String t = weather.hourly[i].timestamp.substring(11, 16);
            html += "<tr><td>" + t + "</td>";
            html += "<td><b>" + String(weather.hourly[i].temp, 1) + " C</b></td>";
            html += "<td>" + String(weather.hourly[i].humidity) + "%</td>";
            html += "<td>" + weather.hourly[i].description + "</td></tr>";
        }
    }

    unsigned long minAgo = (millis() - weather.lastUpdate) / 60000;
    html += F("</table><p style='font-size:0.75em;color:#666;margin-top:10px'>Updates every 30 min | Last: ");
    html += String(minAgo) + " min ago</p></div>";
    return html;
}

/* ═══════════════════════  WEB DASHBOARD  ════════════════════════════ */
void handleWebRoot() {
    String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'><title>ARIA v4.0.4</title>");
    html += F("<style>body{font-family:Arial,sans-serif;background:#0a0e1a;color:#e0e6ed;padding:20px;margin:0}");
    html += F(".card{background:#151d2f;padding:15px;margin:10px 0;border-radius:8px;border:1px solid #2a3f5f;box-shadow:0 4px 6px rgba(0,0,0,0.3)}");
    html += F(".emg{background:#330a0a;border:2px solid #ff0044;animation:pulse 1s infinite}");
    html += F("table{width:100%;border-collapse:collapse;margin:10px 0}");
    html += F("th,td{padding:10px;text-align:left;border-bottom:1px solid #333}");
    html += F("th{background:#1a2332;color:#4a9eff;font-weight:bold}");
    html += F("h1{color:#4a9eff;border-bottom:2px solid #2a3f5f;padding-bottom:10px}");
    html += F("h2{color:#fff;margin-top:0}h3{color:#4a9eff;margin-top:0}");
    html += F("@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.7}}</style>");
    html += F("<meta http-equiv='refresh' content='10'></head><body>");
    html += F("<h1>ARIA v4.0.4 - Fire and Gas Detection System</h1>");
    html += F("<div id='status' class='card'>Loading...</div>");
    html += F("<div id='sensors' class='card'><h3>Active Sensors</h3><p id='sensorList'>--</p></div>");
    html += getWeatherHTML();
    html += F("<div id='log' class='card'><h3>Event Log</h3><p id='logList'>--</p></div>");
    html += F("<script>function u(){fetch('/data').then(r=>r.json()).then(d=>{");
    html += F("document.getElementById('status').innerHTML='<h2>State: '+d.state+'</h2>'");
    html += F("+'<table><tr><th>Parameter</th><th>Value</th></tr>'");
    html += F("+'<tr><td>Temperature</td><td>'+d.temp+' C</td></tr>'");
    html += F("+'<tr><td>Humidity</td><td>'+d.hum+'%</td></tr>'");
    html += F("+'<tr><td>Gas MQ135</td><td>'+d.mq135+'</td></tr>'");
    html += F("+'<tr><td>Gas MQ2</td><td>'+d.mq2+'</td></tr>'");
    html += F("+'<tr><td>Flame</td><td>'+d.flame+'</td></tr>'");
    html += F("+'<tr><td>Confidence</td><td>'+d.conf+'/4</td></tr>'");
    html += F("+'<tr><td>Li-Fi Code</td><td>'+d.lifi+'</td></tr></table>';");
    html += F("document.getElementById('sensorList').innerHTML='<b>Confidence '+d.conf+'/4:</b> '+d.activeSensors;");
    html += F("document.getElementById('logList').innerHTML=d.log;");
    html += F("document.getElementById('status').className=(d.state=='EMERGENCY'?'card emg':'card');");
    html += F("});} setInterval(u,2000); u();</script></body></html>");
    server.send(200, "text/html", html);
}

void handleWebData() {
    String s = sys.manualEmergency ? "EMERGENCY" :
               (sys.currentThreat == THREAT_EMERGENCY ? "EMERGENCY" :
               (sys.currentThreat == THREAT_CAUTION   ? "CAUTION"   : "NORMAL"));

    String logStr = "";
    for (int i = 0; i < sys.log.count; i++) {
        int idx = (sys.log.idx - 1 - i + 10) % 10;
        logStr += sys.log.buf[idx].type + " (" + String(sys.log.buf[idx].ts / 1000) + "s) | ";
    }

    String activeSensors = sys.activeSensors.getActiveList();
    if (sys.currentThreat == THREAT_NORMAL) activeSensors = "All sensors normal";

    String j = "{";
    j += "\"state\":\""         + s                                          + "\",";
    j += "\"temp\":"            + String(sys.tempC, 1)                       +  ",";
    j += "\"hum\":"             + String((int)sys.humidity)                  +  ",";
    j += "\"mq135\":"           + String((int)sys.mq135.reading)             +  ",";
    j += "\"mq2\":"             + String((int)sys.mq2.reading)               +  ",";
    j += "\"flame\":\""         + String(sys.flame.detected ? "YES" : "No")  + "\",";
    j += "\"conf\":"            + String(sys.confScore)                      +  ",";
    j += "\"lifi\":\"0b"        + String(lifi.code5bit, BIN)                 + "\",";
    j += "\"activeSensors\":\"" + activeSensors                              + "\",";
    j += "\"log\":\""           + logStr                                     + "\"";
    j += "}";
    server.send(200, "application/json", j);
}

/* ═══════════════════════  LI-FI TRANSMITTER  ════════════════════════ */
void runLiFiTransmitter() {
    unsigned long now = millis();
    switch (lifi.phase) {
    case LIFI_IDLE:
        if (now - lifi.cycleStartMs >= LIFI_CYCLE_MS) {
            uint8_t code4bit;
            if (sys.manualEmergency || sys.currentThreat == THREAT_EMERGENCY) {
                code4bit = CODE_EMERGENCY; lifi.allClearBurst = 0;
            } else if (sys.currentThreat == THREAT_CAUTION) {
                code4bit = CODE_WARNING;
            } else if (lifi.allClearBurst > 0) {
                code4bit = CODE_ALLCLEAR; lifi.allClearBurst--;
            } else {
                code4bit = CODE_HEARTBEAT;
            }
            lifi.code5bit     = encodeLiFi(code4bit);
            lifi.cycleStartMs = lifi.phaseStartMs = now;
            lifi.phase        = LIFI_START_PULSE;
            lifi.bitIndex     = 4;
            digitalWrite(LASER_PIN, HIGH);

            // Debug: show what we're transmitting
            Serial.print(F("[LIFI] TX code=0b"));
            for (int i = 4; i >= 0; i--) Serial.print((lifi.code5bit >> i) & 1);
            Serial.print(F(" ("));
            if (code4bit == CODE_HEARTBEAT)  Serial.print(F("HEARTBEAT"));
            if (code4bit == CODE_WARNING)    Serial.print(F("WARNING"));
            if (code4bit == CODE_EMERGENCY)  Serial.print(F("EMERGENCY"));
            if (code4bit == CODE_ALLCLEAR)   Serial.print(F("ALLCLEAR"));
            Serial.println(F(")"));
        }
        break;

    case LIFI_START_PULSE:
        if (now - lifi.phaseStartMs >= LIFI_START_MS) {
            lifi.phaseStartMs = now;
            lifi.phase        = LIFI_TRANSMITTING;
            bool b = (lifi.code5bit >> lifi.bitIndex) & 1;
            digitalWrite(LASER_PIN, b ? HIGH : LOW);
        }
        break;

    case LIFI_TRANSMITTING:
        if (now - lifi.phaseStartMs >= LIFI_BIT_MS) {
            lifi.phaseStartMs = now;
            lifi.bitIndex--;
            if (lifi.bitIndex >= 0) {
                bool b = (lifi.code5bit >> lifi.bitIndex) & 1;
                digitalWrite(LASER_PIN, b ? HIGH : LOW);
            } else {
                digitalWrite(LASER_PIN, LOW);
                lifi.phase = LIFI_IDLE;
            }
        }
        break;
    }
}

/* ═══════════════════════  BUTTONS  ══════════════════════════════════ */
void handleButtons() {
    static bool lastEmergencyRead = HIGH;
    bool emgRead = digitalRead(EMERGENCY_BUTTON_PIN);

    if (emgRead == LOW && lastEmergencyRead == HIGH && !sys.manualEmergency) {
        delay(20);
        if (digitalRead(EMERGENCY_BUTTON_PIN) == LOW) {
            sys.manualEmergency = true;
            sys.currentThreat   = THREAT_EMERGENCY;
            sys.confScore       = 4;
            lcdDirty            = true;

            Serial.println(F("\n========================================"));
            Serial.println(F("  MANUAL EMERGENCY ACTIVATED"));
            Serial.println(F("========================================"));

            String alert = "SOS MANUAL EMERGENCY\n\nLocation: ";
            alert += ALERT_LOCATION;
            alert += "\nEmergency button pressed by user.\nIMMEDIATE ACTION REQUIRED";
            sendTelegram(alert);
            logEvent("MANUAL_EMERGENCY");
        }
    }
    lastEmergencyRead = emgRead;

    bool rstRead = digitalRead(RESET_BUTTON_PIN);
    if (rstRead == LOW) {
        if (!sys.resetActive) {
            sys.resetActive    = true;
            sys.resetHoldStart = millis();
            lcd.clear();
            lcd.print(F("Hold 3s to reset"));
            lcd.setCursor(0, 1);
            lcd.print(F("3..."));
        }

        unsigned long held = millis() - sys.resetHoldStart;
        lcd.setCursor(0, 1);
        if      (held > 2000) lcd.print(F("1..."));
        else if (held > 1000) lcd.print(F("2..."));

        if (held >= 3000) {
            sys.resetActive     = false;
            sys.manualEmergency = false;
            sys.currentThreat   = THREAT_NORMAL;
            sys.confScore       = 1;
            lifi.allClearBurst  = ALL_CLEAR_BURST_COUNT;
            lcdDirty            = true;

            Serial.println(F("\n========================================"));
            Serial.println(F("  SYSTEM RESET (AUTHORIZED)"));
            Serial.println(F("========================================"));

            String alert = "ALL CLEAR - SYSTEM RESET\n\nLocation: ";
            alert += ALERT_LOCATION;
            alert += "\nReset by authorized personnel.\nFan relay stopping at Receiver.";
            sendTelegram(alert);
            logEvent("SYSTEM_RESET");

            lcd.clear();
            lcd.print(F("RESET OK"));
            delay(1500);
        }
    } else if (sys.resetActive) {
        sys.resetActive = false;
        lcdDirty        = true;
    }
}

/* ═══════════════════════  CALIBRATION  ══════════════════════════════ */
void runCalibration() {
    Serial.println(F("\n========================================"));
    Serial.println(F("  CALIBRATION - ENSURE CLEAN AIR"));
    Serial.println(F("========================================"));

    lcd.clear(); lcd.print(F("MQ Warmup..."));
    unsigned long warmupStart = millis();
    while (millis() - warmupStart < MQ_WARMUP_MS) {
        unsigned long rem = (MQ_WARMUP_MS - (millis() - warmupStart)) / 1000;
        lcd.setCursor(0, 1);
        lcd.print(F("Warmup: ")); lcd.print(rem); lcd.print(F("s  "));
        dht.readTemperature(); dht.readHumidity();
        delay(1000);
    }

    lcd.clear(); lcd.print(F("Sampling..."));
    float tempSum = 0, humSum = 0, mq135Sum = 0, mq2Sum = 0;
    int   sampleCount = 0;
    unsigned long samplingDuration = CALIBRATION_TOTAL_MS - MQ_WARMUP_MS;
    unsigned long samplingStart    = millis();

    while (millis() - samplingStart < samplingDuration) {
        unsigned long rem = (samplingDuration - (millis() - samplingStart)) / 1000;
        lcd.setCursor(0, 1);
        lcd.print(F("Sample: ")); lcd.print(rem); lcd.print(F("s  "));

        float t = dht.readTemperature();
        float h = dht.readHumidity();
        int   r135 = analogRead(MQ135_AO_PIN);
        int   r2   = analogRead(MQ2_AO_PIN);

        if (validTemp(t))   tempSum  += t;
        if (validHum(h))    humSum   += h;
        if (validADC(r135)) mq135Sum += r135;
        if (validADC(r2))   mq2Sum   += r2;

        sampleCount++;
        delay(1000);
    }

    if (sampleCount > 0) {
        sys.tempBaseline     = tempSum   / sampleCount;
        sys.humBaseline      = humSum    / sampleCount;
        sys.mq135.baseline   = mq135Sum  / sampleCount;
        sys.mq2.baseline     = mq2Sum    / sampleCount;
        sys.mq135.calibrated = true;
        sys.mq2.calibrated   = true;
    }

    sys.calibDoneMs = millis();
    Serial.println(F("\n  Calibration Complete"));
    Serial.printf("  Temp Baseline:     %.1f C\n",   sys.tempBaseline);
    Serial.printf("  Humidity Baseline: %.0f%%\n",   sys.humBaseline);
    Serial.printf("  MQ135 Baseline:    %.0f ADC\n", sys.mq135.baseline);
    Serial.printf("  MQ2 Baseline:      %.0f ADC\n", sys.mq2.baseline);

    lcd.clear(); lcd.print(F("Cal Complete!"));
    delay(1000); lcdDirty = true;
}

/* ═══════════════════════  SENSORS  ══════════════════════════════════ */
void readSensors() {
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (validTemp(t) && validHum(h)) {
        sys.tempC = t; sys.humidity = h; sys.dhtValid = true;
        if (sys.currentThreat == THREAT_NORMAL) {
            const float A = 0.1f;
            sys.tempBaseline = (1.0f - A) * sys.tempBaseline + A * t;
            sys.humBaseline  = (1.0f - A) * sys.humBaseline  + A * h;
        }
    } else {
        sys.dhtValid = false;
    }

    sys.mq135.prevReading = sys.mq135.reading;
    sys.mq135.reading     = compensateMQ(readMQFiltered(MQ135_AO_PIN), sys.tempC, sys.humidity);
    sys.mq135.doTriggered = (digitalRead(MQ135_DO_PIN) == LOW);

    sys.mq2.prevReading   = sys.mq2.reading;
    sys.mq2.reading       = compensateMQ(readMQFiltered(MQ2_AO_PIN), sys.tempC, sys.humidity);
    sys.mq2.doTriggered   = (digitalRead(MQ2_DO_PIN) == LOW);

    if (sys.currentThreat == THREAT_NORMAL) {
        const float A = 0.1f;
        sys.mq135.baseline = (1.0f - A) * sys.mq135.baseline + A * sys.mq135.reading;
        sys.mq2.baseline   = (1.0f - A) * sys.mq2.baseline   + A * sys.mq2.reading;
    }

    // Flame sensor — active-low
    bool rawFlame = (digitalRead(FLAME_PIN) == LOW);
    if (rawFlame == sys.flame.pending) {
        if (++sys.flame.count >= FLAME_DEBOUNCE) {
            if (sys.flame.detected != rawFlame) {
                Serial.println(F("[FLAME] State changed!"));
                lcdDirty = true;
            }
            sys.flame.detected = rawFlame;
            sys.flame.count    = 0;
        }
    } else {
        sys.flame.pending = rawFlame;
        sys.flame.count   = 0;
    }
}

/* ═══════════════════════  THREAT ANALYSIS  ══════════════════════════ */
void analyzeThreats() {
    sys.previousThreat = sys.currentThreat;

    sys.activeSensors.flameActive = false;
    sys.activeSensors.mq135Active = false;
    sys.activeSensors.mq2Active   = false;
    sys.activeSensors.tempActive  = false;

    // EMERGENCY
    if (sys.manualEmergency || sys.flame.detected) {
        sys.currentThreat             = THREAT_EMERGENCY;
        sys.confScore                 = 4;
        sys.activeSensors.flameActive = true;

        if (sys.previousThreat != THREAT_EMERGENCY) {
            lcdDirty = true;
            Serial.println(F("\n  EMERGENCY STATE - Li-Fi 0b11011"));

            String alert = "EMERGENCY ALERT\n\nLocation: ";
            alert += ALERT_LOCATION;
            alert += "\nFIRE DETECTED\n\n";
            if (sys.dhtValid) {
                alert += "Temperature: " + String(sys.tempC, 1) + "C\n";
                alert += "Humidity: "    + String((int)sys.humidity) + "%\n";
            }
            alert += "MQ135: "  + String((int)sys.mq135.reading) + "\n";
            alert += "MQ2: "    + String((int)sys.mq2.reading)   + "\n";
            alert += "Flame: "  + String(sys.flame.detected ? "DETECTED" : "Manual Button") + "\n\n";
            alert += "Fan relay activating at Receiver.\nTAKE IMMEDIATE ACTION";
            sendTelegram(alert);
            logEvent("EMERGENCY_TRIGGERED");
        }
        return;
    }

    // CAUTION
    bool mq135High = sys.mq135.doTriggered || (sys.mq135.reading - sys.mq135.baseline > DEV_ELEVATED);
    bool mq2High   = sys.mq2.doTriggered   || (sys.mq2.reading   - sys.mq2.baseline   > DEV_ELEVATED);
    bool tempHigh  = sys.tempC > TEMP_THRESHOLD_C;

    sys.activeSensors.mq135Active = mq135High;
    sys.activeSensors.mq2Active   = mq2High;
    sys.activeSensors.tempActive  = tempHigh;

    if (mq135High || mq2High || tempHigh) {
        sys.currentThreat = THREAT_CAUTION;
        sys.confScore     = sys.activeSensors.getActiveCount();

        if (sys.previousThreat != THREAT_CAUTION) {
            lcdDirty = true;
            Serial.println(F("\n  CAUTION STATE - Li-Fi 0b10100"));
            Serial.printf("  Active (%d/4): %s\n", sys.confScore, sys.activeSensors.getActiveList().c_str());

            String alert = "CAUTION ALERT\n\nLocation: ";
            alert += ALERT_LOCATION;
            alert += "\nElevated readings detected.\n\n";
            alert += "Active Sensors: " + sys.activeSensors.getActiveList() + "\n";
            alert += "Confidence: " + String(sys.confScore) + "/4\n\n";

            if (sys.dhtValid) {
                alert += "Temperature: " + String(sys.tempC, 1) + "C";
                if (tempHigh) alert += " HIGH";
                alert += "\n";
            }
            if (mq135High) {
                alert += "MQ135 (Air Quality): " + String((int)sys.mq135.reading);
                alert += " (dev +" + String((int)(sys.mq135.reading - sys.mq135.baseline)) + ")\n";
            }
            if (mq2High) {
                alert += "MQ2 (Smoke/LPG): " + String((int)sys.mq2.reading);
                alert += " (dev +" + String((int)(sys.mq2.reading - sys.mq2.baseline)) + ")\n";
            }
            sendTelegram(alert);
            logEvent("CAUTION_TRIGGERED");
        }
        return;
    }

    // NORMAL
    sys.currentThreat = THREAT_NORMAL;
    sys.confScore     = 1;

    if (sys.previousThreat != THREAT_NORMAL) {
        lcdDirty           = true;
        lifi.allClearBurst = ALL_CLEAR_BURST_COUNT;
        Serial.println(F("\n  ALL CLEAR - Li-Fi 0b10010"));

        String alert = "ALL CLEAR\n\nLocation: ";
        alert += ALERT_LOCATION;
        alert += "\nAll readings returned to normal.\nFan relay stopping at Receiver.\nConfidence: 1/4";
        sendTelegram(alert);
        logEvent("NORMAL_RESTORED");
    }
}

/* ═══════════════════════  DISPLAY  ══════════════════════════════════ */
void updateDisplay() {
    if (!lcdDirty && millis() - sys.lcdScrollTimer < LCD_SCROLL_MS) return;

    lcd.clear();
    lcd.setCursor(0, 0);

    if (sys.manualEmergency) {
        lcd.print(F("MANUAL EMERGENCY"));
    } else {
        switch (sys.currentThreat) {
            case THREAT_EMERGENCY: lcd.print(F("! EMERGENCY !")); break;
            case THREAT_CAUTION:   lcd.print(F("CAUTION     "));  break;
            default:               lcd.print(F("NORMAL      "));  break;
        }
        lcd.print(sys.confScore); lcd.print(F("/4"));
    }

    lcd.setCursor(0, 1);
    if (sys.flame.detected || sys.manualEmergency) {
        lcd.print(F("FLAME DETECTED! "));
    } else if (sys.currentThreat == THREAT_CAUTION) {
        String activeList = sys.activeSensors.getActiveList();
        if ((int)activeList.length() > 16) {
            int offset = sys.lcdScrollIndex % (activeList.length() - 15);
            lcd.print(activeList.substring(offset, offset + 16));
            sys.lcdScrollIndex++;
        } else {
            lcd.print(activeList);
            for (int i = activeList.length(); i < 16; i++) lcd.print(" ");
        }
        sys.lcdScrollTimer = millis();
    } else if (sys.dhtValid) {
        char buf[17];
        snprintf(buf, sizeof(buf), "T:%.1fC H:%d%%", sys.tempC, (int)sys.humidity);
        lcd.print(buf);
    } else {
        lcd.print(F("Sensor ERROR    "));
    }

    lcdDirty = false;
}

/* ═══════════════════════  OUTPUTS  ══════════════════════════════════ */
void updateOutputs() {
    ThreatLevel threat = sys.manualEmergency ? THREAT_EMERGENCY : sys.currentThreat;

    digitalWrite(LED_GREEN,  threat == THREAT_NORMAL  ? HIGH : LOW);
    digitalWrite(LED_YELLOW, threat == THREAT_CAUTION ? HIGH : LOW);

    if (threat == THREAT_EMERGENCY) {
        if (millis() - blinkLastMs >= 150) {
            blinkLastMs = millis();
            blinkState  = !blinkState;
            digitalWrite(LED_RED, blinkState ? HIGH : LOW);
        }
    } else {
        digitalWrite(LED_RED, LOW);
        blinkState = false;
    }

    if (threat == THREAT_NORMAL) {
        digitalWrite(BUZZER_PIN, LOW);
        buzzerState = false;
        return;
    }

    unsigned long onTime  = (threat == THREAT_EMERGENCY) ? 100UL : 200UL;
    unsigned long offTime = (threat == THREAT_EMERGENCY) ? 100UL : 600UL;

    if (millis() - buzzerLastMs >= (buzzerState ? onTime : offTime)) {
        buzzerLastMs = millis();
        buzzerState  = !buzzerState;
        digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    }
}

/* ═══════════════════════  HEARTBEAT  ════════════════════════════════ */
void sendHeartbeat() {
    if (!wifiAvailable) return;
    unsigned long now = millis();
    if (now - lastHeartbeat < HEARTBEAT_INTERVAL_MS) return;
    lastHeartbeat = now;

    String msg = "ARIA Heartbeat\n\nLocation: ";
    msg += ALERT_LOCATION;
    msg += "\nStatus: ";
    if      (sys.manualEmergency)                   msg += "MANUAL EMERGENCY";
    else if (sys.currentThreat == THREAT_EMERGENCY) msg += "EMERGENCY";
    else if (sys.currentThreat == THREAT_CAUTION)   msg += "CAUTION";
    else                                             msg += "Normal";

    if (sys.currentThreat != THREAT_NORMAL)
        msg += "\nActive Sensors: " + sys.activeSensors.getActiveList();

    msg += "\nUptime: " + String(millis() / 60000UL) + " min\n\n";

    if (sys.dhtValid) {
        msg += "Temperature: " + String(sys.tempC, 1) + "C\n";
        msg += "Humidity: "    + String((int)sys.humidity) + "%\n";
    }
    msg += "MQ135: "      + String((int)sys.mq135.reading) + "\n";
    msg += "MQ2: "        + String((int)sys.mq2.reading)   + "\n";
    msg += "Flame: "      + String(sys.flame.detected ? "DETECTED" : "No") + "\n";
    msg += "Confidence: " + String(sys.confScore) + "/4";

    sendTelegram(msg);
}

/* ═══════════════════════  SETUP  ════════════════════════════════════ */
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println(F("\n========================================"));
    Serial.println(F("  ARIA v4.0.4 - ESP32 TRANSMITTER"));
    Serial.println(F("  FIX: Telegram queue memory corruption"));
    Serial.println(F("========================================\n"));

    pinMode(LASER_PIN,            OUTPUT);    digitalWrite(LASER_PIN, LOW);
    pinMode(EMERGENCY_BUTTON_PIN, INPUT_PULLUP);
    pinMode(RESET_BUTTON_PIN,     INPUT_PULLUP);
    pinMode(FLAME_PIN,            INPUT_PULLUP);
    pinMode(MQ135_DO_PIN,         INPUT_PULLUP);
    pinMode(MQ2_DO_PIN,           INPUT_PULLUP);

    for (int pin : {LED_GREEN, LED_YELLOW, LED_RED, BUZZER_PIN}) {
        pinMode(pin, OUTPUT); digitalWrite(pin, LOW);
    }

    lcd.init(); lcd.backlight(); lcd.print(F("ARIA v4.0.4"));
    dht.begin();

    /*
     * FIXED: Queue now holds char* pointers, not String objects.
     *
     * sizeof(char*) = 4 bytes on ESP32
     * Each entry is just a pointer to a strdup'd C-string on the heap.
     * The telegramTask takes ownership and calls free() after use.
     */
    telegramQueue = xQueueCreate(5, sizeof(char*));

    /*
     * Stack: 10240 bytes (was 8192)
     * The task needs stack for:
     *   - WiFiClientSecure TLS buffers
     *   - HTTPClient internal buffers
     *   - ArduinoJson serialization
     *   - Local String variables
     */
    xTaskCreatePinnedToCore(telegramTask, "TG_Task", 10240, NULL, 1, NULL, 0);

    lcd.setCursor(0, 1); lcd.print(F("WiFi..."));
    wifiAvailable = connectToWiFi();

    if (wifiAvailable) {
        server.on("/",     handleWebRoot);
        server.on("/data", handleWebData);
        server.begin();
        Serial.print(F("[Web] Dashboard: http://"));
        Serial.println(WiFi.localIP());
        updateWeatherForecast();
    }

    testBuzzer();
    runCalibration();

    sys.lastLoopMs = millis();
    lastHeartbeat  = millis();

    Serial.println(F("\n========================================"));
    Serial.println(F("  ARIA v4.0.4 ONLINE"));
    Serial.println(F("  Telegram: char* queue (fixed)"));
    Serial.println(F("  Weather:  Open-Meteo (free)"));
    Serial.println(F("  Li-Fi:    5-bit parity"));
    Serial.println(F("========================================\n"));

    // Print heap info for debugging
    Serial.printf("[MEM] Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("[MEM] Min free heap: %d bytes\n", ESP.getMinFreeHeap());
}

/* ═══════════════════════  MAIN LOOP  ════════════════════════════════ */
void loop() {
    handleButtons();

    if (wifiAvailable) {
        server.handleClient();
        updateWeatherForecast();

        if (WiFi.status() != WL_CONNECTED) {
            wifiAvailable = false;
            Serial.println(F("[WiFi] Disconnected"));
        }
    } else {
        static unsigned long lastReconnect = 0;
        if (millis() - lastReconnect > WIFI_RECONNECT_MS) {
            lastReconnect = millis();
            if (connectToWiFi()) {
                wifiAvailable = true;
                server.begin();
            }
        }
    }

    if (millis() - sys.lastLoopMs >= LOOP_INTERVAL_MS) {
        sys.lastLoopMs = millis();
        readSensors();
        analyzeThreats();
        updateDisplay();
        updateOutputs();

        Serial.printf("[STATUS] %s Conf=%d/4 | T=%.1fC MQ1=%d MQ2=%d Flame=%s | LiFi=0b",
            sys.currentThreat == THREAT_EMERGENCY ? "EMRG" :
            sys.currentThreat == THREAT_CAUTION   ? "CAUT" : "NORM",
            sys.confScore, sys.tempC,
            (int)sys.mq135.reading, (int)sys.mq2.reading,
            sys.flame.detected ? "YES" : "NO");
        for (int i = 4; i >= 0; i--) Serial.print((lifi.code5bit >> i) & 1);
        Serial.println();

        if (sys.currentThreat != THREAT_NORMAL)
            Serial.printf("         Active: %s\n", sys.activeSensors.getActiveList().c_str());

        // Periodic heap check
        static unsigned long lastHeapCheck = 0;
        if (millis() - lastHeapCheck > 30000UL) {
            lastHeapCheck = millis();
            Serial.printf("[MEM] Free: %d  Min: %d\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
        }
    }

    runLiFiTransmitter();
    sendHeartbeat();
}