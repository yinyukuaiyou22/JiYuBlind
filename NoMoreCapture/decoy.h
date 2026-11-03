#pragma once

#include "lilith_is_all_you_need.h"

/*
 * 替身画面（decoy）模块 —— 完全旁挂，不碰原工程的窗口/DIB/托盘逻辑
 *
 * 原理：受保护窗口设 WDA_EXCLUDEFROMCAPTURE 后，抓屏里那块区域会露出"它下面的内容"。
 *      我们在桌面层之上、所有应用窗口之下铺一张全屏的替身窗口，
 *      抓屏拿到的就是替身画面，而屏幕上用户看到的仍是真实窗口。
 *
 * 唯一硬约束：替身窗口不能被别的窗口"完全遮挡"，否则会被 DWM 遮挡剔除、洞里露出桌面色。
 *            所以停位必须贴着最底层（见 decoy_repark）。
 */

bool decoy_init(struct context* ptr_ctx);
void decoy_destroy(struct context* ptr_ctx);
bool decoy_enable(struct context* ptr_ctx, bool on);
void decoy_repark(struct context* ptr_ctx);
bool decoy_set_image(struct context* ptr_ctx, const wchar_t* path);
bool decoy_snapshot_desktop(struct context* ptr_ctx);
void decoy_sync(struct context* ptr_ctx);
void decoy_on_zorder_changed(struct context* ptr_ctx);
bool decoy_is_own_window(struct context* ptr_ctx, HWND hwnd);
