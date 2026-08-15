#pragma once

#include "pjournal_app.h"

void screen_outline_init();
AppState screen_outline_handle(int key, ScreenContext &ctx);

// 大纲是否处于文本输入子界面(供主循环判断 ` 应否当作"返回")
bool outline_in_edit_mode();
