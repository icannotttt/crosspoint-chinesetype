#include "KeyboardEntryActivity.h"

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/NetworkConstants.h"
#include "util/QRCodeHelper.h"
#include <Utf8.h>

// Keyboard layouts - lowercase
const char* const KeyboardEntryActivity::keyboard[NUM_ROWS] = {
    "QR",
    "`1234567890-=", "qwertyuiop[]\\", "asdfghjkl;'", "zxcvbnm,./",
    "^  ____< OK"  // ^ = shift, _ = space, < = backspace, QR = remote input, OK = done
};

// Keyboard layouts - uppercase/symbols
// Keep SPECIAL ROW at index 5 so it stays a non-rendered placeholder for the bottom function row.
const char* const KeyboardEntryActivity::keyboardShift[NUM_ROWS] = {"~!@#$%^&*()_+", "QWERTYUIOP{}|", "ASDFGHJKL:\"",
                                                                    "ZXCVBNM<>?", "ZXCVBNM<>?", "SPECIAL ROW"};

// Shift state strings
const char* const KeyboardEntryActivity::shiftString[3] = {"shift", "SHIFT", "LOCK"};

void KeyboardEntryActivity::taskTrampoline(void* param) {
  auto* self = static_cast<KeyboardEntryActivity*>(param);
  self->displayTaskLoop();
}

void KeyboardEntryActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&KeyboardEntryActivity::taskTrampoline, "KeyboardEntryActivity",
              4096,               // Stack size (increased for QR code rendering)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void KeyboardEntryActivity::onExit() {
  Activity::onExit();

  // Stop web input server if running
  stopWebInputServer();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

int KeyboardEntryActivity::getRowLength(const int row) const {
  if (row < 0 || row >= NUM_ROWS) return 0;

  // Return actual length of each row based on keyboard layout
  switch (row) {
    case 0:
      return shiftState == 0 ? 2 : 13;  // lower: QR+OK, shift/lock: symbols row
    case 1:
      return 13;  // `1234567890-=
    case 2:
      return 13;  // qwertyuiop[]backslash
    case 3:
      return 11;  // asdfghjkl;'
    case 4:
      return 10;  // zxcvbnm,./
    case 5:
      return 10;  // shift(2), space(4), backspace(2), OK(2)
    default:
      return 0;
  }
}

char KeyboardEntryActivity::getSelectedChar() const {
  const char* const* layout = shiftState ? keyboardShift : keyboard;

  if (selectedRow < 0 || selectedRow >= NUM_ROWS) return '\0';
  if (selectedCol < 0 || selectedCol >= getRowLength(selectedRow)) return '\0';

  return layout[selectedRow][selectedCol];
}

void KeyboardEntryActivity::handleKeyPress() {
  if (selectedRow == TOP_ROW && shiftState == 0) {
    if (selectedCol == TOP_QR_COL) {
      // QR button - start web input server and show QR screen
      startWebInputServer();
      return;
    }
    if (selectedCol == TOP_OK_COL) {
      // Top-row OK button in normal mode
      if (onComplete) {
        onComplete(text);
      }
      return;
    }
  }
  // Handle special row (bottom row with shift, space, backspace, QR, done)
  if (selectedRow == SPECIAL_ROW) {
    if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL) {
      // Shift toggle (0 = lower case, 1 = upper case, 2 = shift lock)
      shiftState = (shiftState + 1) % 3;
      return;
    }

    if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL) {
      // Space bar
      if (maxLength == 0 || text.length() < maxLength) {
        text += ' ';
      }
      return;
    }

    if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL) {
      // Backspace (UTF-8 aware to handle Chinese and other multi-byte chars)
      if (!text.empty()) {
        utf8RemoveLastChar(text);
      }
      return;
    }



    if (selectedCol >= DONE_COL) {
      // Done button
      if (onComplete) {
        onComplete(text);
      }
      return;
    }
  }

  // Regular character
  const char c = getSelectedChar();
  if (c == '\0') {
    return;
  }

  if (maxLength == 0 || text.length() < maxLength) {
    text += c;
    // Auto-disable shift after typing a character in non-lock mode
    if (shiftState == 1) {
      shiftState = 0;
    }
  }
}

