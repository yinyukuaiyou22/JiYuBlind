#include "lilith_is_all_you_need.h"

// 与 control_panel.c 中的 bitwise 标志位保持一致
enum {
    cfg_f_c = 2, // 白名单模式
    cfg_f_d = 3, // 穿透模式
    cfg_f_e = 4  // 老板键
};

static void get_config_path_named(wchar_t* buf, size_t n, const wchar_t* file_name) {
    wchar_t exe_path[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    wchar_t* slash = wcsrchr(exe_path, L'\\');
    if (slash != NULL) {
        *(slash + 1) = L'\0';
    }
    StringCchPrintfW(buf, n, L"%s%s", exe_path, file_name);
}

static void get_config_path(wchar_t* buf, size_t n) {
    get_config_path_named(buf, n, L"JiYuBlind.ini");
}

static void get_legacy_config_path(wchar_t* buf, size_t n) {
    get_config_path_named(buf, n, L"NoMoreCapture.ini");
}

static bool file_exists(const wchar_t* p) {
    DWORD attr = GetFileAttributesW(p);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void config_load(struct context* ctx) {
    if (ctx == NULL) {
        return;
    }
    wchar_t path[MAX_PATH] = { 0 };
    get_config_path(path, MAX_PATH);

    /* 向后兼容：新配置不存在但旧的 NoMoreCapture.ini 存在时，读旧文件（保存仍写新名字） */
    if (!file_exists(path)) {
        wchar_t legacy[MAX_PATH] = { 0 };
        get_legacy_config_path(legacy, MAX_PATH);
        if (file_exists(legacy)) {
            StringCchCopyW(path, MAX_PATH, legacy);
        }
    }
    int whitelist = GetPrivateProfileIntW(L"Config", L"WhiteList", 1, path);
    int transparent = GetPrivateProfileIntW(L"Config", L"Transparent", 1, path);
    int bosskey = GetPrivateProfileIntW(L"Config", L"BossKey", 0, path);
    int hotkey_vk = GetPrivateProfileIntW(L"Config", L"HotKeyVk", 0, path);
    int hotkey_mods = GetPrivateProfileIntW(L"Config", L"HotKeyMods", 0, path);
    ctx->scale_percent = GetPrivateProfileIntW(L"Config", L"Scale", 100, path);
    GetPrivateProfileStringW(L"Config", L"Background", L"", ctx->background_path, MAX_PATH, path);

    /* 替身画面：Mode 0=原版行为（向后兼容） 1=替身画面；DecoyImage 为空则用桌面快照 */
    ctx->decoy.mode = GetPrivateProfileIntW(L"Config", L"Mode", 0, path);
    GetPrivateProfileStringW(L"Config", L"DecoyImage", L"", ctx->decoy.image_path, MAX_PATH, path);

    if (whitelist) {
        SET_BIT(ctx->panel_ctx.bitwise, cfg_f_c);
    }
    else {
        CLEAR_BIT(ctx->panel_ctx.bitwise, cfg_f_c);
    }
    if (transparent) {
        SET_BIT(ctx->panel_ctx.bitwise, cfg_f_d);
    }
    else {
        CLEAR_BIT(ctx->panel_ctx.bitwise, cfg_f_d);
    }
    if (bosskey) {
        SET_BIT(ctx->panel_ctx.bitwise, cfg_f_e);
    }
    else {
        CLEAR_BIT(ctx->panel_ctx.bitwise, cfg_f_e);
    }

    ctx->panel_ctx.virtual_code = (BYTE)(hotkey_vk & 0xFF);
    ctx->panel_ctx.modifiers = (BYTE)(hotkey_mods & 0xFF);
    ctx->panel_ctx.hotkey = (WORD)(hotkey_vk | (hotkey_mods << 8));

    if (ctx->scale_percent < 5 || ctx->scale_percent > 1000) {
        ctx->scale_percent = 100;
    }
}

void config_save(struct context* ctx) {
    if (ctx == NULL) {
        return;
    }
    wchar_t path[MAX_PATH] = { 0 };
    get_config_path(path, MAX_PATH);

    wchar_t num[32] = { 0 };

    StringCchPrintfW(num, 32, L"%d", CHECK_BIT(ctx->panel_ctx.bitwise, cfg_f_c) ? 1 : 0);
    WritePrivateProfileStringW(L"Config", L"WhiteList", num, path);

    StringCchPrintfW(num, 32, L"%d", CHECK_BIT(ctx->panel_ctx.bitwise, cfg_f_d) ? 1 : 0);
    WritePrivateProfileStringW(L"Config", L"Transparent", num, path);

    StringCchPrintfW(num, 32, L"%d", CHECK_BIT(ctx->panel_ctx.bitwise, cfg_f_e) ? 1 : 0);
    WritePrivateProfileStringW(L"Config", L"BossKey", num, path);

    StringCchPrintfW(num, 32, L"%d", (int)ctx->panel_ctx.virtual_code);
    WritePrivateProfileStringW(L"Config", L"HotKeyVk", num, path);

    StringCchPrintfW(num, 32, L"%d", (int)ctx->panel_ctx.modifiers);
    WritePrivateProfileStringW(L"Config", L"HotKeyMods", num, path);

    StringCchPrintfW(num, 32, L"%d", ctx->scale_percent);
    WritePrivateProfileStringW(L"Config", L"Scale", num, path);

    WritePrivateProfileStringW(L"Config", L"Background", ctx->background_path, path);

    StringCchPrintfW(num, 32, L"%d", ctx->decoy.mode);
    WritePrivateProfileStringW(L"Config", L"Mode", num, path);
    WritePrivateProfileStringW(L"Config", L"DecoyImage", ctx->decoy.image_path, path);
}
