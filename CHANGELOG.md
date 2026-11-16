# Changelog

本项目的所有重要变更都会记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

## [1.1.0] - 2026-11-16

### 修复 —— "用着用着就失效"

三类症状（隔一段时间失效 / 浏览器全屏或多标签窗口被看到 / 被远控后失效）实测同源：
**亲和性只跟 HWND 走，窗口一重建就清零，而旧实现只在窗口创建事件里注入一次、白名单还按 HWND 记账。**

实测证据（`diag/reset_probe.c` + `diag/recreate_target.c`，Win11 26200.9711）：

| 动作 | 亲和性 |
|---|---|
| 隐藏→显示 / 切全屏 / 最小化还原 / 改分辨率再还原 / 置顶切换 | 保持不变 |
| 窗口销毁 + 同进程新 HWND 重建 | 归零（旧版此时开始裸奔） |

### 新增

- **载荷内守护线程**（`payload.c`）：`JybMaintain` 之后在目标进程里装 `EVENT_OBJECT_CREATE/SHOW` 钩子 + 1s 兜底扫描，
  把本进程所有可见顶层窗口的亲和性对齐；新建窗口首次设置失败（无 DWM 重定向表面）50ms 后重试。实测新窗口 250ms 内补齐。
- **宿主对账模块**（`guard.c` / `guard.h`）：2s 一拍按进程下发、15s 重试失败项、60s 全量重发；
  白名单改为**按进程 + 映像名**记账，窗口重建/进程重启后自动重绑。
- **远控/会话相关唤醒**：`WM_DISPLAYCHANGE` / `WM_SETTINGCHANGE` / `WM_WTSSESSION_CHANGE`（`WTSRegisterSessionNotification`）都会触发一次对账。
- **退出/崩溃收手**：载荷守护线程等宿主进程句柄，宿主消失即把窗口还原为 `WDA_NONE`，不留"永远抓不到"的窗口。
- **自检开关** `JYB_TEST_HWND` 升级：直接把该窗口所属进程加入白名单并开启对账，便于无人值守验证。

### 改动

- WinEvent 钩子不再在 UI 线程里同步注入（`EnumProcessModulesEx` + `CreateRemoteThread` + `Wait`），只投递一次合并唤醒；
  注入全部移到 guard 的 worker 线程 —— 修掉"窗口一多消息循环被堵、钩子事件被丢"的老问题。
- 载荷临时文件改为 `%TEMP%\jyb_payload_<位数>_<pid>.dll`（固定名 + 每进程一份）：旧实现每次注入换新文件名，
  配合新的对账定时器会把同一个 DLL 在目标进程里装第二份并堆积临时文件；现在命中已加载模块直接调用。
- 注入入口 `JybApply` → `JybMaintain({hwnd, affinity, owner_pid})`，`JybApply` 保留为兼容别名。

### 性能

- 目标进程内守护 ≈ **0.16% 单核**（事件驱动 + 1s 扫描）；宿主对账 ≈ **0%**；全局模式下同上。

### 证据脚本

- `diag/reset_probe.c` / `reset_probe_result.txt`：亲和性在各类事件后的存活矩阵
- `diag/recreate_target.c`：模拟"同进程销毁重建窗口"，并在 +250ms / +1200ms 取样
- `diag/shot.ps1`：GDI BitBlt 抓屏存 PNG + 取指定点颜色，用来验证"抓到的画面里有没有这个窗口"

## [1.0.0] - 2026-10-02

### 新增

- **配置持久化**：设置可保存到 exe 同目录下的 `NoMoreCapture.ini`，启动时自动恢复（白名单/全局、穿透/黑块、老板键与热键、背景图路径、缩放比例）。
- **自定义背景图**：支持在控制面板中选择任意 PNG/JPG/BMP/GIF/TGA 图片作为主窗体背景，路径无效时自动回退到内置背景。
- **自定义缩放比例**：主窗体背景支持百分比缩放（5%–1000%），使用 Catmull-Rom 锐利插值（`stb_image_resize2.h`），避免双线性缩放发糊。
- **背景预览窗口**：应用前可实时预览「自定义背景 + 缩放比例」的最终效果，点击任意位置关闭。
- **WinEvent 钩子**：全局模式下自动保护之后新创建/显示的窗口。

### 修复

- 修复 `CreateDialogParamW` 最后两个参数顺序/类型颠倒的问题（x86 下会出错）。
- 修复老板键 F12 判断逻辑与错误的 `break`，避免「应用」流程被静默跳过。
- 修复 `WM_DESTROY` 中 `UnregisterHotKey` 使用错误窗口句柄。
- 修复注入后 `WaitForSingleObject` 超时仍释放 Shellcode 内存，可能导致目标进程崩溃。
- 修复 `InitCommonControlsEx` 被注释，导致老板键热键控件无法创建。
- 修复 DIB `biSize` 取值错误、缺失的函数原型、指针类型不匹配等编译警告（构建做到 0 警告 0 错误）。