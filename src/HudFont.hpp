#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

// 5x7 bitmap font, the one-channel text atlas sampled by shaders/hud.frag and the HUD layout in units.
// The layout constants below are the single source of truth; the host passes them as uniforms.
namespace hud {

struct Rect {
    int x;
    int y;
    int w;
    int h;
};

inline constexpr std::array<std::string_view, 8> kBrushNames{"SAND", "WATER", "SOLID", "ERASE", "SMOKE", "FIRE", "WOOD", "OIL"};
// The help panel is key labels (kHelpKeys) beside their descriptions (kHelpActions), row by row.
inline constexpr std::array<std::string_view, 6> kHelpKeys{"LMB", "1-8", "[ ]", "F1-F10", "H", "ESC"};
inline constexpr std::array<std::string_view, 6> kHelpActions{"PAINT", "SELECT", "SIZE", "VIEWS", "HELP", "QUIT"};
inline constexpr std::string_view kHintKey = "H";
inline constexpr std::string_view kHintText = "HELP";
// Glyph strip for numbers: digit d is cell d, 'R' (cell 10) labels the brush radius readout.
inline constexpr std::string_view kDigits = "0123456789R";

inline constexpr int kAdvance = 6;  // five glyph columns plus one spacing column
inline constexpr int kGlyphWidth = 5;
inline constexpr int kGlyphHeight = 7;
inline constexpr int kRowPitch = kGlyphHeight + 1;  // atlas row pitch

constexpr int TextWidth(std::size_t characters) {
    return static_cast<int>(characters) * kAdvance - 1;
}

template <std::size_t N>
constexpr int WidestText(const std::array<std::string_view, N>& lines) {
    std::size_t widest = 0;
    for (const std::string_view line : lines) widest = std::max(widest, line.size());
    return TextWidth(widest);
}

// Atlas rects, top to bottom: key labels, action labels, hint, digit strip, brush names.
constexpr int KeyY(std::size_t index) {
    return static_cast<int>(index) * kRowPitch;
}
constexpr Rect KeyRect(std::size_t index) {
    return {0, KeyY(index), TextWidth(kHelpKeys[index].size()), kGlyphHeight};
}
constexpr int ActionY(std::size_t index) {
    return KeyY(kHelpKeys.size()) + static_cast<int>(index) * kRowPitch;
}
constexpr Rect ActionRect(std::size_t index) {
    return {0, ActionY(index), TextWidth(kHelpActions[index].size()), kGlyphHeight};
}
inline constexpr Rect kHintKeyRect{0, ActionY(kHelpActions.size()), TextWidth(kHintKey.size()), kGlyphHeight};
inline constexpr Rect kHintTextRect{0, kHintKeyRect.y + kRowPitch, TextWidth(kHintText.size()), kGlyphHeight};
// w is the cell pitch of the digit strip, not its total width.
inline constexpr Rect kDigitRect{0, kHintTextRect.y + kRowPitch, kAdvance, kGlyphHeight};
inline constexpr int kLabelY = kDigitRect.y + kRowPitch;

constexpr Rect LabelRect(std::size_t index) {
    return {0, kLabelY + static_cast<int>(index) * kRowPitch, WidestText(kBrushNames), kGlyphHeight};
}

inline constexpr int kAtlasWidth = TextWidth(kDigits.size());
inline constexpr int kAtlasHeight = LabelRect(kBrushNames.size()).y;
static_assert(WidestText(kHelpKeys) <= kAtlasWidth && WidestText(kHelpActions) <= kAtlasWidth &&
                  WidestText(kBrushNames) <= kAtlasWidth,
              "atlas must be as wide as its widest text");

// Layout in HUD units (one unit is uiScale framebuffer pixels). Text sits on whole atlas texels and every plate and
// tile edge falls on a unit, so the HUD is crisp pixel art at any integer scale. The stack is the inventory plate
// (a row of eight slots, their key numbers, the selected material and the brush radius) with the help panel below it,
// anchored top-left. It is sized to sit inside the letterbox margin of a 1280x900 window (190 px each side at scale
// 2) and falls back to overlaying the corner of the simulation when the margin is narrower (see FitsInMargin).
inline constexpr int kEdge = 2;  // stack distance from the window edges
inline constexpr int kPlatePad = 2;
inline constexpr int kSlotCount = static_cast<int>(kBrushNames.size());
inline constexpr int kSlotSize = 10;  // eight slots with this edge and padding fill the 190 px margin at scale 2
inline constexpr int kSlotGap = 1;
inline constexpr int kIconSize = 8;  // pixel-art motif inside a slot
inline constexpr int kSlotRowWidth = kSlotCount * kSlotSize + (kSlotCount - 1) * kSlotGap;
inline constexpr int kPanelWidth = kSlotRowWidth + 2 * kPlatePad;
inline constexpr int kDigitGap = 2;  // slot row to key numbers
inline constexpr int kInfoGap = 3;   // key numbers to the material name and radius row
inline constexpr int kInventoryHeight =
    kPlatePad + kSlotSize + kDigitGap + kGlyphHeight + kInfoGap + kGlyphHeight + kPlatePad;
inline constexpr int kPanelGap = 3;  // inventory plate to help panel (or hint)
inline constexpr int kHelpRowPitch = kGlyphHeight + 3;
inline constexpr int kHelpColumnGap = 6;
inline constexpr int kKeyColumn = WidestText(kHelpKeys);
inline constexpr int kHelpPanelHeight =
    2 * kPlatePad + static_cast<int>(kHelpKeys.size()) * kHelpRowPitch - (kHelpRowPitch - kGlyphHeight);
inline constexpr int kHintKeyGap = 4;  // between the hint's key and its text
static_assert(kKeyColumn + kHelpColumnGap + WidestText(kHelpActions) <= kSlotRowWidth, "help rows must fit the plate");
static_assert(kHintKeyRect.w + kHintKeyGap + kHintTextRect.w <= kSlotRowWidth, "hint must fit the plate");

// Left edge of slot `index` in units from the window's left edge; the slot row's top is kSlotTop.
constexpr int SlotLeft(int index) {
    return kEdge + kPlatePad + index * (kSlotSize + kSlotGap);
}
inline constexpr int kSlotTop = kEdge + kPlatePad;
// The stack fits the left letterbox margin when `margin_pixels` holds it with the same gap on its far side.
constexpr bool FitsInMargin(int margin_pixels, int scale) {
    return margin_pixels >= (kEdge + kPanelWidth + kEdge) * scale;
}
static_assert(FitsInMargin(190, 2), "the stack must fit the 1280x900 window's letterbox margin");
// Seconds the help panel stays up after the app starts or H shows it.
inline constexpr double kHelpSeconds = 8.0;

namespace detail {

struct Glyph {
    char character;
    std::array<std::uint8_t, kGlyphHeight> rows;  // top to bottom, bit 4 is the leftmost column
};

inline constexpr std::array<Glyph, 39> kGlyphs{{
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}},
    {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
}};

}  // namespace detail

