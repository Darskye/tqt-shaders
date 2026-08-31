#include "lyrics.h"
#include <string.h>
#include <stdlib.h>

void Lyrics::clear() { n_ = 0; }

const char* Lyrics::text(int i) const {
  if (i < 0 || i >= n_) return "";
  return lines_[i].text;
}

uint32_t Lyrics::timeAt(int i) const {
  if (i < 0 || i >= n_) return 0;
  return lines_[i].ms;
}

// "[mm:ss.xx]" or "[mm:ss.xxx]" or "[mm:ss]". Returns chars consumed, 0 if the
// bracket is not a timestamp (metadata tags land here and are skipped).
static int parseStamp(const char* p, uint32_t* outMs) {
  if (*p != '[') return 0;
  const char* q = p + 1;
  if (*q < '0' || *q > '9') return 0;          // "[ar:" etc

  int mm = 0;
  while (*q >= '0' && *q <= '9') { mm = mm * 10 + (*q - '0'); q++; }
  if (*q != ':') return 0;
  q++;

  int ss = 0;
  while (*q >= '0' && *q <= '9') { ss = ss * 10 + (*q - '0'); q++; }

  int frac = 0, fracDigits = 0;
  if (*q == '.' || *q == ':') {
    q++;
    while (*q >= '0' && *q <= '9' && fracDigits < 3) {
      frac = frac * 10 + (*q - '0'); q++; fracDigits++;
    }
    while (*q >= '0' && *q <= '9') q++;        // ignore extra precision
  }
  if (*q != ']') return 0;
  q++;

  while (fracDigits < 3) { frac *= 10; fracDigits++; }   // to milliseconds
  *outMs = (uint32_t)mm * 60000u + (uint32_t)ss * 1000u + (uint32_t)frac;
  return (int)(q - p);
}

static void trim(char* s) {
  int len = (int)strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\r' || s[len - 1] == '\t'))
    s[--len] = '\0';
  int lead = 0;
  while (s[lead] == ' ' || s[lead] == '\t') lead++;
  if (lead) memmove(s, s + lead, strlen(s + lead) + 1);
}

int Lyrics::parse(const char* lrc) {
  n_ = 0;
  if (!lrc) return 0;

  const char* p = lrc;
  while (*p && n_ < LYRIC_MAX_LINES) {
    const char* eol = strchr(p, '\n');
    int lineLen = eol ? (int)(eol - p) : (int)strlen(p);

    // Collect every timestamp prefixing this line.
    uint32_t stamps[8];
    int nStamps = 0;
    int off = 0;
    while (off < lineLen && nStamps < 8) {
      uint32_t ms;
      int used = parseStamp(p + off, &ms);
      if (!used) break;
      stamps[nStamps++] = ms;
      off += used;
    }

    if (nStamps) {
      char body[LYRIC_MAX_TEXT];
      int bodyLen = lineLen - off;
      if (bodyLen < 0) bodyLen = 0;
      if (bodyLen > LYRIC_MAX_TEXT - 1) bodyLen = LYRIC_MAX_TEXT - 1;
      memcpy(body, p + off, bodyLen);
      body[bodyLen] = '\0';
      trim(body);

      // Empty bodies are kept: they are the gaps between verses, and showing
      // nothing there is correct rather than holding the previous line.
      for (int i = 0; i < nStamps && n_ < LYRIC_MAX_LINES; i++) {
        lines_[n_].ms = stamps[i];
        memcpy(lines_[n_].text, body, strlen(body) + 1);
        n_++;
      }
    }

    if (!eol) break;
    p = eol + 1;
  }

  // Repeated phrases arrive out of order; timeline order is what playback needs.
  for (int i = 1; i < n_; i++) {
    LyricLine key = lines_[i];
    int j = i - 1;
    while (j >= 0 && lines_[j].ms > key.ms) { lines_[j + 1] = lines_[j]; j--; }
    lines_[j + 1] = key;
  }
  return n_;
}

int Lyrics::indexAt(uint32_t ms) const {
  if (n_ == 0 || ms < lines_[0].ms) return -1;
  int lo = 0, hi = n_ - 1, best = -1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (lines_[mid].ms <= ms) { best = mid; lo = mid + 1; }
    else                      { hi = mid - 1; }
  }
  return best;
}
