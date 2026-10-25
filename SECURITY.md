# Security — ARIA (fork)

ARIA is a local hardware system: an ESP32 transmitter, an Arduino Uno
receiver, a laser link between them, Telegram alerts, and a web
dashboard served on the local network. It has no user accounts and no
cloud service of its own, but it does handle credentials and it does
touch safety-adjacent behaviour, so report issues carefully.

## What to never put in a public issue or pull request

- A real Telegram bot token, chat ID, Wi-Fi password, SSID, or
  location. The public transmitter sketch uses placeholders for all of
  these; keep it that way.
- A photo or log that reveals your home network, address, or location.

## Reporting a vulnerability

Please report security issues through a GitHub Issue in this
repository, or by email to Aneesh Parveda at aneeshparveda@gmail.com.
Describe the issue, the sketch version (transmitter v4.0.4 / receiver
v4.0.7 or other), and the conditions that trigger it. Do not include
credentials or personal information in a public issue.

## Known risks, stated honestly

- **Credential exposure:** if a Telegram token or Wi-Fi password was
  ever committed or shared, it must be rotated (Telegram: BotFather →
  revoke; Wi-Fi: change the hotspot password) — deleting the commit
  alone does not un-share it.
- **Local dashboard:** the ESP32 dashboard is served on the local
  network with no authentication. Do not expose it to the internet.
- **Laser:** the Li-Fi transmitter is a laser module. Never look into
  the beam, and never align it by eye.
- **Sensor limits:** MQ sensors are indicative, not calibrated. A
  failure to detect, or a false alarm, is a known limitation of the
  prototype, not a certified-alarm failure mode. Report detection
  failures as bugs, with logs, so thresholds and baselines can be
  improved.
