#include "lilith_is_all_you_need.h"

// ---------------------------------------------------------------------------
// 图像加载 / 缩放 / 渲染
// ---------------------------------------------------------------------------

static unsigned char* load_img_resource(int* out_w, int* out_h, int* out_ch) {
    HMODULE curr_module = GetModuleHandleW(NULL);
    if (curr_module == NULL) {
        return NULL;
    }
    HRSRC resource_handle = FindResourceW(curr_module, MAKEINTRESOURCE(IDR_BACKGROUND_PNG), RT_RCDATA);
    if (resource_handle == NULL) {
        return NULL;
    }
    HGLOBAL handle_global = LoadResource(curr_module, resource_handle);
    if (handle_global == NULL) {
        return NULL;
    }
    DWORD resource_size = SizeofResource(curr_module, resource_handle);
    if (resource_size <= 0) {
        return NULL;
    }
    const void* ptr_data = LockResource(handle_global);
    if (ptr_data == NULL) {
        return NULL;
    }
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load_from_memory((const stbi_uc*)ptr_data, (int)resource_size, &w, &h, &ch, 4);
    if (pixels == NULL) {
        return NULL;
    }
    *out_w = w;
    *out_h = h;
    *out_ch = ch;
    return pixels;
}

static unsigned char* load_img_file(const wchar_t* path, int* out_w, int* out_h, int* out_ch) {
    if (path == NULL || path[0] == L'\0') {
        return NULL;
    }
    HANDLE file_handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    DWORD file_size = GetFileSize(file_handle, NULL);
    if (file_size == 0 || file_size == INVALID_FILE_SIZE) {
        CloseHandle(file_handle);
        return NULL;
    }
    unsigned char* file_buf = (unsigned char*)malloc(file_size);
    if (file_buf == NULL) {
        CloseHandle(file_handle);
        return NULL;
    }
    DWORD bytes_read = 0;
    BOOL read_ok = ReadFile(file_handle, file_buf, file_size, &bytes_read, NULL);
    CloseHandle(file_handle);
    if (!read_ok || bytes_read != file_size) {
        free(file_buf);
        return NULL;
    }
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load_from_memory(file_buf, (int)file_size, &w, &h, &ch, 4);
    free(file_buf);
    if (pixels == NULL) {
        return NULL;
    }
    *out_w = w;
    *out_h = h;
    *out_ch = ch;
    return pixels;
}

// 缩放后返回新 buffer（同时负责释放传入的 px）；scale == 100 时原样返回。
// 使用 Catmull-Rom（锐利插值），避免双线性那种发糊的效果。
static unsigned char* scale_img(unsigned char* px, int* w, int* h, int scale) {
    if (px == NULL) {
        return NULL;
    }
    if (scale == 100) {
        return px;
    }
    int new_w = (int)((long long)(*w) * scale / 100);
    int new_h = (int)((long long)(*h) * scale / 100);
    if (new_w < 1) new_w = 1;
    if (new_h < 1) new_h = 1;
    unsigned char* out = (unsigned char*)malloc((size_t)new_w * new_h * 4);
    if (out == NULL) {
        stbi_image_free(px);
        return NULL;
    }
    stbir_resize(px, *w, *h, (*w) * 4,
                 out, new_w, new_h, new_w * 4,
                 STBIR_RGBA, STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_CATMULLROM);
    stbi_image_free(px);
    *w = new_w;
    *h = new_h;
    return out;
}

static void free_img(struct context* ctx) {
    if (ctx->img_pixels != NULL) {
        stbi_image_free(ctx->img_pixels);
        ctx->img_pixels = NULL;
    }
    ctx->img_x = 0;
    ctx->img_y = 0;
}

bool load_background_image(struct context* ctx) {
    if (ctx == NULL) {
        return false;
    }
    free_img(ctx);
    int w = 0, h = 0, ch = 0;
    unsigned char* px = NULL;
    if (ctx->background_path[0] != L'\0') {
        px = load_img_file(ctx->background_path, &w, &h, &ch);
    }
    if (px == NULL) {
        // 自定义背景失败或未设置，回退到内置背景
        px = load_img_resource(&w, &h, &ch);
        if (px != NULL) {
            ctx->background_path[0] = L'\0';
        }
    }
    if (px == NULL) {
        return false;
    }
    int scale = ctx->scale_percent;
    if (scale < 5 || scale > 1000) {
        scale = 100;
    }
    px = scale_img(px, &w, &h, scale);
    if (px == NULL) {
        return false;
    }
    ctx->img_pixels = px;
    ctx->img_x = w;
    ctx->img_y = h;
    ctx->cnt_channels = ch;
    return true;
}

bool init_img_ctx(struct context* ptr_ctx) {
    if (ptr_ctx == NULL) {
        return false;
    }
    if (ptr_ctx->scale_percent == 0) {
        ptr_ctx->scale_percent = 100;
    }
    return load_background_image(ptr_ctx);
}

