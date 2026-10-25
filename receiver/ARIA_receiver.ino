/*
 ╔════════════════════════════════════════════════════════════════╗
 ║  ARIA v4.0.7 — RECEIVER UNIT (Arduino Uno)                     ║                                        ║
 ╚════════════════════════════════════════════════════════════════╝
*/

#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <avr/wdt.h>

/* ════════════════════════ PIN DEFINITIONS ════════════════════════ */
#define LDR_DO_PIN      2
#define RELAY_PIN       7
#define DFPLAYER_RX    10
#define DFPLAYER_TX    11

/* ════════════════════════ RELAY POLARITY ═════════════════════════ */
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

/* ════════════════════════ LI-FI PROTOCOL ════════════════════════ */
#define LIFI_START_MS       200UL
#define LIFI_BIT_MS         100UL
#define LIFI_SAMPLE_OFFSET   50UL
#define LIFI_MIN_START_MS   150UL
#define LIFI_MAX_START_MS   400UL
#define LIFI_MSG_TIMEOUT_MS 1200UL
#define LIFI_SAMPLE_WINDOW   60UL
#define LIFI_NUM_BITS          5

/* ════════════════════════ LI-FI CODES ═══════════════════════════ */
#define CODE_HEARTBEAT   8
#define CODE_WARNING    10
#define CODE_EMERGENCY  13
#define CODE_ALLCLEAR    9

/* ════════════════════════ MP3 TRACKS ════════════════════════════ */
#define MP3_STARTUP   1
#define MP3_WARNING   2
#define MP3_EMERGENCY 3
#define MP3_ALLCLEAR  4

/* ════════════════════════ TIMING ════════════════════════════════ */
#define RELAY_CONFIRM_CYCLES    3
#define EMERGENCY_REPEAT_MS 30000UL
#define CAUTION_REPEAT_MS   60000UL
#define LIFI_LINK_TIMEOUT_MS 15000UL
#define SAFETY_CHECK_MS     10000UL
#define STATUS_PRINT_MS    300000UL

/* ════════════════════════ ENUMS ══════════════════════════════════ */
enum RxState     : uint8_t { RX_WAIT_START, RX_WAIT_DATA, RX_READ_BITS, RX_PROCESS };
enum ThreatLevel : uint8_t { THREAT_NORMAL = 0, THREAT_CAUTION = 1, THREAT_EMERGENCY = 2 };

/* ════════════════════════ GLOBALS ════════════════════════════════ */
SoftwareSerial        dfSerial(DFPLAYER_RX, DFPLAYER_TX);
DFRobotDFPlayerMini   dfPlayer;
bool                  dfReady       = false;
unsigned long         lastAudioMs   = 0;
uint8_t               lastTrack     = 0;

RxState               rxState        = RX_WAIT_START;
unsigned long         risingEdgeMs   = 0;
uint8_t               receivedBits   = 0;
bool                  sampled[LIFI_NUM_BITS];
bool                  prevLaserOn    = false;
bool                  ldrActiveLow   = true;

ThreatLevel           curThreat          = THREAT_NORMAL;
ThreatLevel           prevThreat         = THREAT_NORMAL;

bool                  relayActive        = false;
int                   relayConfirmCount  = 0;

unsigned long         lastMsgMs          = 0;
bool                  linkLost           = false;
unsigned long         lastSafetyCheckMs  = 0;

unsigned long         bootTime           = 0;
uint16_t              msgCount           = 0;
uint16_t              errorCount         = 0;
uint16_t              parityFails        = 0;
uint16_t              timingErrors       = 0;
uint8_t               lastCode           = 0;
uint16_t              goodFrames         = 0;
uint16_t              startDetections    = 0;

/* ═══════════════════ WATCHDOG DISABLE ════════════════════════════ */
void disableWatchdog() {
    wdt_reset();
    MCUSR = 0;
    WDTCSR |= (1 << WDCE) | (1 << WDE);
    WDTCSR = 0;
}

/* ═══════════════════ LDR READ ═══════════════════════════════════ */
inline bool readLaser() {
    if (ldrActiveLow) {
        return (digitalRead(LDR_DO_PIN) == LOW);
    } else {
        return (digitalRead(LDR_DO_PIN) == HIGH);
    }
}

