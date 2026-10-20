#version 430

// Minimal HUD drawn over the composited simulation with premultiplied-alpha blending.
// Layout is in "units" of uiScale framebuffer pixels (the numbers come from src/HudFont.hpp), and every plate, tile and
// glyph edge falls on a unit, so the pixel art stays crisp at any integer scale. Only the brush ring is anti-aliased.
// The stack (inventory plate and help panel) is anchored top-left: in a 1280x900 window that is inside the letterbox
// margin, and with no margin wide enough (`overlay`) it overlays the corner of the simulation instead. Every region
// tests its bounding box first, so the pass is nearly free outside the plates.
// All motion arrives as eased uniforms (helpMix, slotPop, painting); nothing animates on its own.

layout(location = 0) out vec4 outColor;

uniform sampler2D fontTex;
uniform ivec2 fbSize;
uniform ivec4 simRect;    // x, y, width, height of the simulation viewport, origin bottom-left
uniform ivec2 gridSize;
uniform int uiScale;
uniform int brushMode;
uniform int brushRadius;
uniform ivec2 brushCell;
uniform int cursorValid;
uniform float painting;   // 0..1, eased "mouse held over the simulation"
uniform float helpMix;    // 0..1, eased help panel visibility; the hint shows the remainder
uniform float slotPop[8]; // 0..1, eased selection of each slot
uniform int overlay;      // 1: no margin wide enough, the plates are a little more transparent over the world
uniform int edge;         // stack distance from the window edges, units
uniform int pad;          // plate padding, units
uniform ivec4 inventory;  // panel width, panel height, slot size, slot gap; units
uniform ivec4 spacing;    // digit gap, info gap, panel gap, icon size; units
uniform ivec4 helpLayout; // help panel height, row pitch, column gap, key column; units
uniform ivec4 keyRects[6];
uniform ivec4 actionRects[6];
uniform ivec4 hintKeyRect;
uniform ivec4 hintTextRect;
uniform ivec4 digitRect;  // z is the cell pitch of the digit strip
uniform ivec4 labelRects[8];

const vec3 kRingTints[8] = vec3[8](
    vec3(1.00, 0.84, 0.50),  // sand
    vec3(0.50, 0.86, 0.92),  // water
    vec3(0.80, 0.86, 0.96),  // solid
    vec3(1.00, 0.34, 0.28),  // erase
    vec3(0.80, 0.78, 0.84),  // smoke
    vec3(1.00, 0.62, 0.26),  // fire
    vec3(0.80, 0.56, 0.34),  // wood
    vec3(0.92, 0.62, 0.22)); // oil
const vec3 kPlate = vec3(0.028, 0.024, 0.032);
const vec3 kPlateEdge = vec3(0.58, 0.42, 0.30);   // thin warm pixel edge
const vec3 kSlotFill = vec3(0.070, 0.060, 0.070);
const vec3 kSlotEdge = vec3(0.32, 0.25, 0.22);
const vec3 kSelectEdge = vec3(1.00, 0.72, 0.34);
const vec3 kSelectFill = vec3(0.150, 0.110, 0.080);
const vec3 kShadow = vec3(0.0, 0.0, 0.0);
const vec3 kText = vec3(0.92, 0.88, 0.82);
const vec3 kDimText = vec3(0.60, 0.54, 0.54);
const vec3 kWarm = vec3(1.00, 0.76, 0.42);

vec4 hud = vec4(0.0);

void over(vec3 color, float alpha) {
    hud = vec4(color * alpha, alpha) + hud * (1.0 - alpha);
}

// One-pixel anti-aliased coverage from a signed distance in pixels.
float cover(float d) {
    return clamp(0.5 - d, 0.0, 1.0);
}

float hashPx(ivec2 p, int salt) {
    uint h = uint(p.x + 31 * salt) * 374761393u + uint(p.y + 17 * salt) * 668265263u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return float((h ^ (h >> 16u)) & 1023u) / 1023.0;
}

