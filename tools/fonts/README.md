# tools/fonts —— 救援层控制台点阵字源

本目录只放**子集**，不放上游整库：

| 文件 | 说明 |
|---|---|
| `rescue-16px.hex` | 入库存档的 16 行点阵子集（95 个 ASCII + 脚本当前用到的汉字） |

上游整库（Unifont/文泉驿等，通常 1 MB 以上、随上游版本漂移）只在本地缓存，
`*.hex.gz`、`*.hex.zst` 已进 `.gitignore`，不入库。

## 格式

与 GNU Unifont 的 `.hex` 相同，逐行 `码位:点阵`：

- 每字 **16 行**；
- 每行 **8 列**（32 个 hex 字符）或 **16 列**（64 个 hex 字符）；
- `#` 开头为注释；子集文件头由生成器写入上游文件名与内容 SHA-256。

ASCII（U+0020..U+007E）必须齐全，且落在自己的码位上；汉字必须是 16 列。

## 生成与复现

```sh
# 1) 从上游整库导出子集（只写本目录的 rescue-16px.hex，不动资产）
python tools/gen-rescue-font.py --from-unifont <上游 .hex 或 .hex.gz>

# 2) 生成 bootfiles/zjfont.psf + bootfiles/zjzhmap.sed
python tools/gen-rescue-font.py

# 3) 校验 / 逐字节复现
python tools/gen-rescue-font.py --check
python tools/gen-rescue-font.py --verify
```

生成器**零第三方依赖**（不需要 freetype、不需要系统字体），任何人拿到仓库都能
复现出同一份字节。

## 四条硬约束（改格式前先读）

1. PSF1 最多 **512 个字形**，内核 `max_font_glyphs` 也是 512 —— 每汉字占 2 槽；
2. 汉字必须靠 **左右半 + PUA（U+E000 起）双码位** 拼成 16×16，否则被内核按双宽
   字符"第一格画整字、第二格填空格"挤成 8px；
3. 本镜像 busybox `setfont` **只吃 PSF1 mode 0x03**（PSF2 实测被拒）；
4. ASCII 必须留在 **0x20..0x7E 恒等码位**：VT 擦屏/滚屏直接写 0x20，空格移位会满屏乱码。

## 字源选型

**已选定：GNU Unifont 18.0.01（16×16），SIL OFL 1.1**（上游另有 GPLv2+字体例外双许可，
本产品按 OFL 分发）。上游整库 `unifont-18.0.01.hex.gz` 不入库，只入库本目录的子集。

换源须同时补齐 `THIRD_PARTY_LICENSES.txt` 的五个字段：
名称与版本 / 上游 URL / 内容 SHA-256 / 许可正文 / 修改声明（已子集化并切半重排）；
缺任一项视为发布门禁不通过，不得出包（见 `docs/06-构建发布与许可合规.md` §5）。