/* ═══════════════════ PARITY & DECODE ════════════════════════════ */
bool parityOk(uint8_t frame5bit) {
    uint8_t p = 0;
    for (int i = 0; i < LIFI_NUM_BITS; i++) p ^= (frame5bit >> i) & 1;
    return (p == 0);
}

uint8_t extractCode(uint8_t frame5bit) {
    return (frame5bit >> 1) & 0x0F;
}

/* ═══════════════════ AUDIO CONTROL ══════════════════════════════ */
void playAudio(uint8_t fileNum, bool forcePlay = false) {
    if (!dfReady) return;
    if (fileNum < 1 || fileNum > 4) return;
    if (!forcePlay && (millis() - lastAudioMs < 3000UL)) return;

    Serial.print(F("[AUDIO] Playing track "));
    Serial.print(fileNum);
    Serial.println(F(" OK"));

    dfPlayer.play(fileNum);
    lastAudioMs = millis();
    lastTrack   = fileNum;
}

void handleRepeatAudio() {
    unsigned long now = millis();
    if (curThreat == THREAT_EMERGENCY && now - lastAudioMs >= EMERGENCY_REPEAT_MS) {
        playAudio(MP3_EMERGENCY, true);
    } else if (curThreat == THREAT_CAUTION && now - lastAudioMs >= CAUTION_REPEAT_MS) {
        playAudio(MP3_WARNING, true);
    }
}

/* ═══════════════════ RELAY CONTROL ══════════════════════════════ */
void forceRelayOff() {
    digitalWrite(RELAY_PIN, RELAY_OFF);
    relayActive = false;
    relayConfirmCount = 0;
    Serial.println(F("[RELAY] OFF"));
}

void checkRelayActivation() {
    if (curThreat == THREAT_EMERGENCY) {
        if (relayConfirmCount >= RELAY_CONFIRM_CYCLES && !relayActive) {
            digitalWrite(RELAY_PIN, RELAY_ON);
            relayActive = true;
            Serial.print(F("[RELAY] ON after "));
            Serial.print(relayConfirmCount);
            Serial.println(F(" confirms"));
        }
    } else {
        if (relayActive) {
            digitalWrite(RELAY_PIN, RELAY_OFF);
            relayActive = false;
            Serial.println(F("[RELAY] Deactivated"));
        }
        relayConfirmCount = 0;
    }
}

void safetyWatchdog() {
    unsigned long now = millis();
    if (now - lastSafetyCheckMs < SAFETY_CHECK_MS) return;
    lastSafetyCheckMs = now;
    if (curThreat == THREAT_NORMAL && relayActive) {
        Serial.println(F("[SAFETY] Relay ON in NORMAL - forcing OFF"));
        forceRelayOff();
    }
}

/* ═══════════════════ THREAT MANAGEMENT ══════════════════════════ */
bool setThreat(ThreatLevel newLevel) {
    if (curThreat == newLevel) return false;
    prevThreat = curThreat;
    curThreat  = newLevel;

    Serial.print(F("[THREAT] "));
    switch (prevThreat) {
        case THREAT_NORMAL:    Serial.print(F("NORMAL"));    break;
        case THREAT_CAUTION:   Serial.print(F("CAUTION"));   break;
        case THREAT_EMERGENCY: Serial.print(F("EMERGENCY")); break;
    }
    Serial.print(F(" -> "));
    switch (newLevel) {
        case THREAT_NORMAL:    Serial.println(F("NORMAL"));    break;
        case THREAT_CAUTION:   Serial.println(F("CAUTION"));   break;
        case THREAT_EMERGENCY: Serial.println(F("EMERGENCY")); break;
    }
    return true;
}

/* ═══════════════════ LINK WATCHDOG ══════════════════════════════ */
void handleLinkWatchdog() {
    if (msgCount == 0) return;
    unsigned long now = millis();
    if (!linkLost && (now - lastMsgMs > LIFI_LINK_TIMEOUT_MS)) {
        linkLost = true;
        Serial.println(F("[LINK] LOST"));
        forceRelayOff();
        setThreat(THREAT_NORMAL);
    } else if (linkLost && (now - lastMsgMs <= LIFI_LINK_TIMEOUT_MS)) {
        linkLost = false;
        Serial.println(F("[LINK] Restored"));
    }
}

