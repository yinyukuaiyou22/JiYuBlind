# JiYuBlind

用纯 C 和原生 Windows API 实现的防截图 / 防录屏工具。它把指定窗口（或者除系统进程外的所有窗口）从截图、录屏、屏幕共享里抹掉，也可以把抓到的画面替换成一张预备好的图片，而你自己屏幕上看到的一切完全不受影响。

单文件 exe，静态 CRT，免运行库，双击即用。

## 功能

- 目标捕获：按住控制面板上的雷达图标拖到目标窗口上释放，自动识别并加入白名单。
- 白名单模式：只保护列表里的窗口（按进程记账，同一程序新开的窗口也会被保护）。
- 全局模式：关闭白名单，强制处理除系统核心进程外的所有可见窗口。
- 穿透模式：抓屏里该窗口完全不显示（WDA_EXCLUDEFROMCAPTURE）。
- 遮蔽模式：抓屏里该窗口显示为纯黑块（WDA_MONITOR）。
- 替换画面（实验功能，默认关闭）：抓屏里该窗口被替换成预备图，你的屏幕不受影响。
- 老板键：全局热键一键隐藏 / 恢复受保护窗口和本程序界面。
- 自动对账：点过一次"应用"之后，窗口重建、程序重启、切全屏、改分辨率、锁屏解锁、远控接上或断开，保护都会自动补回。

## 原理

### 为什么必须注入

SetWindowDisplayAffinity 有一个硬性限制：只能修改本进程拥有的顶层窗口，跨进程调用直接失败。所以必须让目标进程自己执行这次调用。本项目用两条载体，DLL 注入为主、shellcode 兜底：

1. DLL 注入（主路径）。exe 以 RCDATA 内嵌 payload32.dll / payload64.dll，运行时释放到 %TEMP%\jyb_payload_<位数>_<pid>.dll（每个目标进程一份、名字固定）。目标进程内 LoadLibraryW 加载后，找到远端模块基址、解析 PE 导出表拿到 JybMaintain 的 RVA，用 CreateRemoteThread 调用 JybMaintain({hwnd, affinity, owner_pid})。
2. shellcode（兜底）。解析目标进程 user32.dll 的导出表（自定义 hash 匹配），拼一段 16 / 40 字节的机器码，VirtualAllocEx + WriteProcessMemory + CreateRemoteThread 直接执行，不落地文件。DLL 路径被拦（杀软、临时目录只读等）时自动回落。

### 为什么它不会用着用着就失效

显示亲和性是每个窗口一份的状态，存在内核里、跟着 HWND 走。实测（脚本见 diag/reset_probe.c，Windows 11 26200）：

| 触发动作 | 亲和性 |
|---|---|
| 同一 HWND 上隐藏再显示 | 保持 |
| 切全屏（改样式并铺满屏幕）、最小化再还原 | 保持 |
| 分辨率 1920x1080 改成 1280x720 再还原（远控接入的典型副作用） | 保持 |
| 置顶 / 取消置顶、被别的窗口压住 | 保持 |
| 窗口销毁后用新 HWND 重建（浏览器切 HTML5 全屏、GPU 进程重启、应用自己重建主窗口等） | 归零 |

也就是说，仅靠"窗口创建时注入一次"必然会出现：新窗口没人补、旧窗口换了句柄就永久漏。所以做了三层兜底：

1. 载荷内守护。JybMaintain 之后，载荷在目标进程里装一个 WinEvent 钩子（EVENT_OBJECT_CREATE / EVENT_OBJECT_SHOW），加 1 秒兜底扫描，把本进程所有可见顶层窗口的亲和性对齐。新建窗口第一次设置可能因为还没有 DWM 重定向表面而失败，50 毫秒后重试一次。实测新窗口 250 毫秒内补齐。
2. 宿主对账。guard.c 每 2 秒按当前模式算出应该被保护的进程集合，按进程下发（一个进程只注入一次，进程内的窗口由载荷自己守）。白名单按"进程 + 映像名"记账，窗口重建或进程重启后自动重绑。
3. 全量重发。每 60 秒把所有进程重发一遍，防止载荷里的守护线程意外死掉。显示模式变化、锁屏解锁、会话切换（WTSRegisterSessionNotification）也会立刻触发一次对账。