// 将 RGBA 源图以预乘 alpha 方式写入 32bpp BGRA DIB 内存
void draw_png_with_premulitply_alpha(struct context* ptr_ctx) {
    if (ptr_ctx == NULL || ptr_ctx->img_pixels == NULL || ptr_ctx->bits == NULL) {
        return;
    }
    BYTE* img = ptr_ctx->img_pixels;
    BYTE* bits = ptr_ctx->bits;
    for (int i = 0; i < ptr_ctx->img_x * ptr_ctx->img_y; i++) {
        BYTE r = img[i * 4 + 0];
        BYTE g = img[i * 4 + 1];
        BYTE b = img[i * 4 + 2];
        BYTE a = img[i * 4 + 3];
        bits[i * 4 + 0] = (BYTE)(b * a / 255); // B
        bits[i * 4 + 1] = (BYTE)(g * a / 255); // G
        bits[i * 4 + 2] = (BYTE)(r * a / 255); // R
        bits[i * 4 + 3] = a;                   // A
    }
}

// 运行时重新加载背景（自定义/内置 + 缩放），重建 DIB 并重绘主窗口
bool rebuild_background(struct context* ctx) {
    if (ctx == NULL || ctx->window_hwnd == NULL || ctx->hdc == NULL) {
        return false;
    }
    if (!load_background_image(ctx)) {
        return false;
    }
    ctx->window_width = ctx->img_x;
    ctx->window_height = ctx->img_y;

    // 创建新的 DIB（不破坏旧的，先暂存到局部变量）
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = ctx->window_width;
    bmi.bmiHeader.biHeight = -ctx->window_height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* new_bits = NULL;
    HBITMAP new_bitmap = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &new_bits, NULL, 0);
    if (new_bitmap == NULL || new_bits == NULL) {
        if (new_bitmap != NULL) DeleteObject(new_bitmap);
        return false;
    }

    // 替换旧的（SelectObject 会返回并解选旧位图）
    HBITMAP old_bitmap = (HBITMAP)SelectObject(ctx->hdc, new_bitmap);
    if (old_bitmap != NULL) {
        DeleteObject(old_bitmap);
    }
    ctx->bitmap = new_bitmap;
    ctx->bits = new_bits;

    SetWindowPos(ctx->window_hwnd, NULL, 0, 0, ctx->window_width, ctx->window_height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    draw_png_with_premulitply_alpha(ctx);

    POINT window_pt = { ctx->window_x, ctx->window_y };
    SIZE window_size = { ctx->window_width, ctx->window_height };
    POINT start_pt = { 0, 0 };
    BLENDFUNCTION blend_opt = { 0 };
    blend_opt.AlphaFormat = AC_SRC_ALPHA;
    blend_opt.SourceConstantAlpha = 255;
    blend_opt.BlendOp = AC_SRC_OVER;
    return UpdateLayeredWindow(ctx->window_hwnd, NULL, &window_pt, &window_size, ctx->hdc, &start_pt, 0, &blend_opt, ULW_ALPHA) != 0;
}

// ---------------------------------------------------------------------------
// 预览窗口
// ---------------------------------------------------------------------------

struct preview_data {
    HBITMAP bitmap;
    HDC hdc;
};

static LRESULT CALLBACK preview_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    struct preview_data* pd = (struct preview_data*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_KEYDOWN:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        if (pd != NULL) {
            if (pd->hdc) DeleteDC(pd->hdc);
            if (pd->bitmap) DeleteObject(pd->bitmap);
            free(pd);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void show_preview_window(const wchar_t* background_path, int scale_percent) {
    int w = 0, h = 0, ch = 0;
    unsigned char* px = NULL;
    if (background_path != NULL && background_path[0] != L'\0') {
        px = load_img_file(background_path, &w, &h, &ch);
    }
    if (px == NULL) {
        px = load_img_resource(&w, &h, &ch);
    }
    if (px == NULL) {
        out(L"预览失败：无法加载图像", err);
        return;
    }
    int scale = scale_percent;
    if (scale < 5 || scale > 1000) scale = 100;
    px = scale_img(px, &w, &h, scale);
    if (px == NULL) {
        out(L"预览失败：缩放失败", err);
        return;
    }

    HINSTANCE inst = GetModuleHandleW(NULL);
    const wchar_t* cls = L"JiYuBlindPreviewClass";
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = preview_wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_HAND);
    wc.lpszClassName = cls;
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc); // 已注册则失败，忽略即可

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp == NULL || bits == NULL) {
        stbi_image_free(px);
        out(L"预览失败：创建位图失败", err);
        return;
    }

    // 预乘绘制
    BYTE* img = px;
    BYTE* dst = (BYTE*)bits;
    for (int i = 0; i < w * h; i++) {
        BYTE r = img[i * 4 + 0], g = img[i * 4 + 1], b = img[i * 4 + 2], a = img[i * 4 + 3];
        dst[i * 4 + 0] = (BYTE)(b * a / 255);
        dst[i * 4 + 1] = (BYTE)(g * a / 255);
        dst[i * 4 + 2] = (BYTE)(r * a / 255);
        dst[i * 4 + 3] = a;
    }
    stbi_image_free(px);

    HDC hdc = CreateCompatibleDC(NULL);
    SelectObject(hdc, bmp);

    int screen_x = GetSystemMetrics(SM_CXSCREEN);
    int screen_y = GetSystemMetrics(SM_CYSCREEN);
    int x = (screen_x - w) / 2;
    int y = (screen_y - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST, cls, L"JiYuBlind 预览（点击关闭）",
        WS_POPUP, x, y, w, h, NULL, NULL, inst, NULL);
    if (hwnd == NULL) {
        DeleteDC(hdc);
        DeleteObject(bmp);
        out(L"预览失败：创建窗口失败", err);
        return;
    }

    struct preview_data* pd = (struct preview_data*)malloc(sizeof(struct preview_data));
    pd->bitmap = bmp;
    pd->hdc = hdc;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pd);

    POINT window_pt = { x, y };
    SIZE window_size = { w, h };
    POINT start_pt = { 0, 0 };
    BLENDFUNCTION blend_opt = { 0 };
    blend_opt.AlphaFormat = AC_SRC_ALPHA;
    blend_opt.SourceConstantAlpha = 255;
    blend_opt.BlendOp = AC_SRC_OVER;
    UpdateLayeredWindow(hwnd, NULL, &window_pt, &window_size, hdc, &start_pt, 0, &blend_opt, ULW_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
}

