#pragma once
#include <stdint.h>

// Parsed LRC lyrics for one track.
//
// LRCLIB returns synced lyrics as lines of "[mm:ss.xx]text". Metadata lines
// ("[ar:...]", "[length:...]") share that shape but have a non-digit after the
// bracket, which is how they get filtered out. A line may carry several
// timestamps when a phrase repeats; each becomes its own entry.

#define LYRIC_MAX_LINES 140
#define LYRIC_MAX_TEXT  96

struct LyricLine {
  uint32_t ms;
  char     text[LYRIC_MAX_TEXT];
};

class Lyrics {
 public:
  void clear();

  // Parses in place from a NUL-terminated LRC body. Returns the line count.
  // Entries come out sorted by timestamp.
  int parse(const char* lrc);

  // Index of the line that should be showing at playback position ms, or -1
  // before the first timestamp. Callers poll this every frame, so it starts
  // from the previous answer instead of rescanning.
  int indexAt(uint32_t ms) const;

  int         count()      const { return n_; }
  bool        empty()      const { return n_ == 0; }
  const char* text(int i)  const;
  uint32_t    timeAt(int i) const;

 private:
  LyricLine lines_[LYRIC_MAX_LINES];
  int       n_ = 0;
};
