# SysRecover - MinGW 构建（无 CMake）
# 工具链：x64 = D:\Prog\ProgIDE\mingw64 (GCC 14.2.0, x86_64-w64-mingw32, UCRT)
#         x86 = D:\Prog\ProgIDE\mingw32 (GCC 14.2.0, i686-w64-mingw32,  UCRT)
# 用法（PowerShell）：
#   $env:Path = "D:\Prog\ProgIDE\mingw64\bin;" + $env:Path
#   mingw32-make -f Makefile package        # ★ 发布包：32 位通用启动器（根）+ x64/ + x86 兼容入口 + 共享资源
#   mingw32-make -f Makefile all            # 仅 x64 真程序 → dist/x64
#   mingw32-make -f Makefile ARCH=x86 all   # 仅 x86 兼容程序 → dist/*_x86.exe
#   mingw32-make -f Makefile check          # 单元测试（纯逻辑，零依赖）
#   mingw32-make -f Makefile clean          # 清理（当前 ARCH）
#
# 架构设计（PIT-199 定稿）：根目录的 SysRecover*.exe 是**32 位通用启动器**
# （launchers 目标，固定用 mingw32 编译）：64 位系统转发到 x64\ 下的真程序，
# 32 位系统转发到同目录 *_x86.exe。所以纯 x64 PE（无 WOW64）也能跑，且 32 位
# Windows 不会报"不是有效的 Win32 应用程序"。**Linux 救援层固定 x86_64**
# （与宿主位数无关）。见 docs/08 §0。

ARCH ?= x64
MINGW32 ?= D:/Prog/ProgIDE/mingw32
MINGW64 ?= D:/Prog/ProgIDE/mingw64
ifeq ($(ARCH),x86)
  MINGW    = $(MINGW32)
  CXX      = $(MINGW)/bin/i686-w64-mingw32-g++
  CC       = $(MINGW)/bin/i686-w64-mingw32-gcc
  AR       = $(MINGW)/bin/ar.exe
  WINDRES  = $(MINGW)/bin/windres.exe
  OBJDIR   = build-x86
  # x86 是**32 位兼容入口**：产物带 _x86 后缀（根目录总入口是 32 位启动器，
  # 但启动器只转发，真正的兼容程序仍在这里）
  DISTDIR  = dist
  CLI_NAME = SysRecover_x86.exe
  GUI_NAME = SysRecoverUI_x86.exe
  CHECK_NAME = SysRecoverCheck_x86.exe
  WIMLIB   = third_party/wimlib/x86
  UCRT     = third_party/ucrt/x86
else
  MINGW    = $(MINGW64)
  CXX      = $(MINGW)/bin/x86_64-w64-mingw32-g++
  CC       = $(MINGW)/bin/x86_64-w64-mingw32-gcc
  AR       = $(MINGW)/bin/ar.exe
  WINDRES  = $(MINGW)/bin/windres.exe
  OBJDIR   = build
  # x64 真程序放在 x64/；根目录 SysRecover*.exe 是 32 位通用启动器（见 launchers）
  DISTDIR  = dist/x64
  CLI_NAME = SysRecover.exe
  GUI_NAME = SysRecoverUI.exe
  CHECK_NAME = SysRecoverCheck.exe
  WIMLIB   = third_party/wimlib
  UCRT     = third_party/ucrt/x64
endif

# 构建脚本用的 Python（PATH 上的 python 可能是 Microsoft Store 占位符，不可用）
PYTHON   ?= python
# 版本号唯一来源 = src/common/version.h（用 tools/version.py 读写；每个问题修完 --bump）
VERSION  := $(shell $(PYTHON) tools/version.py)

# ---- Windows 版本资源（VERSIONINFO）----------------------------------------
# 用户规格（2026-11-06）：右键 exe → 属性 → 详细信息 必须能读到
# 产品名（九转还原）/ 版本 / 公司（DreamGrain）/ 版权。
# 唯一来源仍是 src/common/version.h：tools/gen-versioninfo.py 从它读出版本串与
# SYSRECOVER_PRODUCT/COMPANY/COPYRIGHT 生成 .rc（不在 .rc 里手写数字，避免漂移）；
# 再按架构用自己的 windres 编译，x64 / x86 的 .o 绝不混用。
# 单文件是「自建 stub + 追加载荷」（src/app/ops_selfpack.cpp）：先把 stub 字节原样
# 拷过去、再在末尾追加压缩载荷 —— stub 的 .rsrc 完好，顺序天然是"先加资源、后追载荷"。
VI_GEN      = $(PYTHON) tools/gen-versioninfo.py
VI_ARCH_DIR = $(OBJDIR)/versioninfo
VI_X86_DIR  = build/versioninfo
VI_X86_RES  = $(MINGW32)/bin/windres.exe

VI_RC_GUI   = $(VI_ARCH_DIR)/SysRecoverUI.rc
VI_RC_CLI   = $(VI_ARCH_DIR)/SysRecover.rc
VI_RC_CHECK = $(VI_ARCH_DIR)/SysRecoverCheck.rc
VI_RC_LUI     = $(VI_X86_DIR)/SysRecoverLauncherUI.rc
VI_RC_LCLI    = $(VI_X86_DIR)/SysRecoverLauncherCLI.rc
VI_RC_LCHECK  = $(VI_X86_DIR)/SysRecoverLauncherCheck.rc
VI_RC_EA      = $(VI_X86_DIR)/zj-ea-apply.rc
VI_RC_BOOT    = $(VI_X86_DIR)/zj-restore-boot.rc
VI_RC_PSTUB     = $(VI_X86_DIR)/portable-stub.rc
VI_RC_PSTUB_GUI = $(VI_X86_DIR)/portable-stub-gui.rc

