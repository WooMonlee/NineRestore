# 更新日志

> 版本号唯一来源是 `src/common/version.h`；发布时 `version.h` / git tag / `dist/version.json` 三处同源。
> 命名规则见 `docs/37-版本命名规范.md`（测试版 `1.0 beta.N` / 内部 `1.0.0.N` / 文件 `九转大还原1.0-beta.N.exe`）。

## 1.0 beta.8（2026-11-08，内部 `1.0.0.8`）

> 测试版（Beta）。beta.7 的 4 项复验全通过；本版处理测试台提的两条"用户会碰到"的
> 体验/一致性问题，并改 VMware 虚拟机的进固件提示。

### 1. returnold 的 software_dir 在目标卷上，软件目录回执必然写不到（中）

- 根因：`ops_returnold.cpp` 只判 `fixedDisk` 就把 `software_dir` 设成 `exeDir`；
  程序放在目标盘（最常见的 C:）时，该目录随还原/回退消失（apply 会格式化，
  returnold 的 `L5` 会把根下所有条目搬进 `~new` 再删除），救援层 `softscan`
  又只扫非目标卷 → 两条 `dir=no`，两处回执只有兜底窝一处生效。
- 修复：与 `ops_staged` 同规则——程序在目标盘时 `software_dir` 落到
  `<数据盘>\ZJRESTORE`，救援层能按非目标卷找到，回执两处都真实生效。
  找不到数据盘（单盘整盘为目标）时保留 `exeDir` 兜底并记
  `software dir unusable for logs (on target/removable)`。
- 评估过"换盘前只读挂目标卷解析软件目录"：**不采用**。目录解析得出来也活不过
  交换（`L5` 整树搬家后 `L7` 删除），只读挂载只是多一次挂/卸（NTFS 脏卷还可能
  RO 挂失败），收益为零、风险非零。理由写入 `docs/25` PIT-227。

### 2. 兜底日志窝落点不稳定（中）

- 根因：`claim_bb_home` / 晚到兜底原来按 `list_parts` **设备枚举顺序**抓第一个能挂
  的分区：1.0.0.7 同版本先后落到 `C:\ZJRESTORE-logs`、2GB 小盘、Data 卷。
- 修复：新增 `pick_bb_home_devs()`，规则固定且与枚举顺序无关：
  跳过目标/循环/内存/光驱/软驱/已挂载 → **容量 < 2GiB 不认领** → **内置优先于
  可移动**（`/sys/class/block/<disk>/removable`）→ 容量降序 → 设备名升序。
  结论 = "内置盘中最大的可用分区"；`claim_bb_home` 与晚到 `bb_home_drop` 共用。
  Windows 侧收集器本来就遍历 C..Z（`CollectDeployArtifacts`），不依赖固定盘符。
- 新增 `tests/test_log_home_rule.sh`（7 条，已进 `make check`）：内置优先、容量降序、
  同容量设备名升序、跳过目标、拒绝 2GB 小盘、两处共用、staged/returnold 同规则。

### 3. VMware 虚拟机进固件的提示（体验）

- `common/bioskey` 新增 `IsVmwareGuest()` / `LooksLikeVmware()`（只读注册表
  `HARDWARE\DESCRIPTION\System\BIOS` 的 SMBIOS 标识，含 "VMWARE" 即判定；纯函数有单测）。
- VMware 客户机没有"开机按键窗口"：所有 UEFI 分支的"开机按 <热键>"改为
  "关机，然后用「电源 → 打开电源时进入固件」选 SysRecover"：
  `main_form_bootmenu.cpp` 的已安装悬浮提示、安装确认、安装完成状态，
  以及 `main_form_notice.cpp` 的"还原菜单未执行"正确操作与失败兜底两句。
- 非 VMware 保持原有动态热键（`BootMenuKeyHint()`，不写死 F10/F12）。
- 中英文同步：`tools/i18n-en.py` 新增 5 条，`lang/en.lang` 由 `i18n-wrap.py --gen-lang`
  重新生成。

## 1.0 beta.7（2026-11-08，内部 `1.0.0.7`）

> 测试版（Beta）。修 beta.6 复验未过的 2 项（P1-b 陈旧契约 / P3a returnold 回执），
> 其余 5 项保持通过。

### 1. P1-b（高）陈旧契约被当成"有任务"

- 根因：救援层兜底搜索 `find "$ZJ_M" -maxdepth 4 -name restore-task.conf | head -1`
  只看**第一个命中**，不看来历。测试台留 `~old`（无暂存任务）时，先抓到
  `~old\restore-task.conf`、后抓到 `Program Files\SysRecover\logs\restore-task.conf`，
  根目录确认无 conf 仍被抓 → `_ran=1` → 无人值守 120s 自动选 `[2]`（应为 `[3]`）。
