# 构建与门禁基线报告（阶段 1）

> 目的：为「九转还原 1.0」的重构提供可对照的基线证据。后续阶段每次改动都应能与本报告的数值/结果对比。
>
> 记录时间：2026-11-05（Asia/Shanghai）
> 记录环境：本机（Windows，非 CI），工作目录 `D:\Prog\_Project\9SinoRecover`
> 代码基线：SysRecover 0.9.96（原仓库提交 `8401cdab2de08ba5d12871a9c59edf0b1cfbdf58`），导入提交 `40a12e6`

## 1. 工具链

| 组件 | 版本 / 路径 |
|---|---|
| x64 编译器 | `x86_64-w64-mingw32-g++` 14.2.0（x86_64-posix-seh-rev0，MinGW-Builds），`D:\Prog\ProgIDE\mingw64\bin` |
| x86 编译器 | `i686-w64-mingw32-g++` 14.2.0（i686-ucrt-posix-dwarf，Brecht Sanders r3），`D:\Prog\ProgIDE\mingw32\bin` |
| make | `mingw32-make`（随 mingw64 提供） |
| Python | 3.13.7（仅构建脚本/门禁使用），`C:\Users\Administrator\AppData\Local\Programs\Python313\python.exe` |
| bash | MSYS2 `D:\Prog\ProgIDE\msys64\usr\bin\bash.exe`（仅供 `make check` 的 shell 门禁） |
| git | 2.56.0.windows.4（MinGit 便携版） |

## 2. 可复现命令（PowerShell，`cwd = D:\Prog\_Project\9SinoRecover`）

> 关键环境约束：**构建用的 PATH 不能包含 bash**，否则 GNU Make 会改用 `sh` 执行 cmd 语法的配方而失败；`make check` 反而需要 bash 在 PATH 上。因此分两条 PATH 组合执行。

```powershell
# 构建/打包用（无 bash）
$env:PATH = "D:\Prog\ProgIDE\mingw64\bin;D:\Prog\ProgIDE\mingw32\bin;C:\Users\Administrator\AppData\Local\Programs\Python313;" + $env:PATH
mingw32-make -f Makefile PYTHON='C:/Users/Administrator/AppData/Local/Programs/Python313/python.exe' ARCH=x64 all
mingw32-make -f Makefile PYTHON='C:/Users/Administrator/AppData/Local/Programs/Python313/python.exe' ARCH=x86 all
mingw32-make -f Makefile PYTHON='C:/Users/Administrator/AppData/Local/Programs/Python313/python.exe' package

# 门禁/冒烟用（含 bash）
$env:PATH = "D:\Prog\ProgIDE\msys64\usr\bin;" + $env:PATH
mingw32-make -f Makefile PYTHON='C:/Users/Administrator/AppData/Local/Programs/Python313/python.exe' check
mingw32-make -f Makefile PYTHON='C:/Users/Administrator/AppData/Local/Programs/Python313/python.exe' ARCH=x64 smoke-cli
mingw32-make -f Makefile PYTHON='C:/Users/Administrator/AppData/Local/Programs/Python313/python.exe' dist-check
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
