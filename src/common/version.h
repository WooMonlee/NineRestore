#pragma once
// SysRecover 版本号 —— **唯一来源**（改这一个文件，下面各处自动跟随）：
//   • CLI `version` / `diag` 输出
//   • `_zjresy*.log` 的 software_version 字段（src/boot/task.cpp）
//   • GUI 标题栏副标题（CTitleLabelUI 用 SYSRECOVER_DISPLAY_W 初始化）
//   • dist/version.json（Makefile 调 tools/version.py 生成）
//   • 发布包文件名（tools/portable-pack.py 按渠道与发布号派生）
//   • 救援载荷自报版本 `<ESP>\EFI\ZJRESTORE\rescue-build.txt` / initramfs 内戳
//
// 规则（2026-11-07 用户定稿，详见 docs/37-版本命名规范.md）：
//   主版本.次版本 由人指定（当前 1.0）；
//   测试版：beta 号放**第四段**，每发一版 +1，第三位固定 0；
//           `1.0 beta.N` / 内部 `1.0.0.N` / 文件 `九转大还原1.0-beta.N.exe`
//   正式版：beta 归零，`1.0.0` / 内部 `1.0.0.0` / 文件 `九转大还原1.0.0.exe`
//   正式版之后修 bug：第三位递增，`1.0.1` / 内部 `1.0.1.0` / `九转大还原1.0.1.exe`
//
// 衔接关系：0.9.x 历史不改，`0.9.96 → 1.0 beta.1`（本线起点，2026-11-07）。
// 正式版落地时四段数字会从 `1.0.0.N` 回落到 `1.0.0.0`（仅影响属性页观感）；
// 版本判定按**文件名与渠道**，不靠数字大小比较。
#define SYSRECOVER_MAJOR 1
#define SYSRECOVER_MINOR 0
#define SYSRECOVER_PATCH 0  // 第三位：正式版之后修 bug 递增（beta 期间固定 0）
#define SYSRECOVER_BETA 6   // 第四位：beta 号；0 = 正式版

// 内部四段串（日志 / version.json / 救援载荷构建戳）：beta 1.0.0.2，正式 1.0.0.0
#define SYSRECOVER_VERSION "1.0.0.6"
// 界面显示串（也用于文件名，文件名不带空格）：beta `1.0 beta.2`，正式 `1.0.0`
#define SYSRECOVER_DISPLAY "1.0 beta.6"

// 宽串形态：把窄串字面量加上 L 前缀（两级宏，避免 ## 阻止展开）。
#define SYSRECOVER_WIDEN2(x) L##x
#define SYSRECOVER_WIDEN(x) SYSRECOVER_WIDEN2(x)
#define SYSRECOVER_VERSION_W SYSRECOVER_WIDEN(SYSRECOVER_VERSION)
#define SYSRECOVER_DISPLAY_W SYSRECOVER_WIDEN(SYSRECOVER_DISPLAY)
#define SYSRECOVER_CONTRACT_VERSION 1
// CLI 自检输出里的程序内名（ASCII，不含渠道/版本）。
#define SYSRECOVER_NAME "SysRecover"
// ── VERSIONINFO（exe 属性「详细信息」）用的品牌字段 ────────────────
// 只在这里写一份：tools/gen-versioninfo.py 生成 .rc 时从这里读，
// 公司名/版权沿用仓库 LICENSE（作者 DreamGrain，MIT），不要另编。
#define SYSRECOVER_PRODUCT   "九转还原"
#define SYSRECOVER_COMPANY   "DreamGrain"
#define SYSRECOVER_COPYRIGHT "Copyright (c) 2026 DreamGrain. MIT License"
