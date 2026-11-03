// payload.c —— 注入到目标进程的载荷 DLL
//
// 为什么要有它：显示亲和性 API 有同进程限制（跨进程直接 ERROR_ACCESS_DENIED），
// 所以必须让"目标进程自己"执行调用。两种载体：
//   1) 本 DLL：LoadLibrary 注入后由宿主用 CreateRemoteThread 调导出函数 JybApply
//   2) shellcode：不落地文件的兜底路径
//
// 编译成两份：payload32.dll(x86) / payload64.dll(x64)，分别用于 32/64 位目标。
// 导出名用 payload.def 固定为无修饰的 JybApply（否则 x86 会变成 _JybApply@8）。

#include <windows.h>

// CreateRemoteThread 只能直接传一个指针参数，所以参数必须打包成结构体，远端函数签名也必须是"单个指针"。
// 关键：hwnd 一律用 ULONGLONG（8 字节），保证 32/64 位下 affinity 都在偏移 8——
// 用 ULONG_PTR 会让 32 位布局把 affinity 读到偏移 4，等于读到句柄高 32 位（0），
// 结果变成 SetWindowDisplayAffinity(hwnd, WDA_NONE) 把保护关掉（踩过）。
typedef struct JYB_ARGS {
    ULONGLONG hwnd;
    DWORD     affinity;
} JYB_ARGS;

__declspec(dllexport) DWORD WINAPI JybApply(JYB_ARGS* args) {
    DWORD err;
    if (args == NULL) {
        return 0xE0000001u;
    }
    if (args->hwnd == 0) {
        return 0xE0000002u;
    }
    if (SetWindowDisplayAffinity((HWND)(ULONG_PTR)args->hwnd, args->affinity)) {
        return 1u;
    }
    /* 失败时把 Win32 错误码带回去，0xE0000003 = 有错误码但为 0（不该发生） */
    err = GetLastError();
    return err ? err : 0xE0000003u;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst;
    (void)reason;
    (void)reserved;
    return TRUE;
}