/* ═══════════════════ PROCESS DECODED CODE ════════════════════════ */
void processCode(uint8_t code) {
    msgCount++;
    lastMsgMs = millis();
    linkLost  = false;
    lastCode  = code;

    Serial.print(F("[RX] Code="));
    Serial.print(code);
    Serial.print(F(" (0b"));
    for (int i = 3; i >= 0; i--) Serial.print((code >> i) & 1);
    Serial.println(F(")"));

    switch (code) {
        case CODE_HEARTBEAT:
            Serial.println(F("     -> HEARTBEAT"));
            if (setThreat(THREAT_NORMAL)) playAudio(MP3_ALLCLEAR, true);
            forceRelayOff();
            break;

        case CODE_WARNING:
            Serial.println(F("     -> WARNING"));
            if (curThreat == THREAT_EMERGENCY) forceRelayOff();
            if (setThreat(THREAT_CAUTION)) playAudio(MP3_WARNING, true);
            break;

        case CODE_EMERGENCY:
            Serial.println(F("     -> EMERGENCY"));
            relayConfirmCount++;
            if (relayConfirmCount > RELAY_CONFIRM_CYCLES)
                relayConfirmCount = RELAY_CONFIRM_CYCLES;
            Serial.print(F("     Confirm: "));
            Serial.print(relayConfirmCount);
            Serial.print(F("/"));
            Serial.println(RELAY_CONFIRM_CYCLES);

            if (setThreat(THREAT_EMERGENCY)) playAudio(MP3_EMERGENCY, true);
            checkRelayActivation();
            break;

        case CODE_ALLCLEAR:
            Serial.println(F("     -> ALL CLEAR"));
            forceRelayOff();
            if (setThreat(THREAT_NORMAL)) playAudio(MP3_ALLCLEAR, true);
            break;

        default:
            errorCount++;
            Serial.print(F("     -> UNKNOWN code="));
            Serial.print(code);
            Serial.print(F(" err#"));
            Serial.println(errorCount);
            break;
    }

    if (code != CODE_EMERGENCY) checkRelayActivation();
}

/* ═══════════════════════════════════════════════════════════════════
 *  LI-FI DECODER v4.0.7 — RISING-EDGE REFERENCED
 *
 *  PROBLEM WITH FALLING-EDGE APPROACH:
 *  ════════════════════════════════════
 *  The transmitter sends the start pulse (200ms HIGH) then
 *  IMMEDIATELY outputs data bit 4. If bit4=1, the laser stays
 *  HIGH continuously — there is NO falling edge between the
 *  start pulse and bit4.
 *
 *  For heartbeat 0b10001:
 *    [200ms HIGH start][bit4=1 HIGH][bit3=0 LOW]...
 *    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *    Laser is HIGH for 300ms straight — no falling edge at 200ms!
 *
 *  The receiver waited for a falling edge that never came,
 *  or came 100ms late (when bit3=0), shifting all samples.
 *
 *  SOLUTION:
 *  ═════════
 *  Reference ALL timing from the RISING EDGE of the start pulse.
 *  We know the start pulse is ~200ms, so:
 *    - Bit 4 starts at risingEdge + 200ms
 *    - Bit 3 starts at risingEdge + 300ms
 *    - Bit 2 starts at risingEdge + 400ms
 *    - Bit 1 starts at risingEdge + 500ms
 *    - Bit 0 starts at risingEdge + 600ms
 *
 *  We validate the start by checking that the laser is still ON
 *  at risingEdge + 150ms (confirming it's a real start pulse,
 *  not a data bit glitch).
 *
 *  We sample each bit at its midpoint:
 *    Bit 4: risingEdge + 200 + 50 = 250ms
 *    Bit 3: risingEdge + 300 + 50 = 350ms
 *    Bit 2: risingEdge + 400 + 50 = 450ms
 *    Bit 1: risingEdge + 500 + 50 = 550ms
 *    Bit 0: risingEdge + 600 + 50 = 650ms
 * ═══════════════════════════════════════════════════════════════════ */