// Coverage of an atlas rect whose top-left sits at `origin` (pixels, y down), magnified by uiScale.
float textCoverage(ivec4 rect, ivec2 origin, ivec2 px) {
    ivec2 offset = px - origin;
    if (offset.x < 0 || offset.y < 0) {
        return 0.0;
    }
    ivec2 texel = offset / uiScale;
    if (texel.x >= rect.z || texel.y >= rect.w) {
        return 0.0;
    }
    return texelFetch(fontTex, rect.xy + texel, 0).r;
}

// Text with a one-unit soft shadow underneath so it reads over any plate or background.
void drawText(ivec4 rect, ivec2 origin, ivec2 px, vec3 color, float alpha) {
    over(kShadow, 0.55 * alpha * textCoverage(rect, origin + ivec2(uiScale), px));
    over(color, alpha * textCoverage(rect, origin, px));
}

// One cell of the digit strip; the glyph is one column narrower than its pitch.
float glyphCoverage(int cell, ivec2 origin, ivec2 px) {
    if (cell < 0) {
        return 0.0;
    }
    return textCoverage(ivec4(digitRect.x + cell * digitRect.z, digitRect.y, digitRect.z - 1, digitRect.w), origin, px);
}

void drawGlyph(int cell, ivec2 origin, ivec2 px, vec3 color, float alpha) {
    over(kShadow, 0.55 * alpha * glyphCoverage(cell, origin + ivec2(uiScale), px));
    over(color, alpha * glyphCoverage(cell, origin, px));
}

// A dark translucent box with a one-unit warm edge and its corner units cut off. `u` and `origin` are in units.
void plate(ivec2 u, ivec2 origin, ivec2 size, float alpha) {
    ivec2 r = u - origin;
    if (r.x < 0 || r.y < 0 || r.x >= size.x || r.y >= size.y) {
        return;
    }
    bool edgeX = r.x == 0 || r.x == size.x - 1;
    bool edgeY = r.y == 0 || r.y == size.y - 1;
    if (edgeX && edgeY) {
        return;
    }
    alpha *= overlay == 1 ? 0.88 : 1.0;
    if (edgeX || edgeY) {
        over(kPlateEdge, 0.50 * alpha);
    } else {
        over(kPlate, 0.76 * alpha);
    }
}

// The ring is evaluated in grid space so its radius is the radius the spawn shaders paint. Resting, it is a thin tinted
// ring with a faint dark outline (nothing reaches further than 2 px from the painted radius); painting thickens and
// brightens it, steadily: the stream under it is the motion to watch. Erase is reddish and dashed, with a small cross.
void drawBrushRing() {
    vec2 cell = (gl_FragCoord.xy - vec2(simRect.xy)) / vec2(simRect.zw) * vec2(gridSize);
    if (cell.x < 0.0 || cell.y < 0.0 || cell.x >= float(gridSize.x) || cell.y >= float(gridSize.y)) {
        return;
    }
    float u = float(uiScale);
    float pxPerCell = float(simRect.z) / float(gridSize.x);
    vec2 delta = cell - (vec2(brushCell) + 0.5);
    float d = (length(delta) - float(brushRadius)) * pxPerCell;
    float reach = 2.0 + 1.0 * u * painting;
    int mode = clamp(brushMode, 0, 7);
    // Nothing but erase's cross lies inside the ring's reach.
    if (d > reach || (d < -reach && mode != 3)) {
        return;
    }
    vec3 tint = kRingTints[mode];
    float live = 0.75 * painting;
    float lw = 1.0 + 0.5 * u * live;
    float dash = 1.0;
    if (mode == 3) {
        float count = clamp(floor(6.2831853 * float(brushRadius) * pxPerCell / (9.0 * u)), 8.0, 72.0);
        float phase = fract(atan(delta.y, delta.x) / 6.2831853 * count);
        dash = smoothstep(0.0, 0.06, phase) * (1.0 - smoothstep(0.50, 0.56, phase));
    }
    float band = abs(d) - 0.5 * lw;
    float ring = cover(band) * dash;
    over(kShadow, 0.34 * dash * (1.0 - smoothstep(0.0, 1.0, band)) * clamp(band + 0.5, 0.0, 1.0));
    over(tint, 0.80 * ring * (1.0 + 0.3 * live));
    if (mode == 3) {
        vec2 c = abs(delta) * pxPerCell;
        float arm = min(3.5 * u, 0.4 * float(brushRadius) * pxPerCell);
        float xd = abs(c.x - c.y) * 0.70711;
        if (max(c.x, c.y) < arm) {
            over(tint, 0.85 * cover(xd - 0.5));
        }
    }
}

