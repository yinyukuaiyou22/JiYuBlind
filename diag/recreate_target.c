// recreate_target.c —— 模拟"浏览器重建窗口"：同进程内销毁旧窗口、在原位置建新窗口
// 用法：recreate_target.exe [存活秒数 默认30] [重建时刻 默认6]
// 每次创建都把 <tick,hwnd,pid> 追加到 recreate_target_log.txt
// cl /nologo /O2 /W3 /utf-8 recreate_target.c /Fe:recreate_target.exe /link user32.lib
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>

#define WX 400
#define WY 300
#define WW 520
#define WH 360
#define REAL_COLOR RGB(0, 200, 0)

static FILE* g_log = NULL;
static HWND  g_top = NULL;
static DWORD g_last_top = 0;

static COLORREF grab(int sx, int sy) {
    COLORREF c = RGB(1, 2, 3);
    HDC sdc = GetDC(NULL); HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 1; bi.bmiHeader.biHeight = -1; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    { void* bits = NULL; HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
      HGDIOBJ old = SelectObject(mdc, bmp);
      BitBlt(mdc, 0, 0, 1, 1, sdc, sx, sy, SRCCOPY | CAPTUREBLT);
      if (bits) { BYTE* p = (BYTE*)bits; c = RGB(p[2], p[1], p[0]); }
      SelectObject(mdc, old); DeleteObject(bmp); }
    DeleteDC(mdc); ReleaseDC(NULL, sdc);
    return c;
}

static void log_sample(const char* tag, HWND h, int delay_ms) {
    RECT rc;
    COLORREF c;
    if (h == NULL || !IsWindow(h)) return;
    if (delay_ms > 0) Sleep((DWORD)delay_ms);
    GetWindowRect(h, &rc);
    c = grab((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
    if (g_log) {
        fprintf(g_log, "%lu,%s,%llu,0,0,0,0,0,pixel=0x%02lX%02lX%02lX\n",
                (unsigned long)GetTickCount(), tag, (unsigned long long)(ULONG_PTR)h,
                (unsigned long)GetRValue(c), (unsigned long)GetGValue(c), (unsigned long)GetBValue(c));
        fflush(g_log);
    }
}

static void log_line(const char* what, HWND h) {
    RECT rc = { 0, 0, 0, 0 };
    if (g_log == NULL) return;
    if (h != NULL) GetWindowRect(h, &rc);
    fprintf(g_log, "%lu,%s,%llu,%lu,%ld,%ld,%ld,%ld\n", (unsigned long)GetTickCount(), what,
            (unsigned long long)(ULONG_PTR)h, (unsigned long)GetCurrentProcessId(),
            (long)rc.left, (long)rc.top, (long)rc.right, (long)rc.bottom);
    fflush(g_log);
}

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

static HWND make(void) {
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"JybRecreateWnd", L"recreate-target",
                             WS_POPUP, WX, WY, WW, WH, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (h != NULL) { ShowWindow(h, SW_SHOW); UpdateWindow(h); g_top = h; }
    return h;
}

static void pump(DWORD ms) {
    DWORD end = GetTickCount() + ms; MSG msg;
    while (GetTickCount() < end) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        /* 一直被别的 topmost 窗口压着会污染抓屏取点，这里定期重申置顶（500ms 足够，别污染 CPU 测量） */
        if (g_top != NULL && IsWindow(g_top) && (GetTickCount() - g_last_top) >= 500) {
            g_last_top = GetTickCount();
            SetWindowPos(g_top, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        Sleep(10);
    }
}

int main(int argc, char** argv) {
    WNDCLASSEXW wc;
    HWND h;
    int life = argc > 1 ? atoi(argv[1]) : 30;
    int rebuild_at = argc > 2 ? atoi(argv[2]) : 6;

    g_log = fopen("recreate_target_log.txt", "ab");
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.lpszClassName = L"JybRecreateWnd";
    RegisterClassExW(&wc);

    h = make();
    if (h == NULL) return 1;
    log_line("create1", h);
    pump((DWORD)rebuild_at * 1000);

    /* 关键动作：销毁 + 同进程重建（新 HWND），位置/尺寸不变 */
    DestroyWindow(h);
    log_line("destroy1", NULL);
    pump(200);
    h = make();
    if (h == NULL) return 1;
    log_line("create2", h);
    log_sample("w2_after_250ms", h, 250);
    log_sample("w2_after_1200ms", h, 950);

    pump((DWORD)life * 1000);
    DestroyWindow(h);
    if (g_log) fclose(g_log);
    return 0;
}