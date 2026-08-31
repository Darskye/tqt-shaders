#include "net.h"

#if HAVE_SECRETS

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

// Root CA bundle shipped with the Arduino core. If this symbol resolves we get
// real certificate validation with nothing to maintain; see applyTls().
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

static char     accessToken[256] = {0};
static uint32_t tokenExpiresAt   = 0;      // millis() deadline
static char     statusLine[48]   = "starting";

bool netEnabled() { return true; }

static void setStatus(const char* s) {
  strncpy(statusLine, s, sizeof(statusLine) - 1);
  statusLine[sizeof(statusLine) - 1] = '\0';
}
const char* netStatus() { return statusLine; }

// Single place the TLS trust policy is decided. The access token and refresh
// token both cross these connections, so the chain is validated against the
// core's root bundle rather than accepted blindly.
static void applyTls(WiFiClientSecure& c) {
  c.setCACertBundle(rootca_crt_bundle_start);
  c.setTimeout(12000);
}

void netBegin() {
  setStatus("wifi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                    // sleep adds latency to every poll
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool netConnected() { return WiFi.status() == WL_CONNECTED; }

// ------------------------------------------------------------------ token
static bool refreshAccessToken() {
  WiFiClientSecure client;
  applyTls(client);
  HTTPClient http;
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) {
    setStatus("token: begin failed");
    return false;
  }

  char creds[160];
  snprintf(creds, sizeof(creds), "%s:%s", SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET);
  unsigned char b64[256];
  size_t b64Len = 0;
  mbedtls_base64_encode(b64, sizeof(b64), &b64Len,
                        (const unsigned char*)creds, strlen(creds));
  b64[b64Len] = '\0';

  char authHdr[300];
  snprintf(authHdr, sizeof(authHdr), "Basic %s", (char*)b64);
  http.addHeader("Authorization", authHdr);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "grant_type=refresh_token&refresh_token=" + String(SPOTIFY_REFRESH_TOKEN);
  int code = http.POST(body);
  if (code != 200) {
    char buf[48];
    snprintf(buf, sizeof(buf), "token http %d", code);
    setStatus(buf);
    http.end();
    return false;
  }

  JsonDocument filter;
  filter["access_token"] = true;
  filter["expires_in"]   = true;
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { setStatus("token json"); return false; }

  const char* tok = doc["access_token"];
  if (!tok) { setStatus("no token"); return false; }
  strncpy(accessToken, tok, sizeof(accessToken) - 1);
  accessToken[sizeof(accessToken) - 1] = '\0';

  uint32_t ttl = doc["expires_in"] | 3600;
  tokenExpiresAt = millis() + (ttl - 60) * 1000u;   // refresh a minute early
  return true;
}

// ------------------------------------------------------------------ polling
bool netPollSpotify(NowPlaying& out) {
  if (!netConnected()) { setStatus("wifi..."); return false; }

  if (accessToken[0] == '\0' || (int32_t)(millis() - tokenExpiresAt) >= 0) {
    if (!refreshAccessToken()) return false;
  }

  WiFiClientSecure client;
  applyTls(client);
  HTTPClient http;
  if (!http.begin(client, "https://api.spotify.com/v1/me/player/currently-playing")) return false;

  char authHdr[300];
  snprintf(authHdr, sizeof(authHdr), "Bearer %s", accessToken);
  http.addHeader("Authorization", authHdr);

  int code = http.GET();

  if (code == 204) {                 // nothing playing
    http.end();
    out.valid = false;
    setStatus("nothing playing");
    return true;
  }
  if (code == 401) {                 // token died early
    http.end();
    accessToken[0] = '\0';
    return false;
  }
  if (code != 200) {
    char buf[48];
    snprintf(buf, sizeof(buf), "spotify http %d", code);
    setStatus(buf);
    http.end();
    return false;
  }

  // The full payload is several KB of album art URLs and market lists; the
  // filter keeps only these fields so the document stays small.
  JsonDocument filter;
  filter["progress_ms"]              = true;
  filter["is_playing"]               = true;
  filter["item"]["name"]             = true;
  filter["item"]["duration_ms"]      = true;
  filter["item"]["album"]["name"]    = true;
  filter["item"]["artists"][0]["name"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { setStatus("spotify json"); return false; }

  JsonObject item = doc["item"];
  if (item.isNull()) { out.valid = false; setStatus("no track"); return true; }

  strncpy(out.track,  item["name"] | "", sizeof(out.track) - 1);
  out.track[sizeof(out.track) - 1] = '\0';
  strncpy(out.album,  item["album"]["name"] | "", sizeof(out.album) - 1);
  out.album[sizeof(out.album) - 1] = '\0';
  strncpy(out.artist, item["artists"][0]["name"] | "", sizeof(out.artist) - 1);
  out.artist[sizeof(out.artist) - 1] = '\0';

  out.progressMs = doc["progress_ms"] | 0;
  out.durationMs = item["duration_ms"] | 0;
  out.playing    = doc["is_playing"]  | false;
  out.valid      = true;
  setStatus("ok");
  return true;
}

// ------------------------------------------------------------------ lyrics
static String urlEncode(const char* s) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  for (const char* p = s; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

bool netFetchLyrics(const NowPlaying& np, String& body) {
  if (!netConnected() || !np.valid) return false;

  String url = "https://lrclib.net/api/get?track_name=" + urlEncode(np.track) +
               "&artist_name=" + urlEncode(np.artist) +
               "&album_name="  + urlEncode(np.album) +
               "&duration="    + String(np.durationMs / 1000);

  WiFiClientSecure client;
  applyTls(client);
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", "tqt-shaders (github.com/Darskye/tqt-shaders)");

  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  JsonDocument filter;
  filter["syncedLyrics"] = true;
  filter["instrumental"] = true;
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) return false;

  if (doc["instrumental"] | false) { body = ""; return false; }
  const char* synced = doc["syncedLyrics"];
  if (!synced || !*synced) { body = ""; return false; }
  body = synced;
  return true;
}

#else   // ---------------------------------------------------- no credentials

bool  netEnabled()    { return false; }
void  netBegin()      {}
bool  netConnected()  { return false; }
const char* netStatus() { return "demo mode"; }
bool  netPollSpotify(NowPlaying& out) { (void)out; return false; }
bool  netFetchLyrics(const NowPlaying& np, String& body) { (void)np; (void)body; return false; }

#endif
