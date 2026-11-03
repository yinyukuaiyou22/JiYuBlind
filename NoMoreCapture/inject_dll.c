// inject_dll.c —— DLL 注入路径（主路径）；失败时上层会回退到 shellcode
//
// 流程：内嵌 payload → 释放到 %TEMP% → 目标进程 LoadLibraryW →
//       找到远端模块基址 → 解析 PE 导出表取 JybApply 的 RVA →
//       把 {hwnd, affinity} 写进目标内存 → CreateRemoteThread 调用
//
// 跨架构要点：64 位宿主往 32 位目标注入时，拿不到 32 位 LoadLibraryW 地址，
//            必须解析 SysWOW64\kernel32.dll 的导出 RVA，再叠加目标进程里 kernel32 的基址。

#define _CRT_SECURE_NO_WARNINGS

#include "lilith_is_all_you_need.h"

#include <stdio.h>

/* 来自 core_implement.c */
ULONG_PTR get_target_proc_dll_base(HANDLE proc_handle, DWORD pid, const wchar_t* target_mod_name);

/* 与 payload 里的 JYB_ARGS 必须逐字节一致：hwnd 固定 8 字节，
   否则 32 位 payload 会把 affinity 读到偏移 4（句柄高位）、变成 WDA_NONE */
typedef struct jyb_args {
    ULONGLONG hwnd;
    DWORD     affinity;
} jyb_args;

/* 诊断日志：JYB_INJECT_LOG=1 时把每一步写到 %TEMP%\jyb_inject_log.txt */
static void jyb_log(const wchar_t* step, BOOL ok, DWORD err) {
    wchar_t path[MAX_PATH] = { 0 };
    FILE* f;
    if (GetEnvironmentVariableW(L"JYB_INJECT_LOG", path, 8) == 0) return;
    GetTempPathW(MAX_PATH, path);
    StringCchCatW(path, MAX_PATH, L"jyb_inject_log.txt");
    f = _wfopen(path, L"a");
    if (f == NULL) return;
    fwprintf(f, L"%-34ls ok=%d err=%lu\n", step, (int)ok, (unsigned long)err);
    fclose(f);
}


/* ---------------- 极简 PE 解析（只看导出表，32/64 通用） ---------------- */

typedef struct pe_view {
    const BYTE* img;
    size_t      size;
    DWORD       exp_rva;
} pe_view;

static bool pe_open(const BYTE* img, size_t size, pe_view* v) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)img;
    DWORD nt_off, opt_off, dir_off;
    WORD magic;

    if (img == NULL || size < sizeof(IMAGE_DOS_HEADER)) return false;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    nt_off = (DWORD)dos->e_lfanew;
    if (size < (size_t)nt_off + 4 + sizeof(IMAGE_FILE_HEADER)) return false;
    if (*(const DWORD*)(img + nt_off) != IMAGE_NT_SIGNATURE) return false;

    opt_off = nt_off + 4 + (DWORD)sizeof(IMAGE_FILE_HEADER);
    if (size < (size_t)opt_off + 2) return false;
    magic = *(const WORD*)(img + opt_off);
    /* 数据目录起点：可选头里 DataDirectory 的偏移，PE32 = 96，PE32+ = 112 */
    dir_off = opt_off + ((magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) ? 112u : 96u);
    if (size < (size_t)dir_off + 8) return false;

    v->img = img;
    v->size = size;
    v->exp_rva = *(const DWORD*)(img + dir_off);      /* DataDirectory[0] = Export */
    return v->exp_rva != 0;
}

static DWORD pe_rva2off(const pe_view* v, DWORD rva) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)v->img;
    DWORD nt_off = (DWORD)dos->e_lfanew;
    const IMAGE_FILE_HEADER* fh = (const IMAGE_FILE_HEADER*)(v->img + nt_off + 4);
    const IMAGE_SECTION_HEADER* sec = (const IMAGE_SECTION_HEADER*)
        (v->img + nt_off + 4 + sizeof(IMAGE_FILE_HEADER) + fh->SizeOfOptionalHeader);
    WORD i;

    for (i = 0; i < fh->NumberOfSections; i++) {
        DWORD va = sec[i].VirtualAddress;
        DWORD vs = sec[i].Misc.VirtualSize;
        if (rva >= va && rva < va + vs) {
            return rva - va + sec[i].PointerToRawData;
        }
    }
    return rva; /* 落在头部里的直接映射 */
}