void KeyboardEntryActivity::loop() {
  // If not visible (hidden by parent) ignore all input except maybe QR exit
  if (!isVisible) return;

  // In QR mode, only handle Back button and web server polling
  if (showingQR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      stopWebInputServer();
      showingQR = false;
      updateRequired = true;
    }

    // Poll the web server for incoming requests
    if (webInputServer && webInputServer->isRunning()) {
      webInputServer->handleClient();

      if (webInputServer->hasReceivedText()) {
        std::string received = webInputServer->consumeReceivedText();
        if (maxLength > 0 && text.length() + received.length() > maxLength) {
          received.resize(maxLength - text.length());
        }
        text += received;
        // Return to keyboard view with the new text
        stopWebInputServer();
        showingQR = false;
        updateRequired = true;
      }
    }
    return;
  }

  // Handle navigation
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (selectedRow > 0) {
      selectedRow--;
    } else {
      // Wrap to bottom row
      selectedRow = NUM_ROWS - 1;
    }
    // Clamp column to valid range for new row
    if (selectedRow == TOP_ROW) {
      const int maxCol = getRowLength(TOP_ROW) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    } else {
      const int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    }
    updateRequired = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    if (selectedRow < NUM_ROWS - 1) {
      selectedRow++;
      const int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    } else {
      // Wrap to top row
      selectedRow = 0;
      const int maxCol = getRowLength(selectedRow) - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
    }
    updateRequired = true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    const int maxCol = getRowLength(selectedRow) - 1;
    if (selectedRow == TOP_ROW) {
      const int topMaxCol = getRowLength(TOP_ROW) - 1;
      if (selectedCol > topMaxCol) selectedCol = topMaxCol;
    }
    // Special bottom row case
    if (selectedRow == SPECIAL_ROW) {
      // Bottom row has special key widths
      if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL) {
        // In shift key, move to space
        selectedCol = SPACE_COL;
      } else if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL) {
        // In space bar, move to backspace
        selectedCol = BACKSPACE_COL;
      } else if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL) {
        // In backspace, move to done
        selectedCol = DONE_COL;
      } else if (selectedCol >= DONE_COL) {
        // At done button, wrap to beginning of row
        selectedCol = SHIFT_COL;
      }
      updateRequired = true;
      return;
    }

    if (selectedCol > 0) {
      selectedCol--;
    } else {
      // Wrap to end of current row
      selectedCol = maxCol;
    }
    updateRequired = true;
  }

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    const int maxCol = getRowLength(selectedRow) - 1;
    if (selectedRow == TOP_ROW) {
      const int topMaxCol = getRowLength(TOP_ROW) - 1;
      if (selectedCol > topMaxCol) selectedCol = topMaxCol;
    }
    // Special bottom row case
    if (selectedRow == SPECIAL_ROW) {
      // Bottom row has special key widths
      if (selectedCol >= SHIFT_COL && selectedCol < SPACE_COL) {
        // In shift key, move to space
        selectedCol = SPACE_COL;
      } else if (selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL) {
        // In space bar, move to backspace
        selectedCol = BACKSPACE_COL;
      } else if (selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL) {
        // In backspace, move to done
        selectedCol = DONE_COL;
      } else if (selectedCol >= DONE_COL) {
        // At done button, wrap to beginning of row
        selectedCol = SHIFT_COL;
      }
      updateRequired = true;
      return;
    }

    if (selectedCol < maxCol) {
      selectedCol++;
    } else {
      // Wrap to beginning of current row
      selectedCol = 0;
    }
    updateRequired = true;
  }

  // Selection
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleKeyPress();
    updateRequired = true;
  }

  // Cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (onCancel) {
      onCancel();
    }
    updateRequired = true;
  }
}

