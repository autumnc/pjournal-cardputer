#pragma once

#include "pjournal_app.h"

// Voice dictation screen (xiaozhi cloud ASR). Entered from the editor via
// USER double-click; double-click again to stop and return to the editor.
void screen_voice_init();
AppState screen_voice_handle(int key, ScreenContext &ctx);