int readoutCell(int index, int radius) {
    if (index == 0) {
        return 10;
    }
    if (index == 1) {
        return -1;
    }
    if (radius >= 10) {
        return index == 2 ? radius / 10 : radius % 10;
    }
    return index == 2 ? radius : -1;
}

// 8x8 pixel-art motif for each material; p is the pixel from the top-left. Alpha 0 is empty.
vec4 icon(int slot, ivec2 p) {
    float h = hashPx(p, slot);
    if (slot == 0) {  // sand: a dune with a pale crest
        const int heights[8] = int[8](2, 3, 4, 6, 6, 4, 3, 2);
        int top = 8 - heights[p.x];
        if (p.y < top) {
            return vec4(0.0);
        }
        return vec4(p.y == top ? vec3(1.0, 0.86, 0.56) : mix(vec3(0.84, 0.62, 0.32), vec3(0.98, 0.78, 0.46), h), 1.0);
    }
    if (slot == 1) {  // water: a wave, a bright surface row and a darker body
        int surface = 2 + (p.x / 2) % 2;
        if (p.y < surface) {
            return vec4(0.0);
        }
        if (p.y == surface) {
            return vec4(0.66, 0.92, 0.94, 1.0);
        }
        return vec4(mix(vec3(0.20, 0.62, 0.66), vec3(0.06, 0.30, 0.40), float(p.y - surface) / 5.0) * (0.9 + 0.2 * h), 1.0);
    }
    if (slot == 2) {  // solid: two courses of stone with a lit top row
        bool seam = p.y == 3 || (p.y < 3 && p.x == 3) || (p.y > 3 && p.x == 5);
        vec3 stone = vec3(0.50, 0.56, 0.66) * (0.85 + 0.3 * h);
        if (p.y == 0) {
            stone = vec3(0.78, 0.84, 0.92);
        } else if (p.y == 7) {
            stone *= 0.7;
        }
        return vec4(seam ? vec3(0.20, 0.23, 0.30) : stone, 1.0);
    }
    if (slot == 3) {  // erase: a red cross
        return (p.x == p.y || p.x + p.y == 7) ? vec4(0.95, 0.28, 0.24, 1.0) : vec4(0.0);
    }
    if (slot == 4) {  // smoke: three puffs
        vec2 q = vec2(p) + 0.5;
        float inside = max(max(1.0 - step(2.3, length(q - vec2(2.5, 5.2))), 1.0 - step(2.6, length(q - vec2(5.2, 4.0)))),
                           1.0 - step(1.9, length(q - vec2(3.6, 2.0))));
        return vec4(vec3(p.y < 3 ? 0.80 : 0.62 + 0.10 * h), inside);
    }
    if (slot == 6) {  // wood: a log end with growth rings beside its bark
        vec2 q = vec2(p) + 0.5 - vec2(4.0);
        float r = length(q);
        if (r > 3.9) {
            return vec4(0.0);
        }
        vec3 ring = mix(vec3(0.86, 0.64, 0.38), vec3(0.62, 0.40, 0.20), step(0.5, fract(r * 0.75)));
        return vec4(r > 3.0 ? vec3(0.36, 0.22, 0.12) : ring * (0.9 + 0.2 * h), 1.0);
    }
    if (slot == 7) {  // oil: a dark amber drop with a golden highlight
        const float dropWidths[8] = float[8](0.5, 0.9, 1.5, 2.2, 2.9, 3.2, 3.0, 2.2);
        float dx = float(p.x) + 0.5 - 4.0;
        if (abs(dx) > dropWidths[p.y]) {
            return vec4(0.0);
        }
        vec3 oil = mix(vec3(0.62, 0.36, 0.10), vec3(0.32, 0.17, 0.05), float(p.y) / 7.0) * (0.9 + 0.2 * h);
        if (p.y >= 3 && p.y <= 5 && dx > -2.0 && dx < -0.6) {
            oil = vec3(1.0, 0.82, 0.46);
        }
        return vec4(oil, 1.0);
    }
    // fire: a flame with a yellow middle and a pale core
    const float halfWidths[8] = float[8](0.6, 0.9, 1.5, 2.2, 2.9, 3.3, 3.5, 3.0);
    float dx = abs(float(p.x) + 0.5 - 4.0);
    if (dx > halfWidths[p.y]) {
        return vec4(0.0);
    }
    vec3 flame = vec3(1.0, 0.42, 0.08);
    if (dx < 0.65 * halfWidths[p.y]) {
        flame = vec3(1.0, 0.72, 0.20);
    }
    if (p.y >= 4 && dx < 1.1) {
        flame = vec3(1.0, 0.94, 0.65);
    }
    return vec4(flame, 1.0);
}