- 修复：新增 `conf_is_credible()`，候选逐个过可信度，只认"写者会写的位置"：
  - 目录里有 `SysRecover.exe` / `SysRecoverUI.exe`（程序目录），或目录名就是
    `ZJRESTORE`（exeDir 不可写时的备用写点）；
  - `action` 必须是 `restore` / `returnold`，且带 `target_offset` 或 `target_guid`；
  - 明确排除 `~old` / `~new` 交换残留与 `logs` 日志归档子目录；
  - 有主契约 `_zjresy*.log` 时，再核对 `target_offset`/`target_guid` 一致。
  搜索循环改为"收集候选 → 逐个过滤 → 第一个可信才 `cp`"；全部不可信时明确
  `ignored N conf candidate(s)`，不再静默拿陈旧副本当任务。
- 其它分支核查：`scan_for_log` 只在分区根匹配 `_zjresy*.log`（不递归），主契约 conf
  就在同一日志分区根；`~new` / `zjswap.sh` / `zjswap-undo.sh` 均把契约文件排除在
  交换外。无同类漏洞。
- 新增 `tests/test_conf_credible.sh`（12 条，已进 `make check`）：`~old`/`~new`/`logs`
  拒绝、程序目录/`ZJRESTORE` 接受、无锚点/非法 action/缺目标身份/身份不一致拒绝、
  以及"禁止退回 `head -1`"的静态断言。

### 2. P3a（中）returnold 成功无回执

- 根因：`zjrestore-lite.sh` 置 `RESULT=OK; STEP=done` 后只 `persist_log` 就 `exit 0`；
  EXIT trap 的 OK 分支只写软件目录 + ESP。BIOS 机器没有 ESP、软件目录又可能因挂载点
  变化写不到 → 状态只活在内存里；`init` 又因 `/tmp/zj-bb-final` 存在跳过兜底 →
  C:/D:/F: 根都没有 `ZJRESTORE-status.txt`。