# 三个真程序按当前 ARCH 生成（OriginalFilename 随架构变：SysRecoverUI_x86.exe 等）
$(VI_RC_GUI): src/common/version.h tools/gen-versioninfo.py
	$(VI_GEN) --out $@ --profile gui --original $(GUI_NAME)
$(VI_RC_CLI): src/common/version.h tools/gen-versioninfo.py
	$(VI_GEN) --out $@ --profile cli --original $(CLI_NAME)
$(VI_RC_CHECK): src/common/version.h tools/gen-versioninfo.py
	$(VI_GEN) --out $@ --profile check --original $(CHECK_NAME)

# 固定 x86 的辅助 exe（启动器 / EA 补写器 / 启动项清理 / 单文件 stub）
$(VI_RC_LUI): src/common/version.h tools/gen-versioninfo.py
	$(VI_GEN) --out $@ --profile launcher-ui --original SysRecoverUI.exe
$(VI_RC_LCLI): src/common/version.h tools/gen-versioninfo.py
	$(VI_GEN) --out $@ --profile launcher-cli --original SysRecover.exe
$(VI_RC_LCHECK): src/common/version.h tools/gen-versioninfo.py
	$(VI_GEN) --out $@ --profile launcher-check --original SysRecoverCheck.exe
$(VI_RC_EA): src/common/version.h tools/gen-versioninfo.py
	$(VI_GEN) --out $@ --profile ea --original zj-ea-apply.exe
$(VI_RC_BOOT): src/common/version.h tools/gen-versioninfo.py
	$(VI_GEN) --out $@ --profile boot --original zj-restore-boot.exe
$(VI_RC_PSTUB): src/common/version.h tools/gen-versioninfo.py
	$(VI_GEN) --out $@ --profile pstub --original portable-stub.exe
$(VI_RC_PSTUB_GUI): src/common/version.h tools/gen-versioninfo.py
	$(VI_GEN) --out $@ --profile pstub-gui --original portable-stub-gui.exe

$(OBJDIR)/vi_gui.o: $(VI_RC_GUI)
	$(WINDRES) -I . $< -o $@
$(OBJDIR)/vi_cli.o: $(VI_RC_CLI)
	$(WINDRES) -I . $< -o $@
$(OBJDIR)/vi_check.o: $(VI_RC_CHECK)
	$(WINDRES) -I . $< -o $@

$(VI_X86_DIR)/launcher_ui.o: $(VI_RC_LUI)
	$(VI_X86_RES) -I . $< -o $@
$(VI_X86_DIR)/launcher_cli.o: $(VI_RC_LCLI)
	$(VI_X86_RES) -I . $< -o $@
$(VI_X86_DIR)/launcher_check.o: $(VI_RC_LCHECK)
	$(VI_X86_RES) -I . $< -o $@
$(VI_X86_DIR)/ea_apply.o: $(VI_RC_EA)
	$(VI_X86_RES) -I . $< -o $@
$(VI_X86_DIR)/restore_boot.o: $(VI_RC_BOOT)
	$(VI_X86_RES) -I . $< -o $@
$(VI_X86_DIR)/portable_stub.o: $(VI_RC_PSTUB)
	$(VI_X86_RES) -I . $< -o $@
$(VI_X86_DIR)/portable_stub_gui.o: $(VI_RC_PSTUB_GUI)
	$(VI_X86_RES) -I . $< -o $@

# ---- 告警策略 ---------------------------------------------------------------
# 自研代码一律 -Wall -Wextra 且 **-Werror**：告警即构建失败。禁止用 (void) 之类
# 强制消音；确实需要放宽的单条诊断用 -Wno-xxx 并在使用处写明原因。
# third_party 是 vendored 上游源码（不改），编译期用 -w、头文件用 -isystem 隔离，
# 免得第三方噪音（duilib 自身就有几百条）掩盖我们自己的告警。
WARN_FLAGS = -Wall -Wextra -Werror
CXXFLAGS = -O2 -std=c++17 $(WARN_FLAGS) -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE
INCLUDES = -isystem third_party/wimlib -Isrc
# -s = 链接时剥符号表：发布体积显著下降（实测 x64 CLI -838KB / GUI -1.1MB /
# EA 补写器 -495KB）；崩溃处理只用"模块+偏移"（PIT-084），不依赖符号表。
LDFLAGS  = -static -mconsole -s
LDLIBS   = -L$(WIMLIB) -l:libwim-15.dll -ladvapi32 -lole32 -lshell32 -luuid -lgdi32 -lntdll -lversion -lbcrypt