void drawSlot(int i, ivec2 u, ivec2 slotOrigin) {
    int size = inventory.z;
    ivec2 r = u - slotOrigin;
    float pop = slotPop[i];
    bool border = r.x == 0 || r.y == 0 || r.x == size - 1 || r.y == size - 1;
    if (border && (r.x == 0 || r.x == size - 1) && (r.y == 0 || r.y == size - 1)) {
        return;  // cut corner
    }
    if (border) {
        over(mix(kSlotEdge, kSelectEdge, pop), 0.85);
        return;
    }
    over(mix(kSlotFill, kSelectFill, pop) + vec3(0.10, 0.07, 0.03) * painting * pop, 0.92);
    ivec2 p = r - ivec2((size - spacing.w) / 2);
    if (p.x >= 0 && p.y >= 0 && p.x < spacing.w && p.y < spacing.w) {
        vec4 motif = icon(i, p);
        over(motif.rgb * mix(0.55, 1.0, pop), motif.a);
    }
}

void drawInventory(ivec2 px) {
    int s = uiScale;
    ivec2 u = px / s;
    ivec2 origin = ivec2(edge);
    ivec2 size = inventory.xy;
    ivec2 rel = u - origin;
    if (rel.x < 0 || rel.y < 0 || rel.x >= size.x || rel.y >= size.y) {
        return;
    }
    plate(u, origin, size, 1.0);
    int slot = inventory.z;
    int pitch = slot + inventory.w;
    ivec2 rowOrigin = origin + ivec2(pad);
    int mode = clamp(brushMode, 0, 7);
    // The selected slot's outline glow sits in the gap around it, under the tiles.
    for (int i = 0; i < 8; ++i) {
        float pop = slotPop[i];
        ivec2 q = u - (rowOrigin + ivec2(i * pitch, 0));
        if (pop > 0.001 && q.x >= -1 && q.y >= -1 && q.x <= slot && q.y <= slot &&
            (q.x < 0 || q.y < 0 || q.x >= slot || q.y >= slot)) {
            over(kSelectEdge, 0.28 * pop);
        }
    }
    int within = u.x - rowOrigin.x;
    if (within >= 0 && u.y >= rowOrigin.y && u.y < rowOrigin.y + slot) {
        int i = within / pitch;
        if (i < 8 && within - i * pitch < slot) {
            drawSlot(i, u, rowOrigin + ivec2(i * pitch, 0));
        }
    }
    // Key numbers under each slot: the selected one warm and bright.
    int digitTop = rowOrigin.y + slot + spacing.x;
    if (u.y >= digitTop - 1 && u.y <= digitTop + digitRect.w) {
        for (int i = 0; i < 8; ++i) {
            float pop = slotPop[i];
            ivec2 origin_px = ivec2(rowOrigin.x + i * pitch + (slot - (digitRect.z - 1)) / 2, digitTop) * s;
            drawGlyph(i + 1, origin_px, px, mix(kDimText, kWarm, pop), mix(0.75, 1.0, pop));
        }
    }
    // Selected material and brush radius.
    int infoTop = digitTop + digitRect.w + spacing.y;
    if (u.y >= infoTop - 1 && u.y <= infoTop + digitRect.w) {
        drawText(labelRects[mode], ivec2(rowOrigin.x, infoTop) * s, px, mix(kText, kRingTints[mode], 0.55), 0.6 + 0.4 * slotPop[mode]);
        int radius = clamp(brushRadius, 0, 99);
        int cells = radius >= 10 ? 4 : 3;
        int readoutX = origin.x + size.x - pad - (cells * digitRect.z - 1);
        for (int i = 0; i < cells; ++i) {
            drawGlyph(readoutCell(i, radius), ivec2(readoutX + i * digitRect.z, infoTop) * s, px, i == 0 ? kDimText : kText, 1.0);
        }
    }
}