- 修复：新增 `write_success_receipt()`（软件目录 `logs\` + 兜底窝 `ZJRESTORE-logs\`，
  两处都失败才 `WARN`），apply 成功与 returnold 成功共用同一 helper；returnold 成功后
  依次 `persist_log` → `write_success_receipt` → `bb_esp_drop` → `sync` → `exit 0`。
  `result=OK` / `step=done` 可被外部读到。
- 新增 `tests/test_success_receipt.sh`（5 条，已进 `make check`）：两处直写点、仅兜底窝、
  全失败 `WARN`、静态断言 returnold 成功块调用 helper、apply/returnold 共用同一实现。

## 1.0 beta.6（2026-11-08，内部 `1.0.0.6`）

> 测试版（Beta）。修测试台实测的 4 个缺陷（P1 无人值守死循环优先），并把打包/共享
> 改成"复制 + 原子替换"，保证历史版本不可变。

### 1. P1（高）「返回旧系统」无人值守死循环

- 根因：救援首屏「上次任务未确认完成」三选一在 `read` 超时后**自动选 [1]**（回滚）；
  回滚分支还要人工输大写 `YES` → 无人值守再次超时 → "未确认，返回菜单" → 重印菜单
  → 又自动 [1]，无限循环。已暂存的 `action=returnold` 反而被这个守卫挡在菜单外。
- 修复：
  - 读到 `restore-task.conf` 的 `action=returnold` 时 triage **直接放行**，沿既定动作继续；
  - 无人值守绝不自动选 [1]：有可续任务 → [2]，无任务 → [3]；
  - 人工选 [1] 但 `YES` 连续两次未确认 → 明确放弃（继续暂存任务或重启），不再循环；
  - `reboot_now` 两次 `reboot -f` 仍未生效 → 关机/停机并留结论，不返回菜单。
- 新增 `tests/test_rescue_triage.sh`（4 条，已进 `make check`）：抽取 `triage_previous()`
  本体 + stub 磁盘，钉住"returnold 放行 / 自动不选 [1] / 只重启一次 / YES 超时收场"。
- 行为变化：无人值守从"死循环"变为"沿暂存动作继续或重启一次"；有人值守路径不变。

### 2. P2（中）救援控制台中文不显示

- 根因：initramfs 没有 CJK 控制台字库，`say` 里的中文只剩方块/问号；首屏实测只剩
  `[1][2][3]`。
- 处置：**不塞字库**（ESP 寸土寸金，控制台字体链路也只在部分固件可靠），改为
  **屏幕消息纯 ASCII**（中文只留在 `.sh` 注释与落盘日志）。首屏、回滚确认、重启兜底、
  求助渠道全部改成可读英文。
- 新门禁 `check-shell.py` C8：救援脚本 `say "..."` 里出现 CJK 直接失败。

### 3. P3（中）状态文件与实际结果不一致

- 3a `returnold` 成功后 `ZJRESTORE-status.txt` 仍写 `result=FAILED/step=interrupted`：
  救援脚本 EXIT trap 已写好 `OK/done`，init 又因 `/tmp/zj-bb.done` 为空做了一次
  `zz-blackbox interrupted` 全盘扫，把状态覆盖。修复：脚本收尾写 `/tmp/zj-bb-final`
  （`RESULT != UNKNOWN` 才写），init 只在该标记不存在时兜底；每轮开跑前清标记。
- 3b `rmdir err=145` 记 `fail=1` 但界面报成功：`~old` 里只剩 `.zj-keepold` 时 rmdir
  必然 145；检查标记用的扩展路径多了一个反斜杠（`\\.zj-keepold`），标记删不掉 →
  计数留下；随后补删成功、目录消失，计数没有回落。修复路径并新增纯函数
  `NormalizeDeleteFail(fail, dirStillThere)`：目录消失 → `fail=0`，目录仍在 → `fail>=1`；
  单测 `swapleft_fail_counter_matches_disk`（5 条断言）。

### 4. P4（低，复核为合理约束）程序在目标分区上被拒

- 结论：**保留拒绝**。返回旧系统会把目标分区整棵新系统树（含程序所在目录）改名到
  `~new` 后丢弃；继续执行会把用户唯一的入口悄悄删掉。程序已先自动尝试 `SettleHome`
  复制到其他分区，失败才拒绝。
- 改动只在提示：写清"为什么会被拒 + 自动复制失败原因（`sr.detail`）"，并给出两条
  可行路径（复制到别的分区 / 从 U 盘或 PE 运行）。

### 5. 打包/共享：复制而非硬链接，历史版本不可变

- 根因：测试台把 `share\b3.exe` 做成指向 `dist\SysRecover-Portable.exe` 的硬链接；
  打包 `pack-self` 直接原地写同一输出文件 → 同一 inode 被截断重写，共享端旧版本
  （`1.0.0.3`）一起被改掉。
- 修复：`tools/portable-pack.py` 改为"打包到临时名 → `os.replace` 原子替换"，发布
  复制也走临时名 + 原子替换；绝不在已存在的输出上原地重写。同版本号撞名仍拒绝。
- 已把 `share\b3.exe` 用 `D:\九转大还原1.0-beta.3.exe` 重新复制还原（先断链再复制）。

### 门禁

`make check` 全绿：unit **109 cases / 753 checks**（新增 1 case / 5 checks）、
`test_version` 18、check-docs / i18n / widths / shell（含 C8）/ config / layers /
foreground / secrets / versioninfo、boot-region 17/0、zjswap A–D、
**test_rescue_triage 4 条**。x64 / x86 编译零告警。

## 1.0 beta.5（2026-11-08，内部 `1.0.0.5`）

> 测试版（Beta）。相对 beta.4 只做**代码质量三项**，产品行为不变：拆过大源文件、
> 统一错误语义（部分成功必须显式）、自有代码告警清零。

### 1. 过大源文件按职责拆分（纯搬移，行为不变）

| 原文件 | 拆分后 |
|---|---|
| `src/gui/main_form.cpp` 3616 行 | 8 个单元：窗口/消息泵、提示与事件路由、启动菜单、工具入口、分区与镜像、还原、备份、UI 状态（273–805 行/个） |
| `src/app/selfdiag.cpp` 1668 行 | `selfdiag.cpp`（诊断报告/采集）+ `selfdiag_cleanup.cpp`（菜单回执/散落日志清理/部署痕迹/EA 扫描） |
| `src/app/ops_staged.cpp` 2056 行 | `ops_staged.cpp`（暂存还原阶段）+ `ops_staged_menu.cpp`（菜单绑定契约/陈旧任务清理/一键重试） |
| `src/boot/uefi.cpp` 1638 行 | **不拆**：单一域，载荷部署集中在 `InstallUefiBootEntry` 内部，硬切只会扩大失败面（1.0 稳定优先） |

- 跨 TU 共用的 4 个小工具（`FormatGb` / `TimestampName` / `TimestampDate` / `StageCn`）
  改为 `CMainForm` 私有静态成员；其余单文件工具留在各自分段，不引入新的公共头。
- 门禁工具同步：`check-widths.py` 改为扫全部 `main_form*.cpp`（否则拆分后覆盖检查会静默变弱），
  `i18n-wrap.py` 白名单补 7 个新文件。

### 2. 错误语义：部分成功必须是显式状态

- `WimEngine::Capture` / `Append` 在 `skipFileErrors=true` 下跳过不可读文件后**不再返回 0**
  （0.9.x 会让脚本 `&&` 与"只看 rc"的调用方把不完整备份当完全成功），改为显式
  `kWimPartialSkipped = 1001`：镜像照常可用、非致命、不与 wimlib 错误码重叠。
- `RunBackup` 据此放行主镜像；ESP / EA 子镜像里的文件被跳过时不再把整个备份判失败，
  统一进 `skippedList` → GUI「备份不完整」弹窗、CLI 退出码 7（显式 `--allow-skipped` 才归 0）。
- 失败回滚逐处核对：`ops_preserve`（部分搬迁先回滚再失败）、`ops_returnold` / `ops_staged`
  （契约回滚守卫）、`ops_backup`（ESP 失败有意不回滚主镜像，注释已注明 A 方案语义）——
  未发现"失败仍返回成功且不留痕迹"的路径。
- 新增单测 `wim_partial_skipped_is_explicit_nonzero`，钉住"部分成功非零、且不与对外退出码撞车"。

### 3. 告警：自有代码全部 `-Werror`

- 核实所有自研编译规则（app / cli / gui / launcher / bootfiles / 单测 / 便携包 stub）均带
  `-Wall -Wextra -Werror`；第三方 duilib 保持 `-w`，GUI 的 `-fpermissive` 只放宽 duilib 头解析，
  已在 Makefile 注明"不抑制告警、自研代码仍告警即失败"。
- x64 / x86 两条链编译零告警。

### 门禁

`make check` 全绿：unit **108 cases / 748 checks**（新增 1 case / 5 checks）、`test_version` 18、
check-docs / i18n / widths / shell / config / layers / foreground / secrets / versioninfo、
boot-region 17/0、zjswap A–D。

## 1.0 beta.4（2026-11-08，内部 `1.0.0.4`）

> 测试版（Beta）。相对 beta.3 的改动**只在测试台脚本与文档**，产品代码零改动；
> 出包是为了让"发布号 → 包"一一对应（同号不覆盖）。

### 测试台 `mc.bat`：从"静默退出"改成"会明确失败"

- **问题**：guest 里跑 `mc.bat`，窗口一片空白、几十秒不动，看着像卡死。查清是三件事叠加：
  ① `@echo off` + 每行输出都重定向进 `C:\mc.txt`，控制台按设计就没有任何输出（实测 0 字节）；
  ② 上传用 `-InFile 'C:\mc.txt'`，读的正是 cmd 用 `>>` 占着写句柄的同一个文件 →
     `being used by another process`（连跑 5 次 5/5 命中），外面套裸 `catch{}`，失败被吞、退出码仍 0；
  ③ `Invoke-WebRequest` 没有 `-TimeoutSec`，收集端不可达时每次固定卡 21 秒（实测），四个调用就 84 秒黑窗。
- **修复**：`mc.bat` 收进仓库（`tools/vmtest/mc.bat`，原来只在测试台 share 里，改坏了没人发现）：
  控制台打进度；上传前先 `copy` 出快照再传（结构上避开共享冲突）；每个上传 `-TimeoutSec 10`；
  失败写 `PUT-FAIL: <label> : <原因>` 并 `exit 1`；轮次标签必填（缺则 `exit /b 2`）；
  退出码 0 全成功 / 1 有步骤失败 / 2 参数错误；`wmic` 缺失降级为 WARN（Win11 24H2 已移除）。
  文件保持纯 ASCII（UTF-8 中文 `rem` 行在代码页不匹配时会被 cmd 当成命令执行，实测）。
- 详见 `docs/25-PIT坑位册.md` PIT-218、`docs/34-发布门禁总表.md` §12。

## 1.0 beta.3（2026-11-07，内部 `1.0.0.3`）

> 测试版（Beta）。相对 beta.2 的核心改动：**Linux 救援侧 PBR 判据收紧到与 Windows 侧等价**。

### BIOS/MBR：保留原 NTFS 时不再"扇区 1 非零即完好"

- **问题**：`zjrestore-lite.sh` 在 `KEPT_FS=1`（保留原 NTFS）分支只看分区**扇区 1 是否有任意非零字节**，
  命中就报 `boot region kept intact`。即使 `0x54` 处的代码指纹已被写坏，仍会被判成完好
  → 屏幕"修好了"、实际起不来。
- **新判据（与 Windows 侧 `EnsureNtfsBootRegion` 等价）**：
  1. 分区 offset `0x54` 的 4 字节必须等于 `FA 33 C0 8E`（主判据）；
  2. 扇区 1 起续段非全零（保留，降级为辅助条件）；
  3. 任一不满足 → 用内置 `ntfs-boot-code.bin`（426 B @ `0x54`）+ `ntfs-boot-cont.bin`（4096 B @ 扇区 1）
     重写，并按现有逻辑同步卷末尾备份引导扇区。
- **日志三态**（不再出现含糊的 `kept intact` 单独成句）：
  `boot region ok (0x54=fa33c08e)` / `boot region rewritten` / `ERROR: boot region rewrite failed (0x54=...)`。
- **实现**：抽出纯 shell 函数 `zj_repair_boot_region <dev> <code> <cont> <total_sectors>`，
  由 `tests/test_boot_region.sh`（17 检查）覆盖：判据命中跳过且字节不变、`0x54` 错误重写、
  续段全零重写、缺内置 blob 时报错返回 1、给出总扇区数时末尾备份扇区同步、脚本内不得再出现 `kept intact`。
- 写入路径的 `dd` 去掉 `2>/dev/null`（B 类不消音），探测性读取保留（A 类），
  见 `docs/25-PIT坑位册.md` PIT-217。
- 救援载荷按 `1.0.0.3` 重新构建（`bootfiles/initramfs-zjrestore.cpio.gz` 17,408,065 B，
  内戳 `rescue build stamp: 1.0.0.3`），`version.json` / `rescue-build.txt` / initramfs 内戳三元一致。

## 1.0 beta.2（2026-11-07，内部 `1.0.0.2`）

> 测试版（Beta）。相对 beta.1 的核心改动：**ESP 救援载荷不再备份**。

### ESP 载荷：无备份、先删后写、失败摘启动项

- **取消写前备份**：不再把旧载荷整目录复制到 `EFI\ZJRESTORE\backup-<stamp>\`
  （每份约 17.4MB，ESP 寸土寸金）。旧固件本身即待替换对象，没有保留价值。
- **顺序固定**：检测（版本戳 + 文件大小快路径，不一致才深查 CRC）→ **删除旧载荷** →
  写入新载荷 → 校验字节数与 CRC32 → 进入启动/安装流程。
  先删旧再写新，避免 ESP 剩余空间不足以同时容纳两份载荷。
- **写失败 fail-closed**：旧载荷已删时写新失败，提示「救援载荷写入失败，请重试或更换介质。」，
  并**摘掉指向残骸的 UEFI 启动项**（不留 `Boot####` 让人撞）。纯逻辑单测
  `rescue_payload_deploy_order_is_delete_then_write` / `rescue_write_failure_removes_entry_after_old_payload_deleted` 锁定。