void KeyboardEntryActivity::render() const {
  // do nothing when hidden; parent should redraw its own contents
  if (!isVisible) return;

  if (showingQR) {
    renderQRScreen();
    return;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto metrics = UITheme::getInstance().getMetrics();
  constexpr int INPUT_FONT = UI_12_FONT_ID;
  constexpr int KEY_FONT = UI_10_FONT_ID;

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  // Draw input field
  const int lineHeight = renderer.getLineHeight(INPUT_FONT);
  const int inputStartY =
      metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.verticalSpacing * 4;
  int inputHeight = 0;

  std::string displayText;
  if (isPassword) {
    displayText = std::string(text.length(), '*');
  } else {
    displayText = text;
  }

  // Show cursor at end
  displayText += "_";

  // Render input text across multiple lines
  int lineStartIdx = 0;
  int lineEndIdx = displayText.length();
  int textWidth = 0;
  while (true) {
    std::string lineText = displayText.substr(lineStartIdx, lineEndIdx - lineStartIdx);
    textWidth = renderer.getTextWidth(INPUT_FONT, lineText.c_str());
    if (textWidth <= pageWidth - 2 * metrics.contentSidePadding) {
      renderer.drawText(INPUT_FONT, metrics.contentSidePadding, inputStartY + inputHeight, lineText.c_str());
      if (lineEndIdx == displayText.length()) {
        break;
      }

      inputHeight += lineHeight;
      lineStartIdx = lineEndIdx;
      lineEndIdx = displayText.length();
    } else {
      lineEndIdx -= 1;
    }
  }

  const int inputBoxY = inputStartY - metrics.verticalSpacing / 2;
  const int inputBoxH = inputHeight + lineHeight + metrics.verticalSpacing;
  renderer.drawRoundedRect(metrics.contentSidePadding / 2, inputBoxY, pageWidth - metrics.contentSidePadding,
                           inputBoxH, 2, 8, true);

  // Draw keyboard: vertically centered in available area, each row spans full screen width.
  constexpr int keyHeight = 30;
  constexpr int keySpacingY = 5;
  constexpr int keySpacingX = 2;
  constexpr int keySidePadding = 5;
  const int keyboardHeight = NUM_ROWS * keyHeight + (NUM_ROWS - 1) * keySpacingY;
  const int keyboardTopBound = inputBoxY + inputBoxH + metrics.verticalSpacing * 2;
  const int keyboardBottomBound = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  int keyboardStartY = keyboardTopBound + (keyboardBottomBound - keyboardTopBound - keyboardHeight) / 2;
  if (keyboardStartY < keyboardTopBound) {
    keyboardStartY = keyboardTopBound;
  }

  const auto drawKey = [&](const Rect& r, const char* label, const bool selected) {
    if (selected) {
      renderer.fillRoundedRect(r.x, r.y, r.width, r.height, 5, LightGray);
      renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 1, 5, true);
    } else {
      renderer.fillRoundedRect(r.x, r.y, r.width, r.height, 5, White);
      renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 1, 5, true);
    }

    const int textWidthLocal = renderer.getTextWidth(KEY_FONT, label);
    const int textX = r.x + (r.width - textWidthLocal) / 2;
    const int textY = r.y + (r.height - renderer.getLineHeight(KEY_FONT)) / 2;
    renderer.drawText(KEY_FONT, textX, textY, label, true);
  };

  const char* const* layout = shiftState ? keyboardShift : keyboard;

  for (int row = 0; row < NUM_ROWS; row++) {
    const int rowY = keyboardStartY + row * (keyHeight + keySpacingY);
    const int rowLength = getRowLength(row);
    const int rowContentWidth = pageWidth - keySidePadding * 2 - (rowLength - 1) * keySpacingX;
    const auto unitX = [&](const int unit) {
      return keySidePadding + unit * keySpacingX + (unit * rowContentWidth) / rowLength;
    };

    if (row == TOP_ROW && shiftState == 0) {
      const int qrX = unitX(TOP_QR_COL);
      const int qrW = unitX(TOP_QR_COL + 1) - qrX;
      const bool qrSelected = (selectedRow == TOP_ROW && selectedCol == TOP_QR_COL);
      drawKey(Rect{qrX, rowY, qrW, keyHeight}, "QR", qrSelected);

      const int okX = unitX(TOP_OK_COL);
      const int okW = unitX(TOP_OK_COL + 1) - okX;
      const bool okSelected = (selectedRow == TOP_ROW && selectedCol == TOP_OK_COL);
      drawKey(Rect{okX, rowY, okW, keyHeight}, "OK", okSelected);
      continue;
    }

    // Handle bottom row (row 4) specially with proper multi-column keys
    if (row == SPECIAL_ROW) {
      // Bottom row layout: SHIFT (2 cols) | SPACE (4 cols) | <- (2 cols) | OK (2 cols)

      int currentX = unitX(0);

      // SHIFT key (logical col 0-1)
      const bool shiftSelected = (selectedRow == SPECIAL_ROW && selectedCol >= SHIFT_COL && selectedCol < SPACE_COL);
      const int shiftWidth = SPACE_COL - SHIFT_COL;
      const int shiftXWidth = unitX(SHIFT_COL + shiftWidth) - unitX(SHIFT_COL);
      drawKey(Rect{currentX, rowY, shiftXWidth, keyHeight}, shiftString[shiftState], shiftSelected);
      currentX = unitX(SPACE_COL);

      // Space bar (logical cols 2-5)
      const bool spaceSelected =
          (selectedRow == SPECIAL_ROW && selectedCol >= SPACE_COL && selectedCol < BACKSPACE_COL);
      const int spaceWidth = BACKSPACE_COL - SPACE_COL;
      const int spaceXWidth = unitX(SPACE_COL + spaceWidth) - unitX(SPACE_COL);
      drawKey(Rect{currentX, rowY, spaceXWidth, keyHeight}, "____", spaceSelected);
      currentX = unitX(BACKSPACE_COL);

      // Backspace key (logical cols 6-7)
      const bool bsSelected = (selectedRow == SPECIAL_ROW && selectedCol >= BACKSPACE_COL && selectedCol < DONE_COL);
      const int backspaceWidth = DONE_COL - BACKSPACE_COL;
      const int backspaceXWidth = unitX(BACKSPACE_COL + backspaceWidth) - unitX(BACKSPACE_COL);
      drawKey(Rect{currentX, rowY, backspaceXWidth, keyHeight}, "<-", bsSelected);
      currentX = unitX(DONE_COL);

      // OK button (logical cols 8-9)
      const bool okSelected = (selectedRow == SPECIAL_ROW && selectedCol >= DONE_COL);
      const int okWidth = getRowLength(row) - DONE_COL;
      const int okXWidth = unitX(DONE_COL + okWidth) - unitX(DONE_COL);
      drawKey(Rect{currentX, rowY, okXWidth, keyHeight}, "OK", okSelected);
    } else {
      // Regular rows: render each key individually
      for (int col = 0; col < getRowLength(row); col++) {
        const char c = layout[row][col];
        std::string keyLabel(1, c);
        const int keyX = unitX(col);
        const int keyW = unitX(col + 1) - keyX;
        const bool isSelected = row == selectedRow && col == selectedCol;
        drawKey(Rect{keyX, rowY, keyW, keyHeight}, keyLabel.c_str(), isSelected);
      }
    }
  }

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Back", "Select", "Left", "Right");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

