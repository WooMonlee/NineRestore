# 更新日志

> 版本号唯一来源是 `src/common/version.h`；发布时 `version.h` / git tag / `dist/version.json` 三处同源。

## 1.0.0-beta.1（2026-11-06）

> **这是测试版（Beta），不是正式版。** 二进制未做代码签名，也未在实机上跑通完整备份/还原闭环；
> 面向真实磁盘使用前请先在不重要的机器或虚拟机上验证。内部版本串 `1.0.0-beta.1`，
> 界面显示名「1.0.0 测试版」（英文界面显示 `1.0.0 Beta`）。

基线：SysRecover **0.9.96**（commit `8401cdab`）。本版是「九转还原」工程化重构 + P0/P1 整改，
不改变产品定位与磁盘格式（镜像仍是 WIM/ESD，加密镜像仍是 ZJENC1）。

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

## 版本策略（1.0）

| 阶段 | 标识 | 说明 |
|---|---|---|
| 开发 | `1.0.0`（`version.h`）| 主分支持续修复；每次提交前跑双架构构建 + `make check` |
| 测试版 | `1.0.0-beta.N` + tag `v1.0.0-beta.N` | `version.h` 写完整预发布串（如 `1.0.0-beta.1`），界面显示「1.0.0 测试版」；供小范围试用，不加功能 |
| 发布候选 | `1.0.0-rc.N` + tag `v1.0.0-rc.N` | RC 只修 P0/P1，不加功能 |
| 正式发布 | `1.0.0` + tag `v1.0.0` | 7 天观察期无新的 P0/P1、发布闸门全绿（`docs/26` §2）后打 tag |

- 预发布后缀（`-beta.N` / `-rc.N`）由人指定，`tools/version.py --bump` 不会自动加；
  `bump()` 会丢弃后缀并把补丁号 +1。
- 修订号规则：主/次版本由人指定，补丁号每解决一个问题 +1（`python tools/version.py --bump`）。
- 跨层契约版本单独计（`SYSRECOVER_CONTRACT_VERSION`），只有改字段才递增。
- 发布物：`dist/` 目录版 + `dist/SHA256SUMS.txt`；单文件版由 `make portable` 生成（可选）。
- **签名顺序约束**：若以后做代码签名，必须**先打包、再签名** —— 单文件是"stub + 追加载荷"，
  签名之后再往 PE 末尾追加数据会让签名失效。