- **清理历史遗留备份**：启动时扫描 `EFI\ZJRESTORE\backup-*`，删除并记日志
  （`legacy payload backup removed: <name> (x.x MB)` / `cleanup freed x.x MB`），只删本程序创建的前缀，
  不碰用户文件、不碰 Secure Boot 文件、不删当前在用载荷。
- NVRAM 侧小文本（`Boot<num>.pre.bak`、`bootorder.pre.bak`、`firmware-boot.txt`）保留不动。
- **旧说法作废**：beta.1 记录里的「写前备份 `backup-pre-rewrite-<stamp>` 并被保护不被 stale 清理」
  是已废弃行为，对应单测 `rescue_backup_dir_survives_stale_cleanup` 已删除/重定义。

### 版本命名

- 第三位 = 正式版之后的修 bug 号（beta 期间固定 0）；beta 号在**第四段**；
  文件名 `九转大还原1.0-beta.N.exe`（不带空格），同版本不覆盖旧包；
  新增 `make release` 一步出包（递增 beta → 双架构构建 → 打包 → SHA256）。

## 1.0 beta.1（2026-11-06；旧写法 `1.0.0-beta.1`，内部 `1.0.0.1`）

> **这是测试版（Beta），不是正式版。** 二进制未做代码签名，也未在实机上跑通完整备份/还原闭环；
> 面向真实磁盘使用前请先在不重要的机器或虚拟机上验证。内部版本串 `1.0.0-beta.1`，
> 界面显示名「1.0.0 测试版」（英文界面显示 `1.0.0 Beta`）。

