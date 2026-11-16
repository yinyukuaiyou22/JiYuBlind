// guard.c —— 亲和性对账（兜底保险）
//
// 目标：只要用户点过"应用"，之后就再也不需要他手动再点一次。
//   1) 每 GUARD_TICK_MS 一拍：按当前模式算出"应该被保护"的进程集合
//   2) 进程级下发：同一个进程只注入一次，进程内的载荷（payload.c）自己守着所有顶层窗口
//   3) 白名单按进程记账：窗口销毁重建 / 进程重启后，按映像名重新绑定
//   4) 每 GUARD_FORCE_MS 全量重发一次，防止载荷里的守护线程意外死掉
//
// 线程模型：对账在 UI 线程（EnumWindows + 少量系统调用，微秒级），
//          真正阻塞的注入（CreateRemoteThread / WaitForSingleObject）全在 worker 线程，
//          所以 WinEvent 钩子不再阻塞消息循环，也不会再因为 UI 线程被同步注入卡住而丢事件。

#define _CRT_SECURE_NO_WARNINGS

#include "lilith_is_all_you_need.h"
#include "decoy.h"
#include "guard.h"

enum {
    g_f_a = 0, // 捕获
    g_f_b = 1,
    g_f_c = 2, // 白名单模式
    g_f_d = 3, // 穿透模式
    g_f_e = 4,
    g_f_f = 5,
    g_f_g = 6,
    g_f_h = 7
};

#define GUARD_TIMER_ID        0x6A2D
#define GUARD_TICK_MS         2000    /* 对账周期 */
#define GUARD_PID_RECHECK_MS  15000   /* 同一进程多久重发一次 */
#define GUARD_FORCE_MS        60000   /* 全量重发周期 */
#define GUARD_PID_RETRY_MS    15000   /* 上一次下发失败时，多久再试（分层窗口/受保护进程会一直失败） */

static DWORD guard_affinity(struct context* ptr_ctx) {
    return (CHECK_BIT(ptr_ctx->panel_ctx.bitwise, g_f_d) != 0) ? WDA_EXCLUDEFROMCAPTURE : WDA_MONITOR;
}

