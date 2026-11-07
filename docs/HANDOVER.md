# 交接摘要（1.0.0-beta.1，2026-11-07 收尾轮）

> 本文件只记录**证据撑得住**的状态。没有写"全部完成"：N20 批 1 + 批 2 均已实现并做了
> QEMU/OVMF 固件级实测；**批 2 的 Windows GUI 弹窗本身没有 end-to-end 跑**（单测覆盖读即自删）；
> N7 明确暂缓；Windows 侧"装菜单 → 重启进救援"这一段仍未复测。

## 本轮做完的（2026-11-07）

### 1. docs/35 文件名（`ebff546`）

- 磁盘上的文件名是**合法 UTF-8**，不是编码损坏；旧显示里的乱码只是终端/工具渲染差异。
- 用 `git mv` 改为 `docs/35-救援载荷与手动验证清单.md`，同步 `HANDOVER` 引用。
- 提交信息的 BOM 前缀是写入方式造成的（纯显示问题，内容正确）。

### 2. 安全启动事实核查 + N20 设计（`63a9a5e`，只改文档）

- 现有链（Debian shim 16.1 → Debian 签名的 GRUB 2.12 → Debian 签名的内核
  6.12.107 → 我们的 initramfs）**没有签名问题**，零注册零交互。
- `bootfiles/sb/shimx64.efi` 是 Debian shim，微软 CA2011+CA2023 双签，内嵌 Debian
  厂商证书；`grubx64.efi`、`vmlinuz-zjrestore` 由 Debian 签名。
- **新增自有 EFI 程序不花钱的路径只有 MOK 一次性注册**（用户首次进蓝底 MOK Manager
  按键确认）；否则要微软 UEFI CA 送签。结论：N7 根治 = "需要用户做一次性操作"，
  1.0 不做，`docs/34` 标为已知限制，`docs/33` §5.4 有面向用户的说明。
- N20 设计写入 `docs/34` §5.4（未实现）：尽早归还 BootOrder、保留 Recovery 后备入口、
  Windows 三态感知。**等用户评审后才动码。**
- 新发现 **N21**：Secure Boot/grub 模式下一次性启动项的 disarm 判定失配
  （`init` 只认 `zjtemp`，而 grub 临时项指向 `shimx64.efi`），静态成立、未实机复现、
  未修；与 N20 同一处修复点。`docs/34` §3.6、`docs/33` §5.7。

### 3. N19 主窗置后（`d1af69e`，已修）

- 新增 `src/gui/ui_foreground.{h,cpp}`：`SetForegroundWindow` 失败 →
  `AttachThreadInput` 重试 → `BringWindowToTop` + `SetWindowPos(HWND_TOP)` +
  `SetActiveWindow`；全部失败写 `foreground: restore failed …` 日志，不静默。
- `confirm_dlg` / `passwd_dlg` / `initdisk_dlg` 关闭后统一走
  `sysrecover::RestoreForeground(owner)`。
- `main_win.cpp` 的 `MessageBoxW(nullptr, …)` 改为 `hPrev` 作 owner。
- `main_form.cpp` 所有 `ShellExecuteW(nullptr, …)` 改 `m_hWnd`；「导出驱动」完成后的
  自动 `/select` 改为询问「打开所在文件夹」，不再自动抢前台。
- 新门禁 `tools/check-foreground.py` 进 `make check`：禁止 owner 为空、要求返回值兜底、
  要求三个模态框走统一入口。

### 4. N8 入口类型取证 + N10 收口（`2ab3e87`）

- 安装侧写 `EFI\ZJRESTORE\entry-kind.txt`（`menu` / `temp`）；Direct 模式命令行追加
  `zjentry=menu|temp`；救援层把 `entry=` 写进 `entry-started.txt` 和
  `ZJRESTORE-status.txt`。
- N10 关闭：发布口径固定 `make package`，`payload-gate` 保证源变更自动重建、
  C5/C6/C7 不过即拒绝打包。
- 为不抬高 `2>/dev/null` 基线（lite=184 / init=66），新增语句没有加消音后缀。

