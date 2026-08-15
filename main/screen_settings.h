#pragma once

#include "pjournal_app.h"

// Settings screen entry points
void screen_settings_init();
AppState screen_settings_handle(int key, ScreenContext &ctx);

// 是否处于文本值输入子界面(供主循环判断 ` 应否当作"返回")
bool settings_in_edit_mode();