static bool guard_is_system_window(struct context* ptr_ctx, HWND hwnd) {
    wchar_t cls[64] = { 0 };
    if (hwnd == NULL) return true;
    if (hwnd == ptr_ctx->window_hwnd || hwnd == ptr_ctx->panel_ctx.panel_hwnd) return true;
    if (decoy_is_own_window(ptr_ctx, hwnd)) return true;
    GetClassNameW(hwnd, cls, 64);
    return wcscmp(cls, L"Progman") == 0 ||
           wcscmp(cls, L"WorkerW") == 0 ||
           wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

/* ------------------------------------------------------------------ */
/* worker：所有阻塞的注入都放这里                                        */
/* ------------------------------------------------------------------ */

static bool guard_has_pending_locked(struct context* ptr_ctx) {
    int i;
    for (i = 0; i < GUARD_MAX_PIDS; i++) {
        if (ptr_ctx->guard.pids[i].pending) return true;
    }
    return false;
}

static void guard_run_pending(struct context* ptr_ctx) {
    int i;
    for (i = 0; i < GUARD_MAX_PIDS; i++) {
        struct guard_pid_state job;
        bool has = false;

        EnterCriticalSection(&ptr_ctx->guard.lock);
        if (ptr_ctx->guard.pids[i].pending && ptr_ctx->guard.pids[i].hwnd != NULL) {
            ptr_ctx->guard.pids[i].pending = 0;
            job = ptr_ctx->guard.pids[i];
            has = true;
        }
        LeaveCriticalSection(&ptr_ctx->guard.lock);
        if (!has) continue;
        if (!IsWindow(job.hwnd)) continue;   /* 窗口没了：下一拍重新找一个 */

        {
            bool ok = do_inject_anti_capture(job.pid, job.hwnd, job.want, &ptr_ctx->inject_ctx);
            EnterCriticalSection(&ptr_ctx->guard.lock);
            if (ptr_ctx->guard.pids[i].pid == job.pid) {
                ptr_ctx->guard.pids[i].ok = ok;
                ptr_ctx->guard.pids[i].last_try = GetTickCount();
                if (ok) {
                    ptr_ctx->guard.pids[i].last_ok = ptr_ctx->guard.pids[i].last_try;
                }
            }
            LeaveCriticalSection(&ptr_ctx->guard.lock);
        }
    }
}

static DWORD WINAPI guard_worker_proc(LPVOID param) {
    struct context* ptr_ctx = (struct context*)param;
    if (ptr_ctx == NULL) return 0;
    for (;;) {
        WaitForSingleObject(ptr_ctx->guard.wake, 1000);
        if (InterlockedCompareExchange(&ptr_ctx->guard.stop, 0, 0) != 0) break;
        EnterCriticalSection(&ptr_ctx->guard.lock);
        if (guard_has_pending_locked(ptr_ctx)) {
            ResetEvent(ptr_ctx->guard.idle);
            LeaveCriticalSection(&ptr_ctx->guard.lock);
            guard_run_pending(ptr_ctx);
        }
        else {
            LeaveCriticalSection(&ptr_ctx->guard.lock);
        }
        SetEvent(ptr_ctx->guard.idle);
    }
    return 0;
}

/* 请求"把某个进程对齐到 want"；同一进程 GUARD_PID_RECHECK_MS 内不重复下发 */
static void guard_request_pid(struct context* ptr_ctx, DWORD pid, HWND hwnd, DWORD want, bool force) {
    struct guard_pid_state* slot = NULL;
    struct guard_pid_state* victim = NULL;
    DWORD now = GetTickCount();
    int i;

    if (pid == 0 || hwnd == NULL) return;

    EnterCriticalSection(&ptr_ctx->guard.lock);
    for (i = 0; i < GUARD_MAX_PIDS; i++) {
        if (ptr_ctx->guard.pids[i].pid == pid) { slot = &ptr_ctx->guard.pids[i]; break; }
    }
    if (slot == NULL) {
        for (i = 0; i < GUARD_MAX_PIDS; i++) {
            if (ptr_ctx->guard.pids[i].pid == 0) { slot = &ptr_ctx->guard.pids[i]; break; }
        }
    }
    if (slot == NULL) {
        /* 满了：优先顶掉最久没成功过的那个 */
        victim = &ptr_ctx->guard.pids[0];
        for (i = 1; i < GUARD_MAX_PIDS; i++) {
            if (ptr_ctx->guard.pids[i].last_ok < victim->last_ok) victim = &ptr_ctx->guard.pids[i];
        }
        slot = victim;
        ZeroMemory(slot, sizeof(*slot));
    }
    if (slot->pid != pid) {
        ZeroMemory(slot, sizeof(*slot));
        slot->pid = pid;
    }
    slot->hwnd = hwnd;
    if (!force) {
        if (slot->ok && slot->want == want && (now - slot->last_ok) < GUARD_PID_RECHECK_MS) {
            LeaveCriticalSection(&ptr_ctx->guard.lock);
            return;
        }
        /* 刚失败过就先别试了：per-pixel-alpha 分层窗口（error 8）之类的目标
           每拍重试一次纯属浪费，而且会反复往目标进程里起远程线程 */
        if (!slot->ok && slot->last_try != 0 && (now - slot->last_try) < GUARD_PID_RETRY_MS) {
            LeaveCriticalSection(&ptr_ctx->guard.lock);
            return;
        }
    }
    slot->want = want;
    slot->pending = 1;
    LeaveCriticalSection(&ptr_ctx->guard.lock);
    SetEvent(ptr_ctx->guard.wake);
}

/* ------------------------------------------------------------------ */
/* 白名单记账                                                          */
/* ------------------------------------------------------------------ */

static void guard_fill_entry(struct guard_entry* entry, HWND hwnd) {
    wchar_t path[MAX_PATH] = { 0 };
    DWORD size = MAX_PATH;
    HANDLE proc;
    DWORD pid = 0;

    ZeroMemory(entry, sizeof(*entry));
    entry->hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &pid);
    entry->pid = pid;
    if (pid == 0) return;
    proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == NULL) return;
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        StringCchCopyW(entry->exe, MAX_PATH, name ? name + 1 : path);
    }
    CloseHandle(proc);
}