### 5. 启动烟测（本轮新增，QEMU/OVMF）

- 当前载荷（N20 批 2 重建后）：`initramfs-zjrestore.cpio.gz` 17,407,257 B，726 entries，
  内部戳 `1.0.0-beta.1`，SHA-256 `d1ea1df0f1ce979fcd6e050b33ea160c823044085bae5967f2504c98b745c8e4`。
  相对上一版新增 efivarfs 模块（`kernel/fs/efivarfs/efivarfs.ko.xz`）与批 2 的
  `mark_pending_task`。
- QEMU/OVMF 下走 shim → Debian GRUB → Debian 内核 → 我们的 initramfs：
  `Linux version 6.12.107`、`SR: modules loaded`、
  `entry marker written (/dev/sda1) entry=menu`、
  `ZJRESTORE-status.txt` 含 `build=1.0.0-beta.1` / `entry=menu` / `step=no-task`、
  `no restore task … reboot in 10s`（安全退出）。**PASS**。
- **未覆盖**：Windows 侧"安装菜单 → 固件重启"这一段；本轮没跑该流程
  （VMware 便携版可用，见"环境与收尾状态"），
  QEMU 烟测不经过我们的安装代码，也**不等于**固件在 Secure Boot 开启下验签 shim。

### 6. UDF 真实介质（本轮新增）

- 用 Windows Server 2008 R2 SP1 ISO（真实 Microsoft UDF 介质）在救援内核里：
  `modprobe scsi_mod → ata_piix → sr_mod → udf`，
  `mount -t udf /dev/sr0 /mnt` **rc=0**，列出 `sources/install.wim`
  （2,952,626,175 B），`head -c 4096` 读取 **rc=0**。**PASS**。
- 结论：2026-11-06 的 `mkisofs -udf` `EINVAL` 是 ISO 制作问题，不是内核 UDF 缺陷。
- **未跑** apply 全链路：该介质是安装源（`install.wim`），预检按设计拒绝；
  需要一枚装着完整系统镜像的 UDF 光盘才能跑端到端。

### 7. N20 批 1 + 批 2（2026-11-07，已实现 + QEMU/OVMF 实测）

用户拍板：①=B（保留常驻救援项）、②=恢复原始 BootOrder 语义（默认直接进 Windows）、
③=B（救援首屏三选一 + 求助渠道）、⑤=分两批（批 2 才做 ④）。

- **`bootfiles/alpine/init`**
  - `hand_back_bootorder`：进救援、日志可读之后、写目标盘之前，以
    `EFI/ZJRESTORE/bootorder.pre.bak` 为基准（无则 `bootorder.bak`），去掉我方条目后
    **追加到末尾**；不设 BootNext（残留且指向我方则清）；写失败回 1 → 调用方中止
    （`poweroff -f`）并打印求助渠道。常驻项与 ESP 载荷保留。
  - `ensure_efivars`：busybox initramfs 不会自动挂 efivarfs；改为 `modprobe efivarfs` +
    `mount -t efivarfs`，挂不上→中止（这是实测才暴露的必修项）。
  - `is_our_temp_entry`（N21）：按描述/路径匹配，且用 `od -An -tx1` 打十六进制串
    （busybox 的 `tr -d '\000'` 不可靠），ASCII / UTF-16LE 都认。
  - `triage_previous`：三选一「回滚旧系统（大写 YES 确认）/ 再试新系统 / 仅重启」，
    同屏 QQ 群 1128237828、邮箱 ms_2@qq.com；回滚失败按 `zjswap-undo.sh` 的 rc 2/3/其它
    给可读原因；回滚成功后清 `restore-task.conf` 与 `_zjresy*.log`。
- **`tools/build-debian-rescue.py`**：`kernel/fs/efivarfs/` 不再排除，列入 `EXTRA_KEEP`。
- **`src/tools/restore_boot_main.cpp`**：一次性清理改为保留 UEFI 救援项 + ESP 载荷
  （①=B）；BIOS/MBR 分支保持原样。
