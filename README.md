# ARIA — Autonomous Risk Identification & Alert System

A real-time fire and hazardous-gas detection system built on an ESP32 transmitter
and an Arduino Uno receiver, communicating over a custom laser Li-Fi link and
alerting over Telegram and a local web dashboard.

**Team project — Vasavi College of Engineering, ECE mini-project**
- Parveda Aneesh
- Thammishetty Saimanish
- Chakshu Varma Akkala

Versions in this repo: transmitter v4.0.4 (ESP32), receiver v4.0.7 (Arduino Uno).
See `docs/` for the full project explanation and engineering whitepaper.

> **Security note:** This public copy is sanitized. All Wi-Fi, Telegram and
> location values in the transmitter are placeholders. Fill in your own values
> locally before flashing, and never commit real credentials. If a token or
> password was ever shared or committed elsewhere, rotate it first
> (Telegram: BotFather → revoke; Wi-Fi: change the hotspot password).

## System overview

```
Sensors → ESP32 transmitter → laser Li-Fi → Arduino Uno receiver → relay / audio
                          ↘ Telegram + web dashboard
```

- **Transmitter (ESP32):** reads sensors, classifies threat, drives LCD / LEDs /
  buzzer, hosts a web dashboard with a 5-hour Open-Meteo forecast, sends
  Telegram alerts, and transmits Li-Fi codes every ~1.5 s.
- **Receiver (Arduino Uno):** decodes the Li-Fi beam with an LDR, plays
  DFPlayer Mini audio alerts, and drives a fan relay — with no Wi-Fi of its own,
  so it keeps working if the internet is down.

## Hardware

### Transmitter — ESP32

| Part | Notes |
|---|---|
| DHT22 | Temperature & humidity |
| MQ-135 | Air quality / CO₂ / VOC |
| MQ-2 | Smoke / LPG / methane |
| Flame sensor (IR, active-low) | Instant Emergency on detection |
| Laser module | Li-Fi transmitter |
| 16×2 LCD (I²C, 0x27) | Local status |
| LEDs, buzzer, buttons | Green / Yellow / Red, audible alarm, manual Emergency, 3 s hold Reset |

Transmitter pins (from `transmitter/ARIA_transmitter.ino`):

| Signal | Pin |
|---|---|
| DHT22 | 4 |
| Flame | 18 |
| MQ-135 DO / AO | 13 / 32 |
| MQ-2 DO / AO | 14 / 33 |
| LED green / yellow / red | 2 / 15 / 26 |
| Buzzer | 25 |
| Laser | 12 |
| Emergency button / Reset button | 16 / 17 |

### Receiver — Arduino Uno

| Part | Notes |
|---|---|
| LDR (D2) | Li-Fi receiver, polarity auto-detected on startup |
| Relay (D7, active-low) | Exhaust fan, needs 3 consecutive Emergency frames |
| DFPlayer Mini (RX 10 / TX 11) | MP3 alerts from microSD |
| Speaker | Voice announcements |

## Sensing, calibration and threat logic

- **Calibration (3 min on boot):** 60 s MQ warm-up, then 120 s sampling to learn
  clean-air baselines for temperature, humidity, MQ-135 and MQ-2. Calibrate in
  clean air only.
- **Compensation:** MQ readings are temperature/humidity compensated
  (`reading / (1 + 0.02·(T−20) + 0.01·(H−65))`) and smoothed with an
  8-sample moving average. Baselines adapt slowly in Normal state only —
  they freeze during Caution / Emergency so a real leak cannot be adapted away.
- **Threat levels:**

| Level | Trigger | Response |
|---|---|---|
| Normal (1/4) | All sensors in range | Green LED, heartbeat |
| Caution | MQ elevated, or temp > 40 °C | Yellow LED, slow buzzer, Telegram Caution with active sensors and confidence score |
| Emergency (4/4) | Flame detected, or manual Emergency button | Red LED strobe, fast buzzer, Telegram SOS, fan relay at receiver |

Flame detection uses debounce = 1 by design: a flame reading triggers
Emergency immediately.

## Li-Fi protocol — 5-bit parity

Each frame is a 200 ms start pulse, then 5 bits at 100 ms each (laser ON = 1),
sampled at bit midpoints, referenced to the rising edge of the start pulse
(the receiver v4.0.7 fix for the missing falling edge when bit 4 = 1).

| State | 4-bit code | 5-bit with parity |
|---|---|---|
| Heartbeat | 0b1000 | 0b10001 |
| Warning (Caution) | 0b1010 | 0b10100 |
| Emergency | 0b1101 | 0b11011 |
| All Clear | 0b1001 | 0b10010 |

Parity is even: the receiver discards frames whose 5-bit XOR is not 0.
Link loss (no valid frame for 15 s) forces the relay OFF and returns to Normal.

## Setup

1. Open `transmitter/ARIA_transmitter.ino` in Arduino IDE, board: ESP32.
   Open `receiver/ARIA_receiver.ino`, board: Arduino Uno.
2. Libraries (transmitter): `LiquidCrystal_I2C`, `DHT`, `ArduinoJson`,
   plus ESP32 `WiFi`, `WiFiClientSecure`, `WebServer`, `HTTPClient`,
   `Preferences`, FreeRTOS. Receiver: `DFRobotDFPlayerMini`, `SoftwareSerial`.
3. Replace the placeholder values in the transmitter (see
   `transmitter/secrets.h.example`) with your own Wi-Fi, Telegram and
   location values, locally only.
4. Receiver microSD: place MP3 tracks 1–4 as startup, warning, emergency,
   all-clear.
5. Flash, power on in clean air, let calibration finish, then align the
   laser onto the receiver LDR.

The transmitter dashboard is served on the ESP32's IP (shown on Serial and
LCD) at `/`, with live data at `/data`.

## Repo layout

```
transmitter/ARIA_transmitter.ino   ESP32 transmitter (sanitized)
transmitter/secrets.h.example      Placeholder credential template
receiver/ARIA_receiver.ino         Arduino Uno receiver
docs/ARIA_Project_Explained.html   Project documentation
docs/aria_v4_whitepaper.html       Engineering whitepaper
```

## Docs

- `docs/ARIA_Project_Explained.html` — system, sensors, Li-Fi, threat logic
- `docs/aria_v4_whitepaper.html` — deeper engineering analysis, fail-safes,
  compensation and timing
