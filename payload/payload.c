// payload.c —— 注入到目标进程的载荷 DLL
//
// 为什么要有它：显示亲和性 API 有同进程限制（跨进程直接 ERROR_ACCESS_DENIED），
// 所以必须让"目标进程自己"执行调用。两种载体：
//   1) 本 DLL：LoadLibrary 注入后由宿主用 CreateRemoteThread 调导出函数 JybMaintain
//   2) shellcode：不落地文件的兜底路径（只能一次性设置，没有守护）
//
// 编译成两份：payload32.dll(x86) / payload64.dll(x64)，分别用于 32/64 位目标。
// 导出名用 payload.def 固定为无修饰的 JybMaintain / JybApply（否则 x86 会变成 _JybMaintain@4）。
//
// 【为什么要有守护线程】
// 亲和性是"每个窗口一份"的状态，窗口一旦销毁重建（浏览器切全屏、GPU 进程重启、
// 应用自己重建主窗口、远控软件改动显示拓扑后应用重建窗口……）就归零，而宿主进程
// 只能靠一次性注入去设置，抓不到"新窗口"这个时刻。让载荷自己留在目标进程里，
// 每 200ms 扫一遍本进程的顶层窗口、发现亲和性不一致就补上，是唯一低成本且
// 不依赖宿主存活的兜底方式（实测：同 HWND 上的隐藏/显示、切全屏、最小化还原、
// 改分辨率都不清零亲和性，只有重建窗口会清零——见 .probe/reset_probe_result.txt）。

#include <windows.h>

// CreateRemoteThread 只能直接传一个指针参数，所以参数必须打包成结构体，远端函数签名也必须是"单个指针"。
// 关键：hwnd 一律用 ULONGLONG（8 字节），保证 32/64 位下其余字段偏移一致——
// 用 ULONG_PTR 会让 32 位布局把后面的字段读串（踩过：affinity 读到句柄高 32 位 = 0，
// 结果变成 SetWindowDisplayAffinity(hwnd, WDA_NONE) 把保护关掉）。
typedef struct JYB_ARGS {
    ULONGLONG hwnd;
    DWORD     affinity;
    DWORD     owner_pid;   // 宿主进程 pid：宿主没了就自己收手，别把窗口永久锁死
} JYB_ARGS;

/* 守护节奏：
   - 平时 2s 扫一遍（约 0.2% 单核，抓屏类工具里可以忽略）
   - 目标进程里自己装一个 WinEvent 钩子，窗口一 show/create 就立刻补一次（延迟 < 100ms）
   之前是纯 200ms 轮询，实测目标进程要吃掉 1.7% 单核，对"高性能"这条要求不合格 */
#define JYB_SWEEP_MS     1000
#define JYB_MIN_GAP_MS    150

static volatile LONG g_want_affinity = 0;   // 0 = 守护停止
static volatile LONG g_guard_started = 0;

/* 把本进程所有"可见顶层窗口"的亲和性对齐到 aff；aff == 0 表示撤销 */
static BOOL CALLBACK jyb_guard_enum(HWND hwnd, LPARAM lp) {
    DWORD aff = (DWORD)(ULONG_PTR)lp;
    DWORD pid = 0;
    DWORD cur = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    if (GetWindowDisplayAffinity(hwnd, &cur) && cur == aff) {
        return TRUE;
    }
    /* 刚创建出来的窗口可能还没有 DWM 重定向表面，第一次 Set 会以
       ERROR_NOT_ENOUGH_MEMORY(8) 失败；短暂等一下再试一次就能成功，
       否则要等下一拍扫描（表现为"新窗口有半秒到两秒的裸奔窗口期"）。 */
    if (!SetWindowDisplayAffinity(hwnd, aff)) {
        Sleep(50);
        SetWindowDisplayAffinity(hwnd, aff);
    }
    return TRUE;
}

/* 只负责把守护线程从 MsgWait 里叫醒，真正的补设在循环里做 */
static void CALLBACK jyb_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                    LONG idObject, LONG idChild, DWORD thread, DWORD tick) {
    (void)hook; (void)event; (void)hwnd; (void)idChild; (void)thread; (void)tick;
    if (idObject != OBJID_WINDOW) {
        return;
    }
    PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
}

/* 诊断：JYB_INJECT_LOG=1 时把守护线程的关键步骤写到 %TEMP%\jyb_payload_log.txt（不依赖 CRT） */
static void jyb_dbg(const wchar_t* step, long v) {
    wchar_t path[MAX_PATH] = { 0 };
    wchar_t line[256];
    char flag[8] = { 0 };
    HANDLE file;
    DWORD written = 0;

    if (GetEnvironmentVariableA("JYB_INJECT_LOG", flag, 8) == 0) return;
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"jyb_payload_log.txt");
    wsprintfW(line, L"[%lu] %s = %ld\r\n", (unsigned long)GetCurrentProcessId(), step, v);
    file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile(file, line, (DWORD)(lstrlenW(line) * sizeof(wchar_t)), &written, NULL);
    CloseHandle(file);
}

