// decoy.c —— 替身画面模块（旁挂实现，每个受保护窗口一个替身窗口）
//
// 目标语义：受保护窗口在抓屏里"本体不可见"，抓到的区域是**预备图**或**空白**；
//          而用户自己的物理屏幕完全不受影响（桌面、图标、其他窗口照旧）。
//
// 实现：给每个受保护窗口在【正下方】贴一个同尺寸的替身窗口。受保护窗口设了
//      WDA_EXCLUDEFROMCAPTURE，抓屏时它被剔除，露出的就是下面的替身窗口 → 抓屏得到预备图。
//
// 三条实测得到的硬约束（踩过坑，别改）：
//   1) 替身窗口绝不能置顶（WS_EX_TOPMOST）。两个 topmost 窗口叠在一起时，
//      被遮挡的替身会被 DWM 剔除，洞里露出桌面色 / 纯黑。
//   2) 替身窗口必须紧贴在目标窗口正下方（SetWindowPos(decoy, target, ...)）。
//   3) 窗口创建/销毁只能在主线程做：注入是从 worker 线程发起的，所以这里用
//      400ms 定时器在主线程做一次"对账"(reconcile)，不在注入路径里建窗口。

#define _CRT_SECURE_NO_WARNINGS

#include "lilith_is_all_you_need.h"
#include "decoy.h"

#include <stdio.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#define DECOY_CLASS      L"JiYuBlindDecoyWnd"
#define DECOY_TIMER_ID   0xDEC0
#define DECOY_SYNC_MS    400

static struct context* g_decoy_owner = NULL;
static bool g_decoy_class_registered = false;

static void decoy_commit_slot(struct context* ptr_ctx, struct decoy_slot* slot);
static void decoy_src_solid(struct context* ptr_ctx, BYTE b, BYTE g, BYTE r);

/* ---------- 工具 ---------- */

static bool decoy_is_desktop_class(const wchar_t* cls) {
    return wcscmp(cls, L"Progman") == 0 ||
           wcscmp(cls, L"WorkerW") == 0 ||
           wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           wcscmp(cls, L"ForegroundStaging") == 0 ||
           wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0;
}

static LRESULT CALLBACK decoy_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;     /* 鼠标穿透 */
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 源图：一张覆盖主屏的 32bpp top-down DIB，所有替身窗口都从它上面切片 */
static bool decoy_src_alloc(struct context* ptr_ctx) {
    struct decoy_ctx* d = &ptr_ctx->decoy;
    BITMAPINFO bi;
    HDC screen;
    MONITORINFO mi;

    if (d->src_ready) {
        return true;
    }
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY), &mi);
    d->src_x = mi.rcMonitor.left;
    d->src_y = mi.rcMonitor.top;
    d->src_w = mi.rcMonitor.right - mi.rcMonitor.left;
    d->src_h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    screen = GetDC(NULL);
    if (screen == NULL) return false;
    d->src_dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (d->src_dc == NULL) return false;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = d->src_w;
    bi.bmiHeader.biHeight = -d->src_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    d->src_bmp = CreateDIBSection(d->src_dc, &bi, DIB_RGB_COLORS, &d->src_bits, NULL, 0);
    if (d->src_bmp == NULL || d->src_bits == NULL) return false;
    SelectObject(d->src_dc, d->src_bmp);
    decoy_src_solid(ptr_ctx, 0, 0, 0);
    d->src_ready = true;
    return true;
}

static void decoy_src_solid(struct context* ptr_ctx, BYTE b, BYTE g, BYTE r) {
    struct decoy_ctx* d = &ptr_ctx->decoy;
    BYTE* p = (BYTE*)d->src_bits;
    size_t i, n;
    if (p == NULL) return;
    n = (size_t)d->src_w * d->src_h;
    for (i = 0; i < n; i++) {
        p[i * 4 + 0] = b; p[i * 4 + 1] = g; p[i * 4 + 2] = r; p[i * 4 + 3] = 255;
    }
}

