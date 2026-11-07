# 交接摘要（1.0.0-beta.1，2026-11-07）

> 本文件只记录**证据撑得住**的状态。没有写"全部完成"：`docs/35` 的实机项、
> 新 cpio 的启动烟测、N19 的修复都还没做。

## 本轮做完的（2026-11-07）

### 1. 救援载荷"过期即失败"门禁（N18，已修）

- **`4b7f337`**：`make package` / `make portable` 前置 `payload-gate`；
  `bootfiles/` 任一载荷源文件比 `initramfs-zjrestore.cpio.gz` 新 → 自动重建；
  重建后 `check-shell` C5（9 个成员逐字节，含 3 个二进制）/ C6（mtime）/ C7
  （`zjrescue-version` 必须等于 `version.h`）任一不过 → 拒绝打包。
  `package` 写出 `dist/bootfiles/rescue-build.txt`，`check-dist` 断言
  `version.json == rescue-build.txt == initramfs zjrescue-version`。
- **`5ca5b1c`**：装菜单的 ESP 空间预检（写满/只读常在拷贝前命中）原来只报 MB 数，
  现在同时追加"ESP 旧版本 / 内置新版本"，与拷贝/校验失败分支同一 fail-closed 文案。
- 构建脚本无人值守：`tools/fetch-rescue-apks.py`（固定 21 个 APK，缺包回退 v3.21）
  + `tools/build-debian-rescue.py`；不需要 msys2、不需要交互。**唯一依赖是网络**
  （APK 缓存在 `tools/vmtest/dl/alpine/`，被 gitignore，不随仓库走；无网且无缓存时构建
  会硬失败——这是期望行为）。
- 复查时发现的事实：HEAD 的 cpio（17,391,321 B，11-06 复测所用）内部
  `zjrescue-version=0.9.92`，且 `zj-restore-boot.exe` 与仓库当前文件不一致；
  旧 C5 只比脚本，所以没发现。本轮重建 cpio：724 entries / 491 modules /
  23,971,168 B raw / **17,392,733 B gz**，内部戳 `1.0.0-beta.1`，C5 **9/9**。
- 当次验证：`make check` → `102 cases, 709 checks, 0 failures`；`make package` PASS；
  `make sha256` 76 files；`make dist-check` PASS；负向注入（touch
  `bootfiles/zjrestore-lite.sh`）→ `--payload-only` 报该文件并退出 1，还原 mtime 后 PASS。

### 2. 主窗偶发被压到其它窗口之后（N19，**未修，只取证**）

- 结论：**已确认缺陷（时序相关）**，不是误判。详见 `docs/34` §3.5。
- 可稳定复现的一类（设计内）：`main_form.cpp:902/931/938/2118/1229` 主动打开
  Explorer/浏览器，主窗随之退后；「导出驱动」完成后的自动 `/select` 最容易让人感觉
  "软件自己沉下去"。
- 偶发的一类（缺陷）：`confirm_dlg.cpp:485-486`、`passwd_dlg.cpp:169-170`、
  `initdisk_dlg.cpp:382-383` 关闭模态框后只调 `SetForegroundWindow(owner)`，
  **返回值未检查、无 Z 序回退**；前台锁被占时静默失败。`main_win.cpp:526` 的
  `MessageBoxW(nullptr, …)` 缺 owner。
- 外部控制台工具（`diskpart`/`bcdedit`/`dism`/`wimlib`/`vssadmin`/`mountvol`/`reg`/
  `attrib`/`icacls`）全部走 `RunProcess` 的 `CREATE_NO_WINDOW`，不会弹窗抢前台；
  运行期没有 `runas`。历史同类问题（0.7.55 主窗"藏到文件夹后面"）只在**启动路径**
  修过（`main_win.cpp:718-721`），运行期无等价逻辑。
- 拟改 A/B/C 已写在 `docs/34` §3.5，**等用户选**后再动代码。