# ---- 应用模块静态库（CLI 与 GUI 共用，避免双份编译 ODR 问题） ----
APP_SRC = src/disk/disk.cpp src/disk/partinit.cpp src/disk/partscript.cpp src/disk/mbrcode.cpp src/wim/wim.cpp src/wim/exclude.cpp \
      src/common/process.cpp src/common/logger.cpp src/common/progress.cpp \
      src/common/singleton.cpp src/common/sysinfo.cpp src/common/zip.cpp \
      src/common/selfarch.cpp src/common/crash.cpp src/common/vss.cpp src/common/i18n.cpp src/common/crypt.cpp \
      src/common/relocate.cpp src/common/pathutil.cpp src/common/compname.cpp src/common/cpucap.cpp src/common/ea.cpp src/common/refscan.cpp src/common/bioskey.cpp src/boot/bcd.cpp src/boot/grub.cpp \
      src/boot/uefi.cpp src/boot/rescue_guard.cpp src/boot/bcd_parse.cpp src/boot/bootpath.cpp src/boot/bootself.cpp src/boot/task.cpp src/boot/bootfix.cpp \
      src/app/safety.cpp src/app/advice.cpp \
      src/app/shortcut.cpp src/app/ops_util.cpp src/app/ops_format.cpp src/app/ops_preserve.cpp src/app/ops_boot.cpp src/app/ops_selfpack.cpp src/app/ops_space.cpp src/app/ops_inplace.cpp src/app/ops_backup.cpp src/app/ops_returnold.cpp src/app/ops_staged.cpp src/app/imgsrc.cpp src/app/selfdiag.cpp src/app/universal.cpp src/app/request_precheck.cpp \
      src/app/swapleft.cpp
APP_OBJS = $(patsubst src/%.cpp,$(OBJDIR)/app/%.o,$(APP_SRC))
APP_LIB  = $(OBJDIR)/libapp.a

# ---- CLI（console） ----
CLI_MAIN = src/cli/main.cpp
CLI_OUT = $(DISTDIR)/$(CLI_NAME)
# CLI 的清单是 **asInvoker + 运行期按需提权**（PIT-200；与 GUI 的
# requireAdministrator 不同，别再写成提权清单）。
CLI_RC     = src/cli/SysRecover.rc
CLI_RC_OBJ = $(OBJDIR)/SysRecover_rc.o

# ---- 独立镜像检查程序（需求 2，2026-10-27）----
# 只读检查（快检 + 列子镜像 + 可选全量校验），**不需要管理员** → 不嵌提权清单，
# 也不装崩溃处理器/不自举位数（根目录 SysRecoverCheck.exe 是 x64 启动器）。
CHECK_MAIN = src/tools/wimcheck_main.cpp
CHECK_OUT  = $(DISTDIR)/$(CHECK_NAME)

# ---- Duilib 静态库（经典 Duilib，MIT；见 AGENTS.md PIT-012；源清单 build/duilib.mk 自动生成） ----
DUI_ROOT  = third_party/duilib-master/DuiLib
# 第三方（vendored，MIT）：**不加 -Wall/-Wextra/-Werror**，并且显式 -w 关掉告警 ——
# 上游源码我们一个字节都不改，它自带几百条历史告警，留着只会淹没自研代码的问题。
DUI_FLAGS = -std=c++17 -O1 -fpermissive -w -DUNICODE -D_UNICODE -DWIN32 -D_WIN32_WINNT=0x0601 -D_stdcall=__stdcall -DUILIB_STATIC -isystem $(DUI_ROOT) -isystem $(DUI_ROOT)/Control -isystem $(DUI_ROOT)/Core -isystem $(DUI_ROOT)/Layout -isystem $(DUI_ROOT)/Utils
CXX_DUI = $(CXX) $(DUI_FLAGS)
CC_DUI  = $(CC) -O1 -w -DUNICODE -D_UNICODE -DWIN32 -D_WIN32_WINNT=0x0601 -D_stdcall=__stdcall -DUILIB_STATIC -isystem $(DUI_ROOT)
DUI_LIB = $(OBJDIR)/libduilib.a
include build/duilib.mk

# ---- GUI（windows，经典 Duilib） ----
GUI_SRC = src/gui/main_win.cpp src/gui/main_form.cpp src/gui/ui_skin.cpp src/gui/instance_dlg.cpp src/gui/confirm_dlg.cpp src/gui/passwd_dlg.cpp src/gui/imgsearch.cpp src/gui/initdisk_dlg.cpp
GUI_OUT = $(DISTDIR)/$(GUI_NAME)
GUI_INCLUDES = -Isrc -isystem $(DUI_ROOT) -isystem $(DUI_ROOT)/Control -isystem $(DUI_ROOT)/Core -isystem $(DUI_ROOT)/Layout -isystem $(DUI_ROOT)/Utils
# duilib 头文件对 ISO 一致性要求宽松，故保留 -fpermissive（第三方头文件，不改）。
GUI_FLAGS = -std=c++17 -O1 -fpermissive $(WARN_FLAGS) -DUNICODE -D_UNICODE -DWIN32 -D_WIN32_WINNT=0x0601 -D_stdcall=__stdcall -DUILIB_STATIC
GUI_LDLIBS = -lgdi32 -lcomctl32 -limm32 -lole32 -luuid -lmsimg32 -lshlwapi -luxtheme -ldwmapi -lwinmm -lgdiplus -loleaut32 -ladvapi32 -lshell32 -lntdll -lversion -lversion -lbcrypt

