# 🦇 JiYuBlind

纯 C + 原生 Windows API 实现的「防截图 / 防录屏」工具：把**指定窗口或全部窗口**从截图、录屏、屏幕共享中抹掉，或者把抓到的画面**替换成一张预备好的图**，而你自己看到的屏幕完全不受影响。

单文件 exe、免运行库、点开即用。

---

## ✨ 功能

| 功能 | 说明 |
|---|---|
| **目标捕获（雷达）** | 按住控制面板上的雷达图标拖到任意窗口释放，自动识别并加入白名单 |
| **白名单 / 全局模式** | 只保护列表内窗口；或关闭白名单，强制处理除系统核心进程外的所有可见窗口 |
| **穿透模式** | 抓屏里该窗口**完全不显示**（`WDA_EXCLUDEFROMCAPTURE`） |
| **遮蔽模式** | 抓屏里该窗口显示为**纯黑块**（`WDA_MONITOR`） |
| **替换画面（实验）** | 抓屏里该窗口被替换成**预备图**；你的屏幕不受影响。托盘右键菜单开启，**默认关闭** |
| **老板键** | 全局热键一键隐藏/恢复受保护窗口 + 本程序界面 |
| **无边框自绘 UI** | `stb_image` 解 PNG + `UpdateLayeredWindow`，支持自定义背景图与缩放 |
| **配置持久化** | `JiYuBlind.ini`（自动兼容旧的 `NoMoreCapture.ini`） |

---

## ⚙️ 原理

### 为什么必须注入

`SetWindowDisplayAffinity` 有个硬性限制：**只能修改本进程的窗口**，跨进程调用直接 `ERROR_ACCESS_DENIED`（内核 `win32kfull.sys` 会校验窗口归属）。所以必须让「目标进程自己」执行这次调用。

本项目提供两条载体，**DLL 注入为主、shellcode 为兜底**：

1. **DLL 注入（主路径）**
   - exe 里以 RCDATA 内嵌两份载荷 `payload32.dll` / `payload64.dll`，运行时释放到 `%TEMP%`
   - 目标进程内 `LoadLibraryW` 加载 → 找到远端模块基址 → 解析 PE 导出表拿到 `JybApply` 的 RVA
   - `CreateRemoteThread` 调用 `JybApply({hwnd, affinity})` 完成任务
2. **shellcode（兜底）**
   - 解析目标进程 `user32.dll` 的导出表（自定义 hash 匹配），拼一段 16/40 字节的机器码
   - `VirtualAllocEx` + `WriteProcessMemory` + `CreateRemoteThread` 直接执行
   - 不落地文件，DLL 路径被拦（杀软、临时目录只读等）时自动回落

### 替换画面怎么做到的（实验功能）

1. 受保护窗口设 `WDA_EXCLUDEFROMCAPTURE` → 抓屏时它被剔除
2. 在它**正下方**贴一个**同尺寸**的替身窗口，内容是预备图里「该窗口所在位置」的那一块（`UpdateLayeredWindow` 用 `ptSrc` 直接切片，零额外拷贝）
3. 抓屏剔除上面的真窗口后，露出的就是替身窗口 → **抓到的画面 = 预备图**
4. 你自己的屏幕上，真窗口盖在替身上面，**看不到任何变化**；替身窗口鼠标穿透（`HTTRANSPARENT`），桌面图标照常可点

三条实测得到的硬约束（改动前务必留意）：

- 替身窗口**绝不能加 `WS_EX_TOPMOST`** —— 两个 topmost 相叠时，被遮挡的替身会被 DWM 剔除，抓屏里就变成黑块
- 替身必须用 `SetWindowPos(decoy, target, ...)` 紧贴目标**正下方**
- 同尺寸即可，**不需要外扩**，用户看不到任何边框

---

## 💻 架构支持（请先读这段）

| 运行版本 | 能处理的 64 位目标 | 能处理的 32 位目标 |
|---|---|---|
| **x64（推荐）** | ✅ | ✅ |
| x86 | ❌ 操作系统禁止 32 位进程向 64 位进程 `CreateRemoteThread` | ✅ |