void receiveLiFi() {
    bool laserOn = readLaser();
    unsigned long now = millis();
    bool risingEdge  = ( laserOn && !prevLaserOn);
    prevLaserOn = laserOn;

    switch (rxState) {

    /* ── WAIT FOR RISING EDGE (start of transmission) ────────────── */
    case RX_WAIT_START:
        if (risingEdge) {
            risingEdgeMs = now;
            rxState = RX_WAIT_DATA;
        }
        break;

    /* ── VALIDATE START PULSE ────────────────────────────────────── */
    /*
     * We check at 150ms after rising edge:
     *   - If laser is still ON → valid start pulse (it's at least 150ms long)
     *   - If laser is OFF → it was just a short data glitch, not a start pulse
     *
     * We DON'T wait for a falling edge because it may never come
     * (if bit4=1, laser stays HIGH from start pulse into bit4).
     *
     * After validation, we go straight to reading bits.
     * All bit timing is relative to risingEdgeMs.
     */
    case RX_WAIT_DATA: {
        unsigned long elapsed = now - risingEdgeMs;

        // Check at 150ms — is the laser still on?
        if (elapsed >= LIFI_MIN_START_MS) {
            if (laserOn) {
                // Laser still ON at 150ms+ → valid start pulse
                receivedBits = 0;
                for (int i = 0; i < LIFI_NUM_BITS; i++) sampled[i] = false;
                rxState = RX_READ_BITS;
                startDetections++;
            } else {
                // Laser went OFF before 150ms → not a valid start pulse
                timingErrors++;
                errorCount++;
                rxState = RX_WAIT_START;
            }
        }
        break;
    }

    /* ── SAMPLE DATA BITS ────────────────────────────────────────── */
    /*
     * All times relative to risingEdgeMs (start of start pulse).
     *
     * The transmitter timeline:
     *   0ms:    Laser ON (start pulse begins)
     *   200ms:  Start pulse ends, bit4 begins
     *   300ms:  bit3 begins
     *   400ms:  bit2 begins
     *   500ms:  bit1 begins
     *   600ms:  bit0 begins
     *   700ms:  Laser OFF (transmission complete)
     *
     * Sample centers (from risingEdgeMs):
     *   Bit4: 200 + 0*100 + 50 = 250ms
     *   Bit3: 200 + 1*100 + 50 = 350ms
     *   Bit2: 200 + 2*100 + 50 = 450ms
     *   Bit1: 200 + 3*100 + 50 = 550ms
     *   Bit0: 200 + 4*100 + 50 = 650ms
     *
     * NO Serial.print() in this state — timing critical!
     */
    case RX_READ_BITS: {
        unsigned long elapsed = now - risingEdgeMs;

        if (elapsed > LIFI_MSG_TIMEOUT_MS) {
            timingErrors++;
            errorCount++;
            rxState = RX_WAIT_START;
            break;
        }

        for (int txOrder = 0; txOrder < LIFI_NUM_BITS; txOrder++) {
            int bitPosition = (LIFI_NUM_BITS - 1) - txOrder;

            if (!sampled[bitPosition]) {
                // Calculate sample center from RISING EDGE
                unsigned long sampleCenter = LIFI_START_MS
                                           + (unsigned long)txOrder * LIFI_BIT_MS
                                           + LIFI_SAMPLE_OFFSET;

                if (elapsed >= sampleCenter && elapsed < sampleCenter + LIFI_SAMPLE_WINDOW) {
                    sampled[bitPosition] = true;
                    if (laserOn) {
                        receivedBits |= (1 << bitPosition);
                    }
                }
            }
        }

        bool allDone = true;
        for (int i = 0; i < LIFI_NUM_BITS; i++) {
            if (!sampled[i]) { allDone = false; break; }
        }
        if (allDone) {
            rxState = RX_PROCESS;
        }
        break;
    }

    /* ── PROCESS FRAME ───────────────────────────────────────────── */
    case RX_PROCESS:
        Serial.print(F("[RX] raw=0b"));
        for (int i = LIFI_NUM_BITS - 1; i >= 0; i--)
            Serial.print((receivedBits >> i) & 1);

        if (parityOk(receivedBits)) {
            goodFrames++;
            Serial.print(F(" GOOD #"));
            Serial.println(goodFrames);
            uint8_t code = extractCode(receivedBits);
            processCode(code);
        } else {
            parityFails++;
            errorCount++;
            uint8_t p = 0;
            for (int i = 0; i < LIFI_NUM_BITS; i++) p ^= (receivedBits >> i) & 1;
            Serial.print(F(" PARITY FAIL #"));
            Serial.print(parityFails);
            Serial.print(F(" xor="));
            Serial.print(p);
            Serial.print(F(" would_be="));
            Serial.println(extractCode(receivedBits));
        }
        rxState = RX_WAIT_START;
        break;
    }
}

