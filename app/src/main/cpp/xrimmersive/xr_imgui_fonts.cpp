#include "xr_imgui_fonts.h"

#include "imgui.h"

#include <android/log.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>

#define LOG_TAG "xrimmersive_fonts"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace xrimmersive {
namespace {

struct Candidate {
    const char *path;
    int face;  // in a collection (.ttc)
};

// System fonts with Simplified Chinese, best first. ImGui's stb_truetype reads TrueType (glyf) and
// CFF outlines but not CFF2: Android 12+ ships Noto Sans CJK as a CFF2 variable collection, which
// it cannot parse, so Pico's own TrueType font comes first.
constexpr Candidate kCandidates[] = {
    {"/system/fonts/PICOSansSC-Regular.ttf", 0},
    {"/system/fonts/NotoSansCJK-Regular.ttc", 2},  // faces in fonts.xml order: JP, KR, SC, TC, HK
    {"/system/fonts/NotoSansSC-Regular.otf", 0},
    {"/system/fonts/DroidSansFallbackFull.ttf", 0},
    {"/system/fonts/DroidSansFallback.ttf", 0},
};

// Icons: arrows, technical symbols, enclosed letters, geometric shapes, dingbats. Android ships
// them in Noto Sans Symbols; merged behind the text font, as shadPS4 merges Noto Sans Symbols2.
constexpr Candidate kSymbolCandidates[] = {
    {"/system/fonts/NotoSansSymbols-Regular-Subsetted.ttf", 0},
    {"/system/fonts/NotoSansSymbols-Regular.ttf", 0},
};

struct MappedFont {
    void *data = nullptr;
    size_t size = 0;
    int face = 0;
    const char *path = nullptr;
};

uint32_t Be32(const uint8_t *p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
uint16_t Be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// Whether the face has outlines stb_truetype can draw: glyf or CFF, and no CFF2.
bool Drawable(const uint8_t *data, size_t size, int face) {
    if (size < 12) return false;
    size_t offset = 0;
    if (std::memcmp(data, "ttcf", 4) == 0) {
        const uint32_t faces = Be32(data + 8);
        if (static_cast<uint32_t>(face) >= faces || 12 + 4 * static_cast<size_t>(faces) > size) return false;
        offset = Be32(data + 12 + 4 * face);
    } else if (face != 0) {
        return false;
    }
    if (offset + 12 > size) return false;
    const uint16_t tables = Be16(data + offset + 4);
    if (offset + 12 + 16 * static_cast<size_t>(tables) > size) return false;
    bool outlines = false;
    for (uint16_t i = 0; i < tables; ++i) {
        const uint8_t *tag = data + offset + 12 + 16 * i;
        if (std::memcmp(tag, "CFF2", 4) == 0) return false;
        if (std::memcmp(tag, "glyf", 4) == 0 || std::memcmp(tag, "CFF ", 4) == 0) outlines = true;
    }
    return outlines;
}

// The first candidate that opens and has outlines ImGui can draw, mapped for the process.
MappedFont MapFirst(const Candidate *candidates, size_t count, const char *kind) {
    for (size_t i = 0; i < count; ++i) {
        const Candidate &candidate = candidates[i];
        const int fd = open(candidate.path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        struct stat st{};
        void *data = MAP_FAILED;
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            data = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        }
        close(fd);
        if (data == MAP_FAILED) continue;
        const size_t size = static_cast<size_t>(st.st_size);
        if (!Drawable(static_cast<const uint8_t *>(data), size, candidate.face)) {
            LOGI("%s font %s skipped: no outlines ImGui can draw (CFF2)", kind, candidate.path);
            munmap(data, size);
            continue;
        }
        LOGI("%s font mapped: %s face %d (%zu bytes)", kind, candidate.path, candidate.face, size);
        return {data, size, candidate.face, candidate.path};
    }
    LOGE("No system %s font ImGui can draw", kind);
    return {};
}

const MappedFont &SymbolFontFile() {
    static MappedFont font;
    static std::once_flag once;
    std::call_once(once, [] { font = MapFirst(kSymbolCandidates, std::size(kSymbolCandidates), "symbol"); });
    return font;
}

const MappedFont &CjkFontFile() {
    static MappedFont font;
    static std::once_flag once;
    std::call_once(once, [] { font = MapFirst(kCandidates, std::size(kCandidates), "CJK"); });
    return font;
}

}  // namespace

ImFont *AddCjkFont(ImFontAtlas *atlas, float sizePixels) {
    const MappedFont &file = CjkFontFile();
    if (file.data != nullptr) {
        ImFontConfig config;
        config.FontNo = file.face;
        // The mapping outlives every atlas; the atlas must not free or copy it.
        config.FontDataOwnedByAtlas = false;
        if (ImFont *font = atlas->AddFontFromMemoryTTF(file.data, static_cast<int>(file.size), sizePixels, &config)) {
            // Glyphs the text font lacks (the icons) come from the symbol font.
            const MappedFont &symbols = SymbolFontFile();
            if (symbols.data != nullptr) {
                ImFontConfig merge;
                merge.MergeMode = true;
                merge.FontNo = symbols.face;
                merge.FontDataOwnedByAtlas = false;
                if (!atlas->AddFontFromMemoryTTF(symbols.data, static_cast<int>(symbols.size), sizePixels, &merge)) {
                    LOGE("ImGui could not merge %s; icons are missing", symbols.path);
                }
            }
            return font;
        }
        LOGE("ImGui could not load %s; panels show ASCII only", file.path);
    }
    ImFontConfig fallback;
    fallback.SizePixels = sizePixels;
    return atlas->AddFontDefault(&fallback);
}

}  // namespace xrimmersive