另外两点：所有阻塞的注入（CreateRemoteThread / WaitForSingleObject）都在 worker 线程，WinEvent 钩子只投递一次合并唤醒，不会再出现窗口一多就把 UI 线程堵住、钩子事件被丢的情况；载荷守护线程会等宿主进程句柄，宿主退出或崩溃时把窗口还原成 WDA_NONE，不会留下永远抓不到的窗口。

开销：目标进程内约 0.16% 单核，宿主对账接近 0。

### 替换画面怎么做到的（实验功能）

1. 受保护窗口设 WDA_EXCLUDEFROMCAPTURE，抓屏时它被剔除。
2. 在它正下方贴一个同尺寸的替身窗口，内容是预备图里该窗口所在位置的那一块（UpdateLayeredWindow 用 ptSrc 直接切片，零额外拷贝）。
3. 抓屏剔除上面的真窗口后，露出的就是替身窗口，抓到的画面等于预备图。
4. 你自己屏幕上，真窗口盖在替身上面，看不到变化；替身窗口鼠标穿透，桌面图标照常可点。

三条实测得到的硬约束，改动前务必留意：

- 替身窗口绝不能加 WS_EX_TOPMOST。两个 topmost 相叠时，被遮挡的替身会被 DWM 剔除，抓屏里变成黑块。
- 替身必须用 SetWindowPos(decoy, target, ...) 紧贴目标正下方。
- 同尺寸即可，不需要外扩，用户看不到任何边框。

## 架构支持

| 运行版本 | 处理 64 位目标 | 处理 32 位目标 |
|---|---|---|
| x64（推荐） | 可以 | 可以 |
| x86 | 操作系统禁止 32 位进程向 64 位进程 CreateRemoteThread，安全跳过 | 可以 |

x64 版才是完整形态。x86 版遇到 64 位目标不会崩，只是不生效。

实测矩阵（注入后从目标进程外部读回）：x64 宿主对 x64 / x86 目标，x86 宿主对 x86 目标，都返回 0x11；x86 宿主对 x64 目标被系统拒绝，程序存活。

## 编译

- 工具链：Visual Studio 2026，工程固定 PlatformToolset=v145。用 VS2022 / VS2019 打开会报 MSB8020，需要重定目标或装 v145 生成工具。
- 架构：x64 与 Win32 都是 Release 配置，静态 CRT（/MT），产物为单个 exe。
- 注入载荷自动编译：主工程里的 BuildInjectedPayloads 目标会先编 payload/payload.vcxproj 的 Win32 与 x64 两份，再把 DLL 复制成 payload32.bin / payload64.bin 供资源嵌入，不需要手动先编 DLL。

    msbuild NoMoreCapture\NoMoreCapture.vcxproj /p:Configuration=Release /p:Platform=x64 /m
    msbuild NoMoreCapture\NoMoreCapture.vcxproj /p:Configuration=Release /p:Platform=Win32 /m

产物：x64\Release\JiYuBlind.exe 与 Release\JiYuBlind.exe。

## 使用

1. 双击 JiYuBlind.exe，托盘出现图标。
2. 右键托盘，选择"显示主程序窗体"，按住雷达图标拖到目标窗口上释放，加入白名单，点"应用"。
3. 想全局保护：控制面板里关掉白名单模式，再点"应用"。
4. 想用替换画面：右键托盘，选择"实验：替换画面（Beta）"，会先弹风险说明。

