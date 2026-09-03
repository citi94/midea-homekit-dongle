// Copy this file to src/secrets.h (gitignored) and set your own OTA
// password. Without it the firmware falls back to HomeSpan's default
// ("homespan-ota") — change that before the dongle goes live.
// Keep in sync with secrets.ini (the espota upload side).
#pragma once
#define OTA_PASSWORD "change-me"