- **`src/gui/main_form.cpp`** + `lang/en.lang`：GUI 失败提示追加求助渠道（英文版含邮箱）。
- **QEMU/OVMF 实测（`%TEMP%\zj-n20`，预置 BootOrder=[SysRecover 临时项, Windows]）**：
  - 救援层日志：`efivarfs mounted`、
    `BootOrder handed back from bootorder.pre.bak: original order, rescue entry Boot0001 last`；
  - 在归还之后、重启之前**强杀 QEMU** → 同一 NVRAM 再启动 → 固件直接
    `starting Boot0000 "Windows Boot Manager"`，未再进救援；
  - 固件 Boot Manager（ESC → Boot Manager）手动选 `SysRecover Temporary Restore` →
    救援层启动（`SR: SysRecover rescue (kernel 6.12.107+deb13-amd64)`），条目仍可见、可进。
  - 夹具细节：ESP = QEMU `fat:rw` 目录盘 + `startup.nsh`（`bcfg` 建两条目）；
    Windows 侧用同一个 GRUB 二进制放在 `/EFI/WIN`，只打印 `WINDOWS-DUMMY-BOOTED`。
  - **未覆盖**：Windows GUI 的"装菜单 → 重启 → F10/F12 选 SysRecover"链路本轮仍未跑
    （无 guest 自动化凭据；QEMU/WinPE GUI 点击也未自动化）。

**批 2（④=A，范围锁死，2026-11-07）**

- **`bootfiles/alpine/init`**：`triage_previous` 回滚分支（`zjswap-undo.sh` 返回 0）新增
  `mark_pending_task`，只写 `<旧系统根>\ProgramData\SysRecover\last-task-unfinished.txt`
  三行（`status=unfinished` / `when=YYYY-MM-DD HH:MM:SS` / `action=rollback`）；
  挂载/写入失败只 WARN，不阻塞回滚。**不写注册表、不新增/修改启动项、服务。**
- **`src/app/pending_task.{h,cpp}`**：`TakePendingTaskMarker` 读 UTF-8 后**立即 `DeleteFileW`**
  （先删再提示）；`src/gui/main_form.cpp::ShowPendingTaskNotice` 在每次正常启动的
  `WM_CHECK_RESCUE` 里调用，提示"上次还原任务未完成：系统已回滚到原来的系统。"、
  `记录时间：`、QQ 群 1128237828 / 邮箱 ms_2@qq.com。
- **单测**：`pending_task_marker_read_then_self_delete`（读到内容 → 文件消失 → 二次为假）。
- **QEMU/OVMF 实测**（自建 512MB NTFS VHD 目标盘，盘上预置 `~old\Windows`；
  测试 initramfs 用追加 cpio 覆盖 `zjswap-undo.sh` 为 `exit 0`，不改仓库文件）：
  救援首屏选 `1` + 大写 `YES` → `回滚完成` → `AUTO-RECOVER: rebooting now` →
  重启直接进 Windows（`WINDOWS-DUMMY-BOOTED`）；挂回 VHD 读到标记三行且内容正确。
- **未覆盖**：Windows GUI 实际弹窗（无 guest 自动化），只到单测 + 代码路径/门禁。

### 8. 收尾链（2026-11-07，N20 批 2 后已复跑门禁）

- `make check`（批 2 后复跑）→ `103 cases, 720 checks, 0 failures`；
  `check-i18n 889/889`；`check-shell PASS (init=66, lite=184, initramfs 9/9)`；
  `check-foreground ok`；`test_zjswap PASS`；`check-widths` 仅 1 个既有 WARN。
- x64 / x86 `all` 退出码 0；载荷 17,407,257 B / 726 entries /
  SHA-256 `d1ea1df0f1ce979fcd6e050b33ea160c823044085bae5967f2504c98b745c8e4`。

## 提交号