// ---------------------------------------------------------------------------
// 原有初始化逻辑
// ---------------------------------------------------------------------------

void load_ctx_basic(struct context* ptr_ctx) {
    wchar_t fmt[] = L"%s";
    wchar_t str_class_name[] = L"JiYuBlind_Class";
    wchar_t str_window_name[] = L"JiYuBlind";
    StringCchPrintfW(ptr_ctx->class_name, size_str, fmt, str_class_name);
    StringCchPrintfW(ptr_ctx->window_name, size_str, fmt, str_window_name);
    ptr_ctx->proc_func = lilith_wndproc;
    ptr_ctx->window_x = 100;
    ptr_ctx->window_y = 100;
    ptr_ctx->window_height = ptr_ctx->img_y;
    ptr_ctx->window_width = ptr_ctx->img_x;
}

void class_init(WNDCLASSEXW* ptr_class, struct context* ptr_ctx) {
    ptr_class->hIcon = LoadIconW(ptr_ctx->instance, MAKEINTRESOURCEW(IDI_LILITH_ICON));
    ptr_class->hIconSm = ptr_class->hIcon;
    ptr_class->lpszClassName = ptr_ctx->class_name;
    ptr_class->hInstance = ptr_ctx->instance;
    ptr_class->hCursor = LoadCursor(NULL, IDC_ARROW);
    ptr_class->cbSize = sizeof(WNDCLASSEXW);
    ptr_class->style = CS_HREDRAW | CS_CLASSDC | CS_DBLCLKS;
    ptr_class->lpfnWndProc = lilith_wndproc;
    ptr_class->hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); // 存疑
}

bool init_bmi(struct context* ptr_ctx) {
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = ptr_ctx->window_width;
    bmi.bmiHeader.biHeight = -ptr_ctx->window_height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    ptr_ctx->bitmap = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &ptr_ctx->bits, NULL, 0);
    if (ptr_ctx->bitmap == NULL || ptr_ctx->bits == NULL) {
        return false;
    }
    return true;
}

bool add_tray_icon(struct context* ptr_ctx) {
    if (ptr_ctx == NULL) {
        return false;
    }
    memset(&ptr_ctx->tray_nid, 0, sizeof(NOTIFYICONDATA));
    ptr_ctx->tray_nid.cbSize = sizeof(NOTIFYICONDATA);
    ptr_ctx->tray_nid.hWnd = ptr_ctx->window_hwnd;
    ptr_ctx->tray_nid.uID = 1001;
    ptr_ctx->tray_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    ptr_ctx->tray_nid.uCallbackMessage = WM_TRAYICON;
    ptr_ctx->tray_nid.hIcon = LoadIconW(ptr_ctx->instance, MAKEINTRESOURCEW(IDI_LILITH_ICON));
    StringCchPrintfW(ptr_ctx->tray_nid.szTip, 64, L"JiYuBlind");
    if (Shell_NotifyIcon(NIM_ADD, &ptr_ctx->tray_nid) == TRUE) {
        return true;
    }
    return false;
}

bool init_panel(struct context* ptr_ctx) {
    ptr_ctx->panel_ctx.panel_hwnd = CreateDialogParamW(ptr_ctx->instance, MAKEINTRESOURCEW(IDD_CONTROL_PANEL), ptr_ctx->window_hwnd, dlg_proc, (LPARAM)ptr_ctx);
    if (ptr_ctx->panel_ctx.panel_hwnd != NULL) {
        //ShowWindow(ptr_ctx->panel_ctx.panel_hwnd, SW_SHOW);
        SetWindowPos(ptr_ctx->panel_ctx.panel_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return true;
    }
    return false;
}
