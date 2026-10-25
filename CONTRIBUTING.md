# Contributing — ARIA (fork)

This repository is a fork of [chakshu-byte/ARIA](https://github.com/chakshu-byte/ARIA),
a Vasavi College of Engineering ECE mini-project by Parveda Aneesh,
Thammishetty Saimanish and Chakshu Varma Akkala. It holds an ESP32
transmitter (v4.0.4) and an Arduino Uno receiver (v4.0.7) that communicate
over a custom laser Li-Fi link.

## Fork rule — read this first

- Changes made here live on this fork only. If a change belongs in the
  shared project, say so in the pull request and the maintainer will
  propose it upstream; do not assume a fork commit reaches the upstream
  repo.
- Do not rewrite history, and do not change the team credit in the
  README without the team's agreement.

## Licence — undecided

Neither this fork nor the upstream repo carries a licence file. Until
the team picks one, do not treat the code as licensed for reuse, and
do not add a licence file naming a single author. A licence decision
is a team decision, not a drive-by pull request.

## What is welcome

- Bug fixes to the transmitter or receiver sketches, with the Serial
  log that shows the fault and the fix.
- Improvements to the Li-Fi decoder, threat logic, fail-safes, or docs,
  with the reasoning and the test that was run.
- Corrections to pins, thresholds, timing, or the README where the code
  and the docs disagree — quote the code line that proves it.

## Hardware and safety rules — non-negotiable

1. **Never commit real credentials.** Wi-Fi passwords, Telegram bot
   tokens, chat IDs and locations stay local only. `secrets.h` is in
   `.gitignore` for a reason. If a token was ever committed, rotate it
   before anything else.
2. **Never claim more safety than the hardware gives.** ARIA is a
   college prototype, not a certified smoke or gas alarm. Do not add
   wording that presents it as life-safety certified, and do not
   present MQ sensor readings as calibrated gas concentrations.
3. **Test safely.** Never test flame or gas detection with a real fire
   or a real gas leak in an enclosed space. Use sensor-safe methods
   (for example a lighter held briefly at a safe distance outdoors,
   or the sensor's own test behaviour) and say how you tested it.
4. **Laser safety.** Never align the laser by looking into the beam.
   Align by watching the receiver's Serial output.
5. **Audio files.** Do not commit copyrighted MP3s for the DFPlayer
   tracks. Describe the tracks; let each builder supply their own.

## How to contribute

1. Open an issue first for anything bigger than a typo, so the
   approach can be agreed before code is written.
2. Keep a pull request to one change. Say what changed, why, and how
   it was tested (which board, which sketch version, what Serial
   output). "It compiles" is not a test for a safety-adjacent system.
3. Match the existing code style: pin and timing constants at the top
   of the sketch, thresholds named, no magic numbers buried in the
   loop.
4. Update the README and the relevant doc in the same pull request
   when a pin, threshold, protocol detail, or behaviour changes.