static DWORD WINAPI jyb_guard_thread(LPVOID param) {
    DWORD owner_pid = (DWORD)(ULONG_PTR)param;
    HANDLE owner = NULL;
    HWINEVENTHOOK hook;
    DWORD last_sweep = 0;
    DWORD aff;
    MSG seed;

    if (owner_pid != 0) {
        owner = OpenProcess(SYNCHRONIZE, FALSE, owner_pid);
    }
    /* 必须先建消息队列再装钩子：WINEVENT_OUTOFCONTEXT 依赖本线程的消息循环，
       队列不存在的瞬间装钩子会直接失败（踩过，表现为"补设要等 2s 兜底扫描"） */
    PeekMessageW(&seed, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    /* 注意：绝对不能加 WINEVENT_SKIPOWNPROCESS —— 钩子就装在目标进程里，
       要的正是在"本进程自己的窗口"创建/显示时被叫醒。 */
    hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, NULL, jyb_event_proc, 0, 0,
                           WINEVENT_OUTOFCONTEXT);
    jyb_dbg(L"hook", (long)(ULONG_PTR)hook);
    if (hook == NULL) {
        jyb_dbg(L"hook_err", (long)GetLastError());
    }

    for (;;) {
        DWORD waited;
        MSG msg;

        aff = (DWORD)InterlockedCompareExchange(&g_want_affinity, 0, 0);
        if (aff == 0) {
            break;
        }
        waited = MsgWaitForMultipleObjects(owner != NULL ? 1 : 0, &owner, FALSE,
                                          JYB_SWEEP_MS, QS_ALLINPUT);
        if (owner != NULL && waited == WAIT_OBJECT_0) {
            break;   /* 宿主退出：收手，并把窗口还原 */
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        {
            DWORD now = GetTickCount();
            if ((DWORD)(now - last_sweep) >= JYB_MIN_GAP_MS) {
                last_sweep = now;
                EnumWindows(jyb_guard_enum, (LPARAM)(ULONG_PTR)aff);
            }
        }
    }
    if (hook != NULL) {
        UnhookWinEvent(hook);
    }
    /* 收手时把本进程里被我们改过的窗口还原（否则宿主被杀之后窗口永久抓不到） */
    EnumWindows(jyb_guard_enum, (LPARAM)0);
    if (owner != NULL) {
        CloseHandle(owner);
    }
    InterlockedExchange(&g_guard_started, 0);
    return 0;
}

static DWORD jyb_start_guard(DWORD owner_pid) {
    HANDLE thread;
    if (InterlockedCompareExchange(&g_guard_started, 1, 0) != 0) {
        return 1;   /* 已经在守护了 */
    }
    thread = CreateThread(NULL, 0, jyb_guard_thread, (LPVOID)(ULONG_PTR)owner_pid, 0, NULL);
    if (thread == NULL) {
        InterlockedExchange(&g_guard_started, 0);
        return 0;
    }
    CloseHandle(thread);
    return 1;
}

/* 对外的唯一入口：设置 hwnd 的亲和性，并让本进程进入/退出守护状态 */
__declspec(dllexport) DWORD WINAPI JybMaintain(JYB_ARGS* args) {
    DWORD err;
    if (args == NULL) {
        return 0xE0000001u;
    }
    if (args->hwnd == 0) {
        return 0xE0000002u;
    }
    if (!SetWindowDisplayAffinity((HWND)(ULONG_PTR)args->hwnd, args->affinity)) {
        err = GetLastError();
        /* 失败时把 Win32 错误码带回去，0xE0000003 = 有错误码但为 0（不该发生） */
        return err ? err : 0xE0000003u;
    }
    if (args->affinity == 0) {
        InterlockedExchange(&g_want_affinity, 0);   /* 撤销：守护线程下一拍自己退出并还原 */
        return 1u;
    }
    InterlockedExchange(&g_want_affinity, (LONG)args->affinity);
    if (!jyb_start_guard(args->owner_pid)) {
        /* 守护起不来也不影响这一次已经写成功的亲和性 */
        return 1u;
    }
    /* 立刻对齐一轮，别等 200ms 之后 */
    EnumWindows(jyb_guard_enum, (LPARAM)(ULONG_PTR)args->affinity);
    return 1u;
}

/* 兼容旧导出名：老宿主调用 JybApply 时行为等价于 JybMaintain */
__declspec(dllexport) DWORD WINAPI JybApply(JYB_ARGS* args) {
    return JybMaintain(args);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst;
    (void)reason;
    (void)reserved;
    return TRUE;
}