/* 双线性缩放 + cover 居中裁切，RGBA -> BGRA 直接写进源图 */
static void decoy_src_blit_cover(struct context* ptr_ctx, const unsigned char* src, int sw, int sh) {
    struct decoy_ctx* d = &ptr_ctx->decoy;
    BYTE* dst = (BYTE*)d->src_bits;
    double scale, off_x, off_y;
    int x, y;

    if (src == NULL || sw <= 0 || sh <= 0 || dst == NULL) return;
    scale = (double)d->src_w / (double)sw;
    if ((double)d->src_h / (double)sh > scale) scale = (double)d->src_h / (double)sh;
    off_x = (sw * scale - d->src_w) / 2.0;
    off_y = (sh * scale - d->src_h) / 2.0;

    for (y = 0; y < d->src_h; y++) {
        double sy = (y + off_y) / scale;
        for (x = 0; x < d->src_w; x++) {
            double sx = (x + off_x) / scale;
            int x0, y0, x1, y1;
            double fx, fy, w00, w01, w10, w11;
            const unsigned char* p00; const unsigned char* p01;
            const unsigned char* p10; const unsigned char* p11;
            BYTE* o;
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            x0 = (int)sx; if (x0 > sw - 1) x0 = sw - 1;
            y0 = (int)sy; if (y0 > sh - 1) y0 = sh - 1;
            x1 = x0 + 1; if (x1 > sw - 1) x1 = sw - 1;
            y1 = y0 + 1; if (y1 > sh - 1) y1 = sh - 1;
            fx = sx - x0; fy = sy - y0;
            w00 = (1 - fx) * (1 - fy); w01 = fx * (1 - fy);
            w10 = (1 - fx) * fy;       w11 = fx * fy;
            p00 = src + ((size_t)y0 * sw + x0) * 4;
            p01 = src + ((size_t)y0 * sw + x1) * 4;
            p10 = src + ((size_t)y1 * sw + x0) * 4;
            p11 = src + ((size_t)y1 * sw + x1) * 4;
            o = dst + ((size_t)y * d->src_w + x) * 4;
            o[0] = (BYTE)(p00[2] * w00 + p01[2] * w01 + p10[2] * w10 + p11[2] * w11);
            o[1] = (BYTE)(p00[1] * w00 + p01[1] * w01 + p10[1] * w10 + p11[1] * w11);
            o[2] = (BYTE)(p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11);
            o[3] = 255;
        }
    }
}

/* ---------- 替身窗口 ---------- */

static bool decoy_ensure_class(struct context* ptr_ctx) {
    WNDCLASSEXW wc;
    if (g_decoy_class_registered) return true;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = decoy_wndproc;
    wc.hInstance = ptr_ctx->instance;
    wc.lpszClassName = DECOY_CLASS;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    g_decoy_class_registered = true;
    return true;
}

/* 把源图里"该窗口所在位置"的那一块，作为替身窗口的内容提交上去 */
static void decoy_commit_slot(struct context* ptr_ctx, struct decoy_slot* slot) {
    struct decoy_ctx* d = &ptr_ctx->decoy;
    POINT dst, srcp;
    SIZE size;
    BLENDFUNCTION bf;
    int w, h, sx, sy;

    if (slot == NULL || slot->decoy == NULL || !d->src_ready) return;
    w = slot->rect.right - slot->rect.left;
    h = slot->rect.bottom - slot->rect.top;
    sx = slot->rect.left - d->src_x;
    sy = slot->rect.top - d->src_y;
    if (w <= 0 || h <= 0) return;

    /* 落在源图之外的窗口：夹一下，别让 ULW 读越界 */
    if (sx < 0) { w += sx; sx = 0; }
    if (sy < 0) { h += sy; sy = 0; }
    if (sx + w > d->src_w) w = d->src_w - sx;
    if (sy + h > d->src_h) h = d->src_h - sy;
    if (w <= 0 || h <= 0) return;

    dst.x = slot->rect.left;
    dst.y = slot->rect.top;
    srcp.x = sx;
    srcp.y = sy;
    size.cx = w;
    size.cy = h;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(slot->decoy, NULL, &dst, &size, d->src_dc, &srcp, 0, &bf, ULW_ALPHA);
}