| 提交 | 内容 |
|---|---|
| `ebff546` | 规范 docs/35 文件名（磁盘上本是合法 UTF-8，`git mv`） |
| `63a9a5e` | 安全启动签名事实核查（N7）+ N20 设计 + N21 新发现 |
| `d1af69e` | N19 主窗置后修复 + `check-foreground` 门禁 |
| `2ab3e87` | N8 入口类型取证 + N10 收口 |
| 紧随其后的文档提交 | 本交接页 + docs/33 §5.6 + docs/34 收尾轮证据 |
| `7f68896` | N20 批 1：早归还 BootOrder（pre.bak 基准 + 救援项垫底）+ N21 判定修复 + efivarfs 模块 + 三选一/求助渠道 |
| （本条提交） | N20 批 2：回滚后写"上次任务未完成"标记（读即自删）+ 单测 + 文档 |

## 已修 / 待验证 / 待决定

**已修（有代码或门禁撑住）**

- N19 主窗置后（`d1af69e`，静态门禁 + 代码路径）。
- N10 发布流程项（`2ab3e87`，`package` 口径 + 三元版本断言）。
- N8 低成本部分：`entry-kind.txt` / `zjentry=` / `entry=`（`2ab3e87`，烟测实证）。
- docs/35 文件名（`ebff546`）。
- N18 后续"新 cpio 启动烟测"（QEMU/OVMF，见上）。
- **N20 批 1**（①②③）：代码 + QEMU/OVMF 固件级实测（中途杀进程→Windows、手选救援项→救援）。
- **N20 批 2**（④=A）：救援层写标记 + GUI 读即自删；单测 + QEMU/OVMF 回滚链路实测
  （回滚 → 重启进 Windows → 挂回盘读到标记）。
- **N21**：与 N20 批 1 同一处修复，随批 1 关闭。

**待验证 / 仍开放**

- **N20 批 2 的 Windows GUI 弹窗**：单测覆盖"读到即删、二次为假"，但**没有**在 Windows
  里实际登录看到提示（无 guest 自动化）；不写成"端到端已验证"。
- **N7**：根治暂缓（1.0 不做）；`docs/34` §5.1 与 `docs/33` §5.4 已写已知限制。
- **Windows 侧装菜单 → 重启进救援**：VMware 可用，但本轮**未执行**该流程
  （无 guest 自动化凭据；QEMU/WinPE GUI 点击未自动化）。
- **UDF apply 全链路**：UDF 支持已证实，但缺"完整系统镜像在 UDF 上"的介质。
- **`docs/35` 物理机项**：USB 固定盘、BIOS/MBR 仍标 **[物理机]**，不猜结论。
- 七项实机第 1/4/5/7 项：仍"无法判定 / 部分验证"，不得写成已验证。

**待用户决定**

- 暂无：N20 批 1/批 2 均已按用户 2026-11-07 的决策实现。

## 环境与收尾状态（2026-11-07）

- **VMware 便携版可用**（2026-11-07 核查，更正此前"未安装"的误判）：
  `D:\Software\System\_VMware\VMware17.6.1\vmrun.exe` → True；
  `...\VMdisks\初心Win10x64\Windows 10 x64.vmx` → True；
  `vmrun list` → `Total running VMs: 0`；`listSnapshots` → 5 个
  （含 `pre-rerun-newpayload-20261106`）。此前只查 `C:\Program Files*`，误判为未安装。
  Windows 侧"装菜单 → 重启进救援"因此**可以复测**，只是本轮没跑。
- QEMU 可用：`D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe`；
  烟测后已 `poweroff`，无残留 QEMU 进程。
- UDF 测试用的硬链接目录 `D:\Software\System\_udf-media` 已删除（原 ISO 未动）。
- 工作区：提交后应为干净（`git status --short` 无输出）。

## 下次开工从这里接

1. `git status --short`（应为空）→ `make check`（应 PASS，含 C5 9/9）。
2. **N20 批 2（④）已完成**；如需补证据，优先做 Windows GUI 的"读到标记后弹窗"端到端。
3. VMware 可用时，补"Windows 侧装菜单 → 固件重启进救援"烟测。
4. 批 1 门禁通过后，用 `D:\Software\System\_VMware` 的 7z 副本做 BIOS/MBR 传统引导链路。
5. 拿一枚"完整系统镜像在 UDF 上"的介质，补 apply 端到端。
6. 物理机到位后按 `docs/35` 跑 USB 固定盘、BIOS/MBR。