基线：SysRecover **0.9.96**（commit `8401cdab`）。本版是「九转还原」工程化重构 + P0/P1 整改，
不改变产品定位与磁盘格式（镜像仍是 WIM/ESD，加密镜像仍是 ZJENC1）。

### 未发布修正（2026-11-07，N17 实测）

- **ESP 写前备份目录被 stale 清理误删**（`src/boot/uefi.cpp` `InstallUefiBootEntry`）：
  旧载荷重写时刚建好的 `backup-pre-rewrite-<时间戳>` 被"定点清 stale"判为非本次文件集而删除
  （日志 `stale dir removed: backup-pre-rewrite-…`），备份形同虚设。现在 `backup-*` 不参与
  stale 清理，且只保留**最新一份**（更早的备份仍清理，避免多份 ~17MB 载荷塞满 ESP）；
  纯逻辑抽到 `rescue_guard.h::IsRescueBackupDirName` / `NewestRescueBackupName`，
  单测 `rescue_backup_dir_survives_stale_cleanup` 锁定。
- 0.9.93 旧载荷 → 新版接管实测记录、客户"扫描后安装"失败链条，见
  `docs/34-发布门禁总表.md` C.4。

### 未发布修正（2026-11-07，装菜单→重启→救援全链路实测）

- **图标**：改用用户定稿的 `NineRestore_小尺寸增强.ico`（16/24 小尺寸增强帧），
  覆盖 `resources/SysRecover.ico`，GUI/CLI/检查器/便携壳统一随构建嵌入；
  `ProductName` 仍为「九转还原」，未改品牌文案。