# ---- 提权清单（requireAdministrator）：windres 编成 .o 再链入 ----
# 备份/还原核心功能（枚举分区、写 BCD/引导、wimlib 挂载）全需管理员权限，
# 故让 exe 启动即请求提权：双击时由加载器弹一次 UAC，无需「右键→以管理员身份运行」。
# manifest 方式不改变工作目录（不像 ShellExecute runas 重启进程会把 CWD 变成
# System32），因此 skin\ 与 libwim-15.dll 的相对定位不受影响。
# 注意：32 位程序在 64 位系统上会**自举成 x64\同名**（selfarch.cpp）—— 父进程已提权，
# CreateProcess 子进程（也是 requireAdministrator）不会再弹第二次 UAC（PIT-082）。
GUI_RC     = src/gui/SysRecoverUI.rc
# 输出到 $(OBJDIR) 根（该目录必然已存在——build/duilib.mk 就在里面并被 include），
# 这样规则里不需要 mkdir，也就不依赖 shell 是 cmd 还是 sh（既有规则里的
# "if not exist ..." 是 cmd 语法，在 sh 下会语法错误，别再仿写）。
GUI_RC_OBJ = $(OBJDIR)/SysRecoverUI_rc.o

all: cli gui wimcheck
	@copy /Y $(subst /,\,$(WIMLIB))\libwim-15.dll $(subst /,\,$(DISTDIR))\ >nul
	@if exist $(subst /,\,$(UCRT))\*.dll copy /Y $(subst /,\,$(UCRT))\*.dll $(subst /,\,$(DISTDIR))\ >nul

cli: $(CLI_OUT)
wimcheck: $(CHECK_OUT)

# 独立输出目录（cmd 的 mkdir 会自动建中间目录，故 dist\x64 可直接建）
$(OBJDIR):
	@if not exist $(subst /,\,$(OBJDIR)) mkdir $(subst /,\,$(OBJDIR))

$(DISTDIR):
	@if not exist $(subst /,\,$(DISTDIR)) mkdir $(subst /,\,$(DISTDIR))

$(OBJDIR)/app/%.o: src/%.cpp
	@if not exist $(subst /,\,$(OBJDIR))\app\$(subst /,\,$(dir $*)) mkdir $(subst /,\,$(OBJDIR))\app\$(subst /,\,$(dir $*))
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# 头文件依赖（-MMD 生成的 .d）：改了 src/common/version.h 等头文件后能自动重编
# 受影响的对象，否则「改了版本号、编译出来还是旧号」（版本号规则的隐形坑）。
-include $(APP_OBJS:.o=.d)

# 阶段 2 拆分修复：`ar rcs` 只会「添加/替换」成员，**不会删除**已从源清单移除的
# 旧目标文件（如拆除 ops.cpp 后残留的 ops.o）。残留成员会把旧符号带进链接，
# 轻则掩盖重复定义、重则引用已删除的函数导致 undefined reference。
# 因此每次建库前先删掉归档，保证 libapp.a 与当前源清单严格一致。
$(APP_LIB): $(APP_OBJS)
	@if exist $(subst /,\,$@) del /Q $(subst /,\,$@)
	$(AR) rcs $@ $(APP_OBJS)

$(CLI_RC_OBJ): $(CLI_RC) src/cli/SysRecover.manifest resources/SysRecover.ico | $(OBJDIR)
	$(WINDRES) -I src/cli -I resources $< -o $@