static bool decoy_add_slot(struct context* ptr_ctx, HWND target) {
    struct decoy_ctx* d = &ptr_ctx->decoy;
    struct decoy_slot* slot;
    DWORD ex_style;

    if (d->slot_count >= DECOY_MAX_SLOTS) return false;
    if (!decoy_ensure_class(ptr_ctx)) return false;

    slot = &d->slots[d->slot_count];
    ZeroMemory(slot, sizeof(*slot));
    slot->target = target;
    GetWindowRect(target, &slot->rect);
    if (slot->rect.right <= slot->rect.left || slot->rect.bottom <= slot->rect.top) return false;

    /* 注意：绝对不要加 WS_EX_TOPMOST —— 加了会被 DWM 遮挡剔除，抓屏里变黑 */
    ex_style = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT;
    slot->decoy = CreateWindowExW(ex_style, DECOY_CLASS, L"decoy", WS_POPUP,
                                  slot->rect.left, slot->rect.top,
                                  slot->rect.right - slot->rect.left,
                                  slot->rect.bottom - slot->rect.top,
                                  NULL, NULL, ptr_ctx->instance, NULL);
    if (slot->decoy == NULL) return false;

    /* 插到目标窗口正下方 */
    SetWindowPos(slot->decoy, target, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    decoy_commit_slot(ptr_ctx, slot);
    ShowWindow(slot->decoy, SW_SHOWNOACTIVATE);
    d->slot_count++;
    return true;
}

static void decoy_drop_slot(struct context* ptr_ctx, int index) {
    struct decoy_ctx* d = &ptr_ctx->decoy;
    if (index < 0 || index >= d->slot_count) return;
    if (d->slots[index].decoy != NULL) {
        DestroyWindow(d->slots[index].decoy);
    }
    d->slot_count--;
    if (index != d->slot_count) {
        d->slots[index] = d->slots[d->slot_count];
    }
    ZeroMemory(&d->slots[d->slot_count], sizeof(struct decoy_slot));
}

/* ---------- 对账（主线程定时器驱动） ---------- */

static int decoy_collect_targets(struct context* ptr_ctx, HWND* out, int cap) {
    int n = 0, i;

    /* 自检开关：把本程序自己的窗口也贴一张替身，方便在无第三方窗口时验证抓屏替换 */
    if (cap > 0) {
        wchar_t probe[8];
        if (GetEnvironmentVariableW(L"LILITH_DECOY_SELFTEST", probe, 8) > 0) {
            out[n++] = ptr_ctx->window_hwnd;
        }
    }
    /* 白名单模式：用现有 target[] 列表 */
    if (CHECK_BIT(ptr_ctx->panel_ctx.bitwise, 2 /* f_c */) != 0) {
        for (i = 0; i < 256 && n < cap; i++) {
            if (ptr_ctx->target[i] != NULL && IsWindow(ptr_ctx->target[i])) {
                out[n++] = ptr_ctx->target[i];
            }
        }
        return n;
    }
    /* 全局模式：枚举所有可见顶层窗口 */
    {
        HWND h = GetTopWindow(NULL);
        while (h != NULL && n < cap) {
            DWORD cloak = 0;
            wchar_t cls[64];
            if (h != ptr_ctx->window_hwnd && IsWindowVisible(h) && !IsIconic(h)) {
                GetClassNameW(h, cls, 64);
                DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloak, sizeof(cloak));
                if (cloak == 0 && !decoy_is_desktop_class(cls) && !decoy_is_own_window(ptr_ctx, h)) {
                    out[n++] = h;
                }
            }
            h = GetWindow(h, GW_HWNDNEXT);
        }
    }
    return n;
}

