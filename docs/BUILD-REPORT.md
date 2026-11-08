# 构建与门禁基线报告（阶段 1）

> 目的：为「九转还原 1.0」的重构提供可对照的基线证据。后续阶段每次改动都应能与本报告的数值/结果对比。
>
> 记录时间：2026-11-05（Asia/Shanghai）
> 记录环境：本机（Windows，非 CI），工作目录为仓库根目录（下文记作 `<仓库根>`）
> 代码基线：SysRecover 0.9.96（原仓库提交 `8401cdab2de08ba5d12871a9c59edf0b1cfbdf58`），导入提交 `40a12e6`

## 1. 工具链

| 组件 | 版本 / 路径 |
|---|---|
| x64 编译器 | `x86_64-w64-mingw32-g++` 14.2.0（x86_64-posix-seh-rev0，MinGW-Builds），由构建 PATH 提供 |
| x86 编译器 | `i686-w64-mingw32-g++` 14.2.0（i686-ucrt-posix-dwarf，Brecht Sanders r3），由构建 PATH 提供 |
| make | `mingw32-make`（随 mingw64 提供） |
| Python | 3.13.7（仅构建脚本/门禁使用），由构建 PATH 提供 |
| bash | MSYS2（仅供 `make check` 的 shell 门禁），由构建 PATH 提供 |
| git | 2.56.0.windows.4（MinGit 便携版） |

## 2. 可复现命令（PowerShell，`cwd = <仓库根>`）

> 关键环境约束：**构建用的 PATH 不能包含 bash**，否则 GNU Make 会改用 `sh` 执行 cmd 语法的配方而失败；`make check` 反而需要 bash 在 PATH 上。因此分两条 PATH 组合执行。

```powershell
# 构建/打包用（无 bash）
$env:PATH = "<mingw64>\bin;<mingw32>\bin;<python>;" + $env:PATH
mingw32-make -f Makefile PYTHON='<python>' ARCH=x64 all
mingw32-make -f Makefile PYTHON='<python>' ARCH=x86 all
mingw32-make -f Makefile PYTHON='<python>' package

# 门禁/冒烟用（含 bash）
$env:PATH = "<msys2>\usr\bin;" + $env:PATH
mingw32-make -f Makefile PYTHON='<python>' check
mingw32-make -f Makefile PYTHON='<python>' ARCH=x64 smoke-cli
mingw32-make -f Makefile PYTHON='<python>' dist-check
```

## 3. 实测结果（阶段 1 收尾时）

| 命令 | 期望 | 实际 |
|---|---|---|
| `make check` | 全部用例与门禁 PASS | **PASS**：70 用例 / 613 断言，0 失败；`test_version` 13 项检查 0 失败；check-docs / check-i18n / check-widths / check-shell / check-config / check-layers / check-secrets 全部 PASS（check-widths 有 1 条既有 WARN，非失败） |
| `make ARCH=x64 all` | 生成 x64 三件套 | **OK**：`dist/x64/SysRecover.exe` 1 694 720 B（1.62 MB）、`SysRecoverUI.exe` 2 679 808 B（2.56 MB）、`SysRecoverCheck.exe` 469 504 B（0.45 MB） |
| `make ARCH=x86 all` | 生成 x86 三件套 | **OK**：`dist/SysRecover_x86.exe` 1 893 888 B（1.81 MB）、`SysRecoverUI_x86.exe` 2 922 496 B（2.79 MB）、`SysRecoverCheck_x86.exe` 596 992 B（0.57 MB） |
| `make package` | 部署发行目录并跑 `check-docs --strict-dist` | **OK**：根目录 32 位启动器 + `x64/` 真程序 + 共享资源；`--strict-dist` PASS |
| `make ARCH=x64 smoke-cli` | CLI 与镜像检查程序冒烟通过 | **PASS**：CLI 19 例 + 镜像检查程序 5 例 |
| `make dist-check` | 发行目录结构/许可/资源齐全 | **PASS** |

