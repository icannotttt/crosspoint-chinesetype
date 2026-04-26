#pragma once
#include "EpdFont.h"
#include "EpdFontStyles.h"

class EpdFontFamily {
 public:
  //enum Style : uint8_t { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3, UNDERLINE = 4 };
 typedef EpdFontStyles::Style Style;
  static constexpr Style REGULAR = EpdFontStyles::REGULAR;
  static constexpr Style BOLD = EpdFontStyles::BOLD;
  static constexpr Style ITALIC = EpdFontStyles::ITALIC;
  static constexpr Style UNDERLINE = EpdFontStyles::UNDERLINE;
  static constexpr Style BOLD_ITALIC = EpdFontStyles::BOLD_ITALIC;

  EpdFontFamily() : regular(nullptr), bold(nullptr), italic(nullptr), underline(nullptr), boldItalic(nullptr) {}

  explicit EpdFontFamily(const EpdFont* regular, const EpdFont* bold = nullptr, const EpdFont* italic = nullptr,
                         const EpdFont* underline = nullptr,const EpdFont* boldItalic = nullptr)
      : regular(regular), bold(bold), italic(italic), underline(underline), boldItalic(boldItalic) {}
  ~EpdFontFamily() = default;
  void getTextDimensions(const char* string, int* w, int* h, Style style = EpdFontStyles::REGULAR) const;
  int getTextAdvance(const char* string, Style style = EpdFontStyles::REGULAR) const;
  bool hasPrintableChars(const char* string, Style style = EpdFontStyles::REGULAR) const;
  const EpdFontData* getData(Style style = EpdFontStyles::REGULAR) const;
  const EpdGlyph* getGlyph(uint32_t cp, Style style = EpdFontStyles::REGULAR) const;

  const EpdFont* getFont(Style style = EpdFontStyles::REGULAR) const;

  // Helper to load glyph bitmap seamlessly from either static or custom (SD-based) fonts
  const uint8_t* loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer, Style style = EpdFontStyles::REGULAR) const;

 private:
  const EpdFont* regular;
  const EpdFont* bold;
  const EpdFont* italic;
  const EpdFont* underline;
  const EpdFont* boldItalic;

};