$(CLI_OUT): $(CLI_MAIN) $(CLI_RC_OBJ) $(OBJDIR)/vi_cli.o $(APP_LIB) $(WIMLIB)/libwim-15.dll src/common/version.h | $(DISTDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(CLI_MAIN) $(CLI_RC_OBJ) $(OBJDIR)/vi_cli.o $(APP_LIB) -o $(CLI_OUT) $(LDFLAGS) $(LDLIBS)

# 镜像检查程序：console、无提权清单（不要求管理员）、-lcomdlg32（多选打开框）。
# 带 VERSIONINFO（右键属性要能看到版本号）。
$(CHECK_OUT): $(CHECK_MAIN) $(OBJDIR)/vi_check.o $(APP_LIB) $(WIMLIB)/libwim-15.dll src/common/version.h | $(DISTDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(CHECK_MAIN) $(OBJDIR)/vi_check.o $(APP_LIB) -o $(CHECK_OUT) $(LDFLAGS) $(LDLIBS) -lcomdlg32

$(OBJDIR)/duilib:
	@if not exist $(subst /,\,$(OBJDIR))\duilib mkdir $(subst /,\,$(OBJDIR))\duilib

$(DUI_OBJS): | $(OBJDIR)/duilib

$(DUI_LIB): $(DUI_OBJS)
	$(AR) rcs $@ $(DUI_OBJS)

gui: $(DUI_LIB) $(APP_LIB) $(GUI_OUT)

$(GUI_RC_OBJ): $(GUI_RC) src/gui/SysRecoverUI.manifest resources/SysRecover.ico | $(OBJDIR)
	$(WINDRES) -I src/gui -I resources $< -o $@

$(GUI_OUT): $(GUI_SRC) $(GUI_RC_OBJ) $(OBJDIR)/vi_gui.o $(DUI_LIB) $(APP_LIB) src/common/version.h | $(DISTDIR)
	$(CXX) $(GUI_FLAGS) $(GUI_INCLUDES) $(GUI_SRC) $(GUI_RC_OBJ) $(OBJDIR)/vi_gui.o $(APP_LIB) $(DUI_LIB) -o $(GUI_OUT) -static -mwindows -s $(GUI_LDLIBS) -L$(WIMLIB) -l:libwim-15.dll

# ---- x64 总入口（图标 + 调用 x64\ 下的真程序）----
# 根目录只放这个 x64 启动器；纯 x64 PE（没有 WOW64）也能运行。
# 32 位兼容程序是 dist\SysRecoverUI_x86.exe / SysRecover_x86.exe（由 ARCH=x86 构建）。
# PIT-199：根启动器必须是 **32 位** 的通用入口 —— 64 位系统上它转发到 x64\，
# 32 位系统上转发到同目录的 *_x86.exe；用 x64 工具链编译会让 32 位 Windows
# 直接报"不是有效的 Win32 应用程序"（网友反馈"命令行用不了了"）。
LAUNCHER_CXX = $(MINGW32)/bin/i686-w64-mingw32-g++
LAUNCHER_UI  = dist/SysRecoverUI.exe
LAUNCHER_CLI = dist/SysRecover.exe
LAUNCHER_CHECK = dist/SysRecoverCheck.exe
LAUNCHER_UI_RC  = build/SysRecoverLauncherUI_rc.o
LAUNCHER_CLI_RC = build/SysRecoverLauncherCLI_rc.o

launchers: $(LAUNCHER_UI) $(LAUNCHER_CLI) $(LAUNCHER_CHECK)

$(LAUNCHER_UI_RC): src/launcher/SysRecoverLauncherUI.rc src/gui/SysRecoverUI.manifest resources/SysRecover.ico
	@if not exist build mkdir build
	$(MINGW32)/bin/windres.exe -I resources -I src/gui $< -o $@

$(LAUNCHER_CLI_RC): src/launcher/SysRecoverLauncherCLI.rc src/cli/SysRecover.manifest
	@if not exist build mkdir build
	$(MINGW32)/bin/windres.exe -I src/cli $< -o $@

$(LAUNCHER_UI): src/launcher/launcher.cpp $(LAUNCHER_UI_RC) $(VI_X86_DIR)/launcher_ui.o
	@if not exist dist mkdir dist
	$(LAUNCHER_CXX) -O2 -std=c++17 $(WARN_FLAGS) -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -DZJ_LAUNCH_GUI $< $(LAUNCHER_UI_RC) $(VI_X86_DIR)/launcher_ui.o -o $@ -static -mwindows -s -luser32

$(LAUNCHER_CLI): src/launcher/launcher.cpp $(LAUNCHER_CLI_RC) $(VI_X86_DIR)/launcher_cli.o
	@if not exist dist mkdir dist
	$(LAUNCHER_CXX) -O2 -std=c++17 $(WARN_FLAGS) -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 $< $(LAUNCHER_CLI_RC) $(VI_X86_DIR)/launcher_cli.o -o $@ -static -mconsole -s -luser32

# 镜像检查入口：console + 等待子进程并透传退出码（脚本/拖放都能拿到结果）。
# **不带提权清单** —— 检查镜像只需读权限；但带 VERSIONINFO。
$(LAUNCHER_CHECK): src/launcher/launcher.cpp $(VI_X86_DIR)/launcher_check.o
	@if not exist dist mkdir dist
	$(LAUNCHER_CXX) -O2 -std=c++17 $(WARN_FLAGS) -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -DZJ_LAUNCH_CHECK $< $(VI_X86_DIR)/launcher_check.o -o $@ -static -mconsole -s -luser32

clean:
	-del /Q $(subst /,\,$(CLI_OUT)) $(subst /,\,$(GUI_OUT)) $(subst /,\,$(CHECK_OUT)) 2>nul
	-del /S /Q $(subst /,\,$(OBJDIR)) 2>nul

# ---- 首启 EA 补写器（PIT-122/123）----
# 独立极小 exe（不链 wimlib/i18n）：救援层把它投放到目标系统后，由本地组策略
# 「启动脚本」在**登录前以 SYSTEM** 拉起（QEMU 全链实测），把 eapack.dat 里的
# EA 用 NtSetEaFile 写回，随后自清理。固定用 **x86 工具链**：x86/x64 目标
# Windows 都能跑（还原目标位数未知；32 位系统也要能用）。
EA_APPLY_SRC = src/tools/ea_apply_main.cpp src/common/ea.cpp src/common/refscan.cpp
EA_APPLY_X86 = $(MINGW32)/bin/i686-w64-mingw32-g++
EA_APPLY_OUT = bootfiles/zj-ea-apply.exe
ea-apply: $(EA_APPLY_OUT)
$(EA_APPLY_OUT): $(EA_APPLY_SRC) src/common/version.h $(VI_X86_DIR)/ea_apply.o
	$(EA_APPLY_X86) $(CXXFLAGS) $(INCLUDES) $(EA_APPLY_SRC) $(VI_X86_DIR)/ea_apply.o -o $(EA_APPLY_OUT) -static -mconsole -municode -s -lntdll -ladvapi32

# ---- “恢复引导菜单”首启清理器（日常还原临时项；PIT-170）----
# 注意：**故意不嵌 requireAdministrator 清单**。启动文件夹先以普通权限启动它，
# 它先弹提示框，再由自己 ShellExecute(runas) 请求 UAC；若清单要求提权，
# UAC 会抢在提示框之前。
BOOT_CLEANUP_SRC = src/tools/restore_boot_main.cpp
BOOT_CLEANUP_X86 = $(MINGW32)/bin/i686-w64-mingw32-g++
BOOT_CLEANUP_OUT = bootfiles/zj-restore-boot.exe
boot-cleanup: $(BOOT_CLEANUP_OUT)
$(BOOT_CLEANUP_OUT): $(BOOT_CLEANUP_SRC) src/boot/bcd.h src/boot/bootnames.h src/boot/cleanup_marker.h $(VI_X86_DIR)/restore_boot.o | $(OBJDIR)
	$(BOOT_CLEANUP_X86) $(CXXFLAGS) $(INCLUDES) $(BOOT_CLEANUP_SRC) $(VI_X86_DIR)/restore_boot.o -o $(BOOT_CLEANUP_OUT) -static -mconsole -municode -s -ladvapi32 -lshell32
# ---- 单元测试（0.7 线重做：2026-10-11 自 graveyard/tests 迁回；旧 45 用例对 0.7
#      源码复验全绿后原样恢复，另补 0.7 新纯逻辑 DecideSwapLeftover 决策表用例）----
TEST_SRC   = tests/tiny_test.cpp tests/unit_tests.cpp tests/main.cpp
TEST_UNITS = src/common/sysinfo.cpp src/wim/exclude.cpp src/boot/task.cpp src/common/zip.cpp src/app/advice.cpp \
      src/common/i18n.cpp src/common/crypt.cpp src/common/selfarch.cpp src/common/relocate.cpp src/common/pathutil.cpp src/boot/bcd_parse.cpp src/boot/bootpath.cpp \
      src/common/ea.cpp src/common/refscan.cpp src/app/swapleft.cpp src/common/logger.cpp src/common/bioskey.cpp \
      src/disk/partscript.cpp src/boot/bootself.cpp src/common/process.cpp \
      src/boot/bcd.cpp src/app/request_precheck.cpp
TEST_BIN   = $(OBJDIR)/tests.exe
$(TEST_BIN): $(TEST_SRC) $(TEST_UNITS) src/common/rescue_decision.h src/common/version.h tests/tiny_test.h | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(TEST_SRC) $(TEST_UNITS) -o $(TEST_BIN) -static -mconsole -ladvapi32 -lole32 -luuid -lntdll -lbcrypt

# 崩溃探针（不进 make check —— 它会真的崩）。产物在 tests\crash-out\（gitignore）。
CRASH_PROBE = $(OBJDIR)/crash_probe.exe
$(CRASH_PROBE): tests/crash_probe.cpp src/common/crash.cpp src/common/version.h | $(OBJDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tests/crash_probe.cpp src/common/crash.cpp -o $(CRASH_PROBE) -static -mconsole
crash-test: $(CRASH_PROBE)
	-$(CRASH_PROBE) tests\crash-out
	@if exist tests\crash-out\logs\crash\crash-*.dmp (echo CRASH DUMP: OK) else (echo CRASH DUMP: MISSING && exit 1)

# ---- 一致性门禁（check = 单元测试 + 版本脚本自检 + 文档/i18n/宽度/shell 门禁）----
# 冒烟（QEMU）仍封存（tools/vmtest 在 graveyard），单独 make smoke 不可用；见 docs/20。
# 版本资源断言（用户 2026-11-06）：用 PowerShell 真读三个 exe 的 VersionInfo，
# 与 version.h 比对；缺资源/不一致即失败 —— 以后换构建方式也别想悄悄丢掉。
check: $(TEST_BIN) $(CLI_OUT) $(GUI_OUT) $(CHECK_OUT)
	$(TEST_BIN)
	$(PYTHON) tests/test_version.py
	$(PYTHON) tools/check-docs.py
	$(PYTHON) tools/check-i18n.py
	$(PYTHON) tools/check-widths.py
	$(PYTHON) tools/check-shell.py
	$(PYTHON) tools/check-config.py
	$(PYTHON) tools/check-layers.py
	$(PYTHON) tools/check-secrets.py
	$(PYTHON) tools/check-versioninfo.py $(DISTDIR)/$(GUI_NAME) $(DISTDIR)/$(CLI_NAME) $(DISTDIR)/$(CHECK_NAME)
	@bash tests/test_zjswap.sh

# ---- ★ 发布包：根目录 = x64 总入口 + x86 兼容入口；x64/ = x64 真程序；共享资源放根目录 ----
# 需要**两套工具链**：x64 走 PATH 上的 g++（请先把 mingw64\bin 加到 PATH），x86 走绝对路径。
# 递归调用 make 时**必须把 $(MAKE) 的正斜杠换成反斜杠**：mingw32-make 把 $(MAKE) 展开成
# `D:/Prog/.../mingw32-make.exe`，而 shell 是 cmd，正斜杠路径会被判为"系统找不到指定的路径"。
SELF = $(subst /,\,$(MAKE))
package: ea-apply boot-cleanup launchers build/portable_stub.exe build/portable_stub_gui.exe
	@echo === [1/3] 构建 x64 -^> dist/x64 ===
	$(SELF) -f Makefile ARCH=x64 all
	@echo === [2/3] 构建 x86 -^> dist（根，入口）===
	$(SELF) -f Makefile ARCH=x86 all
	@echo === [3/3] 组装共享资源到 dist/ ===
	@if not exist dist\bootfiles mkdir dist\bootfiles
	@if not exist dist\bootfiles\bcd mkdir dist\bootfiles\bcd
# 自足引导资产（2026-10-27，用户反馈 2：极简 PE 无 bootsect/bcdboot/bcdedit）：
#   mbr-boot-code.bin  磁盘 MBR 引导代码（440B，分区向导用）
#   ntfs-boot-code.bin / ntfs-boot-cont.bin  微软 NTFS PBR（426B + 4096B）
#   bcd\bios.bcd / bcd\uefi.bcd  BCD 模板（BIOS 静态；UEFI 运行时替换两处 GUID）
	@copy /Y bootfiles\mbr-boot-code.bin dist\bootfiles\ >nul
	@copy /Y bootfiles\ntfs-boot-code.bin dist\bootfiles\ >nul
	@copy /Y bootfiles\ntfs-boot-cont.bin dist\bootfiles\ >nul
	@copy /Y bootfiles\bcd\bios.bcd dist\bootfiles\bcd\ >nul
	@copy /Y bootfiles\bcd\uefi.bcd dist\bootfiles\bcd\ >nul
	@copy /Y bootfiles\grldr dist\bootfiles\ >nul
	@copy /Y bootfiles\grldr.mbr dist\bootfiles\ >nul
	@copy /Y bootfiles\vmlinuz-zjrestore dist\bootfiles\ >nul
	@copy /Y bootfiles\initramfs-zjrestore.cpio.gz dist\bootfiles\ >nul
	@copy /Y bootfiles\zjrestore-lite.sh dist\bootfiles\ >nul
	@copy /Y bootfiles\zjlog.sh dist\bootfiles\ >nul
	@copy /Y bootfiles\zj-ea-apply.exe dist\bootfiles\ >nul
	@copy /Y bootfiles\zj-restore-boot.exe dist\bootfiles\ >nul
# 单文件封装 stub（PIT-207）：GUI「更多功能>> 打包成单文件」与 `pack-self` 都用它
	@copy /Y build\portable_stub.exe dist\bootfiles\portable-stub.exe >nul
	@copy /Y build\portable_stub_gui.exe dist\bootfiles\portable-stub-gui.exe >nul
# 异机还原：随包放一个 Drivers\ 说明（用户把启动驱动放这里；无 .inf 时仅内置）
	@if not exist dist\Drivers mkdir dist\Drivers
	@copy /Y bootfiles\Drivers-README.txt dist\Drivers\README.txt >nul
# 交付目录不留运行期垃圾：从 dist 直接运行过 exe 时会在 dist\logs 落日志/诊断包
# （LogBaseDir 的"程序目录"分支）——打包时清掉，避免把测试残留当交付物分发。
	@if exist dist\logs rmdir /S /Q dist\logs
	@if not exist dist\bootfiles\sb mkdir dist\bootfiles\sb
# package 只增不删 → 换链时旧资产会残留（grub-ubuntu.efi 曾与 grubx64.efi 并存）。
# 注意：这里必须用 make 的 `#` 注释；命令行注释 `::` 在「单独一条 cmd /c」下不是合法命令。
	@if exist dist\bootfiles\sb\grub-ubuntu.efi del /Q dist\bootfiles\sb\grub-ubuntu.efi >nul
	@copy /Y bootfiles\sb\shimx64.efi dist\bootfiles\sb\ >nul
	@copy /Y bootfiles\sb\grubx64.efi dist\bootfiles\sb\ >nul
	@copy /Y bootfiles\sb\grub.cfg dist\bootfiles\sb\ >nul
	@copy /Y THIRD_PARTY_LICENSES.txt dist\ >nul
	@copy /Y LICENSE dist\ >nul
# 随包使用说明（面向用户；文件名用 ASCII 以免 cmd 在中文代码页下解析出错，内容中文）
	@copy /Y resources\README.txt dist\README.txt >nul
	@if not exist dist\resources\themes\default\main mkdir dist\resources\themes\default\main
	@copy /Y resources\themes\default\global.xml dist\resources\themes\default\ >nul
	@copy /Y resources\themes\default\main\main.xml dist\resources\themes\default\main\ >nul
	@if not exist dist\skin mkdir dist\skin
	@copy /Y skin\main.xml dist\skin\ >nul
	@copy /Y skin\instance.xml dist\skin\ >nul
	@copy /Y skin\confirm.xml dist\skin\ >nul
	@copy /Y skin\passwd.xml dist\skin\ >nul
# 词典（i18n）：AppDir() 定位 <root>\lang\ —— x64\ 会自动上移到 dist\，两套 exe 共用
	@if not exist dist\lang mkdir dist\lang
	@copy /Y lang\*.lang dist\lang\ >nul
	@echo {"name":"SysRecover","version":"$(VERSION)","arch":"x86+x64"} > dist\version.json
	@dir dist\SysRecover.exe dist\SysRecoverUI.exe dist\SysRecoverCheck.exe dist\SysRecover_x86.exe dist\SysRecoverUI_x86.exe dist\SysRecoverCheck_x86.exe dist\x64\SysRecover.exe dist\x64\SysRecoverUI.exe dist\x64\SysRecoverCheck.exe
# 打完包再严格核一次"指针"（version.json ↔ version.h 等）—— 防止"改了源码没打包"
	@$(PYTHON) tools/check-docs.py --strict-dist

# ---- 发布校验：生成 dist/SHA256SUMS.txt（零依赖；发布闸门要求）----
sha256:
	$(PYTHON) tools/write-sha256.py dist

# ---- 单文件（便携）版：7-Zip SFX 自解压（PIT-201，用户 2026-11-01 需求）----
# 交付形态：dist\SysRecover-Portable.exe（GUI）/ SysRecover-Portable-CLI.exe（命令行）。
# 解压到 **固定目录** %LOCALAPPDATA%\SysRecover\app（不是随机 %TEMP%）：
#   · 日志/语言包/皮肤/救援载荷的位置在多次运行之间保持稳定；
#   · 单文件找不到镜像的问题已在 imgsearch 修掉（TEMP 运行兜底扫非系统盘）。
# 7-Zip 只是**构建期工具**（与 Python/QEMU 同类），不随产品分发。
SEVENZIP ?= C:\Program Files\7-Zip\7z.exe
PORTABLE_GUI = dist\SysRecover-Portable.exe
PORTABLE_CLI = dist\SysRecover-Portable-CLI.exe

PORTABLE_STUB = build/portable_stub.exe
PORTABLE_STUB_GUI = build/portable_stub_gui.exe
PORTABLE_RC = build/portable_rc.o
PORTABLE_OUT = dist/SysRecover-Portable.exe

# PIT-204：单文件也要带品牌图标（否则 Explorer/任务栏是白板）。
# 只嵌 ICON，不进清单（清单保持默认 asInvoker：真正要提权的是它拉起的 UI 子进程）。
$(PORTABLE_RC): src/tools/portable.rc resources/SysRecover.ico
	@if not exist build mkdir build
	$(MINGW32)/bin/windres.exe -I . -I resources $< -o $@

$(PORTABLE_STUB): src/tools/portable_main.cpp src/common/xpress.h $(PORTABLE_RC) $(VI_X86_DIR)/portable_stub.o
	@if not exist build mkdir build
	$(MINGW32)/bin/i686-w64-mingw32-g++ -O2 -std=c++17 $(WARN_FLAGS) -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 $< $(PORTABLE_RC) $(VI_X86_DIR)/portable_stub.o -o $@ -static -municode -mconsole -s -lshell32 -luser32

# PIT-215（用户 2026-11-03）：**GUI 子系统版 stub** —— 双击时 Windows 根本不会
# 创建控制台（控制台子系统的进程在进门之前就已经有一个黑框了，只能"藏"不能"没有"）。
# 代价：cmd/PowerShell 不会等 GUI 程序，**退出码拿不到** → 所以另出一个 CLI 版。
$(PORTABLE_STUB_GUI): src/tools/portable_main.cpp src/common/xpress.h $(PORTABLE_RC) $(VI_X86_DIR)/portable_stub_gui.o
	@if not exist build mkdir build
	$(MINGW32)/bin/i686-w64-mingw32-g++ -O2 -std=c++17 $(WARN_FLAGS) -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 $< $(PORTABLE_RC) $(VI_X86_DIR)/portable_stub_gui.o -o $@ -static -municode -mwindows -s -lshell32 -luser32


# ⚠️ 最后一个参数 D:/ 是**约定的发布位置**：每个新版本的单文件直接放 D 盘根目录
# （用户 2026-11 明确要求）。这不是"产物写错盘"，别再改回 dist/ 或加盘根守门。
portable: package $(PORTABLE_STUB) $(PORTABLE_STUB_GUI)
	@echo === portable: single-file pack (GUI=no console flash) ===
	$(PYTHON) tools/portable-pack.py "$(PORTABLE_STUB_GUI)" "$(PORTABLE_OUT)" dist D:/
	@echo === portable: single-file pack (CLI=exit codes) ===
	$(PYTHON) tools/portable-pack.py "$(PORTABLE_STUB)" "$(PORTABLE_CLI)" dist

# ---- 制品/CLI 冒烟（需先 package/all；不碰磁盘）----
dist-check:
	@if exist dist\logs rmdir /S /Q dist\logs
	$(PYTHON) tools/check-dist.py dist
smoke-cli:
	$(PYTHON) tools/smoke-cli.py $(subst /,\,$(DISTDIR))\SysRecover.exe $(subst /,\,$(DISTDIR))\$(CHECK_NAME)

# ---- B 引擎端到端演练（docs/24 P2-4；QEMU + 断言，数分钟；演练盘不入库）----
# 先重建演练盘（陈旧 drill.raw 会报 Cannot find GRLDR），再跑 run-drill 断言。
drill:
	$(PYTHON) tools/vmtest/mk-drill.py
	powershell -ExecutionPolicy Bypass -File tools/vmtest/run-drill.ps1 -Secs 240

.PHONY: all cli gui wimcheck clean package check crash-test smoke drill sha256 dist-check smoke-cli