## 4. 已知非缺陷项

- `make smoke-cli` 依赖 `make package` 先部署 `dist/lang/`：`check --lang en help` 需要语言包。只构建不打包时冒烟失败属预期，已在 `docs/ARCHITECTURE.md` 的「构建与门禁」注明。
- `check-widths` 的 1 条 WARN 为 0.9.96 既有文案宽度告警，未在本阶段扩大。

## 5. 阶段 1 交付物

- 单一基线提交 `40a12e6`：原项目源码/构建脚本/文档整体导入（471 文件，约 56.8 MB，排除 dist、构建产物、msys64/Python313、演练盘镜像、私钥）；MIT `LICENSE`、`THIRD_PARTY_LICENSES.txt`、原作者署名与版权声明原样保留。
- `docs/ARCHITECTURE.md`（中文，目标结构）与 `docs/ARCHITECTURE-0.9.96.en.md`（原英文文档留存）。
- 本报告。

## 6. 阶段 2 结构重构验证记录

### 6.1 提交序列（`97a0d6c..HEAD`，均由 `git log --oneline` 实取）

```
c8f96f0 refactor(ops): 格式化/清空卷收敛为 PrepareTargetVolume（行为不变）
15d5ea3 拆出 ops_staged.cpp 并删除空的 ops.cpp（纯机械搬迁）
7f21aa5 拆出 ops_returnold.cpp（返回旧系统，纯机械搬迁）
ced9e11 拆出 ops_backup.cpp（备份主流程，纯机械搬迁）
bfc962f 拆出 ops_inplace.cpp（就地还原，纯机械搬迁）
375f79a 拆出 ops_space.cpp（空间预检/落盘搬家，纯机械搬迁）
42cd9f1 拆出 ops_selfpack.cpp（自打包/解包，纯机械搬迁）
01c37e0 拆出 ops_boot.cpp（ESP/引导修复，纯机械搬迁）
ea12f6e 拆出 ops_preserve.cpp（~old 保留/回滚，纯机械搬迁）
cae6552 拆出 ops_format.cpp（格式化/清空卷，纯机械搬迁）
7432d75 refactor(ops): 拆出 ops_util.cpp（纯机械搬迁）
cf9dc5b refactor(ops): 拆分前最小可见性调整 —— BootWriteBlockedHint 去掉 static
```

每个提交都单独通过 `make ARCH=x64 all` + `make ARCH=x86 all` + `make check` 后才提交。

### 6.2 拆分结果

| 文件 | 行数 | 主要内容 |
|---|---:|---|
| `ops_util.cpp` | 263 | 字符串/路径/目录工具、`AppendHistory`、菜单标记 |
| `ops_format.cpp` | 114 | `PrepareTargetVolume`（唯一入口）+ 两个 static 实现 |
| `ops_preserve.cpp` | 119 | `PreserveOldSystem` / `RollbackPreservedOld` |
| `ops_boot.cpp` | 658 | ESP 挂载/定位、`BiosBcdLooksValid`、`BootWriteBlockedHint`、`RepairBoot` |
| `ops_selfpack.cpp` | 262 | `PackSelfToSingleFile` / `ExtractSelfToFolder` |
| `ops_space.cpp` | 334 | 空间预检、`SettleHome`、swap 空间估算 |
| `ops_inplace.cpp` | 634 | `RunDirectRestore`、离线改机名、RunOnce、首启清理 |
| `ops_backup.cpp` | 638 | `RunBackup`、备份目标探测、ESP/EA 排除配置 |
| `ops_returnold.cpp` | 419 | `StageReturnOld` 系列 |
| `ops_staged.cpp` | 1774 | 暂存还原主流程、状态机、菜单绑定、重试 |

`git diff --shortstat 97a0d6c..HEAD` → `13 files changed, 5298 insertions(+), 4706 deletions(-)`（增量主要是每个文件重复的 include 前导与拆分头注释）。

