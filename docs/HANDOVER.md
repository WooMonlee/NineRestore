# 交接摘要（1.0.0-beta.1）

> 历史：本文件最初是 2026-11-05 夜"安全停机"的交接点；2026-11-06 已把剩余项做完，
> 下面改为**当前状态 + 仍未完成的实机事项**。

## 当前状态

- 分支 `main`；版本 `1.0.0-beta.1`（界面显示「1.0.0 测试版」，英文 `1.0.0 Beta`）。
- 本地标签 `v1.0.0-beta.1`（标注标签，**未推送** —— 仓库没有配置 remote，推到哪里待定）。
- 工作区干净；`dist/` 与 `dist/version.json` 都已是 beta.1（dist 不入库）。

## 已完成（2026-11-06）

1. **VERSIONINFO**：`tools/gen-versioninfo.py` 从 `src/common/version.h` 单源生成 `.rc`
   （产品名 `九转还原`、公司 `DreamGrain`、MIT 版权都集中在 version.h 的三个宏），
   x64/x86 各用本架构 `windres` 编译链接；覆盖 GUI/CLI/Check、三个启动器、
   `zj-ea-apply`、`zj-restore-boot`、两个单文件 stub。
   单文件是「stub + 追加载荷」，外层 exe 实测能读到完整字段。
2. **门禁**：`make check` 新增 `tools/check-versioninfo.py`（PowerShell 真读 VersionInfo，
   与 version.h 比对）；`check-dist` 也校验单文件外层与 bootfiles stub。
3. **打包**：`package` → `portable`（GUI/CLI 两个单文件）→ `sha256`（75 个文件）→ `dist-check` 全 PASS；
   旧 1.0.0 产物已清理。`D:\九转大还原1.0.0-beta.1.exe` 为最新单文件 GUI 版。
4. **标签**：`v1.0.0-rc1` 已删除，重打 `v1.0.0-beta.1`。
5. **文档**：CHANGELOG（beta 章节 + 版本策略 + 先打包再签名约束）、README 中英、
   `docs/33`（标题/正文改「1.0.0 测试版」，写明未签名与未实机验证）、`docs/26`、
   `docs/BUILD-REPORT` §9.3 + §10（双架构与单文件外层 VersionInfo 实测）。

## 仍未完成（需要实机 / 需要用户决定）

1. **界面肉眼确认**：本环境无法启动 GUI 截图。异机还原勾选框与「更多功能>>」菜单的
   实际观感、Tab 焦点顺序**需肉眼确认**。
2. **7 项实机确认**（清单见 `docs/BUILD-REPORT.md` §9.6）：救援层挑盘、keep 清空不完整、
   追加中途失败、`OK_BOOT_UNFIXED` 回执、USB 固定盘开关、线程上限、NVRAM 写保护。
3. **CI 首次真实运行**：workflow 的 i686 工具链下载与 PATH 逻辑仅本地推演。
4. **推送**：无 remote、无认证；推送到哪个仓库由用户决定。
5. 未来若做代码签名：必须**先打包、再签名**（签名后追加载荷会使签名失效）。

## 新会话接手第一件事

跑 `git status`（应为干净）与 `ARCH=x64 check`（应 PASS，含 versioninfo 断言）；
然后按上面"仍未完成"逐项推进，不需要再碰版本号或打包脚本。