// visibility helpers
void KeyboardEntryActivity::show() {
    isVisible = true;
    updateRequired = true;
}

void KeyboardEntryActivity::hide() {
    isVisible = false;
    updateRequired = true;
}

void KeyboardEntryActivity::renderQRScreen() const {
  const auto pageWidth = renderer.getScreenWidth();

  // Use same line spacing as File Transfer for consistency
  constexpr int LINE_SPACING = 28;
  // QR size: same as File Transfer (6px per module)
  constexpr int QR_TOTAL = QRCodeHelper::qrSize();  // 198px

  renderer.clearScreen();

  // Title - matching File Transfer style
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "输入文字", true, EpdFontFamily::BOLD);

  if (webInputServer && webInputServer->isRunning()) {
    if (webInputServer->isApMode()) {
      // === AP mode layout (matching File Transfer) ===
      int apStartY = 55;

      renderer.drawCenteredText(UI_10_FONT_ID, apStartY, "Hotspot Mode", true, EpdFontFamily::BOLD);

      std::string ssidInfo = "Network: " + webInputServer->getApSSID();
      renderer.drawCenteredText(UI_10_FONT_ID, apStartY + LINE_SPACING, ssidInfo.c_str());

      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 2, "连接wifi:");

      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 3,
                                "或扫描二维码连接wifi.");

      // WiFi QR code (same size as File Transfer)
      const std::string wifiQR = webInputServer->getWifiQRString();
      QRCodeHelper::drawQRCode(renderer, (pageWidth - QR_TOTAL) / 2, apStartY + LINE_SPACING * 4, wifiQR);

      apStartY += QR_TOTAL - 4 * QRCodeHelper::DEFAULT_PX + 3 * LINE_SPACING;

      // URL section
      const std::string url = webInputServer->getUrl();
      renderer.drawCenteredText(UI_10_FONT_ID, apStartY + LINE_SPACING * 3, url.c_str(), true, EpdFontFamily::BOLD);

      // Show IP address as fallback
      std::string ipUrl = "or http://" + webInputServer->getIP() + "/";
      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 4, ipUrl.c_str());

      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 5, "在您的浏览器中打开此 URL");

      renderer.drawCenteredText(SMALL_FONT_ID, apStartY + LINE_SPACING * 6, "或使用手机扫描二维码：");

      // URL QR code (same size as File Transfer)
      QRCodeHelper::drawQRCode(renderer, (pageWidth - QR_TOTAL) / 2, apStartY + LINE_SPACING * 7, url);

    } else {
      // === STA mode layout (WiFi already connected, matching File Transfer) ===
      constexpr int staStartY = 65;

      const std::string ip = webInputServer->getIP();

      std::string ipInfo = "IP Address: " + ip;
      renderer.drawCenteredText(UI_10_FONT_ID, staStartY, ipInfo.c_str());

      // Show web server URL prominently
      std::string webUrl = "http://" + ip + "/";
      renderer.drawCenteredText(UI_10_FONT_ID, staStartY + LINE_SPACING * 2, webUrl.c_str(), true, EpdFontFamily::BOLD);

      // Also show hostname URL using shared constant
      std::string hostnameUrl = std::string("or http://") + NetworkConstants::AP_HOSTNAME + ".local/";
      renderer.drawCenteredText(SMALL_FONT_ID, staStartY + LINE_SPACING * 3, hostnameUrl.c_str());

      renderer.drawCenteredText(SMALL_FONT_ID, staStartY + LINE_SPACING * 4, "在您的浏览器中打开此 URL");

      renderer.drawCenteredText(SMALL_FONT_ID, staStartY + LINE_SPACING * 5, "或使用手机扫描二维码：");

      // URL QR code (same size as File Transfer)
      QRCodeHelper::drawQRCode(renderer, (pageWidth - QR_TOTAL) / 2, staStartY + LINE_SPACING * 6, webUrl);
    }
  } else {
    const auto pageHeight = renderer.getScreenHeight();
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, "Starting server...", true, EpdFontFamily::BOLD);
  }

  // Button hints - matching File Transfer style
  const auto labels = mappedInput.mapLabels("\xC2\xAB Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void KeyboardEntryActivity::startWebInputServer() {
  if (!webInputServer) {
    webInputServer.reset(new KeyboardWebInputServer());
  }

  if (!webInputServer->isRunning()) {
    webInputServer->start();
  }

  showingQR = true;
  updateRequired = true;
}

void KeyboardEntryActivity::stopWebInputServer() {
  if (webInputServer) {
    webInputServer->stop();
    webInputServer.reset();
  }
}