static DWORD pe_find_export(const pe_view* v, const char* name) {
    DWORD off = pe_rva2off(v, v->exp_rva);
    const IMAGE_EXPORT_DIRECTORY* ed;
    const DWORD* names;
    const WORD*  ords;
    const DWORD* funcs;
    DWORD i;

    if (off == 0 || (size_t)off + sizeof(IMAGE_EXPORT_DIRECTORY) > v->size) return 0;
    ed = (const IMAGE_EXPORT_DIRECTORY*)(v->img + off);
    names = (const DWORD*)(v->img + pe_rva2off(v, ed->AddressOfNames));
    ords = (const WORD*)(v->img + pe_rva2off(v, ed->AddressOfNameOrdinals));
    funcs = (const DWORD*)(v->img + pe_rva2off(v, ed->AddressOfFunctions));

    for (i = 0; i < ed->NumberOfNames; i++) {
        const char* n = (const char*)(v->img + pe_rva2off(v, names[i]));
        if (n != NULL && strcmp(n, name) == 0) {
            return funcs[ords[i]];
        }
    }
    return 0;
}

/* ---------------- 内嵌 payload ---------------- */

static const BYTE* payload_bytes(bool want64, DWORD* out_size) {
    HINSTANCE self = GetModuleHandleW(NULL);
    HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(want64 ? IDR_PAYLOAD64 : IDR_PAYLOAD32), RT_RCDATA);
    HGLOBAL glob;
    if (res == NULL) return NULL;
    glob = LoadResource(self, res);
    if (glob == NULL) return NULL;
    *out_size = SizeofResource(self, res);
    return (const BYTE*)LockResource(glob);
}

/* 把内嵌 payload 落到 %TEMP%，返回路径。
 *
 * 关键：文件名必须**按目标进程 pid 唯一**。
 * 早期版本用固定名 jyb_payload_64.dll 且以独占方式(share=0)写入，
 * 结果第一个目标进程 LoadLibrary 之后该文件映像节被系统锁住，
 * 后续写入全部返回 ERROR_SHARING_VIOLATION(32)，
 * DLL 注入路径从第二个窗口起全部失效（全局模式尤其明显）。
 * 现在改成每个目标进程一个独立文件名，且以共享模式打开。
 */
static const wchar_t* payload_temp_path(bool want64, DWORD target_pid) {
    /* 每个目标进程一份独立文件：缓存 8 个（环形），键 = pid */
    #define PAYLOAD_CACHE_SLOTS 8
    static wchar_t cache[PAYLOAD_CACHE_SLOTS][MAX_PATH] = { { 0 } };
    static DWORD cache_pid[PAYLOAD_CACHE_SLOTS] = { 0 };
    static bool  cache_64[PAYLOAD_CACHE_SLOTS] = { 0 };
    static int   cache_next = 0;

    const BYTE* data;
    DWORD size = 0;
    wchar_t temp[MAX_PATH] = { 0 };
    HANDLE file;
    DWORD written = 0;
    int slot_idx;
    wchar_t* slot;

    /* 命中：同 pid + 同位数 */
    for (slot_idx = 0; slot_idx < PAYLOAD_CACHE_SLOTS; slot_idx++) {
        if (cache_pid[slot_idx] == target_pid && cache_64[slot_idx] == want64 && cache[slot_idx][0] != L'\0') {
            DWORD attr = GetFileAttributesW(cache[slot_idx]);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                return cache[slot_idx];
            }
            cache[slot_idx][0] = L'\0';
        }
    }

    data = payload_bytes(want64, &size);
    if (data == NULL || size == 0) return NULL;

    GetTempPathW(MAX_PATH, temp);
    slot_idx = cache_next;
    cache_next = (cache_next + 1) % PAYLOAD_CACHE_SLOTS;
    slot = cache[slot_idx];
    StringCchPrintfW(slot, MAX_PATH, L"%sjyb_payload_%s_%lu_%lu.dll",
                     temp, want64 ? L"64" : L"32",
                     (unsigned long)target_pid, (unsigned long)GetTickCount());

    /* 共享模式：即使同名文件已被别的进程加载，也能覆盖写入 */
    file = CreateFileW(slot, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        slot[0] = L'\0';
        return NULL;
    }
    WriteFile(file, data, size, &written, NULL);
    CloseHandle(file);
    if (written != size) {
        slot[0] = L'\0';
        return NULL;
    }
    cache_pid[slot_idx] = target_pid;
    cache_64[slot_idx] = want64;
    return slot;
}

