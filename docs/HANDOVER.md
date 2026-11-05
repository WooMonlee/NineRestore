# 交接摘要（2026-11-05 夜，安全停机点）

本文件只记「明天怎么接着干」，细节依据见 `docs/BUILD-REPORT.md` 与 `CHANGELOG.md`。

## 当前状态

- 分支 `main`，当前提交 **`ee7e77b`**
  （`ui(1.0.0-beta.1): 「打包日志」与「异机还原」位置对调；版本改测试版并支持预发布后缀`）。
- 工作区干净；`ARCH=x64 all` / `ARCH=x86 all` / `make check` 三项全绿（见下「今晚已验证」）。
- 本地标签仍是旧的 `v1.0.0-rc1`（未推送），**尚未**改名为 `v1.0.0-beta.1`。
- `dist/` 是打包产物（不入库）：`dist/version.json` 已手工同步为 `1.0.0-beta.1`，
  但 **`dist/` 里的 exe、SHA256SUMS、单文件包都还是 1.0.0 的旧产物**，必须重新打包。

## 今晚已验证（可复现）

```text
ARCH=x64 all   -> exit 0
ARCH=x86 all   -> exit 0
ARCH=x64 check -> exit 0
  80 cases, 636 checks, 0 failures        (单元测试)
  17 checks, 0 failures                   (tests/test_version.py)
  check-docs:  PASS  version=1.0.0-beta.1 contract_version=1
  check-i18n:  OK (860 keys, 860 lang entries)
  check-widths: OK (1 WARN：FormatLabel 既有告警，非本次引入)
  check-shell / check-config / check-layers / check-secrets: PASS
  test_zjswap: PASS (A fail-closed/rollback, B happy path, C marker gate, D post-verify)
```

## 已改（本提交内）

1. 界面位置对调：`skin/main.xml` 的 `Universal` 改为第三步常驻勾选框
   （`pos="340,298,438,331"`，文案「异机还原」，`shortcut="U"`）；
   `LogBtn` 加 `visible="false"`，入口移到「更多功能>>」菜单第一项。
   `src/gui/main_form.cpp`：菜单项 1 改为 `Tr(L"打包日志")`，命令 1 走 `ExportSupportFlow()`；
   还原模式 `Vis(Universal,true)` / `Pos(Universal,340,298,438,331)`；备份模式仍隐藏。
2. Tab 焦点顺序：`skin/main.xml` 中 `BootMenuBtn` 声明提前到 `MoreBtn` 之前，
   与视觉从左到右（静默 → 异机还原 → 安装到启动 → 更多功能）一致。
3. 版本：`src/common/version.h` → `1.0.0-beta.1`；
   `src/gui/ui_skin.cpp::VersionDisplay()` 派生显示名「1.0.0 测试版」（英文 Beta）；
   `tools/version.py`、`tools/check-docs.py` 接受 SemVer 预发布后缀；`tests/test_version.py` +4 断言。
4. i18n：删死键 `使用异机还原` / `已关闭异机还原` / `已启用异机还原（…）` / `异机`；
   `异机还原=Universal`（受 98px 槽位宽度门禁限制，英文用短式，细节见工具提示）；
   新增 `测试版=Beta`；`lang/en.lang` 由 `tools/i18n-en.py` 重新生成。

## 未改（明天全部要做）

1. **VERSIONINFO 资源**（用户明确要求，今晚未动）：
   - 由 `src/common/version.h` 生成的临时 `.rc`（脚本），`windres` 按 x64/x86 分别编译、分别链接；
     字段：`FileDescription` / `FileVersion` / `ProductName`(九转还原) / `ProductVersion` /
     `CompanyName`(DreamGrain，取自 `LICENSE`) / `LegalCopyright`(MIT) /
     `OriginalFilename` / `InternalName`。
   - 覆盖清单：`dist/SysRecoverUI.exe`、`dist/x64/SysRecoverUI.exe`、`dist/SysRecover.exe`、
     `dist/x64/SysRecover.exe`、`dist/SysRecoverCheck*.exe`、三个根启动器、`zj-ea-apply.exe`、
     `zj-restore-boot.exe`、`build/portable_stub*.exe`（逐个在 Makefile 里确认）。
   - 单文件：**外层 stub 必须有 VERSIONINFO**（右键看到的就是这一层），内层载荷保持一致；
     顺序必须是「先给 stub 加资源 → 再追加载荷」，并在 `check-dist` 里读外层 exe 的 `VersionInfo`。
   - `make check` 加 PowerShell 断言：`(Get-Item exe).VersionInfo.ProductVersion/FileVersion`
     与 `version.h` 一致，缺失即失败。
   - 文档补一条约束：**先打包、再签名**（签名后再追加数据会使签名失效）。
2. `make package` → `make sha256` → `make dist-check`（含新增的单文件 VersionInfo 断言），重算 `SHA256SUMS`。
3. 标签：删本地 `v1.0.0-rc1`，重打 `v1.0.0-beta.1`（未推送，不需要 force push）。
4. 文档同步：`CHANGELOG.md`（`1.0.0-rc1` → `1.0.0-beta.1`，说明「测试版」含义）、
   `docs/33-已知问题与限制（1.0）.md`（标题/正文改「1.0.0 测试版」，明确未签名与未实机验证）、
   `README.md` / `README.en.md`、`docs/26-公开前准备与1.0观察期.md`、`PLAN.md`（预发布后缀规则）。
5. 界面肉眼确认：本环境无法启动 GUI 截图，**界面顺序需肉眼确认**。

## 明天第一件事

按顺序：① 写 `tools/gen-versioninfo.py` + Makefile 接线（先只做 GUI/CLI/Check，跑通 windres 与 PowerShell 断言）；
② 再接启动器与单文件 stub；③ `make package` + `make sha256` + `make dist-check`；④ 重打标签；⑤ 文档同步。

## 需要用户确认

1. **英文文案**：`异机还原` 的英文最终用 `Universal`（`Universal restore` 实测 111px，超出 98px 槽位）；
   如需完整英文，得把按钮槽位加宽或改两行，请确认取舍。
2. **CompanyName / LegalCopyright**：拟用 `DreamGrain` + `Copyright (c) 2026 DreamGrain. MIT License`
   （取自仓库 `LICENSE`）；若要用别的公司名请先给值，不要我猜。
3. **标签命名**：`v1.0.0-beta.1`（点分后缀，与内部串一致）；若要 `v1.0.0-beta1` 请说明。
4. **推送**：仓库仍无 remote/认证，本次不推送；何时推送待定。