### 配置项（JiYuBlind.ini，与 exe 同目录）

    [Config]
    WhiteList=1        ; 1=白名单模式，0=全局模式
    Transparent=1      ; 1=穿透(WDA_EXCLUDEFROMCAPTURE)，0=遮蔽(WDA_MONITOR)
    BossKey=0          ; 老板键开关
    HotKeyVk=0         ; 老板键虚拟键码
    HotKeyMods=0       ; 修饰键
    Scale=100          ; 主界面背景缩放百分比
    Background=        ; 自定义背景图路径
    Mode=0             ; 0=原版行为，1=替换画面（实验，默认关闭）
    DecoyImage=        ; 替换画面所用图片，留空表示启动时截一张当前桌面

### 自检与排查开关

- JYB_TEST_HWND=<十进制窗口句柄>：启动时把该窗口所属进程加入白名单并直接开启对账，便于无人值守验证。
- JYB_INJECT_LOG=1：写日志到 %TEMP%\jyb_inject_log.txt（注入步骤）和 %TEMP%\jyb_payload_log.txt（载荷守护线程）。

## 已知边界

挡不住的，按抓屏路径分而不是按软件品牌分：

- 镜像驱动或显示驱动层采集。AnyDesk、TeamViewer、向日葵、ToDesk 都可能挂镜像驱动或走自己的视频驱动，这类采集发生在合成进抓屏表面之前，WDA 管不到。被远控时画面是否被挡，取决于对方当前用的是 GDI / DXGI 还是镜像驱动。
- PrintWindow(PW_RENDERFULLCONTENT)。让窗口自己重绘一遍，等于绕开 DWM 的排除逻辑，部分版本上仍返回黑块，但不能依赖。
- 硬件采集卡、手机拍屏、虚拟机直通。
- 独占全屏 Direct3D，绕过 DWM 合成。
- DWM 未启用（Win7 经典主题等）时 WDA 不生效。
- per-pixel-alpha 的分层窗口（UpdateLayeredWindow）。SetWindowDisplayAffinity 会直接失败并返回 ERROR_NOT_ENOUGH_MEMORY，程序自己的主窗体就是这种窗口，所以它对自己用的是临时摘掉 WS_EX_LAYERED 的老技巧。
- Win11 上个别浏览器主窗口有公开案例报告 SetWindowDisplayAffinity 返回 8 导致设不上（本机 Win11 26200 实测 Edge 正常）。遇到时可开 JYB_INJECT_LOG 看失败步骤。

替换画面的额外限制：只覆盖走 BitBlt / DXGI Desktop Duplication 的抓屏路径；只替换受保护窗口的区域，桌面与未保护窗口仍是真实画面。

风险：进程注入行为可能被部分 EDR / 杀软启发式拦截，被拦时 DLL 路径失败会回落到 shellcode；程序会主动跳过 explorer.exe、dwm.exe、cmd.exe、conhost.exe 等系统进程，避免打崩桌面。

## 验证与证据

- diag/reset_probe.c：亲和性在各类事件后的存活矩阵。
- diag/recreate_target.c：模拟同进程销毁并重建窗口，在 +250ms / +1200ms 取样。
- diag/shot.ps1：GDI BitBlt 抓屏存 PNG 并取指定点颜色，用来验证抓到的画面里有没有这个窗口。
- docs/evidence：旧版窗口重建后失效、新版重建后仍被排除、真实 Edge 窗口被排除的抓屏对比。

## 来源与声明

开发者：Bad_Dream_UwU（原作者）

本项目基于 https://github.com/lilith-is-all-you-need/NoMoreCapture 修改而来

由银鱼分发，仅限个人使用，如有侵权请联系删除。

免责声明：本项目仅供 C 语言系统编程、Windows 内部机制与逆向工程的学习、研究与交流使用。请遵守当地法律法规与你所在组织的制度，请勿用于考试作弊、规避企业监管或其他非法用途。开发者对因使用本软件造成的任何直接或间接后果不承担连带责任。
