# 九转还原（9SinoRecover）架构说明

> 基线：SysRecover 0.9.96（原仓库提交 `8401cdab`）。
> 本文描述**目标结构**；标注「阶段 2」的部分表示重构尚未落地，当前代码仍是 0.9.96 原样移植。

## 1. 定位与边界

- 产品中文名：**九转还原**；仓库/目录名：`9SinoRecover`。
- 定位：Windows 系统备份 / 还原 / 引导修复工具。两条还原引擎：
  - **A 引擎（swap）**：Windows 侧把镜像解压到目标卷的 `~new`，重启进 Linux 救援层后仅做目录改名交换。
  - **B 引擎（block）**：重启进 Linux 救援层，用 `mkntfs` + `wimlib-imagex` 直接写分区（异机还原走这条）。
- 许可：继承原项目 MIT `LICENSE` 与 `THIRD_PARTY_LICENSES.txt`（wimlib、Duilib、shim/GRUB 等），保留原作者署名与版权声明。新增代码同样以 MIT 发布。

## 2. 构建工具链与验证结果

| 组件 | 要求 | 本机实测 |
|---|---|---|
| C++ 编译器 | mingw-w64 GCC 14.2.0（UCRT），x86_64 与 i686 两套 | `D:\Prog\ProgIDE\mingw64` / `mingw32` |
| 构建工具 | `mingw32-make`（GNU Make 3.8x+） | 随 mingw64 提供 |
| 资源编译 | `windres` | 随 mingw-w64 提供 |
| Python | 3.10+（仅构建脚本与门禁用，不随产品分发） | 3.13.7（原项目自带 `Python313`，已复制到本机独立目录） |
| bash | `bash -n` 语法门禁 + `make check` 的 shell 检查需要 | msys2 / Git for Windows 的 `bash` |
| git | 版本管理 | MinGit 2.56.0.4（便携版，无管理员权限） |
| 7-Zip | 仅 `make portable`（单文件封装）需要 | 已具备 |

构建注意：
- **必须在 cmd/PowerShell 环境下调用 make**。Makefile 的配方使用了 `if not exist`、`copy /Y` 等 cmd 内建命令；若把 `bash` 放进了 `PATH`（Make 会改用 `sh` 作为 shell），配方会因 cmd 语法直接失败。
- `make check` 的 `tools/check-shell.py` 需要 `bash` 在 `PATH` 上（或装在 Git for Windows 默认路径）。因此推荐：构建用纯净 PATH，门禁时再把 bash 目录加入 PATH。

实测（2026-11-05，本机）：

| 命令 | 结果 |
|---|---|
| `mingw32-make check` | **PASS**：70 用例 / 613 断言 0 失败；test_version 13 检查 0 失败；check-docs / check-i18n / check-widths / check-shell / check-config / check-layers / check-secrets 全 PASS（check-widths 1 条既有 WARN） |
| `mingw32-make ARCH=x64 all` | **OK**：`dist/x64/SysRecover.exe` 1.62 MB、`SysRecoverUI.exe` 2.56 MB、`SysRecoverCheck.exe` 0.45 MB |
| `mingw32-make ARCH=x86 all` | **OK**：`dist/SysRecover_x86.exe` 1.81 MB、`SysRecoverUI_x86.exe` 2.79 MB、`SysRecoverCheck_x86.exe` 0.57 MB |
| `mingw32-make package` | **OK**：根目录 x64 启动器（32 位 stub）+ `x64/` 真程序 + 共享资源；末尾 `check-docs --strict-dist` PASS |
| `mingw32-make ARCH=x64 smoke-cli` | **PASS**：CLI 19 例 + 镜像检查程序 5 例 |
| `mingw32-make dist-check` | **PASS** |

## 3. 目录结构（目标）