- **实测结论（写入门禁表 C.4 §7）**：`fb21851` 之后重造 0.9.93 旧载荷现场，再走
  「安装到启动 → 扫描后安装」：日志判 stale → 重写，`backup-pre-rewrite-20261107-205701` 落盘且
  **不再被 stale 清理**；ESP initramfs 17,407,298 B、SHA256 与 `bootfiles/` 一致。
  同一轮重启进救援（VMware UEFI，`bootsequence` 免热键）：
  `entry-started.txt` = `entry=menu` / `build=1.0.0-beta.1`，
  `ZJRESTORE-status.txt` = `result=OK` / `step=done` / `entry=menu` / `build=1.0.0-beta.1`。
- **未判定**：首启三选一（N20）的屏幕行为本轮未取证（还原后系统来自 ESD、无 VMware Tools，
  无法截图送键），见 C.4 §8。

### 数据安全（P0）

- **H1 暂存交换 fail-closed**：`zjswap.sh` 任一 `move` 失败即非零退出（退出码 4），
  回滚已搬走的旧项、保留 `~new`、不留 `~old`、新树不泄漏到根；交换后校验新树关键文件（缺 `winload` → 5，保留现场）。
- **H2 引导修复降级**：救援层修引导失败不再报 `result=OK`，改为 `result=OK_BOOT_UNFIXED`；
  Windows 侧判定抽成纯函数 `RescueStatusIsOk`，`OK_BOOT_UNFIXED` / `FAILED` / 带前缀一律不当成功，并加单测。
- **H3 旧系统保留失败回滚**：`PreserveOldSystem` 部分移动失败时先回滚再返回失败，不再留下"半旧系统"。
- **H4 校验契约**：格式化目标分区前强制执行**毫秒级快速结构预检**；全量校验改为显式入口
  （GUI 勾选 / CLI `--verify-image`，默认关），跳过时在日志与结果页写明"镜像未校验"；
  旧系统兜底保留在 `~old`，可回退。
- **H5 空间与残留**（原"临时文件替换"建议经复核撤回）：追加前按"临时文件≈镜像大小"做空间预检；
  写入失败后清理本轮残留临时文件，不采用 `.bak` 双份替换。
- **H6 救援侧启动项清理**：按 marker 精确删除 `Boot####`，从 `bootorder.bak` 恢复 BootOrder，
  全部成功才删除标记（失败保留标记等下次重试）。
- **H7 单实例锁**：暂存还原入口改用 RAII 操作锁，移镜像、解压、格式化、写引导全部纳入锁内，
  避免并发实例互踩。
- **H8 救援层缺字段 fail-closed**：`zjrestore-lite.sh` 缺 `image_index` 不再默认 1；
  只有 `_zjresy*.log`、没有 `restore-task.conf` 时不再"接着跑"；`target_offset` /
  `target_size` / `pt_type` / `repair_boot` / `keep_old` 缺一即失败
  （`STEP=no-image-index` / `no-contract` / `contract-incomplete`）。
  **语义变更**：契约不全时由「静默推测默认值继续」改为「显式失败、不碰目标分区」
  （黑匣子模式除外：它只收集日志、不执行写入）。

