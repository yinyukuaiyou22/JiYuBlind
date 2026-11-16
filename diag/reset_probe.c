// reset_probe.c — 证据实验：SetWindowDisplayAffinity 的状态在哪些事件后会被清掉
// 自带窗口 + 自带抓屏（GDI BitBlt 屏幕 DC，等价 Print Screen / QQ截图路径），同进程可直接 GetWindowDisplayAffinity
// cl /nologo /O2 /W3 /utf-8 reset_probe.c /Fe:reset_probe.exe /link user32.lib gdi32.lib
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>

#define WIN_W 520
#define WIN_H 380
#define REAL_COLOR RGB(0, 200, 0)

static HWND g_wnd = NULL;
static FILE* g_fp = NULL;

static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(REAL_COLOR); FillRect(dc, &rc, b); DeleteObject(b);
        EndPaint(h, &ps); return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static void pump(DWORD ms) {
    DWORD end = GetTickCount() + ms; MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(5);
    }
}

static COLORREF grab(int sx, int sy) {
    COLORREF c = RGB(1, 2, 3);
    HDC sdc = GetDC(NULL); HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 1; bi.bmiHeader.biHeight = -1; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL; HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    BitBlt(mdc, 0, 0, 1, 1, sdc, sx, sy, SRCCOPY | CAPTUREBLT);
    if (bits) { BYTE* p = (BYTE*)bits; c = RGB(p[2], p[1], p[0]); }
    SelectObject(mdc, old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(NULL, sdc);
    return c;
}

// 窗口中心 3x3 采样，返回 "REAL" / "MASKED" / 具体色值
static const char* sample_state(COLORREF* out_first) {
    RECT rc; int cx, cy, i, real_cnt = 0, n = 0;
    GetWindowRect(g_wnd, &rc);
    cx = (rc.left + rc.right) / 2; cy = (rc.top + rc.bottom) / 2;
    for (i = -1; i <= 1; i++) {
        RECT r2; int x, y;
        for (x = -1; x <= 1; x++) {
            COLORREF c;
            (void)r2;
            c = grab(cx + i * 20, cy + x * 20);
            if (n == 0 && out_first) *out_first = c;
            if (c == REAL_COLOR) real_cnt++;
            n++;
        }
    }
    (void)cy;
    return real_cnt > 0 ? "REAL(抓屏里能看到窗口)" : "MASKED(抓屏里看不到窗口)";
}

static void report(const char* step) {
    COLORREF c = RGB(0, 0, 0);
    DWORD aff = 0xFFFFFFFF;
    BOOL ok = GetWindowDisplayAffinity(g_wnd, &aff);
    const char* st = sample_state(&c);
    fprintf(g_fp, "%-46s affinity=%s0x%02lX  capture=%s  first_pixel=0x%06lX\n",
            step, ok ? "" : "READFAIL/", ok ? (unsigned long)aff : 0UL, st,
            (unsigned long)((c & 0xFF) << 16 | (c & 0xFF00) | ((c >> 16) & 0xFF)));
    fflush(g_fp);
}

static HWND make_window(int x, int y, int w, int h) {
    return CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ResetProbeWnd", L"reset-probe",
                           WS_POPUP, x, y, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

int main(void) {
    WNDCLASSEXW wc; MSG msg;
    g_fp = fopen("reset_probe_result.txt", "wb");
    if (!g_fp) g_fp = stdout;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.lpszClassName = L"ResetProbeWnd";
    RegisterClassExW(&wc);

    g_wnd = make_window(220, 180, WIN_W, WIN_H);
    if (!g_wnd) { fprintf(g_fp, "create failed %lu\n", GetLastError()); fclose(g_fp); return 1; }
    ShowWindow(g_wnd, SW_SHOW); UpdateWindow(g_wnd); pump(300);

    report("[0] baseline 无亲和性");

    SetWindowDisplayAffinity(g_wnd, 0x11);
    pump(300);
    report("[1] 设置 WDA_EXCLUDEFROMCAPTURE 后");

    ShowWindow(g_wnd, SW_HIDE); pump(400);
    ShowWindow(g_wnd, SW_SHOW); UpdateWindow(g_wnd); pump(400);
    report("[2] SHOW->HIDE->SHOW 之后");

    // 模拟全屏：改样式 + 铺满屏幕 + 再还原
    SetWindowLongPtrW(g_wnd, GWL_STYLE, WS_POPUP);
    SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 1920, 1080, SWP_FRAMECHANGED | SWP_NOACTIVATE);
    pump(400);
    report("[3a] 切全屏(样式+铺满) 之后");
    SetWindowPos(g_wnd, HWND_TOPMOST, 220, 180, WIN_W, WIN_H, SWP_FRAMECHANGED | SWP_NOACTIVATE);
    pump(400);
    report("[3b] 还原窗口尺寸 之后");

    ShowWindow(g_wnd, SW_MINIMIZE); pump(500);
    ShowWindow(g_wnd, SW_RESTORE); pump(500);
    report("[4] 最小化->还原 之后");

    // 显示模式变化（远控/RDP 接入时的典型副作用）
    {
        DEVMODEW cur; DEVMODEW target; LONG rc_set, rc_back;
        ZeroMemory(&cur, sizeof(cur)); cur.dmSize = sizeof(cur);
        EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &cur);
        ZeroMemory(&target, sizeof(target)); target.dmSize = sizeof(target);
        target.dmPelsWidth = 1280; target.dmPelsHeight = 720; target.dmBitsPerPel = 32;
        target.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
        pump(200);
        rc_set = ChangeDisplaySettingsW(&target, 0);
        pump(1200);
        report("[5a] 分辨率 1920x1080 -> 1280x720 之后");
        rc_back = ChangeDisplaySettingsW(&cur, 0);
        pump(1200);
        fprintf(g_fp, "     (ChangeDisplaySettings set=%ld back=%ld)\n", (long)rc_set, (long)rc_back);
        report("[5b] 分辨率还原之后");
        if (rc_set != DISP_CHANGE_SUCCESSFUL || rc_back != DISP_CHANGE_SUCCESSFUL) {
            ChangeDisplaySettingsW(NULL, 0);
        }
    }

    // 窗口销毁 + 同进程同位置重建（浏览器切全屏建新窗口 / 应用自己重建窗口的等价场景）
    DestroyWindow(g_wnd); pump(300);
    g_wnd = make_window(220, 180, WIN_W, WIN_H);
    ShowWindow(g_wnd, SW_SHOW); UpdateWindow(g_wnd); pump(400);
    report("[6] 销毁后用同进程新 HWND 重建");

    // 在新 HWND 上重新设置，再做一次 DWM 相关的"覆盖层变化"（置顶/取消置顶）
    SetWindowDisplayAffinity(g_wnd, 0x11); pump(300);
    report("[7a] 新窗口重新设置亲和性");
    SetWindowPos(g_wnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); pump(400);
    report("[7b] 取消置顶(另起一个 topmost 窗口压在上面) 之后");
    SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); pump(300);

    fprintf(g_fp, "--- 结束，销毁窗口 ---\n");
    DestroyWindow(g_wnd);
    fclose(g_fp);
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
