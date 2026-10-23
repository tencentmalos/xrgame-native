#include "xr_imgui_style.h"

#include <algorithm>
#include <cfloat>

namespace xrimmersive::style {

namespace {

ImVec4 Color(ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); }

}  // namespace

ImU32 Lighter(ImU32 color, int amount) {
    auto channel = [&](int shift) {
        return static_cast<ImU32>(std::clamp(static_cast<int>((color >> shift) & 0xFF) + amount, 0, 255)) << shift;
    };
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) |
           (color & IM_COL32_A_MASK);
}

void ApplyPanelStyle(ImGuiStyle &style) {
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 24.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 22.0f;  // pill-shaped choices
    style.PopupRounding = 12.0f;
    style.ScrollbarRounding = 9.0f;
    style.ScrollbarSize = 16.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = {32.0f, 28.0f};
    style.FramePadding = {16.0f, 8.0f};
    style.ItemSpacing = {12.0f, 10.0f};
    ImVec4 *c = style.Colors;
    c[ImGuiCol_WindowBg] = Color(kCard);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Border] = Color(kCardBorder);
    c[ImGuiCol_Separator] = Color(kSeparator);
    c[ImGuiCol_Text] = Color(kText);
    c[ImGuiCol_TextDisabled] = Color(kTextDim);
    c[ImGuiCol_Button] = Color(kButton);
    c[ImGuiCol_ButtonHovered] = Color(kButtonHovered);
    c[ImGuiCol_ButtonActive] = Color(kButtonActive);
    // Drop-downs: a pill like the choices, a dark list with the selected row in blue.
    c[ImGuiCol_FrameBg] = Color(kButton);
    c[ImGuiCol_FrameBgHovered] = Color(Lighter(kButton, 18));
    c[ImGuiCol_FrameBgActive] = Color(kButtonActive);
    c[ImGuiCol_PopupBg] = Color(IM_COL32(38, 38, 42, 252));
    c[ImGuiCol_Header] = Color(kSelected);
    c[ImGuiCol_HeaderHovered] = Color(kButtonHovered);
    c[ImGuiCol_HeaderActive] = Color(kButtonActive);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = Color(kCardBorder);
    c[ImGuiCol_ScrollbarGrabHovered] = Color(kButtonHovered);
    c[ImGuiCol_ScrollbarGrabActive] = Color(kButtonActive);
}

void ApplyKeyboardStyle(ImGuiStyle &style) {
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 18.0f;
    style.FrameRounding = 10.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    ImVec4 *c = style.Colors;
    c[ImGuiCol_WindowBg] = Color(kKeyboardBase);
    c[ImGuiCol_Border] = Color(kKeyboardLine);
    c[ImGuiCol_Text] = Color(kKeyText);
    c[ImGuiCol_Button] = Color(kKey);
    c[ImGuiCol_ButtonHovered] = Color(Lighter(kKey, 28));
    c[ImGuiCol_ButtonActive] = Color(kKeySpecial);
}

void DrawButtonBadge(ImDrawList *draw, ImVec2 center, float radius, const char *letter, ImU32 fill) {
    draw->AddCircleFilled(center, radius, fill, 32);
    draw->AddCircle(center, radius, IM_COL32(255, 255, 255, 90), 32, 1.5f);
    ImFont *font = ImGui::GetFont();
    const float size = radius * 1.35f;
    const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0.0f, letter);
    draw->AddText(font, size, {center.x - text.x * 0.5f, center.y - text.y * 0.5f}, IM_COL32(16, 18, 22, 255), letter);
}

void DrawFocus(ImDrawList *draw, ImVec2 min, ImVec2 max, float rounding) {
    draw->AddRectFilled(min, max, kFocusFill, rounding);
    draw->AddRect({min.x + 2.0f, min.y + 2.0f}, {max.x - 2.0f, max.y - 2.0f}, kFocusBorder, rounding, 0, 2.0f);
}

bool IconButton(const char *id, const char *icon, const char *label, ImVec2 size, ImU32 fill, const char *badge,
                ImU32 badgeColor) {
    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Lighter(fill, 30));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Lighter(fill, -20));
    const bool pressed = ImGui::Button(id, size);
    ImGui::PopStyleColor(3);

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float height = max.y - min.y;
    const float gap = height * 0.18f;
    const float badgeRadius = height * 0.24f;
    const ImVec2 iconSize = icon != nullptr ? ImGui::CalcTextSize(icon) : ImVec2(0, 0);
    const ImVec2 labelSize = label != nullptr ? ImGui::CalcTextSize(label) : ImVec2(0, 0);
    float width = labelSize.x;
    if (icon != nullptr) width += iconSize.x + (label != nullptr ? gap : 0.0f);
    if (badge != nullptr) width += 2.0f * badgeRadius + gap;
    float x = min.x + (max.x - min.x - width) * 0.5f;
    const float centerY = (min.y + max.y) * 0.5f;
    if (badge != nullptr) {
        DrawButtonBadge(draw, {x + badgeRadius, centerY}, badgeRadius, badge, badgeColor);
        x += 2.0f * badgeRadius + gap;
    }
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    if (icon != nullptr) {
        draw->AddText({x, centerY - iconSize.y * 0.5f}, text, icon);
        x += iconSize.x + gap;
    }
    if (label != nullptr) draw->AddText({x, centerY - labelSize.y * 0.5f}, text, label);
    return pressed;
}

}  // namespace xrimmersive::style