### 备份 / 还原可靠性（P1）

- **M1 子镜像配对**：ESP / ZJEA 子镜像按"大于系统索引的最近一个"配对，遇到下一个系统镜像即停止，
  不再把别的系统镜像错配上。
- **M2 / M3 / M4 keep 模式与清理范围**：keep 模式清空不完整 → 在任何写入前中止
  （`result=FAILED` + `step=wipe-incomplete`）；目标卷挂不上 → `wipe-not-mountable`，不回退去格式化；
  引导文件清理只删带本工具特征（`$ZJRDIR` / `grldr.mbr` 含 `ZJRESTORE`）的文件；
  ESP 优先同盘，无法判断时仅接受唯一候选，否则拒绝。
- **M5 救援层身份校验**：候选目标盘同时校验磁盘序列号与分区 GUID（来自 `restore-task.conf`），
  不匹配则跳过并报错，降低挑错盘风险。
- **M6 镜像不静默覆盖**：搬移/落盘不再用 `MOVEFILE_REPLACE_EXISTING`；同名文件自动改名 `name_1.wim`。
- **M7 备份前预检**：FAT32 单文件 4GB 上限与目标空间预检，不足时以 `ADV_SPACE` 明确失败。
- **M8 格式化可靠性**：先探测 `diskpart` 可用性，缺失时回退到内置流程；脚本改为按卷选择，
  失败返回明确错误码。
