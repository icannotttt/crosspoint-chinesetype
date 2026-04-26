#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits (ignore UNDERLINE bit for font selection)
  const bool hasBold = (style & EpdFontStyles::BOLD) != 0;
  const bool hasItalic = (style & EpdFontStyles::ITALIC) != 0;
  const bool hasunderline = (style & EpdFontStyles::UNDERLINE) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  } else if (hasunderline && underline) {
    return underline;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
    getFont(style)->getTextDimensions(string, w, h, style);
}

int EpdFontFamily::getTextAdvance(const char* string, const Style style) const {
  return getFont(style)->getTextAdvance(string, style);
}

bool EpdFontFamily::hasPrintableChars(const char* string, const Style style) const {
  return getFont(style)->hasPrintableChars(string, style);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const {
  const EpdFont* font = getFont(style);
  return font ? font->getData(style) : nullptr;
}

const EpdGlyph* EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const {
 const EpdFont* font = getFont(style);
  return font ? font->getGlyph(cp, style) : nullptr;
}

const uint8_t* EpdFontFamily::loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer, const Style style) const {
  const EpdFont* font = getFont(style);
  return font ? font->loadGlyphBitmap(glyph, buffer, style) : nullptr;
}