void decoy_sync(struct context* ptr_ctx) {
    struct decoy_ctx* d = &ptr_ctx->decoy;
    HWND wanted[DECOY_MAX_SLOTS];
    int want_n = 0, i, j;

    if (ptr_ctx == NULL || !d->active) return;
    if (!d->src_ready) return;

    want_n = decoy_collect_targets(ptr_ctx, wanted, DECOY_MAX_SLOTS);

    /* 1) 清掉失效/不再需要/尺寸变化的槽 */
    for (i = d->slot_count - 1; i >= 0; i--) {
        struct decoy_slot* slot = &d->slots[i];
        RECT rc;
        int keep = 0;
        if (!IsWindow(slot->target) || !IsWindowVisible(slot->target)) {
            decoy_drop_slot(ptr_ctx, i);
            continue;
        }
        for (j = 0; j < want_n; j++) {
            if (wanted[j] == slot->target) { keep = 1; break; }
        }
        if (!keep) {
            decoy_drop_slot(ptr_ctx, i);
            continue;
        }
        GetWindowRect(slot->target, &rc);
        if (memcmp(&rc, &slot->rect, sizeof(RECT)) != 0) {
            slot->rect = rc;
            SetWindowPos(slot->decoy, NULL, rc.left, rc.top,
                         rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
            /* 位置变了要重新贴回目标正下方，并重切内容 */
            SetWindowPos(slot->decoy, slot->target, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            decoy_commit_slot(ptr_ctx, slot);
        }
    }

    /* 2) 给新出现的受保护窗口补替身 */
    for (j = 0; j < want_n && d->slot_count < DECOY_MAX_SLOTS; j++) {
        int exists = 0;
        for (i = 0; i < d->slot_count; i++) {
            if (d->slots[i].target == wanted[j]) { exists = 1; break; }
        }
        if (!exists) {
            decoy_add_slot(ptr_ctx, wanted[j]);
        }
    }
}

static void CALLBACK decoy_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick) {
    (void)hwnd; (void)msg; (void)id; (void)tick;
    if (g_decoy_owner != NULL) {
        decoy_sync(g_decoy_owner);
    }
}

/* ---------- 对外接口 ---------- */

bool decoy_init(struct context* ptr_ctx) {
    struct decoy_ctx* d = &ptr_ctx->decoy;
    if (d->ready) return true;
    if (!decoy_src_alloc(ptr_ctx)) {
        out(L"替身画面：源图分配失败", err);
        return false;
    }
    g_decoy_owner = ptr_ctx;
    d->ready = true;
    if (d->mode == 1 && d->timer == 0) {
        d->timer = SetTimer(NULL, DECOY_TIMER_ID, DECOY_SYNC_MS, decoy_timer_proc);
    }
    return true;
}

bool decoy_snapshot_desktop(struct context* ptr_ctx) {
    struct decoy_ctx* d = &ptr_ctx->decoy;
    HDC screen;
    HGDIOBJ old;
    BOOL hid_main = FALSE;

    if (!decoy_src_alloc(ptr_ctx)) return false;

    /* 抓快照时藏起自己的界面，别把自己的窗口烤进预备图 */
    if (IsWindowVisible(ptr_ctx->window_hwnd)) { ShowWindow(ptr_ctx->window_hwnd, SW_HIDE); hid_main = TRUE; }
    if (ptr_ctx->panel_ctx.panel_hwnd != NULL && IsWindowVisible(ptr_ctx->panel_ctx.panel_hwnd)) {
        ShowWindow(ptr_ctx->panel_ctx.panel_hwnd, SW_HIDE);
    }
    Sleep(60);

    screen = GetDC(NULL);
    if (screen == NULL) {
        if (hid_main) ShowWindow(ptr_ctx->window_hwnd, SW_SHOW);
        return false;
    }
    old = SelectObject(d->src_dc, d->src_bmp);
    BitBlt(d->src_dc, 0, 0, d->src_w, d->src_h, screen, d->src_x, d->src_y, SRCCOPY | CAPTUREBLT);
    SelectObject(d->src_dc, old);
    ReleaseDC(NULL, screen);

    if (hid_main) ShowWindow(ptr_ctx->window_hwnd, SW_SHOW);
    return true;
}

static bool decoy_src_from_file(struct context* ptr_ctx, const wchar_t* path) {
    FILE* fp;
    unsigned char* src;
    int sw = 0, sh = 0, ch = 0;

    fp = _wfopen(path, L"rb");
    if (fp == NULL) return false;
    src = stbi_load_from_file(fp, &sw, &sh, &ch, 4);
    fclose(fp);
    if (src == NULL) return false;
    decoy_src_blit_cover(ptr_ctx, src, sw, sh);
    stbi_image_free(src);
    return true;
}

bool decoy_set_image(struct context* ptr_ctx, const wchar_t* path) {
    struct decoy_ctx* d = &ptr_ctx->decoy;
    int i;

    if (!decoy_src_alloc(ptr_ctx)) return false;
    if (path == NULL || path[0] == L'\0') {
        d->image_path[0] = L'\0';
        if (!decoy_snapshot_desktop(ptr_ctx)) return false;
    } else {
        if (!decoy_src_from_file(ptr_ctx, path)) {
            out(L"替身画面：加载图片失败", err);
            return false;
        }
        StringCchCopyW(d->image_path, MAX_PATH, path);
    }
    for (i = 0; i < d->slot_count; i++) {
        decoy_commit_slot(ptr_ctx, &d->slots[i]);
    }
    return true;
}

bool decoy_enable(struct context* ptr_ctx, bool on) {
    struct decoy_ctx* d = &ptr_ctx->decoy;
    int i;

    if (!on) {
        for (i = d->slot_count - 1; i >= 0; i--) {
            decoy_drop_slot(ptr_ctx, i);
        }
        d->active = false;
        return true;
    }

    if (!decoy_init(ptr_ctx)) return false;
    if (d->image_path[0] != L'\0') {
        decoy_src_from_file(ptr_ctx, d->image_path);
    } else {
        decoy_snapshot_desktop(ptr_ctx);
    }
    d->active = true;
    if (d->timer == 0) {
        d->timer = SetTimer(NULL, DECOY_TIMER_ID, DECOY_SYNC_MS, decoy_timer_proc);
    }
    decoy_sync(ptr_ctx);
    return true;
}

void decoy_on_zorder_changed(struct context* ptr_ctx) {
    /* 定时器已在做对账，这里只处理"替身被压到目标窗口上面"这类层级漂移 */
    struct decoy_ctx* d = &ptr_ctx->decoy;
    int i;
    if (ptr_ctx == NULL || !d->active) return;
    for (i = 0; i < d->slot_count; i++) {
        if (IsWindow(d->slots[i].target) && IsWindow(d->slots[i].decoy)) {
            SetWindowPos(d->slots[i].decoy, d->slots[i].target, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }
}

bool decoy_is_own_window(struct context* ptr_ctx, HWND hwnd) {
    int i;
    if (ptr_ctx == NULL) return false;
    for (i = 0; i < ptr_ctx->decoy.slot_count; i++) {
        if (ptr_ctx->decoy.slots[i].decoy == hwnd) return true;
    }
    return false;
}

void decoy_destroy(struct context* ptr_ctx) {
    struct decoy_ctx* d = &ptr_ctx->decoy;
    int i;

    if (d->timer != 0) {
        KillTimer(NULL, DECOY_TIMER_ID);
        d->timer = 0;
    }
    for (i = d->slot_count - 1; i >= 0; i--) {
        decoy_drop_slot(ptr_ctx, i);
    }
    if (d->src_bmp != NULL) { DeleteObject(d->src_bmp); d->src_bmp = NULL; }
    if (d->src_dc != NULL) { DeleteDC(d->src_dc); d->src_dc = NULL; }
    d->src_bits = NULL;
    d->src_ready = false;
    d->ready = false;
    d->active = false;
    g_decoy_owner = NULL;
}
