#pragma once

/*
 * guard —— 亲和性对账/兜底模块
 *
 * 为什么需要它：SetWindowDisplayAffinity 是"每个窗口一份"的状态，只在设置的那一刻写一次。
 * 实测（.probe/reset_probe_result.txt）：同一 HWND 上隐藏/显示、切全屏、最小化还原、
 * 改分辨率都不会丢；**窗口销毁重建（新 HWND）必丢**。浏览器切 HTML5 全屏、GPU 进程重启、
 * 应用自己重建主窗口、被远控改了显示拓扑后应用重建窗口，都会走到"重建"这条路上。
 * 老实现只在 WinEvent 的 CREATE/SHOW 时刻注入一次，白名单还按 HWND 记账，
 * 于是新窗口既不在名单里、也没有任何机制回头补 => 表现为"隔一段时间失效一次"、
 * "全屏/多标签窗口被看到"。
 *
 * 本模块的分工：
 *   - 目标进程内的载荷自己守护（payload.c：每 200ms 对齐本进程所有顶层窗口）
 *   - 宿主这边只做"策略对账"：哪些进程该被保护、进程重启/换 HWND 后重新绑定，
 *     并且**每 2s 一拍、每 60s 全量重发一次**，宿主或载荷任一边挂掉都能自愈。
 */

struct context;

bool guard_init(struct context* ptr_ctx);
void guard_shutdown(struct context* ptr_ctx);

/* 对账控制 */
bool guard_arm(struct context* ptr_ctx, bool on);           /* 应用 / 撤销 */
void guard_kick(struct context* ptr_ctx);                   /* 有窗口事件了，尽快对账一次 */
void guard_tick(struct context* ptr_ctx);                   /* 定时器/消息驱动的对账主体 */
void guard_flush(struct context* ptr_ctx, DWORD timeout_ms);/* 等 worker 把当前这轮下发做完 */
void guard_remove_all(struct context* ptr_ctx);             /* 撤销所有保护（退出/取消应用） */

/* 白名单记账（按进程绑定：同进程的新窗口自动算作受保护） */
void guard_clear_entries(struct context* ptr_ctx);
bool guard_add_entry(struct context* ptr_ctx, HWND hwnd);
int  guard_count(struct context* ptr_ctx);
HWND guard_hwnd_at(struct context* ptr_ctx, int index);
bool guard_has_process(struct context* ptr_ctx, DWORD pid);