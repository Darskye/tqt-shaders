#pragma once
#include <Arduino.h>

// Secrets are optional at build time: without src/secrets.h the firmware still
// builds and runs, it just stays in demo mode instead of reaching the network.
#if __has_include("secrets.h")
  #include "secrets.h"
  #define HAVE_SECRETS 1
#else
  #define HAVE_SECRETS 0
#endif

struct NowPlaying {
  char     track[100];
  char     artist[100];
  char     album[100];
  uint32_t progressMs;
  uint32_t durationMs;
  bool     playing;
  bool     valid;        // false when nothing is playing or the poll failed
};

bool netEnabled();                 // built with credentials?
void netBegin();                   // start the WiFi join (non-blocking)
bool netConnected();
const char* netStatus();           // short human-readable state for the panel

// Polls Spotify. Refreshes the access token when needed. Returns true when
// `out` was updated with a fresh answer.
bool netPollSpotify(NowPlaying& out);

// Fetches synced LRC lyrics. Returns true and fills `body` on success;
// false when the track has no synced lyrics on LRCLIB.
bool netFetchLyrics(const NowPlaying& np, String& body);
