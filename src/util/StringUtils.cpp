#include "StringUtils.h"

#include <cstring>

namespace StringUtils {

std::string sanitizeFilename(const std::string& name, size_t maxLength) {
  std::string result;
  result.reserve(name.size());

  for (char c : name) {
    // Replace invalid filename characters with underscore（原逻辑保留）
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      result += '_';
    } 
    // 核心修改：保留 ASCII 可打印字符 + 所有非ASCII字符（含中文）
    else if (c >= 32 && c < 127) {
      // Keep printable ASCII characters（原逻辑保留）
      result += c;
    }
    // 新增：保留非ASCII字符（中文/其他Unicode字符），仅跳过空字符
    else if (c != '\0') {
      result += c;
    }
    // Skip non-printable characters（仅空字符会被跳过）
  }

  // Trim leading/trailing spaces and dots（原逻辑保留）
  size_t start = result.find_first_not_of(" .");
  if (start == std::string::npos) {
    return "book";  // Fallback if name is all invalid characters
  }
  size_t end = result.find_last_not_of(" .");
  result = result.substr(start, end - start + 1);

  // Limit filename length（原逻辑保留）
  if (result.length() > maxLength) {
    result.resize(maxLength);
  }

  return result.empty() ? "book" : result;
}

bool checkFileExtension(const std::string& fileName, const char* extension) {
  if (fileName.length() < strlen(extension)) {
    return false;
  }

  const std::string fileExt = fileName.substr(fileName.length() - strlen(extension));
  for (size_t i = 0; i < fileExt.length(); i++) {
    if (tolower(fileExt[i]) != tolower(extension[i])) {
      return false;
    }
  }
  return true;
}

bool checkFileExtension(const String& fileName, const char* extension) {
  if (fileName.length() < strlen(extension)) {
    return false;
  }

  String localFile(fileName);
  String localExtension(extension);
  localFile.toLowerCase();
  localExtension.toLowerCase();
  return localFile.endsWith(localExtension);
}

}  // namespace StringUtils