// The help panel sits under the inventory and the hint ("H HELP") takes its place when it is hidden: the hint leaves
// first (gone by helpMix 0.4) and the panel arrives after it (from 0.3), sliding down a few units as it fades in.
void drawHelp(ivec2 px) {
    float alpha = clamp((helpMix - 0.3) / 0.7, 0.0, 1.0);
    if (alpha <= 0.002) {
        return;
    }
    int s = uiScale;
    ivec2 u = px / s;
    ivec2 origin = ivec2(edge, edge + inventory.y + spacing.z - int(round((1.0 - alpha) * 3.0)));
    ivec2 size = ivec2(inventory.x, helpLayout.x);
    ivec2 rel = u - origin;
    if (rel.x < 0 || rel.y < 0 || rel.x >= size.x || rel.y >= size.y) {
        return;
    }
    plate(u, origin, size, alpha);
    int local = rel.y - pad;
    if (local < 0) {
        return;
    }
    int row = local / helpLayout.y;
    if (row > 5 || local - row * helpLayout.y >= digitRect.w) {
        return;
    }
    ivec2 rowOrigin = origin + ivec2(pad, pad + row * helpLayout.y);
    drawText(keyRects[row], rowOrigin * s, px, kWarm, alpha);
    drawText(actionRects[row], (rowOrigin + ivec2(helpLayout.w + helpLayout.z, 0)) * s, px, kDimText + 0.10, alpha);
}

void drawHint(ivec2 px) {
    float alpha = clamp(1.0 - helpMix * 2.5, 0.0, 1.0);
    if (alpha <= 0.002) {
        return;
    }
    int s = uiScale;
    ivec2 origin = ivec2(edge + pad, edge + inventory.y + spacing.z + 1);
    drawText(hintKeyRect, origin * s, px, kWarm, 0.80 * alpha);
    drawText(hintTextRect, (origin + ivec2(hintKeyRect.z + 4, 0)) * s, px, kDimText, 0.80 * alpha);
}

void main() {
    ivec2 px = ivec2(int(gl_FragCoord.x), fbSize.y - 1 - int(gl_FragCoord.y));
    if (cursorValid == 1) {
        drawBrushRing();
    }
    drawInventory(px);
    drawHelp(px);
    drawHint(px);
    if (hud.a <= 0.0) {
        discard;
    }
    outColor = hud;
}