/* ---------------- 远端加载 DLL ---------------- */

static DWORD export_rva_of_system_dll(const wchar_t* dll_path, const char* fn) {
    HANDLE file;
    DWORD size = 0, got = 0, rva = 0;
    BYTE* buf;
    pe_view v;

    file = CreateFileW(dll_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(file, NULL);
    if (size == 0 || size == INVALID_FILE_SIZE) { CloseHandle(file); return 0; }
    buf = (BYTE*)malloc(size);
    if (buf == NULL) { CloseHandle(file); return 0; }
    if (ReadFile(file, buf, size, &got, NULL) && got == size) {
        if (pe_open(buf, size, &v)) {
            rva = pe_find_export(&v, fn);
        }
    }
    free(buf);
    CloseHandle(file);
    return rva;
}

static HMODULE find_remote_module(HANDLE proc, bool target32, const wchar_t* dll_name) {
    HMODULE mods[1024];
    DWORD need = 0;
    DWORD filter = target32 ? LIST_MODULES_32BIT : LIST_MODULES_64BIT;
    wchar_t name[MAX_PATH];

    if (!EnumProcessModulesEx(proc, mods, sizeof(mods), &need, filter)) return NULL;
    {
        DWORD cnt = need / sizeof(HMODULE);
        DWORD i;
        for (i = 0; i < cnt; i++) {
            if (GetModuleBaseNameW(proc, mods[i], name, MAX_PATH)) {
                if (_wcsicmp(name, dll_name) == 0) return mods[i];
            }
        }
    }
    return NULL;
}

/* ---------------- 对外入口 ---------------- */

bool inject_via_dll(HANDLE proc_handle, DWORD pid, HWND target_hwnd, DWORD affinity, bool target_is_wow64) {
    const wchar_t* dll_path;
    const BYTE* data;
    DWORD data_size = 0, export_rva;
    SIZE_T path_bytes;
    LPVOID remote_path = NULL;
    LPVOID remote_args = NULL;
    HMODULE k32_base;
    DWORD loadlib_rva;
    HANDLE thread;
    jyb_args args;
    pe_view v;
    bool ok = false;

    if (proc_handle == NULL || target_hwnd == NULL) return false;

    dll_path = payload_temp_path(target_is_wow64 ? false : true, pid);
    if (dll_path == NULL) { jyb_log(L"payload_temp_path", FALSE, GetLastError()); return false; }
    jyb_log(L"payload_temp_path", TRUE, 0);

    data = payload_bytes(target_is_wow64 ? false : true, &data_size);
    if (data == NULL || data_size == 0) { jyb_log(L"payload_bytes", FALSE, GetLastError()); return false; }
    if (!pe_open(data, data_size, &v)) { jyb_log(L"pe_open", FALSE, 0); return false; }
    export_rva = pe_find_export(&v, "JybApply");
    if (export_rva == 0) { jyb_log(L"pe_find_export(JybApply)", FALSE, 0); return false; }
    jyb_log(L"pe_find_export(JybApply)", TRUE, export_rva);

    /* 1) 把 DLL 路径写进目标进程 */
    path_bytes = (wcslen(dll_path) + 1) * sizeof(wchar_t);
    remote_path = VirtualAllocEx(proc_handle, NULL, path_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_path == NULL) { jyb_log(L"VirtualAllocEx(path)", FALSE, GetLastError()); return false; }
    if (!WriteProcessMemory(proc_handle, remote_path, dll_path, path_bytes, NULL)) {
        jyb_log(L"WriteProcessMemory(path)", FALSE, GetLastError()); goto cleanup;
    }

    /* 2) 找到目标进程内 kernel32 的 LoadLibraryW 地址（跨架构要取对应位数的副本） */
    k32_base = (HMODULE)get_target_proc_dll_base(proc_handle, pid, L"kernel32.dll");
    if (k32_base == NULL) { jyb_log(L"get_target_proc_dll_base(kernel32)", FALSE, GetLastError()); goto cleanup; }
    jyb_log(L"get_target_proc_dll_base(kernel32)", TRUE, (DWORD)(ULONG_PTR)k32_base);

    loadlib_rva = export_rva_of_system_dll(
        target_is_wow64 ? L"C:\\Windows\\SysWOW64\\kernel32.dll" : L"C:\\Windows\\System32\\kernel32.dll",
        "LoadLibraryW");
    if (loadlib_rva == 0) { jyb_log(L"export_rva_of_system_dll(LoadLibraryW)", FALSE, GetLastError()); goto cleanup; }
    jyb_log(L"export_rva_of_system_dll(LoadLibraryW)", TRUE, loadlib_rva);

    /* 3) 远程 LoadLibraryW */
    thread = CreateRemoteThread(proc_handle, NULL, 0,
                                (LPTHREAD_START_ROUTINE)((ULONG_PTR)k32_base + loadlib_rva),
                                remote_path, 0, NULL);
    if (thread == NULL) { jyb_log(L"CreateRemoteThread(LoadLibraryW)", FALSE, GetLastError()); goto cleanup; }
    WaitForSingleObject(thread, 15000);
    CloseHandle(thread);
    jyb_log(L"CreateRemoteThread(LoadLibraryW)", TRUE, 0);

    /* 4) 找到远端模块基址（不能用 GetExitCodeThread 取 HMODULE，64 位会被截断） */
    {
        const wchar_t* base_name = wcsrchr(dll_path, L'\\');
        HMODULE remote_base = find_remote_module(proc_handle, target_is_wow64, base_name ? base_name + 1 : dll_path);
        if (remote_base == NULL) { jyb_log(L"find_remote_module(payload)", FALSE, GetLastError()); goto cleanup; }
        jyb_log(L"find_remote_module(payload)", TRUE, (DWORD)(ULONG_PTR)remote_base);

        /* 5) 参数结构体写进目标进程 */
        args.hwnd = (ULONGLONG)(ULONG_PTR)target_hwnd;
        args.affinity = affinity;
        remote_args = VirtualAllocEx(proc_handle, NULL, sizeof(args), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remote_args == NULL) goto cleanup;
        if (!WriteProcessMemory(proc_handle, remote_args, &args, sizeof(args), NULL)) goto cleanup;

        /* 6) 调用 JybApply(args) */
        thread = CreateRemoteThread(proc_handle, NULL, 0,
                                    (LPTHREAD_START_ROUTINE)((ULONG_PTR)remote_base + export_rva),
                                    remote_args, 0, NULL);
        if (thread == NULL) { jyb_log(L"CreateRemoteThread(JybApply)", FALSE, GetLastError()); goto cleanup; }
        {
            DWORD wait_ret = WaitForSingleObject(thread, 10000);
            DWORD exit_code = 0;
            if (wait_ret == WAIT_OBJECT_0 && GetExitCodeThread(thread, &exit_code)) {
                ok = (exit_code != 0);
            }
            jyb_log(L"JybApply result", ok, exit_code);
            CloseHandle(thread);
        }
    }

cleanup:
    if (remote_args != NULL) VirtualFreeEx(proc_handle, remote_args, 0, MEM_RELEASE);
    if (remote_path != NULL) VirtualFreeEx(proc_handle, remote_path, 0, MEM_RELEASE);
    /* 注入失败时清掉临时 DLL，避免 %TEMP% 堆积；成功时不能删（目标进程还映射着它） */
    if (!ok && dll_path != NULL) {
        DeleteFileW(dll_path);
    }
    return ok;
}