// Renders every string above into kAtlasWidth x kAtlasHeight texels (0 or 255). Unknown characters throw
// instead of silently drawing blanks.
inline std::vector<std::uint8_t> BakeAtlas() {
    std::vector<std::uint8_t> texels(static_cast<std::size_t>(kAtlasWidth * kAtlasHeight), 0U);
    const auto draw = [&](const Rect& rect, std::string_view text) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == ' ') continue;
            const auto glyph = std::find_if(detail::kGlyphs.begin(), detail::kGlyphs.end(),
                                            [&](const detail::Glyph& entry) { return entry.character == text[i]; });
            if (glyph == detail::kGlyphs.end()) throw std::runtime_error("HUD font has no glyph for a baked character.");
            for (int row = 0; row < kGlyphHeight; ++row) {
                for (int column = 0; column < kGlyphWidth; ++column) {
                    if (((glyph->rows[static_cast<std::size_t>(row)] >> (kGlyphWidth - 1 - column)) & 1U) == 0U) continue;
                    const int x = rect.x + static_cast<int>(i) * kAdvance + column;
                    texels[static_cast<std::size_t>((rect.y + row) * kAtlasWidth + x)] = 255U;
                }
            }
        }
    };
    for (std::size_t i = 0; i < kHelpKeys.size(); ++i) draw(KeyRect(i), kHelpKeys[i]);
    for (std::size_t i = 0; i < kHelpActions.size(); ++i) draw(ActionRect(i), kHelpActions[i]);
    draw(kHintKeyRect, kHintKey);
    draw(kHintTextRect, kHintText);
    draw(kDigitRect, kDigits);
    for (std::size_t i = 0; i < kBrushNames.size(); ++i) draw(LabelRect(i), kBrushNames[i]);
    return texels;
}

}  // namespace hud
