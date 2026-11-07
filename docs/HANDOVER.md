# 交接摘要（1.0.0-beta.1，2026-11-07 收尾轮）

> 本文件只记录**证据撑得住**的状态。没有写"全部完成"：N20 未实现、N21 未修、
> N7 明确暂缓，Windows 侧"装菜单 → 重启进救援"这一段仍未复测。

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
- 为不抬高 `2>/dev/null` 基线（lite=184 / init=70），新增语句没有加消音后缀。

### 5. 启动烟测（本轮新增，QEMU/OVMF）

- 当前载荷：`initramfs-zjrestore.cpio.gz` 17,393,120 B，724 entries，内部戳
  `1.0.0-beta.1`，SHA-256 `30C69064F147D5244C3670DC30171562A3289185894CD1F0C8AA93E4EDFEB6BA`。
- QEMU/OVMF 下走 shim → Debian GRUB → Debian 内核 → 我们的 initramfs：
  `Linux version 6.12.107`、`SR: modules loaded`、
  `entry marker written (/dev/sda1) entry=menu`、
  `ZJRESTORE-status.txt` 含 `build=1.0.0-beta.1` / `entry=menu` / `step=no-task`、
  `no restore task … reboot in 10s`（安全退出）。**PASS**。
- **未覆盖**：Windows 侧"安装菜单 → 固件重启"这一段；本机 VMware 未安装，
  QEMU 烟测不经过我们的安装代码，也**不等于**固件在 Secure Boot 开启下验签 shim。

### 6. UDF 真实介质（本轮新增）

- 用 Windows Server 2008 R2 SP1 ISO（真实 Microsoft UDF 介质）在救援内核里：
  `modprobe scsi_mod → ata_piix → sr_mod → udf`，
  `mount -t udf /dev/sr0 /mnt` **rc=0**，列出 `sources/install.wim`
  （2,952,626,175 B），`head -c 4096` 读取 **rc=0**。**PASS**。
- 结论：2026-11-06 的 `mkisofs -udf` `EINVAL` 是 ISO 制作问题，不是内核 UDF 缺陷。
- **未跑** apply 全链路：该介质是安装源（`install.wim`），预检按设计拒绝；
  需要一枚装着完整系统镜像的 UDF 光盘才能跑端到端。

### 7. 收尾链

- `make package` → 退出码 0；`make sha256` → 76 files；`make dist-check` → PASS。
- `make check` → `102 cases, 709 checks, 0 failures`；`check-i18n 884/884`；
  `check-shell PASS (init=70, lite=184, initramfs 9/9)`；`check-foreground ok`；
  `test_zjswap PASS`；`check-widths` 仅 1 个既有 WARN。
- x64 / x86 `all` 均在 `make package` 内退出码 0。

## 提交号

| 提交 | 内容 |
|---|---|
| `ebff546` | 规范 docs/35 文件名（磁盘上本是合法 UTF-8，`git mv`） |
| `63a9a5e` | 安全启动签名事实核查（N7）+ N20 设计 + N21 新发现 |
| `d1af69e` | N19 主窗置后修复 + `check-foreground` 门禁 |
| `2ab3e87` | N8 入口类型取证 + N10 收口 |
| 紧随其后的文档提交 | 本交接页 + docs/33 §5.6 + docs/34 收尾轮证据 |

## 已修 / 待验证 / 待决定

**已修（有代码或门禁撑住）**

- N19 主窗置后（`d1af69e`，静态门禁 + 代码路径）。
- N10 发布流程项（`2ab3e87`，`package` 口径 + 三元版本断言）。
- N8 低成本部分：`entry-kind.txt` / `zjentry=` / `entry=`（`2ab3e87`，烟测实证）。
- docs/35 文件名（`ebff546`）。
- N18 后续"新 cpio 启动烟测"（QEMU/OVMF，见上）。

**待验证 / 仍开放**

- **N20**：设计已出（`docs/34` §5.4），未实现；需用户评审。
- **N21**：Secure Boot 一次性项 disarm 失配；未修、未实机复现；与 N20 合并实现。
- **N7**：根治暂缓（1.0 不做）；`docs/34` §5.1 与 `docs/33` §5.4 已写已知限制。
- **Windows 侧装菜单 → 重启进救援**：本轮无 VMware，未复测。
- **UDF apply 全链路**：UDF 支持已证实，但缺"完整系统镜像在 UDF 上"的介质。
- **`docs/35` 物理机项**：USB 固定盘、BIOS/MBR 仍标 **[物理机]**，不猜结论。
- 七项实机第 1/4/5/7 项：仍"无法判定 / 部分验证"，不得写成已验证。

**待用户决定**

- N20 设计是否按 §5.4 实现（实现时先修 N21）。
- 是否联系报障用户换新包重试。

## 环境与收尾状态（2026-11-07）

- 本机**未安装 VMware**：`vmrun.exe` 不存在（`C:\Program Files*` 下无 VMware 目录），
  只剩 `D:\Software\System\_VMware\VMdisks\初心Win10x64\Windows 10 x64.vmx`。
- QEMU 可用：`D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe`；
  烟测后已 `poweroff`，无残留 QEMU 进程。
- UDF 测试用的硬链接目录 `D:\Software\System\_udf-media` 已删除（原 ISO 未动）。
- 工作区：提交后应为干净（`git status --short` 无输出）。

## 下次开工从这里接

1. `git status --short`（应为空）→ `make check`（应 PASS，含 C5 9/9）。
2. N20 评审通过后，先修 N21（disarm 判定改为认 `temp-entry.txt` 条目号），
   再实现"早归还 + Recovery 后备 + Windows 三态"。
3. 有 VMware 环境时，补"Windows 侧装菜单 → 固件重启进救援"烟测。
4. 拿一枚"完整系统镜像在 UDF 上"的介质，补 apply 端到端。
5. 物理机到位后按 `docs/35` 跑 USB 固定盘、BIOS/MBR。
