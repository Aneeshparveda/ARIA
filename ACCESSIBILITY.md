# Accessibility — ARIA (fork)

## Commitment

This repository aims to be usable by as many people as possible. If
something in this repository is a barrier to you, please report it —
see "How to report a barrier" below — and it will be treated as a
genuine issue, not a cosmetic complaint.

## What is accessible today

- The README is plain-text Markdown with structured headings, tables
  and descriptive link text, so it can be read by screen readers and
  other text tools.
- The two project docs are standalone HTML pages that can be opened
  in any browser.
- The system itself gives alerts in more than one channel: LEDs and
  LCD (visual), buzzer and voice audio (audible), Telegram messages
  (phone), and a fan relay (physical action) — so a single missed
  channel does not mean a missed alert.

## Known limitations

- This repository has not had a dedicated accessibility audit. The
  HTML docs may not meet any particular accessibility standard, and
  the on-device LCD is a 16×2 character display with no alternative
  output of its own.
- The hardware build itself (soldering, wiring, laser alignment) has
  no adapted instructions yet; if you need them, report it and they
  will be written.

No dedicated accessibility audit has been carried out on this
repository, so the lists above describe intent and known gaps, not
certified conformance to any standard.

## How to report a barrier

- Open an issue in this repository describing the barrier, where it
  occurs (file or page), and what would help.
- Or email Aneesh Parveda at aneeshparveda@gmail.com with "Accessibility" in the
  subject.

Please do not include private information in a public issue. Reports
are handled by a solo maintainer, so there is no guaranteed response
time — but every report will be read and, where a fix is possible, it
will be made.

## Feedback on this statement

If this statement itself is unclear or incomplete, that is also worth
reporting, in the same way as any other barrier.
