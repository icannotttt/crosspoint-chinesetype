#include "Hyphenator.h"

#include <vector>

#include "HyphenationCommon.h"
// #include "LanguageRegistry.h"  // Legacy path kept below as commented code for easy re-enable.

const LanguageHyphenator* Hyphenator::cachedHyphenator_ = nullptr;

namespace {

// [LEGACY_LANGUAGE_PATH_START]
// Maps a BCP-47 language tag to a language-specific hyphenator.
// const LanguageHyphenator* hyphenatorForLanguage(const std::string& langTag) {
//   if (langTag.empty()) return nullptr;
//
//   // Extract primary subtag and normalize to lowercase (e.g., "en-US" -> "en").
//   std::string primary;
//   primary.reserve(langTag.size());
//   for (char c : langTag) {
//     if (c == '-' || c == '_') break;
//     if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
//     primary.push_back(c);
//   }
//   if (primary.empty()) return nullptr;
//
//   return getLanguageHyphenatorForPrimaryTag(primary);
// }
// [LEGACY_LANGUAGE_PATH_END]

// Maps a codepoint index back to its byte offset inside the source word.
size_t byteOffsetForIndex(const std::vector<CodepointInfo>& cps, const size_t index) {
  return (index < cps.size()) ? cps[index].byteOffset : (cps.empty() ? 0 : cps.back().byteOffset);
}

// Builds a vector of break information from explicit hyphen markers in the given codepoints.
std::vector<Hyphenator::BreakInfo> buildExplicitBreakInfos(const std::vector<CodepointInfo>& cps) {
  std::vector<Hyphenator::BreakInfo> breaks;

  // Scan every codepoint looking for explicit/soft hyphen markers that are surrounded by letters.
  for (size_t i = 1; i + 1 < cps.size(); ++i) {
    const uint32_t cp = cps[i].value;
    if (!isExplicitHyphen(cp) || !isAlphabetic(cps[i - 1].value) || !isAlphabetic(cps[i + 1].value)) {
      continue;
    }
    // Offset points to the next codepoint so rendering starts after the hyphen marker.
    breaks.push_back({cps[i + 1].byteOffset, isSoftHyphen(cp)});
  }

  return breaks;
}

}  // namespace

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string& word, const bool includeFallback) {
  // [HY_FORCE_ON_NEW_START]
  (void)includeFallback;

  if (word.empty()) {
    return {};
  }

  // Convert to codepoints and normalize word boundaries.
  auto cps = collectCodepoints(word);
  trimSurroundingPunctuationAndFootnote(cps);

  // [LEGACY_LANGUAGE_BREAKS_START]
  // const auto* hyphenator = cachedHyphenator_;
  // [LEGACY_LANGUAGE_BREAKS_END]

  // Explicit hyphen markers (soft or hard) take precedence over generated breaks.
  auto explicitBreakInfos = buildExplicitBreakInfos(cps);
  if (!explicitBreakInfos.empty()) {
    return explicitBreakInfos;
  }

  // Always provide generic fallback break points when hyphenation is enabled,
  // independent of publication language metadata.
  const size_t minPrefix = 2;
  const size_t minSuffix = 2;
  std::vector<size_t> indexes;
  for (size_t idx = minPrefix; idx + minSuffix <= cps.size(); ++idx) {
    indexes.push_back(idx);
  }

  // [LEGACY_LANGUAGE_BREAKS_START]
  // Ask language hyphenator for legal break points.
  // std::vector<size_t> indexes;
  // if (hyphenator) {
  //   indexes = hyphenator->breakIndexes(cps);
  // }
  //
  // // Only add fallback breaks if needed
  // if (includeFallback && indexes.empty()) {
  //   const size_t minPrefix = hyphenator ? hyphenator->minPrefix() : 2;
  //   const size_t minSuffix = hyphenator ? hyphenator->minSuffix() : 2;
  //   for (size_t idx = minPrefix; idx + minSuffix <= cps.size(); ++idx) {
  //     indexes.push_back(idx);
  //   }
  // }
  // [LEGACY_LANGUAGE_BREAKS_END]

  if (indexes.empty()) {
    return {};
  }

  std::vector<Hyphenator::BreakInfo> breaks;
  breaks.reserve(indexes.size());
  for (const size_t idx : indexes) {
    breaks.push_back({byteOffsetForIndex(cps, idx), true});
  }

  return breaks;
  // [HY_FORCE_ON_NEW_END]
}

// [LEGACY_SET_LANGUAGE_START]
// void Hyphenator::setPreferredLanguage(const std::string& lang) { cachedHyphenator_ = hyphenatorForLanguage(lang); }
// [LEGACY_SET_LANGUAGE_END]

// [HY_FORCE_ON_NEW_START]
void Hyphenator::setPreferredLanguage(const std::string& lang) {
  (void)lang;
  cachedHyphenator_ = nullptr;
}
// [HY_FORCE_ON_NEW_END]
