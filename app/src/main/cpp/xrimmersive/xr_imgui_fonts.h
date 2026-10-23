#pragma once

struct ImFontAtlas;
struct ImFont;

namespace xrimmersive {

// A system font with Simplified Chinese (and Latin) at `sizePixels` for an ImGui atlas built with
// RendererHasTextures, so glyphs are rasterised as they are first drawn: Pico's PICOSansSC on
// Swan, else a CJK font stb_truetype can read (not the CFF2 Noto Sans CJK of Android 12+). The file
// is mapped once per process and never copied. Falls back to ImGui's default (ASCII only) font.
// Any thread that owns the atlas.
ImFont *AddCjkFont(ImFontAtlas *atlas, float sizePixels);

}  // namespace xrimmersive