### 3. 文档与清单

- `docs/34`：新增 N18/N19 行、§3.4（门禁机制与实测）、§3.5（取证与复现表）、
  §4.2 第 5/8/9 项、§6 N18 实测、§7 行为行。
- `docs/33`：更新"产物说明"（`package` 自动重建并拒绝打包）；§4 补充
  "打开日志/导出/官网会切到 Explorer/浏览器"以及偶发置后的用户说明。
- **`docs/35-救援载荷与手动验证清单.md`（新）**：真实机逐步清单，
  每步写"期望看到什么"，覆盖旧 0.9.x→新菜单、ESP 写满/只读 fail-closed、
  `repair-boot`、USB 固定盘、BIOS/MBR；后两项标注 **[物理机]**。

## 提交号

| 提交 | 内容 |
|---|---|
| `6d239e8` | N17：ESP 旧载荷治理（下发前自证版本、重写前备份、fail-closed） |
| `61680a1` | N17 补：备份不改写源文件属性 |
| `4b7f337` | N18：initramfs 纳入 Make 依赖 + C5/C6/C7 + dist 三元版本断言 + 重建 cpio |
| `5ca5b1c` | N18 补：ESP 空间不足分支也带双方版本 |
| `f14674d` | 文档：docs/33、docs/34（N18/N19）、docs/35 清单 |
| 紧随其后的文档提交 | 本交接页更新（`git log -- docs/HANDOVER.md` 可见） |

## 已修 / 待实机 / 待用户决定

**已修（有门禁或单测撑住）**

- ESP 旧载荷不再被静默沿用（N17；单测 5 条，`6d239e8`）。
- 打包链不再把过期 initramfs 带进 dist（N18；C5 9/9、C6、C7、dist 三元断言，`4b7f337`）。
- 写满/只读的失败提示含双方版本（`5ca5b1c`）。

**待实机（`docs/35`）**

- A：真实机 ESP 为 0.9.x → 装新菜单，核对 `stale -> rewrite`、备份目录、
  `rescue-build.txt`、字节数一致。
- B1/B2：ESP 写满 / 只读 → fail-closed 且含双方版本，自报版本不被写成新版。
- C：同条件下 `repair-boot` 成功/失败两条路径。
- D：真实 USB 固定盘（**[物理机]**）。
- E：BIOS/MBR 机型（**[物理机]**）。
- 新 cpio（`4b7f337`）**尚未做**"从它启动进救援层"的烟测；不把"字节一致"当成"启动已验证"。

**待用户决定**

- 是否重装旧载荷专门做一次"旧 vs 新"的 S1/S2/S3 逐项对照（本轮未做）。
- 是否让报障用户换新包重试（本轮未联系）。
- N19 修复走 A（`RestoreForeground` 封装 + 回退）/ B（补 owner）/ C（取消自动
  `/select`）中的哪一种。

## 环境与收尾状态（2026-11-07）

- 虚拟机：`vmrun list` → `Total running VMs: 0`；`Windows 10 x64.vmx` 中
  **无** `bios.forceSetupOnce`；无残留 8123/902 监听；无 `zj*`/`8123` 防火墙规则
  （只剩 `vmware-authd` 服务本身）。
- 未动快照 `pre-manual-esd-20261106`、`post-fail-pre-manual-20261106`。
- 工作区：提交后应为干净（`git status --short` 无输出）。

## 下次开工从这里接

1. `git status --short`（应为空）→ `make check`（应 PASS，含 C5 9/9）。
2. 有真实旧载荷机器时，先做 `docs/35` A + C；没有就等用户安排。
3. 用户选定 N19 方案后，按 `docs/34` §3.5 实现并补单测/回归。
4. 然后回到开放队列：N7（固件静默回落只缓解）、N8（diag 缺字段）、N10（发布流程）、
   UDF 真实介质、七项实机未判定项；每项独立提交。