### 6.3 逻辑未丢失/未重复的机械证明

用脚本对比拆分前后的顶层函数集合（原 `ops.cpp` vs 10 个 `ops_*.cpp`）：

| 指标 | 期望 | 实际 |
|---|---|---|
| 函数数 | 与原文件一致 | 67 vs 67 |
| 原文件有、拆分后无（MISSING） | 无 | none |
| 拆分后有、原文件无（EXTRA） | 无 | none |
| 拆分后重复定义 | 无 | none |
| `src/app/ops.cpp` 是否残留 | 否 | 已删除 |

（`ErrorAdvice(...)=delete;` 是删除声明而非函数，未计入 67。）

### 6.4 可复现验证清单

```powershell
# 1) 构建（PATH 不含 bash/sh，否则 Make 会改用 sh，cmd 语法配方失败）
$env:PATH = "D:\Prog\ProgIDE\mingw64\bin;D:\Prog\ProgIDE\mingw32\bin;D:\Prog\ProgIDE\python\Python313;C:\Windows\System32;C:\Windows"
mingw32-make -f Makefile PYTHON=D:/Prog/ProgIDE/python/Python313/python.exe ARCH=x64 all
mingw32-make -f Makefile PYTHON=D:/Prog/ProgIDE/python/Python313/python.exe ARCH=x86 all

# 2) 门禁（PATH 需含 bash；MAKESHELL 强制配方仍走 cmd）
$env:PATH = "D:\Prog\ProgIDE\mingw64\bin;D:\Prog\Github\Github_UGit\resources\app\git\bin;C:\Windows\System32;C:\Windows"
$env:MAKESHELL = "C:\Windows\System32\cmd.exe"
mingw32-make -f Makefile SHELL=cmd.exe PYTHON=D:/Prog/ProgIDE/python/Python313/python.exe check

# 3) 打包 + SHA256 + 分发校验（顺序不能反）
mingw32-make -f Makefile SHELL=cmd.exe PYTHON=D:/Prog/ProgIDE/python/Python313/python.exe package
mingw32-make -f Makefile SHELL=cmd.exe PYTHON=D:/Prog/ProgIDE/python/Python313/python.exe sha256
mingw32-make -f Makefile SHELL=cmd.exe PYTHON=D:/Prog/ProgIDE/python/Python313/python.exe dist-check

# 4) 结构证明
git log --oneline 97a0d6c..HEAD
git diff --shortstat 97a0d6c..HEAD
git ls-files src/app | Select-String ops
```

| 步骤 | 期望 | 实际（2026-11-05） |
|---|---|---|
| x64 all | 退出码 0，生成 `dist/x64/SysRecover*.exe` | PASS |
| x86 all | 退出码 0，生成 `dist/SysRecover*_x86.exe` | PASS |
| check | `70 cases, 613 checks, 0 failures` + 全部门禁 PASS | PASS |
| package | 退出码 0，`--strict-dist` PASS | PASS（退出码 0） |
| smoke-cli | CLI 19 例 + 检查程序 5 例全 PASS | PASS（含 `check --lang en help`） |
| 函数集合对比 | 67/67，无缺失/多余/重复 | PASS |

### 6.5 构建系统注意（拆分后新增）

- `ar rcs` 只增换、不删除：源清单移除文件后，旧 `.o` 会残留在 `libapp.a` 里。归档规则已改为「先删后建」；但 **Make 无法感知源清单变化**，因此源清单变更后首次构建必须 `make clean`（或手动删 `build*/libapp.a`）。CI 每次全量构建，不受影响。
- 拆分后 `src/app/ops_internal.h` 是唯一新增的内部头：只放跨模块声明，不含实现；`ops.h` 对外 API 未改。

## 7. 阶段 3（P0 数据安全整改）实测证据

环境同上（Windows 10/11 x64 本机，GCC 14.2.0 MinGW-w64 x64/x86，Python 3.13，MSYS2 bash）。
时间：2026-11-05。

