#pragma once
// 首启清理的结果标记（%ProgramData%\SysRecover\boot-cleanup.done）与启动 cmd 契约。
//
// 0.9.96 的缺口：清理失败（NVRAM 只读/写满）时也写 "done"，而 Startup 里的
// SysRecover-RestoreBoot.cmd 只看“文件存在”就自删 —— 失败被静默吞掉：用户看不到
// 提示，下一次登录也不会重试。1.0 起：
//   * 标记必须带结果：ok=1（清理完成）/ ok=0（清理失败）；
//   * cmd 只在 ok=1 时自删；ok=0 或无标记时保留，下次登录重跑 helper；
//   * helper 失败时保留自身与 RunOnce，并弹一次可感知的提示。
// 判定串与 cmd 模板只在本头文件维护一份，ops_inplace.cpp 写入、
// restore_boot_main.cpp 产出标记、单测按同一常量校验，避免三处口径漂移。
namespace sysrecover {

inline constexpr char kCleanupMarkerOkText[] = "ok=1\r\n";
inline constexpr char kCleanupMarkerFailText[] = "ok=0\r\n";
// cmd 用 findstr /C:"ok=1" 判定；单测引用同一常量。
inline constexpr char kCleanupMarkerOkToken[] = "ok=1";

// 标记文本是否表示“清理已完成”。只认 ok=1；空文本/其它内容一律按未完成处理
//（fail-closed：宁可多跑一次清理，也不能把失败当成成功）。
inline bool CleanupMarkerSaysOk(const char* text) {
    if (!text) return false;
    for (const char* p = text; *p; ++p) {
        const char* a = p;
        const char* b = kCleanupMarkerOkToken;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == 0) return true;
    }
    return false;
}

// 写入新系统启动文件夹的 cmd。语义：
//   * 已有 ok=1 标记 → 直接自删（上一次已清理完）；
//   * 已有 ok=0 标记 → 保留文件，下次登录再试（本次已提示过）；
//   * 无标记 → 启动 helper；helper 完成后按标记决定自删/保留。
inline constexpr char kFirstBootCleanupCmd[] =
    "@echo off\r\n"
    "set \"D=%ProgramData%\\SysRecover\"\r\n"
    "if not exist \"%D%\\boot-cleanup.done\" goto run\r\n"
    "findstr /C:\"ok=1\" \"%D%\\boot-cleanup.done\" >nul 2>&1\r\n"
    "if errorlevel 1 goto :eof\r\n"
    "del \"%~f0\"\r\n"
    "goto :eof\r\n"
    ":run\r\n"
    "\"%D%\\zj-restore-boot.exe\"\r\n"
    "findstr /C:\"ok=1\" \"%D%\\boot-cleanup.done\" >nul 2>&1\r\n"
    "if errorlevel 1 goto :eof\r\n"
    "del \"%~f0\"\r\n";

}  // namespace sysrecover