```
9SinoRecover/
├── Makefile                  构建入口（x64 / x86 / package / check / smoke-cli）
├── LICENSE                   MIT（原项目）
├── THIRD_PARTY_LICENSES.txt  第三方许可清单（wimlib、Duilib、shim/GRUB…）
├── CHANGELOG.md              阶段 5 交付
├── src/
│   ├── common/               进程内通用设施（日志、i18n、加密、VSS、EA、路径…）
│   ├── disk/                 磁盘/分区枚举、GPT/MBR、分区脚本
│   ├── wim/                  wimlib 封装（备份/追加/应用/校验）
│   ├── boot/                 BCD 解析与写入、UEFI 变量、GRUB、引导修复、暂存任务契约
│   ├── app/                  业务编排层（备份 / 暂存 / 就地还原 / 返回旧系统 / 交换）
│   ├── cli/                  命令行入口
│   ├── gui/                  Duilib 图形界面
│   ├── launcher/             32 位总入口 stub（按位数转发）
│   └── tools/                独立小工具（镜像检查、EA 补写、首启清引导）
├── bootfiles/                救援层资产（vmlinuz + initramfs、zjswap.sh、zjrestore-lite.sh、GRUB/shim、BCD 模板）
├── third_party/              duilib-master（静态库源码）、wimlib（DLL + 头文件）、ucrt
├── resources/ skin/ lang/    GUI 皮肤、图标、语言包
├── tests/                    单元测试（tiny_test + unit_tests）与故障注入回归（阶段 4）
├── tools/                    构建/门禁/演练脚本（Python + PowerShell，不随产品分发）
├── docs/                     设计与运维文档（含 ARCHITECTURE.md 本篇）
└── .github/workflows/        CI（阶段 4 扩展为 x64/x86 构建 + 测试 + 打包 + SHA256）
```

## 4. 模块边界

| 层 | 模块 | 职责 | 明确不做 |
|---|---|---|---|
| 基础设施 | `common/*` | 日志、i18n、进度、异常/崩溃、VSS、加密（ZJENC1）、EA、路径、进程、压缩封装 | 不知道"备份/还原"业务语义 |
| 设备层 | `disk/*`、`wim/*` | 枚举磁盘与分区、读写分区表、调用 wimlib | 不做策略决策（不决定该不该格式化） |
| 引导层 | `boot/*` | BCD 读写、UEFI 启动项、GRUB 部署、引导修复、暂存契约与状态机 | 不直接操作镜像内容 |
| 编排层 | `app/*` | 备份/还原流程、破坏性操作前检查、锁、回滚、契约落盘 | 不直接拼 BCD 二进制（调 `boot/*`） |
| 前端 | `cli/*`、`gui/*` | 参数解析、确认交互、进度呈现、结果页 | 不复制业务判定（与 `app/*` 共用同一函数） |
| 救援层 | `bootfiles/*.sh`、`bootfiles/alpine/init` | 重启后的目录交换、B 引擎 apply、引导修复、清理 | 不依赖 Windows API |

### 破坏性操作的唯一入口（阶段 2 落地）

所有会改目标盘的路径（就地格式化/清空、暂存解压、交换、返回旧系统）必须经过同一个前置检查函数，顺序固定：

1. 目标盘身份确认（磁盘序列号 / 分区 GUID，不只靠 offset+size）；
2. 镜像快速结构预检（头、XML、资源表越界、文件大小吻合、完整性表存在）；
3. 操作锁（`AcquireOpLock`，RAII 释放，函数入口第一行）；
4. 空间预检（含 `~old` 保留所需的额外空间、临时文件所需空间）；
5. 仅以上全部通过才允许格式化/清空/写盘。

## 5. 依赖关系（文本图）

```
cli/main.cpp ─┐
gui/*.cpp    ─┴─► app/ops.* ─┬─► boot/*  ─► common/*
                             ├─► wim/*   ─► wimlib (DLL)
                             ├─► disk/*  ─► Win32 (IOCTL / SetupAPI)
                             └─► common/*

bootfiles/zjrestore-lite.sh ─► bootfiles/zjswap.sh / zjswap-undo.sh / zjlog.sh
        ▲                                   ▲
        └── 读取 Windows 侧写入的契约文件（_zjresy*.log / restore-task.conf）
```

约束：`app` 不反向依赖 `cli`/`gui`；`boot` 不依赖 `app`；`common` 不依赖上层。CLI 与 GUI 通过链接同一个 `libapp.a` 共享实现（现有 Makefile 已如此组织，继续保持）。