/* ═══════════════════ LDR POLARITY DETECTION ═════════════════════ */
void detectLDRPolarity() {
    Serial.println(F("[LDR] Detecting polarity..."));

    int highCount = 0;
    for (int i = 0; i < 20; i++) {
        if (digitalRead(LDR_DO_PIN) == HIGH) highCount++;
        delay(50);
    }

    Serial.print(F("[LDR] Idle reads: "));
    Serial.print(highCount);
    Serial.println(F("/20 HIGH"));

    if (highCount >= 14) {
        ldrActiveLow = true;
        Serial.println(F("[LDR] ACTIVE-LOW (HIGH=no light, LOW=laser)"));
    } else if (highCount <= 6) {
        ldrActiveLow = false;
        Serial.println(F("[LDR] ACTIVE-HIGH (HIGH=laser, LOW=no light)"));
    } else {
        ldrActiveLow = true;
        Serial.println(F("[LDR] WARNING: Ambiguous! Defaulting ACTIVE-LOW"));
    }
}

/* ═══════════════════ DFPLAYER INIT ══════════════════════════════ */
bool initDFPlayer() {
    Serial.println(F("\n[DFPLAYER] Initializing..."));
    dfSerial.begin(9600);
    delay(3000);

    uint8_t resetCmd[] = {0x7E, 0xFF, 0x06, 0x0C, 0x00, 0x00, 0x00, 0xFE, 0xEE, 0xEF};
    for (uint8_t i = 0; i < sizeof(resetCmd); i++) dfSerial.write(resetCmd[i]);
    delay(2000);

    for (uint8_t attempt = 1; attempt <= 3; attempt++) {
        Serial.print(F("[DFPLAYER] Attempt "));
        Serial.print(attempt);
        Serial.print(F("/3 ... "));

        if (dfPlayer.begin(dfSerial, true, true)) {
            delay(500);
            dfPlayer.volume(25);
            delay(200);
            dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
            delay(200);
            dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);
            delay(500);
            Serial.println(F("SUCCESS"));
            return true;
        }
        Serial.println(F("FAILED"));
        if (attempt < 3) delay(2000);
    }
    Serial.println(F("[DFPLAYER] Failed"));
    return false;
}

