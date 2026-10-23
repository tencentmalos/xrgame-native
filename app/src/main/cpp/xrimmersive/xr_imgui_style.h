#pragma once

#include "imgui.h"

// The look of the XR panels drawn with ImGui (launch panel, keyboard), after shadPS4's ImGui UI:
// its card dialog's palette (np_profile_dialog_ui.cpp) for panels, its IME keyboard's palette
// (ime_kb_layout.h) for the keyboard, icons as symbol-font glyphs (shadPS4 merges Noto Sans
// Symbols2; xr_imgui_fonts.cpp merges Android's Noto Sans Symbols) and controller glyphs and focus
// marks drawn with the draw list.
namespace xrimmersive::style {

// Icons, UTF-8, from the merged symbol font.
inline constexpr const char *kPlay = "\xE2\x96\xB6";       // U+25B6
inline constexpr const char *kPrev = "\xE2\x97\x80";       // U+25C0
inline constexpr const char *kClose = "\xE2\x9C\x95";      // U+2715
inline constexpr const char *kCheck = "\xE2\x9C\x93";      // U+2713
inline constexpr const char *kBackspace = "\xE2\x8C\xAB";  // U+232B
inline constexpr const char *kReturn = "\xE2\x8F\x8E";     // U+23CE
inline constexpr const char *kShift = "\xE2\x87\xA7";      // U+21E7
inline constexpr const char *kCaps = "\xE2\x87\xAA";       // U+21EA
inline constexpr const char *kKeyboard = "\xE2\x8C\xA8";   // U+2328
inline constexpr const char *kGear = "\xE2\x9A\x99";       // U+2699
inline constexpr const char *kWarning = "\xE2\x9A\xA0";    // U+26A0
inline constexpr const char *kExpand = "\xE2\x96\xB8";     // U+25B8
inline constexpr const char *kCollapse = "\xE2\x96\xBE";   // U+25BE
inline constexpr const char *kSpace = "\xE2\x90\xA3";      // U+2423

// Panels: shadPS4's card dialog.
inline constexpr ImU32 kCard = IM_COL32(28, 28, 31, 255);
inline constexpr ImU32 kCardBorder = IM_COL32(82, 82, 89, 255);
inline constexpr ImU32 kHeaderTop = IM_COL32(0, 87, 158, 255);
inline constexpr ImU32 kHeaderBottom = IM_COL32(0, 56, 107, 255);
inline constexpr ImU32 kSeparator = IM_COL32(56, 56, 61, 255);
inline constexpr ImU32 kText = IM_COL32(242, 242, 245, 255);
inline constexpr ImU32 kTextDim = IM_COL32(166, 166, 173, 255);
inline constexpr ImU32 kButton = IM_COL32(46, 46, 51, 255);
inline constexpr ImU32 kButtonHovered = IM_COL32(0, 112, 191, 255);
inline constexpr ImU32 kButtonActive = IM_COL32(0, 84, 153, 255);
inline constexpr ImU32 kSelected = IM_COL32(0, 112, 191, 255);
inline constexpr ImU32 kFocusBorder = IM_COL32(248, 248, 248, 255);
inline constexpr ImU32 kFocusFill = IM_COL32(255, 255, 255, 18);
inline constexpr ImU32 kWarningColor = IM_COL32(240, 184, 64, 255);
inline constexpr ImU32 kAccentLight = IM_COL32(107, 200, 255, 255);

// Keyboard: shadPS4's IME keyboard.
inline constexpr ImU32 kKeyboardBase = IM_COL32(18, 18, 18, 246);
inline constexpr ImU32 kKeyboardLine = IM_COL32(70, 70, 70, 255);
inline constexpr ImU32 kTextField = IM_COL32(22, 37, 60, 255);
inline constexpr ImU32 kKey = IM_COL32(35, 35, 35, 255);
inline constexpr ImU32 kKeyFunction = IM_COL32(60, 60, 60, 255);
inline constexpr ImU32 kKeySpecial = IM_COL32(30, 90, 170, 255);
inline constexpr ImU32 kKeyText = IM_COL32(230, 230, 230, 255);

// Controller buttons, in the colours players know them by.
inline constexpr ImU32 kBadgeA = IM_COL32(74, 205, 120, 255);
inline constexpr ImU32 kBadgeB = IM_COL32(232, 90, 90, 255);
inline constexpr ImU32 kBadgeX = IM_COL32(70, 150, 250, 255);
inline constexpr ImU32 kBadgeY = IM_COL32(240, 200, 70, 255);

// The card dialog's style values (rounding, padding, borders, colours) on `style`.
void ApplyPanelStyle(ImGuiStyle &style);
// The IME keyboard's.
void ApplyKeyboardStyle(ImGuiStyle &style);

// Brightens `color` by `amount` (0-255) per channel, for hovered and pressed variants.
ImU32 Lighter(ImU32 color, int amount);

// A controller button glyph: a filled circle with the button's letter.
void DrawButtonBadge(ImDrawList *draw, ImVec2 center, float radius, const char *letter, ImU32 fill);

// The focus mark of shadPS4's keyboard: a faint white overlay and a 2 px light border.
void DrawFocus(ImDrawList *draw, ImVec2 min, ImVec2 max, float rounding);

/**
 * A button whose face shows, centred: a controller badge (when `badge` is set), an icon (when set)
 * and a label. `fill` is its colour; hovering and pressing brighten it. True when clicked.
 */
bool IconButton(const char *id, const char *icon, const char *label, ImVec2 size, ImU32 fill,
                const char *badge = nullptr, ImU32 badgeColor = kBadgeA);

}  // namespace xrimmersive::style
