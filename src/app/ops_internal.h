// 九转还原：ops 拆分后的跨模块内部声明（阶段 2 机械拆分产物）。
//
// 来源：0.9.96 的 src/app/ops.cpp 把这些辅助函数定义在匿名命名空间里（单一
// 翻译单元，天然内部可见）。按模块拆分后，它们需要跨翻译单元复用，因此统一
// 在这里声明、在对应模块里定义。
//
// 约束：本头文件只做**可见性**迁移，函数签名与函数体保持逐字不变；不含任何
// 行为改动。阶段 3 起的整改不得修改这里的签名而不更新全部调用点。
//
// 注意：只声明「已拆出模块」里的函数。若某函数仍留在 ops.cpp 的匿名命名空间，
// 这里重复声明会让匿名命名空间的隐式 using 与本声明形成二义性（重载解析冲突）。
#pragma once

#include <windows.h>

#include <string>

#include "../disk/disk.h"
#include "ops.h"

namespace sysrecover {

// ── ops_util.cpp ──
bool IsNetworkPath(const std::wstring& p);
std::wstring MenuFlagPath();
bool HasInstalledMenu();
void SetMenuFlag(bool installed);
std::string W2U(const std::wstring& w);
std::wstring U2W(const std::string& s);
std::wstring ExePath();
bool IsDir(const std::wstring& p);
// M9：长路径前缀（\\?\ / \\?\UNC\）；已带前缀或相对路径原样返回。
std::wstring LongPath(const std::wstring& p);
void DeleteTreeRec(const std::wstring& dir);
bool CopyDir(const std::wstring& src, const std::wstring& dst);
bool SameDrive(wchar_t a, wchar_t b);
void AppendHistory(const char* action, const std::wstring& image,
                   const std::wstring& target, unsigned long long ms,
                   const char* detail);

// ── ops_format.cpp ──
// 目标卷准备的唯一入口（格式化 / 只清空内容）。实现细节（FormatTargetVolume /
// WipeVolumeContents）留在 ops_format.cpp 内部，防止调用方绕过统一入口。
int PrepareTargetVolume(const std::wstring& root, wchar_t letter, bool formatIt,
                        std::string& log, std::string* rawOut);

// ── ops_preserve.cpp ──
int PreserveOldSystem(const std::wstring& root, std::string& log);
void RollbackPreservedOld(const std::wstring& root, std::string& log);

// ── ops_boot.cpp ──
void EspDiagAppendAt(const std::wstring& dir, const std::string& text);
void EspDiagLine(const std::string& s);
void EspDiagVolumes();
PartitionStyle DiskStyleOf(uint32_t diskIndex);
bool MountEspByGuid(const std::wstring& volumeOrGuid, std::wstring& root,
                    std::string& log);
std::wstring FindEspDriveByLabel(const PartitionInfo& esp);
bool AcquireEspRoot(std::wstring& espRoot, bool& guidMatched, std::string& log,
                    int targetDisk = -1);
bool BiosBcdLooksValid(wchar_t letter, std::string& detail);
bool ToolAvailable(const wchar_t* exe);
std::string BootWriteBlockedHint();

// ── ops_inplace.cpp ──
bool SetComputerNameOffline(const std::wstring& winDir, const std::wstring& name,
                            std::string& log);
bool InjectOfflineRunOnce(const std::wstring& newDir, const wchar_t* hiveName,
                          const wchar_t* valueName, const wchar_t* cmd,
                          DWORD type, const char* what);
bool InstallFirstBootCleanup(const std::wstring& newDir,
                             const std::wstring& exeDir, std::string& log);
// StageRestoreImpl 在「PE 里放行」分支直接调用就地还原（0.9.96 同一翻译单元，
// ops.h 未导出；拆分后由本头文件承担跨模块声明）。
int RunDirectRestore(const RestoreRequest& req, const PartitionInfo& target,
                     const std::wstring& imagePath, int espIndex, int eaIndex,
                     std::string& err, ProgressFn progress);

}  // namespace sysrecover