void guard_clear_entries(struct context* ptr_ctx) {
    if (ptr_ctx == NULL) return;
    EnterCriticalSection(&ptr_ctx->guard.lock);
    ptr_ctx->guard.count = 0;
    ZeroMemory(ptr_ctx->guard.entries, sizeof(ptr_ctx->guard.entries));
    LeaveCriticalSection(&ptr_ctx->guard.lock);
}

bool guard_add_entry(struct context* ptr_ctx, HWND hwnd) {
    struct guard_entry fresh;
    int i;

    if (ptr_ctx == NULL || hwnd == NULL || !IsWindow(hwnd)) return false;
    guard_fill_entry(&fresh, hwnd);
    if (fresh.pid == 0) return false;

    EnterCriticalSection(&ptr_ctx->guard.lock);
    for (i = 0; i < ptr_ctx->guard.count; i++) {
        /* 同一进程只留一条：真正的保护单位是进程，不是单个窗口 */
        if (ptr_ctx->guard.entries[i].pid == fresh.pid) {
            ptr_ctx->guard.entries[i].hwnd = hwnd;
            LeaveCriticalSection(&ptr_ctx->guard.lock);
            return true;
        }
    }
    if (ptr_ctx->guard.count >= GUARD_MAX_ENTRIES) {
        LeaveCriticalSection(&ptr_ctx->guard.lock);
        return false;
    }
    ptr_ctx->guard.entries[ptr_ctx->guard.count++] = fresh;
    LeaveCriticalSection(&ptr_ctx->guard.lock);
    return true;
}

int guard_count(struct context* ptr_ctx) {
    int n;
    if (ptr_ctx == NULL) return 0;
    EnterCriticalSection(&ptr_ctx->guard.lock);
    n = ptr_ctx->guard.count;
    LeaveCriticalSection(&ptr_ctx->guard.lock);
    return n;
}

HWND guard_hwnd_at(struct context* ptr_ctx, int index) {
    HWND hwnd = NULL;
    if (ptr_ctx == NULL) return NULL;
    EnterCriticalSection(&ptr_ctx->guard.lock);
    if (index >= 0 && index < ptr_ctx->guard.count) {
        hwnd = ptr_ctx->guard.entries[index].hwnd;
    }
    LeaveCriticalSection(&ptr_ctx->guard.lock);
    return hwnd;
}

bool guard_has_process(struct context* ptr_ctx, DWORD pid) {
    bool found = false;
    int i;
    if (ptr_ctx == NULL || pid == 0) return false;
    EnterCriticalSection(&ptr_ctx->guard.lock);
    for (i = 0; i < ptr_ctx->guard.count; i++) {
        if (ptr_ctx->guard.entries[i].pid == pid) { found = true; break; }
    }
    LeaveCriticalSection(&ptr_ctx->guard.lock);
    return found;
}

/* 按映像名找同应用的窗口（进程重启 / 窗口重建后重新绑定） */
struct guard_find_ctx {
    const wchar_t* exe;
    HWND found;
    DWORD pid;
};

static BOOL CALLBACK guard_find_enum(HWND hwnd, LPARAM lparam) {
    struct guard_find_ctx* f = (struct guard_find_ctx*)lparam;
    wchar_t path[MAX_PATH] = { 0 };
    wchar_t name[MAX_PATH] = { 0 };
    DWORD size = MAX_PATH;
    DWORD pid = 0;
    HANDLE proc;

    if (!IsWindowVisible(hwnd)) return TRUE;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;
    proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == NULL) return TRUE;
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        StringCchCopyW(name, MAX_PATH, base ? base + 1 : path);
    }
    CloseHandle(proc);
    if (name[0] != L'\0' && _wcsicmp(name, f->exe) == 0) {
        f->found = hwnd;
        f->pid = pid;
        return FALSE;
    }
    return TRUE;
}

/* 把失效的条目重新绑到同应用的活动窗口上 */
static void guard_rebind_entries(struct context* ptr_ctx) {
    int i;
    EnterCriticalSection(&ptr_ctx->guard.lock);
    for (i = 0; i < ptr_ctx->guard.count; i++) {
        struct guard_entry* e = &ptr_ctx->guard.entries[i];
        DWORD pid = 0;

        if (e->hwnd != NULL && IsWindow(e->hwnd)) {
            GetWindowThreadProcessId(e->hwnd, &pid);
            if (pid != 0) { e->pid = pid; continue; }
        }
        e->hwnd = NULL;
        e->pid = 0;
        if (e->exe[0] != L'\0') {
            struct guard_find_ctx f;
            f.exe = e->exe;
            f.found = NULL;
            f.pid = 0;
            EnumWindows(guard_find_enum, (LPARAM)&f);
            if (f.found != NULL) {
                e->hwnd = f.found;
                e->pid = f.pid;
            }
        }
    }
    LeaveCriticalSection(&ptr_ctx->guard.lock);
}

