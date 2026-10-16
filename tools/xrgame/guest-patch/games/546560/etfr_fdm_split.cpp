// SPDX-License-Identifier: GPL-3.0-or-later
// Half-Life: Alyx (546560), renderingpipelines.dll: one foveal region per eye in the fragment
// density map of the single-layer stereo mode.
//
// The game builds one static FDM per render target size (ceil(RT / 32) texels) and moves it with
// VK_EXT_fragment_density_map_offset. Its pattern is one square (or circle) of full density
// centred in the whole map. With true multiview each layer is one eye, so that is right; in the
// default mode (multiview instancing) both eyes share one double-wide layer, and the full-density
// region sits on the seam between the eyes. For a single-layer map wider than tall this builds the
// game's own pattern twice, at half width, and places one in each eye's half. The offset package
// (etfr_offset_eye) scales the gaze offset to one eye's width to match.

namespace {
constexpr xrg_u64 kSplit = 1; // counter: maps split per eye, last = map width in texels
constexpr xrg_u64 kKept = 2;  // counter: maps left as the game builds them, last = layer count
constexpr xrg_u64 kSize = 1;  // log: a split map's width << 16 | height

// MSVC std::vector<unsigned char>.
struct ByteVector {
    unsigned char* first;
    unsigned char* last;
    unsigned char* end;
};

// Layer count of the map being created; 0 outside the game's create function.
unsigned g_layers;
// One eye's pattern. The game's builder appends to it, so it must never need to grow: a map of
// 260x111 texels (two 4160-pixel eyes) needs 2 * 130 * 111 bytes per eye.
alignas(16) unsigned char g_half[64 * 1024];
} // namespace

extern "C" {
// renderingpipelines+0x16C70: returns the FDM texture of an RT size, creating it on first use.
void* patched_create_fdm(void* out, int rt_width, int rt_height, unsigned layers) {
    g_layers = layers;
    void* result = original_create_fdm(out, rt_width, rt_height, layers);
    g_layers = 0;
    return result;
}

// renderingpipelines+0x5450: appends width * height texels of two bytes (R8G8 density) to `out`,
// which the caller has reserved for that and emptied.
xrg_i64 patched_fdm_pattern(int level, char square, unsigned width, unsigned height, void* vector) {
    ByteVector* out = static_cast<ByteVector*>(vector);
    const unsigned left = width / 2;
    const unsigned right = width - left;
    const xrg_u64 bytes = 2ull * width * height;
    if (g_layers != 1 || width <= height || left == 0 || out->last != out->first ||
        static_cast<xrg_u64>(out->end - out->first) < bytes || 2ull * right * height > sizeof(g_half)) {
        xrg_sdk_counter(kKept, g_layers);
        return original_fdm_pattern(level, square, width, height, vector);
    }
    for (unsigned eye = 0; eye < 2; ++eye) {
        const unsigned eye_width = eye ? right : left;
        ByteVector half{g_half, g_half, g_half + sizeof(g_half)};
        original_fdm_pattern(level, square, eye_width, height, &half);
        if (static_cast<xrg_u64>(half.last - half.first) != 2ull * eye_width * height) {
            out->last = out->first;
            xrg_sdk_counter(kKept, g_layers);
            return original_fdm_pattern(level, square, width, height, vector);
        }
        for (unsigned y = 0; y < height; ++y)
            memcpy(out->first + 2ull * (static_cast<xrg_u64>(y) * width + (eye ? left : 0)),
                   g_half + 2ull * y * eye_width, 2ull * eye_width);
    }
    out->last = out->first + bytes;
    xrg_sdk_counter(kSplit, width);
    xrg_sdk_log(kSize, static_cast<xrg_i64>(width) << 16 | height);
    return 0;
}
}