### 7.1 双架构构建

| 命令 | 结果 |
|---|---|
| `mingw32-make ARCH=x64 all` | 退出码 0 |
| `mingw32-make ARCH=x86 all` | 退出码 0 |

### 7.2 门禁（`make check`，PATH 含 `msys64\usr\bin`）

```
79 cases, 630 checks, 0 failures
check-docs: PASS  (version=0.9.96 contract_version=1 契约键 34 个)
check-i18n: OK (838 keys, 838 lang entries)
check-widths: OK (1 WARN: skin:FormatLabel zh 39px > budget 38px)
check-shell: PASS (zjrestore-lite.sh=178, zjswap.sh=13, init=63 ...)
check-config: PASS / check-layers: PASS / check-secrets: PASS
test_zjswap: PASS (A fail-closed/rollback, B happy path, C marker gate, D post-verify)
```

### 7.3 H1「改前失败 / 改后通过」证据

同一份 `tests/test_zjswap.sh`，分别指向 **0.9.96 插桩副本**（仅加测试脚手架：跳过 mount、`umount` 置空、注入 move 失败，交换逻辑保持原样）与**新脚本**：

改前（0.9.96）：

```
FAIL: A: exit=0 (want 4)
FAIL: A: ~new was removed (must be kept)
FAIL: A: ~old left behind after rollback
FAIL: A: new tree leaked into root
FAIL: D: exit=0 (want 5)
FAIL: D: ~new removed although verify failed
test_zjswap: FAIL
rc=1
```

改后（HEAD）：

```
test_zjswap: PASS (A fail-closed/rollback, B happy path, C marker gate, D post-verify)
```

用例含义：A 注入旧项 move 失败 → 必须 exit 4、回滚已搬项、保留 `~new`、不得留下 `~old`、新树不得泄漏到根；B 正常路径 → exit 0、删 `~new`、保留 `~old`；C 缺 `.zj-done` → exit 2；D 新树缺 `winload` → exit 5 且保留 `~new`。

## 8. 阶段 4（P1 + 测试 + CI）实测证据

环境同上。时间：2026-11-05。

### 8.1 原子提交清单（每批提交前跑双架构构建 + 门禁）

