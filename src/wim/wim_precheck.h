// 九转还原 1.0 —— H4：WIM/ESD **快速结构预检**的纯逻辑部分。
// 设计目的：把"什么算结构性损坏"从文件 I/O 里拆出来，便于单元测试直接喂字节，
// 也便于 wim.cpp / imgsrc.cpp 复用同一套判定（不做两份）。
//
// ⚠️ 边界说明：这里只拦"镜像不完整/被截断/拷贝中断"这类结构问题，
// **不承诺发现块级损坏**（那必须靠显式全量校验 Verify）。
#pragma once

#include <cstddef>
#include <string>

namespace sysrecover {

// 资源表条目（RESHDR_DISK_SHORT，24 字节）：size/flags、offset、原始大小。
// 旧实现只按 16 字节（size+offset）解析，漏掉了 offset_table 与 original_size，
// 导致 xmlData 被当成 offset_table、bootMetadata 被当成 xmlData —— 实测
// （2026-11-06 实机验证）连 DISM 自己生成的合法 WIM 都会被误判"资源表越界"。
#pragma pack(push, 1)
struct WimResEntryRaw {
    unsigned long long sizeFlags;
    unsigned long long offset;
    unsigned long long originalSize;
};

// WIM 头里本预检要用到的字段（前 148 字节；按 WIM 规范顺序）。
struct WimHeaderRaw {
    unsigned char magic[8];
    unsigned int cbSize;
    unsigned int version;
    unsigned int flags;
    unsigned int chunkSize;
    unsigned char guid[16];
    unsigned short partNumber;
    unsigned short totalParts;
    unsigned int imageCount;
    WimResEntryRaw offsetTable;
    WimResEntryRaw xmlData;
    WimResEntryRaw bootMetadata;
    unsigned int bootIndex;
    WimResEntryRaw integrity;
};
#pragma pack(pop)

// 资源表条目：size 高 56 位是大小，低 8 位是标志；offset 是文件内偏移。
inline unsigned long long WimResSize(unsigned long long raw) {
    return raw & 0x00FFFFFFFFFFFFFFull;
}

// WIM 头的 version **不是**"主版本<<16 | 次版本"两段式：wimlib 只认两个值
//   0x00010D00  普通 WIM（LZX/XPRESS 压缩）
//   0x00000E00  固态 ESD（solid/LZMS 压缩；本工具导出的 .esd 就是这种）
// 旧判据按 major=version>>16 判定，固态 ESD 算出 major=0 → 直接拒。
// 后果：0.9.96 能用的 .esd 在 1.0 全部被"镜像版本不受支持"挡在门外
// （2026-11-06 用户实测，docs/34 N13 回归）。
inline bool IsSupportedWimVersion(unsigned int version) {
    if (version == 0x00000E00u) return true;  // 固态 ESD
    const unsigned int major = version >> 16;
    return major >= 1 && major <= 2;  // 普通 WIM：0x10D00、将来 2.x
}

// 稳定原因码（测试直接断言这些字符串，中文文案在 wim.cpp 里映射）。
namespace wim_precheck_reason {
inline constexpr const char* kOk = "";
inline constexpr const char* kBadMagic = "bad-magic";
inline constexpr const char* kBadVersion = "bad-version";
inline constexpr const char* kBadHeaderSize = "bad-header-size";
inline constexpr const char* kShortFile = "file-shorter-than-header";
inline constexpr const char* kNoImages = "no-images";
inline constexpr const char* kResourceOutOfRange = "resource-out-of-range";
inline constexpr const char* kXmlTooLarge = "xml-too-large";
inline constexpr const char* kXmlUnreadable = "xml-unreadable";
inline constexpr const char* kXmlNotParsable = "xml-not-parsable";
inline constexpr const char* kImageCountMismatch = "image-count-mismatch";
}  // namespace wim_precheck_reason

struct WimPrecheckOutcome {
    bool ok = true;
    std::string reason = wim_precheck_reason::kOk;
};

// H5（1.0）：追加前的空间预检判据。
// wimlib_overwrite 会先写一个约等于整个镜像大小的临时文件，再原子改名；
// 因此空闲空间必须 >= 镜像文件大小（相等也允许：临时文件与原文件并存）。
inline bool HasSpaceForWimTemp(unsigned long long freeBytes,
                               unsigned long long imageBytes) {
    return freeBytes >= imageBytes;
}

// 输入：
//   fileSize  —— 镜像文件实际字节数（GetFileSizeEx 结果）
//   hdrLen    —— 已读到的头部字节数（不足 sizeof(WimHeaderRaw) 视为"文件过小"）
//   xmlReadable / xmlOk —— XML 资源是否完整读到、读到的内容（UTF-16 转 ASCII 后）
inline WimPrecheckOutcome CheckWimStructure(unsigned long long fileSize,
                                            const unsigned char* hdr,
                                            std::size_t hdrLen,
                                            bool xmlReadable,
                                            bool xmlTooLarge,
                                            const std::string& xml) {
    WimPrecheckOutcome out;
    if (hdr == nullptr || hdrLen < sizeof(WimHeaderRaw)) {
        out.ok = false;
        out.reason = wim_precheck_reason::kShortFile;
        return out;
    }
    const WimHeaderRaw& h = *reinterpret_cast<const WimHeaderRaw*>(hdr);
    if (!(h.magic[0] == 'M' && h.magic[1] == 'S' && h.magic[2] == 'W' &&
          h.magic[3] == 'I' && h.magic[4] == 'M' && h.magic[5] == 0 &&
          h.magic[6] == 0 && h.magic[7] == 0)) {
        out.ok = false;
        out.reason = wim_precheck_reason::kBadMagic;
        return out;
    }
    if (!IsSupportedWimVersion(h.version)) {
        out.ok = false;
        out.reason = wim_precheck_reason::kBadVersion;
        return out;
    }
    if (h.cbSize < sizeof(WimHeaderRaw) || h.cbSize > 4096) {
        out.ok = false;
        out.reason = wim_precheck_reason::kBadHeaderSize;
        return out;
    }
    if (fileSize < h.cbSize) {
        out.ok = false;
        out.reason = wim_precheck_reason::kShortFile;
        return out;
    }
    if (h.imageCount == 0) {
        out.ok = false;
        out.reason = wim_precheck_reason::kNoImages;
        return out;
    }
    const unsigned long long xmlSize = WimResSize(h.xmlData.sizeFlags);
    const unsigned long long xmlOffset = h.xmlData.offset;
    const unsigned long long bootSize = WimResSize(h.bootMetadata.sizeFlags);
    const unsigned long long bootOffset = h.bootMetadata.offset;
    const unsigned long long intSize = WimResSize(h.integrity.sizeFlags);
    const unsigned long long intOffset = h.integrity.offset;
    auto outOfRange = [&](unsigned long long sz, unsigned long long off) {
        return sz != 0 && (off > fileSize || sz > fileSize - off);
    };
    if (xmlSize == 0 || outOfRange(xmlSize, xmlOffset) ||
        outOfRange(bootSize, bootOffset) || outOfRange(intSize, intOffset)) {
        out.ok = false;
        out.reason = wim_precheck_reason::kResourceOutOfRange;
        return out;
    }
    if (xmlTooLarge || xmlSize > 256ull * 1024 * 1024) {
        out.ok = false;
        out.reason = wim_precheck_reason::kXmlTooLarge;
        return out;
    }
    if (!xmlReadable) {
        out.ok = false;
        out.reason = wim_precheck_reason::kXmlUnreadable;
        return out;
    }
    if (xml.find("<WIM") == std::string::npos ||
        xml.find("</WIM>") == std::string::npos) {
        out.ok = false;
        out.reason = wim_precheck_reason::kXmlNotParsable;
        return out;
    }
    std::size_t nImg = 0, p = 0;
    while ((p = xml.find("<IMAGE", p)) != std::string::npos) {
        const std::size_t after = p + 6;  // "<IMAGE" 之后必须是空白或 '>'
        if (after < xml.size() &&
            (xml[after] == ' ' || xml[after] == '>' || xml[after] == '\t' ||
             xml[after] == '\r' || xml[after] == '\n'))
            ++nImg;
        p = after;
    }
    if (nImg != h.imageCount) {
        out.ok = false;
        out.reason = wim_precheck_reason::kImageCountMismatch;
        return out;
    }
    // 完整性表是**可选**项（wimlib 不写时 size=offset=0，DISM 也可能留一个旧
    // 偏移）。上面已确认"声明了大小就必须落在文件内"，这里不再对"size 为 0
    // 但 offset 非 0"另行拒绝 —— 可选项异常不该把合法镜像整体挡下
    // （2026-11-06 用户要求：宁可强警告 + 继续）。
    return out;
}

// 「完整系统镜像 vs 安装源/残镜像」判据（ops 的 0.7 + 0.8 合流）。
// 输入是**目录表查询结果**，不是文件名/大小：
//   winloadKnown / systemKnown —— 查询本身是否成功（false = 查不了，如加密容器）
//   hasWinload / hasSystemHive —— 镜像里是否真有
//     \Windows\system32\winload.exe    （Win7+ 的加载器）
//     \Windows\System32\config\SYSTEM  （目标系统的注册表）
// 规则：**只有明确查到缺失才拒绝**；查询失败一律放行（由 wimlib/救援层兜底）。
//   缺少 SYSTEM 的多是安装源镜像（install.wim/esd）或只含部分文件的合成镜像，
//   这种镜像还原后必然 0xc000000f —— 真非系统镜像，必须拒（2026-10-06/11-06 实测）。
enum class SystemImageKind {
    kOk,
    kNotWin7Plus,   // 没有 winload.exe：Vista 以前系统 / 安装源 / 残镜像
    kNotRestorable, // 有 winload 但没有 config\SYSTEM：安装源 / 合成镜像
};

inline SystemImageKind ClassifySystemImage(bool winloadKnown, bool hasWinload,
                                           bool systemKnown, bool hasSystemHive) {
    if (winloadKnown && !hasWinload) return SystemImageKind::kNotWin7Plus;
    if (systemKnown && !hasSystemHive) return SystemImageKind::kNotRestorable;
    return SystemImageKind::kOk;
}

}  // namespace sysrecover