即：**x64 版才是完整形态**。x86 版遇到 64 位目标会安全跳过（不会崩，只是不生效）。

实测矩阵（注入后从外部 `GetWindowDisplayAffinity` 读回）：

| 宿主 | 目标 | 结果 |
|---|---|---|
| x64 | x64 | `0x11` ✅ |
| x64 | x86 | `0x11` ✅ |
| x86 | x86 | `0x11` ✅ |
| x86 | x64 | 跳过，程序存活 ✅ |

---

## 🔨 编译

- **工具链**：Visual Studio 2026（工程固定 `PlatformToolset=v145`）。用 VS2022/2019 打开会报 `MSB8020`，需要重定目标或装 v145 生成工具。
- **架构**：`x64` 与 `Win32` 均为 `Release` 配置，静态 CRT（`/MT`），产物为单个 exe。
- **注入载荷会自动编译**：主工程里有 `BuildInjectedPayloads` 目标，构建时会自动按 Win32/x64 编 `payload/payload.vcxproj`，再把两份 DLL 复制成 `payload32.bin` / `payload64.bin` 供资源嵌入。**不需要手动先编 DLL**。

```powershell
$msb = "C:\Program Files\Microsoft Visual Studio\...\MSBuild\Current\Bin\MSBuild.exe"
& $msb "NoMoreCapture\NoMoreCapture.vcxproj" /p:Configuration=Release /p:Platform=x64 /m
& $msb "NoMoreCapture\NoMoreCapture.vcxproj" /p:Configuration=Release /p:Platform=Win32 /m
```

---

## 🚀 使用

1. 双击 `JiYuBlind.exe`，托盘出现图标
2. 右键托盘 → **显示主程序窗体** → 拖雷达到目标窗口，加入白名单 → **应用**
3. 想全局保护：控制面板里关掉白名单模式
4. 想用替换画面：右键托盘 → **实验：替换画面（Beta）**（会弹风险说明）

### 配置项（`JiYuBlind.ini`）

```ini
[Config]
WhiteList=1        ; 1=白名单模式，0=全局模式
Transparent=1      ; 1=穿透(WDA_EXCLUDEFROMCAPTURE)，0=黑块(WDA_MONITOR)
BossKey=0          ; 老板键开关
HotKeyVk=0         ; 老板键虚拟键码
HotKeyMods=0       ; 修饰键
Scale=100          ; 主界面背景缩放百分比
Background=        ; 自定义背景图路径
Mode=0             ; 0=原版行为，1=替换画面（实验，默认关闭）
DecoyImage=        ; 替换画面所用图片；留空=启动时截一张当前桌面
```

---

## ⚠️ 已知边界（不夸大）

**挡不住的**
- **Windows.Graphics.Capture（WGC）**：Xbox Game Bar、新版 Teams、部分浏览器共享走这条，`WDA_*` 对其单窗口捕获可能无效
- **硬件采集卡 / 手机拍屏 / 虚拟机直通**：完全无效
- **对方只抓单个窗口**（`PrintWindow`、WGC 单窗口项）：替换画面拿不到，退化为黑块/空白
- **独占全屏 Direct3D**：绕过 DWM 合成，保护无效
- **DWM 被关闭**（Win7 经典主题等）：`WDA_*` 不生效

**替换画面额外限制**
- 只覆盖走 `BitBlt` / `DXGI Desktop Duplication` 的抓屏路径
- 仅替换受保护窗口的区域，桌面与未保护窗口仍是真实画面

**风险**
- 进程注入行为会被部分 EDR / 杀软启发式拦截；被拦时 DLL 路径失败会回落到 shellcode
- 本项目会主动跳过 `explorer.exe`、`dwm.exe`、`cmd.exe`、`conhost.exe` 等系统进程，避免打崩桌面

---

## 👤 开发者

**Bad_Dream_UwU**

> **⚠️ 免责声明**
> 本项目仅供 C 语言系统编程、Windows 内部机制与逆向工程的学习、研究与交流使用。请遵守当地法律法规与你所在组织的制度，请勿用于考试作弊、规避企业监管或其他非法用途。开发者对因使用本软件造成的任何直接或间接后果不承担连带责任。