/* ------------------------------------------------------------------ */
/* 对账主体                                                            */
/* ------------------------------------------------------------------ */

struct guard_walk_ctx {
    struct context* ptr_ctx;
    DWORD want;
};

static BOOL CALLBACK guard_global_enum(HWND hwnd, LPARAM lparam) {
    struct guard_walk_ctx* w = (struct guard_walk_ctx*)lparam;
    DWORD pid = 0;

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (guard_is_system_window(w->ptr_ctx, hwnd)) return TRUE;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;
    if (is_unsafe_process_pid(pid)) return TRUE;    /* explorer / dwm / cmd / conhost 绝不碰 */
    guard_request_pid(w->ptr_ctx, pid, hwnd, w->want, false);
    return TRUE;
}

void guard_tick(struct context* ptr_ctx) {
    DWORD want;
    DWORD now;

    if (ptr_ctx == NULL || !ptr_ctx->guard.armed) return;
    InterlockedExchange(&ptr_ctx->guard.dirty, 0);
    want = guard_affinity(ptr_ctx);
    now = GetTickCount();

    if (CHECK_BIT(ptr_ctx->panel_ctx.bitwise, g_f_c) == 0) {
        /* 全局模式：所有可见顶层窗口 => 按进程下发 */
        struct guard_walk_ctx w;
        w.ptr_ctx = ptr_ctx;
        w.want = want;
        EnumWindows(guard_global_enum, (LPARAM)&w);
    }
    else {
        /* 白名单模式：先按映像名重绑，再按进程下发 */
        int i, n;
        HWND snapshot[GUARD_MAX_ENTRIES];
        guard_rebind_entries(ptr_ctx);
        EnterCriticalSection(&ptr_ctx->guard.lock);
        n = ptr_ctx->guard.count;
        if (n > GUARD_MAX_ENTRIES) n = GUARD_MAX_ENTRIES;
        for (i = 0; i < n; i++) snapshot[i] = ptr_ctx->guard.entries[i].hwnd;
        LeaveCriticalSection(&ptr_ctx->guard.lock);
        for (i = 0; i < n; i++) {
            DWORD pid = 0;
            if (snapshot[i] == NULL || !IsWindow(snapshot[i])) continue;
            GetWindowThreadProcessId(snapshot[i], &pid);
            if (pid == 0 || is_unsafe_process_pid(pid)) continue;
            guard_request_pid(ptr_ctx, pid, snapshot[i], want, false);
        }
    }

    /* 全量重发：宿主这一侧的"保险丝"，载荷守护线程没了也能补回来 */
    if ((DWORD)(now - ptr_ctx->guard.last_force) >= GUARD_FORCE_MS) {
        int i;
        ptr_ctx->guard.last_force = now;
        EnterCriticalSection(&ptr_ctx->guard.lock);
        for (i = 0; i < GUARD_MAX_PIDS; i++) {
            struct guard_pid_state* s = &ptr_ctx->guard.pids[i];
            if (s->pid != 0 && s->hwnd != NULL) {
                s->ok = false;
                s->pending = 1;
            }
        }
        LeaveCriticalSection(&ptr_ctx->guard.lock);
        SetEvent(ptr_ctx->guard.wake);
    }
}

static void CALLBACK guard_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD tick) {
    (void)hwnd; (void)msg; (void)id; (void)tick;
    if (g_ptr_ctx != NULL) {
        guard_tick(g_ptr_ctx);
    }
}

void guard_kick(struct context* ptr_ctx) {
    if (ptr_ctx == NULL || !ptr_ctx->guard.armed) return;
    /* 合并：一拍内只投递一次，WinEvent 再密也不会把队列塞爆 */
    if (InterlockedExchange(&ptr_ctx->guard.dirty, 1) == 0) {
        if (ptr_ctx->window_hwnd != NULL) {
            PostMessageW(ptr_ctx->window_hwnd, WM_GUARD_TICK, 0, 0);
        }
    }
}