| 提交 | 覆盖编号 | 要点 |
|---|---|---|
| `2c36c30` | M8、M9 | 格式化工具预检 + diskpart 回退；长路径声明与 `\\?\` 前缀 |
| `3ef7c65` | M14 | 契约/日志改 `std::string` 拼接，写后回读校验关键键 |
| `794fa95` | M2、M3、M4 | keep 清空不完整即中止；清理范围收敛；ESP 按目标盘匹配 |
| `4739c42` | M11、M12、M13、L1 | USB 显式开关；盘数上限 128 + extent 重试；备份不完整退出 7；提权保留工作目录；12 处译文不再当格式串 |
| `fdcaeb5` | M1、M5、M6、M7、M10、M15 | 子镜像配对；救援层序列号/GUID 双校验；镜像不静默覆盖；备份预检；明文清扫；ESP 暂存失败降级 |
| `e4be3d7` | L5、L6、L7、H2 | UEFI 项双条件清理 + 写失败返回；wim 线程上限 8；删死代码；回执 OK 判定纯函数化 + 单测 |
| `8c0be91` | L2（CI） | x86 补 32 位门禁/冒烟并修产物路径；新增 release job（package + SHA256 + dist-check）；同步重建载荷 |

### 8.2 构建与门禁

| 命令 | 实际 |
|---|---|
| `mingw32-make ARCH=x64 all` | 退出码 0（`dist/x64` CLI/GUI/Check 三件） |
| `mingw32-make ARCH=x86 all` | 退出码 0（`dist/*_x86` 三件） |
| `mingw32-make -f Makefile SHELL=cmd.exe check` | `80 cases, 636 checks, 0 failures`；test_version 13 checks 0 失败 |
| check-docs / check-i18n | PASS / `OK (863 keys, 863 lang entries)` |
| check-widths / check-shell | `OK (1 WARN)` / `PASS (zjrestore-lite.sh=184, init=70, deny-list 8 clean)` |
| check-config / check-layers / check-secrets | 全部 PASS |
| test_zjswap | PASS（A/B/C/D 四场景） |
| `package` → `sha256` → `dist-check` | 退出码 0 / `SHA256SUMS: 73 file(s)` / `check-dist: PASS` |

### 8.3 关键改动的可验证证据

- **M12（USB/盘数/extent）**：`disk.cpp` 中 `BusTypeUsb` 同时置 `isUsb`/`isRemovable`；默认 `EnumerateDisks(false)` 过滤，GUI 菜单项「显示 USB 设备（默认隐藏，谨慎选择）」传 `true`；列表行尾追加「（USB 设备）」。上限 16→128；extent 缓冲 8 起、`ERROR_MORE_DATA` 倍增（cap 64）。
- **M11（备份完整性）**：有跳过文件时 CLI 返回 7，`--allow-skipped` 才归 0；帮助与退出码表已更新。
- **M2/M3/M4（keep 模式）**：脚本在 apply 前中止并写 `STEP=wipe-incomplete` / `wipe-not-mountable`；清理只删带本工具特征的引导文件；ESP 优先同盘，无法判断时仅唯一候选。复现步骤见 `docs/07-测试矩阵与回归记录.md` §6 ④。
- **M7/M10（备份预检/清扫）**：追加前按 `源用量/2 + 128MB` 预检（`ADV_SPACE`）；加密备份持锁后清理 `dest.plain.tmp`/`.stage*`。
- **H2 回归**：`RescueStatusIsOk` 单测覆盖 `result=OK_BOOT_UNFIXED`、`result=FAILED`、带前缀、CRLF + 行尾空格（`tests/unit_tests.cpp` `rescue_status_ok_exact`）。
- **L5**：`RemoveUefiEntry` 对 `WriteVar`/`DeleteVar`/`BootNext` 失败返回 false，主流程 `return 1`，日志写明 `removal FAILED`。

### 8.4 未自动化 / 需实机确认的部分

- 测试 ③（追加中途失败原镜像可用）与 ④（keep 清空不完整中止）需要真实 WIM 写入或 Linux 挂载环境，**不进 `make check`**；可复现步骤与通过判据见 `docs/07` §6。
- 测试 ⑤（引导修复失败降级上报）核心判定已由 `rescue_status_ok_exact` 覆盖；端到端需真机/VM。
- L3（`ops.cpp` 单体拆分）已在阶段 2 完成（函数集合 67/67 一致）。

## 9. 1.0 交付总说明

### 9.1 目录结构

| 路径 | 作用 |
|---|---|
| `src/app/` | 业务编排：备份、还原（就地/暂存/返回旧系统）、格式化、空间预检、镜像源等（阶段 2 由 `ops.cpp` 拆分） |
| `src/boot/` | UEFI/BIOS 启动链：启动项、任务与契约文件、bootfix、菜单安装 |
| `src/cli/` | 命令行入口（命令、退出码、JSON 输出） |
| `src/common/` | 公共基础设施：路径/长路径、锁、日志、i18n、CPU 能力、救援回执判定（纯逻辑） |
| `src/disk/` | 磁盘/分区枚举（含 USB 显式开关、extent 重试） |
| `src/gui/` | Win32/DirectUI 界面与清单（`longPathAware`） |
| `src/launcher/` | 根启动器（x64 入口、提权、控制台交接） |
| `src/tools/` | `wimcheck`、`zj-restore-boot`、EA apply 等小工具 |
| `src/wim/` | wimlib 封装（线程上限、捕获/应用/追加/校验） |
| `bootfiles/` | 救援层载荷：内核、initramfs、`zjrestore-lite.sh`、`zjswap*.sh`、启动资产 |
| `tests/` | 单元测试与脚本回归（`test_zjswap.sh`、`test_version.py`） |
| `tools/` | 门禁脚本（docs/i18n/宽度/shell/config/layers/secrets/dist）、打包与 SHA256 |
| `docs/` | 架构、构建报告、测试矩阵、已知问题、接手指南与历史归档 |
| `lang/`、`skin/`、`resources/` | 双语词典、皮肤、图标与清单 |
| `third_party/` | vendored wimlib（含第三方许可） |
| `dist/` | 构建/打包输出（不入库；含 `SHA256SUMS.txt`） |

规模：`src/` 共 63 个 `.cpp`、53 个 `.h`。

### 9.2 各阶段成果

| 阶段 | 成果 |
|---|---|
| ① 基线立骨 | 从只读源建立可独立构建的 1.0 工程；双架构构建与 `make check` 跑通；文档骨架 |
| ② 纯结构重构 | `ops.cpp`（4705 行）拆为 13 个 `ops_*.cpp`；函数集合 67/67 一致、行为不变；每提交可编译；格式化收敛先行为一致 |
| ③ P0 数据安全 | H1 交换 fail-closed + 后置校验、H2 引导降级回执、H3 旧系统保留回滚、H4 校验契约、H5 空间预检/残留清理、H6 救援侧启动项、H7 单实例锁；H1 回归脚本"改前失败、改后通过" |
| ④ P1 + 测试 + CI | M1~M15 全部（配对/身份校验/不静默覆盖/预检/长路径/USB 开关/备份完整性/契约回读/ESP 降级等）；L1/L4/L5/L6/L7；CI x86 门禁 + release 打包/SHA256；测试③④可复现步骤 |
| ⑤ 发布工程 | 版本 1.0.0-beta.1、`v1.0.0-beta.1` 标签；`CHANGELOG.md`、已知问题页、README 中英事实修正；VERSIONINFO 资源（含单文件外层 stub）；`package → sha256(75) → dist-check` 全绿 |

### 9.3 测试结果（1.0.0 本机）

- 双架构构建：`ARCH=x64 all`、`ARCH=x86 all` 退出码 **0**。
- `make check`：**80 cases / 636 checks / 0 failures**；含 `test_zjswap` A~D、回执判定、WIM 结构预检。
- 门禁：i18n `860/860`、docs PASS（`version=1.0.0-beta.1 contract_version=1 契约键 34（救援读 22）`）、
  widths OK（1 WARN，历史项）、shell PASS（deny-list 8 clean）、config/layers/secrets 全 PASS、
  **versioninfo PASS（x64/x86 三个 exe 各 8 个字段与 version.h 同值）**。
- 发布链：`package` 0 → `SHA256SUMS: 75 file(s)` → `check-dist: PASS`（含单文件外层 exe 的 VersionInfo）。
- H1 证据：改前 `FAIL: A: exit=0 (want 4)` / `~new was removed` / `~old left behind` / `new tree leaked`；
  改后 `test_zjswap: PASS`。

### 9.4 与原项目 0.9.96 的差异

1. **结构**：`ops.cpp` 单体重构为 13 个模块；新增 `src/common/rescue_decision.h`、`src/app/ops_util|space|staged|inplace` 等；公共设施（长路径、锁、i18n）集中。
2. **行为（数据安全）**：交换/清空/引导修复由"尽力而为 + 告警"改为 **fail-closed**；旧系统默认保留到首启成功后再清理（可回退）；格式化前强制结构预检，全量校验改为显式入口。
3. **行为（可靠性）**：USB 默认隐藏但可显式打开；磁盘上限 128、extent 可重试；备份不完整返回 7；同名镜像不覆盖；长路径支持。
4. **工程**：双架构 CI 门禁 + 发布打包/SHA256；12 处译文格式串问题清零并加门禁；wim 线程上限 8。
5. **不变量**：镜像格式（WIM/ESD/ZJENC1）、CLI 主命令、MIT + 第三方许可、原作者署名保持不变。

### 9.5 剩余风险

1. **未签名**：SmartScreen/杀软拦截无法在发布前消除；被拦截时暂存会明确失败，不会静默继续。
2. **真机覆盖不足**：救援层挑盘、引导修复、异机驱动在多种 RAID/双盘/加密环境下的实际命中率仍需现场验证。
3. **NVRAM 写保护/写满**：已改为返回失败（L5），但不同固件返回码差异大，极端机型可能仍需手动清理。
4. **BitLocker/EFS/重解析点**未做专项兼容验证；操作前需解密或备份。
5. **CI 首次真实运行**：workflow 的 i686 工具链下载地址与 PATH 逻辑为本地推演，需首次 Actions 运行确认。
6. **镜像格式边界**：仅 WIM/ESD（+`.zje`）；GHOST/SNA/TBI 等不支持。

### 9.6 需实机确认的 7 项

1. 救援层按磁盘序列号 + 分区 GUID 挑中目标盘（多盘同型号）。
2. keep 模式清空不完整时在 apply 前中止，目标盘仍可挂载（`docs/07` §6 ④）。
3. 追加中途失败/断电后原镜像仍可校验通过（`docs/07` §6 ③）。
4. 引导修复失败回执为 `OK_BOOT_UNFIXED`，Windows 侧不报"还原成功"。
5. USB 固定盘勾选后可见且标注正确，默认仍隐藏。
6. ≥16 核机器备份线程被限制为 8，峰值内存可控。
7. NVRAM 写保护/写满时临时 UEFI 启动项清理失败能被上层感知（退出码 1）。

## 10. 版本资源（VERSIONINFO）实测（2026-11-06）

生成方式：`tools/gen-versioninfo.py` 从 `src/common/version.h` 读版本/产品/公司/版权，
按 profile 生成 `.rc`（`#pragma code_page(65001)`），再按架构各用自己的 `windres` 编译并链接：
x64 用 `mingw64/bin/windres`，x86 用 `mingw32/bin/windres`；x64/x86 的 `.o` 分目录存放，不混用。

单文件版是「自建 stub + 追加载荷」（`src/app/ops_selfpack.cpp` 先整段拷贝 stub 字节、
再在 PE 末尾追加压缩载荷），因此顺序天然是"先给 stub 加资源、再追加载荷"——
实测外层 exe 的 `.rsrc` 完好，资源管理器能直接读到版本。

读取命令（与用户在资源管理器看到的是同一份数据）：

```powershell
(Get-Item .\dist\x64\SysRecoverUI.exe).VersionInfo |
  Select-Object ProductName,ProductVersion,FileVersion,CompanyName,LegalCopyright,FileDescription,OriginalFilename,InternalName
```

实测输出（x64 / x86 / 单文件外层，节选）：

| 文件 | ProductName | ProductVersion | FileVersion | CompanyName | LegalCopyright |
|---|---|---|---|---|---|
| `dist/x64/SysRecoverUI.exe` | 九转还原 | 1.0.0-beta.1 | 1.0.0-beta.1 | DreamGrain | Copyright (c) 2026 DreamGrain. MIT License |
| `dist/SysRecoverUI_x86.exe` | 九转还原 | 1.0.0-beta.1 | 1.0.0-beta.1 | DreamGrain | 同上 |
| `dist/SysRecover-Portable.exe`（外层） | 九转还原 | 1.0.0-beta.1 | 1.0.0-beta.1 | DreamGrain | 同上 |

自动化：`make check` 调 `tools/check-versioninfo.py dist/x64/...`；
`make dist-check` 额外校验单文件外层与 `bootfiles/portable-stub*.exe`。
**签名顺序约束**：以后若做代码签名，必须"先打包、再签名"，签名后追加载荷会让签名失效。
