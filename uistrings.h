#ifndef UISTRINGS_H
#define UISTRINGS_H

#include <QString>

// ---------------------------------------------------------------------------
// Compile-time UI language selection.
//
// Default build  -> Korean UI.
// Define ENGLISH_UI (via CMake: -DENGLISH_UI=ON) -> English UI.
//
// Usage:  label->setText(LSTR(u8"한글", u8"English"));
//   - In the Korean build  LSTR(ko, en) yields the Korean string.
//   - In the English build LSTR(ko, en) yields the English string.
//
// Both strings live side by side so they are easy to keep in sync, and the
// choice is resolved at compile time (zero runtime cost). Song lyrics are NOT
// affected by this — only player UI text.
// ---------------------------------------------------------------------------
#ifdef ENGLISH_UI
  #define LSTR(ko, en) QString::fromUtf8(en)
#else
  #define LSTR(ko, en) QString::fromUtf8(ko)
#endif

#endif // UISTRINGS_H
