# GitHub 迁移说明（旧仓库 → 本仓库）

> 状态：**本地准备阶段，尚未推送**。本文只写"推之前要准备什么、推之后旧仓库要挂什么"，
> 不包含任何已经执行的远端操作。
>
> 目标仓库：`https://github.com/WooMonlee/NineRestore`（public）
> 旧仓库：`https://github.com/WooMonlee/SysRecover-Windows-System-Restore-Backup`

## 1. 一句话关系

**旧仓库里的 `SysRecover-Windows-System-Restore-Backup` 是本产品 0.9.x 的历史仓库；
本仓库 `NineRestore` 是同一产品的 1.0 版，代码已重写，今后所有功能、修复与发布都在这里。**

## 2. 两个仓库的对应关系

| 项目 | 旧仓库 | 本仓库 |
|---|---|---|
| 仓库名 | `SysRecover-Windows-System-Restore-Backup` | `NineRestore` |
| 产品版本 | 0.9.x（截至 0.9.96） | 1.0 起（当前 `1.0 RC2` / 内部 `1.0.0.16`） |
| 代码关系 | 1.0 的前身 | **同一产品，代码重写**；不继承旧仓库的提交历史 |
| 状态 | **历史归档，不再追加功能更新** | 活跃开发与发布 |
| 发布 | 保留原有 0.9.x Release | 新的 1.0 Release |
| 产品名 | 旧界面 / 旧启动项使用 `SysRecover` | 对外产品名「九转还原 / NineRestore」，启动项名 `NineRestore` |

> 说明：程序内部仍有 `SysRecover` 字样（exe 文件名、`%LOCALAPPDATA%\SysRecover`、
> `ProgramData\SysRecover`、命令行自检名）。这些是**代码/资源实体**，不是对外品牌名，
> 也不出现在启动项显示名里。启动项显示名已统一为 `NineRestore`。

## 3. 为什么另开新仓库，而不是在旧仓库继续

1. **代码是重写，不是增量改造**：目录结构、模块边界、救援层构建方式都与 0.9.x 不同；
   强行接在旧历史上会让 `git log`、Issue、Release 全部混在一起。
2. **旧仓库名带旧产品名**：继续用旧名会让新用户以为下载到的是同一个 0.9.x 程序。
3. **旧 Release 需要保留**：0.9.x 仍有用户在用在旧机器上，直接删仓库或改历史会造成下载断链。
4. **迁移时机**：审查清单处理完、真机四项验收通过、RC 定稿后，**一次性推送**。
   在此之前不做任何远端操作（不 push、不建仓库、不改旧仓库设置）。

## 4. 旧仓库置顶文案（可直接复制）

### 4.1 旧仓库 README 顶部横幅（中文）

```markdown
> ## ⚠️ 本仓库是 0.9.x 历史版本仓库
>
> 本产品已迁移到新仓库：**https://github.com/WooMonlee/NineRestore**
>
> - 0.9.x 的代码与 Release 保留在这里，仅作历史归档，**不再追加功能更新**；
> - 1.0 是同一产品的**代码重写版**，功能、修复与新发布都在新仓库；
> - 新用户请直接前往新仓库下载：https://github.com/WooMonlee/NineRestore/releases
> - 从 0.9.x 升级到 1.0：可直接覆盖安装，不需要先卸载（详见新仓库 README 的「从 0.9.x 升级」）。
```

### 4.2 旧仓库置顶 Issue（推荐标题与正文）

标题：

```text
【置顶】本仓库为 0.9.x 历史版本，1.0 已迁移至 NineRestore
```

正文：

```markdown
本仓库（SysRecover-Windows-System-Restore-Backup）是 0.9.x 的历史归档。

- 1.0 是同一产品的**代码重写版**，新仓库地址：
  https://github.com/WooMonlee/NineRestore
- 新版本下载：
  https://github.com/WooMonlee/NineRestore/releases
- 0.9.x 的 Release 继续保留，供旧机器回退使用；但**不再追加功能更新**。
- 从 0.9.x 升级到 1.0：可直接覆盖安装，不需要先卸载。旧版安装过的启动菜单
  也不需要先删：
  - BIOS/MBR：1.0 首次启动会自动检测旧 `menu.lst`，把标题、内核参数和救援载荷升级到当前版本；
  - UEFI：ESP 上的旧救援载荷会在下次点「安装到启动」或下一次暂存还原时替换；
    替换前旧菜单仍可用（进入的是旧版救援层），覆盖程序目录不会让它失效；
  - 在 1.0 里再点一次「安装到启动」可把固件里的显示名统一为 NineRestore，
    并自动清理同类的旧名重复条目。
- 反馈渠道不变：QQ 群 1128237828 / 邮箱 ms_w@qq.com。

本 Issue 仅作迁移说明，请不要在此提交 1.0 的问题；1.0 的问题请到新仓库提 Issue。
```

### 4.3 旧仓库 About / Description（英文可选）

```text
Historical repository for NineRestore 0.9.x. The active 1.0 codebase and releases live at
https://github.com/WooMonlee/NineRestore — this repository is archived and receives no new features.
```

## 5. 0.9.x 的 Release 怎么标成"历史版本"

GitHub **没有**名为 "historical" 的发布状态。可选的做法只有下面几种，按推荐顺序：

1. **保留 Release，在正文顶部加历史版本横幅（推荐，必做）**
   每个 0.9.x Release 的说明最上方加：

   ```markdown
   > **历史版本（0.9.x）**：本版本属于旧仓库，已停止功能更新。
   > 1.0 及以后的版本请前往 https://github.com/WooMonlee/NineRestore/releases
   ```

2. **把 0.9.x 标为 Pre-release（可选）**
   仅当旧 Release 仍被 GitHub 判为 "Latest"、且会误导新用户下载旧版时才做。
   勾选后它不再出现在 "Latest" 位置，但下载链接仍然有效。

3. **不要删除旧 Release，也不要删除旧 tag。**
   删除会让旧机器的下载链接失效，也会破坏"从 0.9.x 升级"的可追溯性。

4. **不要修改旧 Release 的附件或哈希。**
   历史版本的价值在于"当时发的就是这份文件"；重新打包会制造无法核对的版本。

## 6. 推送顺序（推的时候才做，现在不做）

1. 旧仓库：先加 README 横幅、置顶 Issue、必要时把 0.9.x 标为 Pre-release。
2. 新仓库：创建 public 仓库 `NineRestore`，默认分支 `main`。
3. 本仓库：配置 remote、首次推送（见 `docs/39-发布与推送检查清单.md` 第 1 节）。
4. 新仓库：打 tag、发布 1.0 Release、上传单文件与 `SHA256SUMS.txt`。
5. 旧仓库：在新 Release 说明里回链旧仓库，形成双向导航。

> 顺序的理由：先让旧仓库"指路"，再让新仓库"接客"；避免出现新仓库已公开、旧仓库还写着
> "最新版本"的窗口期。

## 7. 明确不做的事

- 不删除旧仓库、不归档（archive）旧仓库、不改旧仓库的默认分支。
- 不把旧仓库的历史合并进新仓库，不 force-push 覆盖旧历史。
- 不在旧仓库上继续发 1.0 的 Release。
- 不在迁移过程中改写本仓库的中间提交（是否 squash 见 `docs/39` 的利弊说明，由用户决定）。