- **M9 长路径**：清单声明 `longPathAware`，`DeleteTreeRec` 等全程使用 `\\?\` 长路径前缀（幂等，支持 UNC）。
- **M10 加密备份明文清扫**：持单实例锁后清理上次崩溃遗留的 `dest.plain.tmp` / `.stage` 文件。
- **M11 备份完整性退出码**：有文件读不到时 CLI 返回 **7**（备份不完整）；显式 `--allow-skipped` 才归 0。
- **M12 磁盘枚举**：默认仍隐藏 USB；USB 固定盘可用菜单「显示 USB 设备（谨慎选择）」显式打开，
  列表标注"（USB 设备）"；磁盘上限 16→128；`ERROR_MORE_DATA` 时倍增 extent 缓冲重试（上限 64）。
- **M13 提权工作目录**：UAC 提权后保留调用方工作目录，相对路径参数不再失效。
- **M14 契约/日志写读**：改 `std::string` 拼接，去掉 8192 截断风险；写后回读校验
  `contract_version` / `keep_old` / `target_guid` / `target_disk_serial`。
- **M15 ESP 暂存失败**：加密镜像的 ESP 子镜像暂存失败时返回错误码 5，不再只告警继续。

### 修正（L 系列）

- **L1**：12 处把翻译串当 `snprintf` 格式串的写法改为固定格式串 + 数值单独格式化；
  门禁新增"译文占位符必须与键一致"检查。
- **L4**：修正 `SysRecover.manifest` 注释与实现不一致。
- **L5**：UEFI 启动项删除改为"描述 + 设备路径"双条件匹配，避免误删同名条目；
  写 NVRAM（`WriteVar` / `DeleteVar` / `BootNext`）失败时返回非零，不再假装清理成功。
- **L6**：wimlib 线程上限 8，避免高核数机器在 PE 下峰值内存过高；实际线程数写入日志。
- **L7**：删除 `formatTarget` 分支内恒为 false 的 `oldPreserved` 回滚死代码。
- **L3**：`ops.cpp`（4705 行单体）已拆为 13 个 `ops_*.cpp`（阶段 2，函数集合 67/67 一致，行为不变）。

### 界面 / 版本呈现（beta.1 新增）

- 「**异机还原**」改为还原模式第三步的常驻勾选框（原「使用异机还原」藏在下拉菜单里）；
  「**打包日志**」移到「更多功能>>」菜单第一项（原为常驻按钮）。控件声明顺序与视觉从左到右
  一致，Tab 焦点不走样；菜单命令用常量 id，不依赖索引。
- 英文文案同步：`异机还原=Universal`、`打包日志=Package log`、新增 `测试版=Beta`；
  删除 4 个随改动失效的旧键（`使用异机还原` / `已关闭异机还原` / `已启用异机还原（…）` / `异机`）。
- **「重启进/出安全模式」保留为「一次性 + 可恢复」**（2026-11-06 修正误删）：该入口在
  「打包日志」完成后的结果框里，不是独立功能页。选择后先提示 BitLocker 恢复密钥与双系统
  `{current}` 可能指向另一系统；设置 safeboot 前会武装「下一次登录自动清除」，武装失败即
  拒绝设置（宁可不进，也不把机器锁在安全模式）；启动时若发现 safeboot 仍被置位，会提示并
  可一键退出并重启。此语义正常，不应被描述为「删除安全模式功能」。
- 版本显示名由 `src/gui/ui_skin.cpp::VersionDisplay()` 从 `version.h` 派生：
  `1.0.0-beta.N` → 「1.0.0 测试版」，`1.0.0-rc.N` → 「1.0.0 RC」，正式版原样；不另写第二份数字。
- **Windows VERSIONINFO**：`tools/gen-versioninfo.py` 从 `version.h` 生成 `.rc`（公司名/版权
  集中在 `SYSRECOVER_COMPANY` / `SYSRECOVER_COPYRIGHT` 两个宏），按架构用自己的 `windres`
  编译并链接。GUI/CLI/Check、三个启动器、`zj-ea-apply`、`zj-restore-boot`、两个单文件 stub
  全部带资源；单文件先给 stub 加资源、再追加载荷，所以外层 exe 在「属性 → 详细信息」里
  直接能看到 `九转还原` / `1.0.0-beta.1` / `DreamGrain` / MIT 版权。
- `make check` 与 `make dist-check` 都加了 PowerShell 断言（`(Get-Item exe).VersionInfo`
  必须与 `version.h` 同值），缺资源或不一致直接失败。
- `tools/version.py` / `tools/check-docs.py` 支持 SemVer 预发布后缀；`bump()` 会把
  `1.0.0-beta.1` 提升到 `1.0.1`（丢弃后缀）。

### 工程 / 发布

- **L2**：CI 增加 x86 32 位单测与冒烟、修正 x86 产物路径；新增 release job：
  双工具链 `package` → `sha256` → `dist-check`，上传完整 `dist`（含 `SHA256SUMS.txt`）。
- `make check` 现为 **80 用例 / 636 断言**，含 H1 交换回归（`tests/test_zjswap.sh`）、
  回执判定、WIM 结构预检等纯逻辑测试。
- 双架构构建（GCC 14.2.0 MinGW-w64 x64/x86）与全部门禁（docs/i18n/宽度/shell/config/layers/secrets）通过。

### 文档

- 新增 `docs/ARCHITECTURE.md`（拆分表、模块边界、构建与门禁实操）。
- 新增 `docs/BUILD-REPORT.md`（阶段 1~4 实测证据；含 H1 "改前失败 / 改后通过"）。
- `docs/07-测试矩阵与回归记录.md` 增补 1.0 回归项（追加中途失败、keep 清空不完整中止）。
- 新增 `docs/33-已知问题与限制（1.0）.md`。

## 版本策略（1.0，2026-11-07 定稿）

完整规则见 `docs/37-版本命名规范.md`。摘要：

| 阶段 | 文件名 | 界面显示 | 内部四段 |
|---|---|---|---|
| 测试版 | `九转大还原1.0-beta.N.exe` | `1.0 beta.N` | `1.0.0.N` |
| 正式版 | `九转大还原1.0.0.exe` | `1.0.0` | `1.0.0.0` |
| 正式版之后 | `九转大还原1.0.1.exe` 起 | `1.0.1` | `1.0.1.0` |

- 主/次版本由人指定；第三位只用于**正式版之后**的修 bug 递增（beta 期间固定 0）；
  beta 号在**第四段**，每发一版 +1，正式版归零。
- 文件名不带空格；beta 只是渠道标记，不进数字段。
- 同版本号不覆盖旧包（`tools/portable-pack.py` 存在即拒绝），`make release` 一步出包。
- 衔接：`0.9.96 → 1.0 beta.1`；已打出的旧包 `D:\九转大还原1.0.0-beta.1.exe` 记为
  `1.0 beta.1`，只规范文件名，不重新构建（其内部版本资源串仍是旧写法 `1.0.0-beta.1`）。
- **已知副作用**：正式版四段 `1.0.0.0` 小于最后一个 beta（如 `1.0.0.8`），属性页看似"变旧"；
  仅影响观感，产品无自动更新，版本判定按文件名与渠道字段，不靠数字大小。
- 跨层契约版本单独计（`SYSRECOVER_CONTRACT_VERSION`），只有改字段才递增。
- 发布物：`dist/` 目录版 + `dist/SHA256SUMS.txt`；单文件版由 `make portable` 生成并发布到 `D:\`。
- **签名顺序约束**：若以后做代码签名，必须**先打包、再签名** —— 单文件是"stub + 追加载荷"，
  签名之后再往 PE 末尾追加数据会让签名失效。
