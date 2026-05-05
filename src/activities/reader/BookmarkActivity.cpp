#include "BookmarkActivity.h"

#include "components/UITheme.h"
#include <GfxRenderer.h>

void BookmarkActivity::onEnter() {
  Activity::onEnter();

  const bool ok = BookmarkStore::append(cachePath, bookPath, progressPercent, pos1, pos2, pos3);

  renderer.clearScreen();
  GUI.drawPopup(renderer, ok ? "书签已添加" : "书签添加失败");
  renderer.displayBuffer();
  delay(600);

  if (ok) {
    Serial.printf("[%lu] [BMK] Bookmark appended: %s\n", millis(), (cachePath + "/bookmark.bin").c_str());
  } else {
    Serial.printf("[%lu] [BMK] Bookmark append failed: %s\n", millis(), bookPath.c_str());
  }

  if (onDone) {
    onDone(ok);
  }
}