bool guard_arm(struct context* ptr_ctx, bool on) {
    if (ptr_ctx == NULL) return false;
    if (on) {
        ptr_ctx->guard.armed = true;
        ptr_ctx->guard.last_force = 0;   /* 立刻走一次全量 */
        guard_tick(ptr_ctx);
        return true;
    }
    guard_remove_all(ptr_ctx);
    return true;
}

void guard_remove_all(struct context* ptr_ctx) {
    int i, n;
    if (ptr_ctx == NULL) return;
    /* 没开过就别去撤销：否则退出程序时会往所有进程里注入一遍 WDA_NONE */
    if (!ptr_ctx->guard.armed) {
        guard_clear_entries(ptr_ctx);
        return;
    }
    ptr_ctx->guard.armed = false;
    InterlockedExchange(&ptr_ctx->guard.dirty, 0);

    if (CHECK_BIT(ptr_ctx->panel_ctx.bitwise, g_f_c) == 0) {
        EnumWindows(enum_windows_proc_remove, (LPARAM)ptr_ctx);
    }
    else {
        n = guard_count(ptr_ctx);
        for (i = 0; i < n; i++) {
            HWND hwnd = guard_hwnd_at(ptr_ctx, i);
            if (hwnd != NULL && IsWindow(hwnd)) {
                remove_anti_capture(hwnd, ptr_ctx);
            }
        }
    }
    guard_clear_entries(ptr_ctx);
    EnterCriticalSection(&ptr_ctx->guard.lock);
    ZeroMemory(ptr_ctx->guard.pids, sizeof(ptr_ctx->guard.pids));
    LeaveCriticalSection(&ptr_ctx->guard.lock);
}

/* 等 worker 把当前这轮做完（"应用"按钮的进度语义） */
void guard_flush(struct context* ptr_ctx, DWORD timeout_ms) {
    if (ptr_ctx == NULL) return;
    guard_tick(ptr_ctx);
    SetEvent(ptr_ctx->guard.wake);
    WaitForSingleObject(ptr_ctx->guard.idle, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

bool guard_init(struct context* ptr_ctx) {
    if (ptr_ctx == NULL) return false;
    if (ptr_ctx->guard.worker != NULL) return true;

    InitializeCriticalSection(&ptr_ctx->guard.lock);
    ptr_ctx->guard.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    ptr_ctx->guard.idle = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (ptr_ctx->guard.wake == NULL || ptr_ctx->guard.idle == NULL) {
        return false;
    }
    ptr_ctx->guard.worker = CreateThread(NULL, 0, guard_worker_proc, ptr_ctx, 0, NULL);
    if (ptr_ctx->guard.worker == NULL) {
        return false;
    }
    ptr_ctx->guard.timer = SetTimer(NULL, GUARD_TIMER_ID, GUARD_TICK_MS, guard_timer_proc);
    return true;
}

void guard_shutdown(struct context* ptr_ctx) {
    if (ptr_ctx == NULL) return;
    InterlockedExchange(&ptr_ctx->guard.stop, 1);
    if (ptr_ctx->guard.timer != 0) {
        KillTimer(NULL, GUARD_TIMER_ID);
        ptr_ctx->guard.timer = 0;
    }
    if (ptr_ctx->guard.wake != NULL) SetEvent(ptr_ctx->guard.wake);
    if (ptr_ctx->guard.worker != NULL) {
        /* 注入最长可能卡在 WaitForSingleObject(10s)，给它足够时间自己收尾；
           真超时就不关句柄了：进程马上退出，泄漏比"关掉还在用的句柄"安全 */
        if (WaitForSingleObject(ptr_ctx->guard.worker, 15000) == WAIT_OBJECT_0) {
            CloseHandle(ptr_ctx->guard.worker);
            ptr_ctx->guard.worker = NULL;
            if (ptr_ctx->guard.wake != NULL) { CloseHandle(ptr_ctx->guard.wake); ptr_ctx->guard.wake = NULL; }
            if (ptr_ctx->guard.idle != NULL) { CloseHandle(ptr_ctx->guard.idle); ptr_ctx->guard.idle = NULL; }
        }
    }
    ptr_ctx->guard.armed = false;
}