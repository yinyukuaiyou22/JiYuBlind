# Changelog

本项目的所有重要变更都会记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

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