## 6. 基线文件 → 目标位置映射（阶段 2 完成后）

| 旧位置（0.9.96） | 新位置 | 说明 |
|---|---|---|
| `src/app/ops.cpp`（4705 行，单体） | `src/app/ops_util.cpp` | 路径/字符串/目录工具（`IsNetworkPath`、`W2U`、`DeleteTreeRec`、`CopyDir`…） |
| 同上 | `src/app/ops_space.cpp` | `CanRestoreInPlace`、`CheckRestoreSpace`、`EstimateSwapSpace`、`QuerySwapSpace`、`SettleHome` |
| 同上 | `src/app/ops_format.cpp` | `FormatTargetVolume`、`WipeVolumeContents` + 统一「破坏性操作前置检查」 |
| 同上 | `src/app/ops_preserve.cpp` | `PreserveOldSystem`、`RollbackPreservedOld`（与交换/回滚共用） |
| 同上 | `src/app/ops_backup.cpp` | `RunBackup`、`AppendHistory`、`ProbeBackupDest` |
| 同上 | `src/app/ops_inplace.cpp` | `RunDirectRestore`、离线改计算机名、RunOnce 注入、首启清理助手 |
| 同上 | `src/app/ops_staged.cpp` | `StageRestoreImpl/StageRestore/StageRestoreMenu`、`CheckStagedTaskState`、`CleanupStaleStagedTask`、`RetryStagedRestore` |
| 同上 | `src/app/ops_returnold.cpp` | `StageReturnOld`、`DetectReturnOldCapability`、`StageReturnOldByDiskPart` |
| 同上 | `src/app/ops_boot.cpp` | `RepairBoot`、ESP 挂载/定位、UEFI 提示、`BootWriteBlockedHint` |
| 同上 | `src/app/ops_selfpack.cpp` | `PackSelfToSingleFile`、`ExtractSelfToFolder` |
| `src/app/ops.h` | 保持 `src/app/ops.h` | 对外 API 不变；内部实现拆到上述文件 |
| `src/boot/uefi.cpp`、`boot/bcd*`、`boot/grub.cpp`、`boot/task.cpp` | 位置不变 | 仅按整改项打补丁 |
| `src/wim/wim.cpp` | 位置不变 | 补结构预检/空间预检/线程上限 |
| `src/cli/main.cpp` | 位置不变 | 退出码分档、提权工作目录、`--allow-skipped` |
| `src/gui/*` | 位置不变 | 降级结果页、明文临时文件清扫 |
| `bootfiles/zjswap.sh`、`zjrestore-lite.sh`、`alpine/init` | 位置不变 | 脚本按 H1/H2/M2/M3/M4/M5/H6 改造 |
| `docs/*`（原 70 篇） | 保留原名 | 历史文档不改写；新增本篇与阶段 5 文档 |

## 7. 阶段划分（与交付约定一致）

1. **阶段 1 基线立骨**（已完成）：导入 0.9.96、恢复 x64/x86 构建与门禁/冒烟、本篇架构文档。
2. **阶段 2 纯结构重构**：按上表拆分 `ops.cpp`，行为不变，每个提交都能编译；格式化/清空逻辑收敛为一个带前置校验的函数。
3. **阶段 3 P0（H1、H2、H3、H7；H4/H5 按修订契约）**：
   - H1 `zjswap.sh` fail-closed；
   - H2 引导修复失败 → 降级结果 `OK_BOOT_UNFIXED`；
   - H3 `PreserveOldSystem` 部分失败回滚；
   - H7 暂存路径入口加锁（RAII）；
   - H4 改为**毫秒级快速结构预检**（格式化前强制）+ 全量校验仅作显式入口 + `~old` 保留到首次成功启动；
   - H5 撤回（`wimlib_overwrite` 本身原子）；只并入空间预检与残留临时文件清理；
   - H6 降级（优先级排在 H1/H2/H3/H7 之后）。
4. **阶段 4 P1 与测试**：M1-M15、L1-L7 中的适用项；`tests/` 可执行回归 + CI。
5. **阶段 5 发布工程**：CHANGELOG、版本策略、已知问题、README 重写、`make package` + SHA256。