/* ═══════════════════ STATUS REPORT ══════════════════════════════ */
void printStatus() {
    Serial.println(F(""));
    Serial.println(F("========================================="));
    Serial.println(F("       RECEIVER STATUS v4.0.7            "));
    Serial.println(F("========================================="));

    unsigned long mins = (millis() - bootTime) / 60000UL;
    Serial.print(F("  Uptime: ")); Serial.print(mins); Serial.println(F(" min"));

    Serial.print(F("  Threat: "));
    switch (curThreat) {
        case THREAT_EMERGENCY: Serial.println(F("EMERGENCY")); break;
        case THREAT_CAUTION:   Serial.println(F("CAUTION"));   break;
        default:               Serial.println(F("NORMAL"));    break;
    }

    Serial.print(F("  Relay: "));
    Serial.println(relayActive ? F("ON (fan)") : F("OFF"));

    Serial.print(F("  LDR: "));
    Serial.println(ldrActiveLow ? F("ACTIVE-LOW") : F("ACTIVE-HIGH"));

    Serial.print(F("  Messages: "));    Serial.println(msgCount);
    Serial.print(F("  Good frames: ")); Serial.println(goodFrames);
    Serial.print(F("  Starts: "));      Serial.println(startDetections);
    Serial.print(F("  Parity fails: "));Serial.println(parityFails);
    Serial.print(F("  Timing errs: ")); Serial.println(timingErrors);

    Serial.print(F("  Link: "));
    Serial.println(linkLost ? F("LOST") : F("OK"));
    Serial.print(F("  Audio: "));
    Serial.println(dfReady ? F("Ready") : F("Offline"));

    extern int __heap_start, *__brkval;
    int v;
    int freeRam = (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
    Serial.print(F("  RAM: ")); Serial.print(freeRam); Serial.println(F(" bytes"));
    Serial.println(F("========================================="));
}

/* ═══════════════════ SETUP ═══════════════════════════════════════ */
void setup() {
    disableWatchdog();
    Serial.begin(115200);
    delay(1000);

    Serial.println(F("\n========================================="));
    Serial.println(F("  ARIA v4.0.7 - RECEIVER"));
    Serial.println(F("  FIX: Rising-edge referenced timing"));
    Serial.println(F("  No falling-edge dependency"));
    Serial.println(F("========================================="));

    bootTime = millis();

    digitalWrite(RELAY_PIN, RELAY_OFF);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, RELAY_OFF);
    relayActive       = false;
    relayConfirmCount = 0;
    Serial.println(F("[INIT] Relay OFF"));

    pinMode(LDR_DO_PIN, INPUT);
    Serial.println(F("[INIT] LDR on D2"));

    detectLDRPolarity();

    prevLaserOn = readLaser();
    Serial.print(F("[INIT] Laser: "));
    Serial.println(prevLaserOn ? F("ON") : F("OFF"));

    dfReady = initDFPlayer();
    if (dfReady) {
        Serial.println(F("[INIT] Playing startup..."));
        playAudio(MP3_STARTUP, true);
        delay(5000);
    } else {
        Serial.println(F("[INIT] NO AUDIO - check wiring/SD"));
    }

    curThreat         = THREAT_NORMAL;
    prevThreat        = THREAT_NORMAL;
    relayActive       = false;
    relayConfirmCount = 0;
    linkLost          = false;
    lastMsgMs         = millis();
    lastSafetyCheckMs = millis();

    Serial.println(F("\nReady - waiting for LiFi...\n"));
    printStatus();
}

/* ═══════════════════ MAIN LOOP ═══════════════════════════════════ */
void loop() {
    static unsigned long lastStatusMs = 0;
    static unsigned long lastDebugMs  = 0;

    receiveLiFi();

    handleLinkWatchdog();
    safetyWatchdog();
    handleRepeatAudio();

    if (millis() - lastStatusMs >= STATUS_PRINT_MS) {
        lastStatusMs = millis();
        printStatus();
    }

    if (millis() - lastDebugMs >= 5000UL) {
        lastDebugMs = millis();
        Serial.print(F("[DBG] LDR_raw="));
        Serial.print(digitalRead(LDR_DO_PIN) ? F("H") : F("L"));
        Serial.print(F(" Laser="));
        Serial.print(readLaser() ? F("ON") : F("OFF"));
        Serial.print(F(" St="));
        switch (rxState) {
            case RX_WAIT_START: Serial.print(F("WAIT")); break;
            case RX_WAIT_DATA: Serial.print(F("VALD")); break;
            case RX_READ_BITS: Serial.print(F("BITS")); break;
            case RX_PROCESS:   Serial.print(F("PROC")); break;
        }
        Serial.print(F(" Thr="));
        switch (curThreat) {
            case THREAT_NORMAL:    Serial.print(F("N")); break;
            case THREAT_CAUTION:   Serial.print(F("C")); break;
            case THREAT_EMERGENCY: Serial.print(F("E")); break;
        }
        Serial.print(F(" Rly="));
        Serial.print(relayActive ? F("ON") : F("OFF"));
        Serial.print(F(" Msg="));   Serial.print(msgCount);
        Serial.print(F(" Good="));  Serial.print(goodFrames);
        Serial.print(F(" Par="));   Serial.print(parityFails);
        Serial.print(F(" Tim="));   Serial.print(timingErrors);
        Serial.print(F(" Str="));   Serial.println(startDetections);
    }
}