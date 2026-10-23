#version 430

layout(location = 0) out vec4 outColor;

uniform usampler2D materialTex;
uniform sampler2D waterAmountTex;
uniform sampler2D smokeDensityTex;
uniform sampler2D fireEmissiveTex;
uniform sampler2D temperatureTex;
uniform sampler2D waterVelocityTex;
uniform sampler2D gasVelocityTex;
uniform sampler2D gasPressureTex;
uniform sampler2D combustionTex;
uniform sampler2D fuelTex;
uniform sampler2D sandOccupancyTex;
uniform sampler2D sandMoistureTex;  // R8 mean moisture of the cell's sand (sand_rasterize.comp)
uniform usampler2D boundaryTex;
uniform sampler2D oilTex;    // RG16F, x: oil share of the liquid in a cell (oil_field.comp)
uniform sampler2D steamTex;  // R16F gas grid steam (steam_update.comp)
uniform sampler2D lightTex;  // RGBA16F mip chain of the fire's emitted light, 4x4 cells per base texel (light_build.comp)
uniform ivec2 gridSize;
uniform ivec2 fbSize;   // framebuffer size in pixels
uniform ivec4 simRect;  // x, y, width, height of the simulation in the framebuffer, origin bottom-left
uniform int renderMode;
uniform int lighting;   // 1: albedo * (ambient + fire glow + lantern); 0: albedo alone, which tests/AppLifecycleTests.cpp mirrors
uniform vec3 lantern;   // cursor lantern: centre cell x, y and radius in cells; radius <= 0 switches it off
uniform float fxTime;   // simulation seconds (tick count times the fixed step): flame tongues and heat shimmer; lit frames only
uniform int debugReplayOk;
uniform int debugPassCount;
uniform float debugPassTimings[7];

layout(std430, binding = 0) buffer DebugCounters {
    uint nanInfDetected;
    uint particleOverflowBlocked;
    uint pressureNonconverged;
    uint inactiveTileMisses;
    uint waterVelocityClamped;
};

// One 3x3 neighbourhood of a scalar field: a triangle filter, the 4-neighbour maximum, and the
// raw cell with the cell above it (for top-lit edges).
struct Taps {
    float filtered;
    float dilated;
    float centre;
    float above;
};

// One texel, scaled by gain and saturated at 1 when gain is nonzero.
float tap(sampler2D tex, vec2 coord, float gain) {
    float value = texture(tex, coord).r;
    return gain > 0.0 ? min(gain * value, 1.0) : value;
}

Taps tapScalar(sampler2D tex, vec2 coord, vec2 texel, float gain) {
    float c = tap(tex, coord, gain);
    float e = tap(tex, coord + vec2(texel.x, 0.0), gain);
    float w = tap(tex, coord - vec2(texel.x, 0.0), gain);
    float n = tap(tex, coord + vec2(0.0, texel.y), gain);
    float s = tap(tex, coord - vec2(0.0, texel.y), gain);
    float diagonals = tap(tex, coord + texel, gain) +
                      tap(tex, coord + vec2(texel.x, -texel.y), gain) +
                      tap(tex, coord + vec2(-texel.x, texel.y), gain) +
                      tap(tex, coord - texel, gain);
    return Taps((c * 4.0 + (e + w + n + s) + diagonals * 0.5) / 10.0, max(max(c, e), max(max(w, n), s)), c, n);
}

// PCG2D integer hash; tests/AppLifecycleTests.cpp mirrors it bit for bit.
uvec2 pcg2d(uvec2 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    v.x += v.y * 1664525u;
    v.y += v.x * 1664525u;
    v ^= v >> 16u;
    return v;
}

float unitFloat(uint h) {
    return float(h >> 8u) * (1.0 / 16777216.0);
}

// Soft 0..1 "is there water here" used for the depth cue.
float waterPresence(vec2 coord) {
    return smoothstep(0.01, 0.06, textureLod(waterAmountTex, coord, 0.0).r);
}

// Dark-cave palette. Everything in the scene is an albedo; the light (ambient, the fire's glow and the cursor lantern)
// multiplies it at the end, and only flames, the hot-gas halo and (drawn after it, spark.vert) sparks are emissive. tests/AppLifecycleTests.cpp
// (render-modes) mirrors the albedo functions below with lighting = 0.
// The back wall is distant: dark, low in contrast and only coarsely textured, so painted rock (bright, fine-grained,
// rimmed) reads as the solid foreground even as a thin stroke.
const vec3 kCave = vec3(0.125, 0.118, 0.150);  // back wall, scaled by its coarse tone
const vec3 kRockDark = vec3(0.30, 0.32, 0.39);
const vec3 kRockMid = vec3(0.46, 0.49, 0.57);
const vec3 kRockLight = vec3(0.62, 0.66, 0.75);
const vec3 kRockMoss = vec3(0.36, 0.40, 0.18);
const vec3 kCrustTop = vec3(0.88, 0.91, 0.96);   // lit rim on exposed top edges
const vec3 kCrustSide = vec3(0.66, 0.70, 0.79);
const float kCaveShadow = 0.60;  // back wall lost under the drop shadow of rock (and wood) up and to the left
const float kContactShade = 0.30;  // albedo lost by sand, water and back wall touching rock (ambient occlusion)
const vec3 kSandRelaxed = vec3(0.82, 0.64, 0.38);  // ochre
const vec3 kSandCompressed = vec3(0.66, 0.48, 0.27);
const float kSandWetDarkening = 0.45;  // albedo lost by saturated sand
const vec3 kWaterShallow = vec3(0.16, 0.60, 0.64);  // murky teal
const vec3 kWaterDeep = vec3(0.05, 0.34, 0.42);
// Light is summed in linear units and applied as its square root, which is the albedo times light in gamma-2 space.
const vec3 kAmbient = vec3(0.180, 0.205, 0.300);  // dim moonlight
const vec3 kLanternColor = vec3(1.0, 0.46, 0.16);
// Sand occupancy is mass over rest density, so a full cell reads 1. The coverage, stress and rim constants below were
// tuned on occupancy that saturated at half that density; sampling it with this gain, saturated, keeps the look.
const float kSandGain = 2.0;

// Static value noise on whole cells: the cell itself and 2x2, 8x8 and 16x16 blocks. Chunky patches read as pixel art.
float stoneTone(ivec2 cell) {
    return 0.30 * unitFloat(pcg2d(uvec2(cell)).x) + 0.25 * unitFloat(pcg2d(uvec2(cell >> 1) + 7u).x) +
           0.25 * unitFloat(pcg2d(uvec2(cell >> 3) + 101u).x) + 0.20 * unitFloat(pcg2d(uvec2(cell >> 4) + 202u).y);
}

// The back wall of the cave, also the letterbox margin (which passes framebuffer pixels as cells): 4x4, 8x8 and 16x16
// blocks only, no per-cell grain.
vec3 caveAlbedo(ivec2 cell) {
    float tone = 0.40 * unitFloat(pcg2d(uvec2(cell >> 2) + 11u).x) + 0.35 * unitFloat(pcg2d(uvec2(cell >> 3) + 101u).x) +
                 0.25 * unitFloat(pcg2d(uvec2(cell >> 4) + 202u).y);
    return kCave * (0.70 + 0.60 * tone);
}

bool solidAt(ivec2 cell) {
    return texelFetch(materialTex, clamp(cell, ivec2(0), gridSize - 1), 0).r >= 2u;
}

// Shading of an open cell by the solids (rock and wood) next to it: x is the drop shadow cast down and to the right
// (1 under a solid one cell up-left, fading over four), y is 1 when one of the four neighbours is solid.
vec2 solidShade(ivec2 cell) {
    float touch = solidAt(cell + ivec2(1, 0)) || solidAt(cell - ivec2(1, 0)) || solidAt(cell + ivec2(0, 1)) ||
                  solidAt(cell - ivec2(0, 1)) ? 1.0 : 0.0;
    float shadow = touch;
    for (int k = 1; k <= 4; ++k) {
        if (solidAt(cell + ivec2(-k, k))) {
            shadow = max(shadow, float(5 - k) / 4.0);
            break;
        }
    }
    return vec2(shadow, touch);
}

// Smooth value noise on the integer lattice, for flame tongues.
float valueNoise(vec2 x) {
    ivec2 i = ivec2(floor(x));
    vec2 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = unitFloat(pcg2d(uvec2(i) + 41u).x);
    float b = unitFloat(pcg2d(uvec2(i + ivec2(1, 0)) + 41u).x);
    float c = unitFloat(pcg2d(uvec2(i + ivec2(0, 1)) + 41u).x);
    float d = unitFloat(pcg2d(uvec2(i + ivec2(1, 1)) + 41u).x);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Rim strength along `dir`: 1 for a cell next to open space, falling to 0 over `reach` cells, 0 deeper in the rock.
float rimStrength(ivec2 cell, ivec2 dir, int reach) {
    for (int k = 1; k <= reach; ++k) {
        if (texelFetch(materialTex, clamp(cell + dir * k, ivec2(0), gridSize - 1), 0).r != 2u) {
            return float(reach + 1 - k) / float(reach);
        }
    }
    return 0.0;
}

// Stone: multi-tone cool grey with olive moss patches, a darker interior and a crusty rim (six cells deep on exposed
// tops, three on the sides and undersides). The rim fades out by a per-cell threshold, so it breaks up like dithered
// pixel art.
vec3 rockAlbedo(ivec2 cell) {
    uvec2 fine = pcg2d(uvec2(cell));
    float tone = stoneTone(cell);
    vec3 color = mix(mix(kRockDark, kRockMid, smoothstep(0.30, 0.50, tone)), kRockLight, smoothstep(0.60, 0.80, tone));
    float moss = smoothstep(0.70, 0.78, 0.6 * unitFloat(pcg2d(uvec2(cell >> 3) + 303u).x) + 0.4 * unitFloat(pcg2d(uvec2(cell >> 1) + 404u).y));
    color = mix(color, kRockMoss, 0.7 * moss);
    float rimTop = rimStrength(cell, ivec2(0, 1), 6);
    float rimBottom = rimStrength(cell, ivec2(0, -1), 3);
    float rimSide = max(rimStrength(cell, ivec2(-1, 0), 3), rimStrength(cell, ivec2(1, 0), 3));
    color *= mix(0.80, 1.0, max(max(rimTop, rimBottom), rimSide));
    float jitter = unitFloat(fine.y) * 0.95;
    float crustTop = rimTop > jitter ? 1.0 : 0.0;
    float crustSide = rimSide > jitter ? 1.0 : 0.0;
    float crustBottom = rimBottom > jitter ? 1.0 : 0.0;
    float crustTone = 0.82 + 0.36 * unitFloat(fine.x);
    color = mix(color, mix(kCrustTop, kRockMoss * 1.7, 0.5 * moss) * crustTone, 0.95 * crustTop);
    color = mix(color, kCrustSide * crustTone, 0.55 * crustSide * (1.0 - crustTop));
    color *= 1.0 - 0.35 * crustBottom * (1.0 - crustTop);
    return color;
}

// Blackbody-like ramp over normalised temperature: deep red (about 900 K), orange, yellow, then yellow-white
// (about 1500 K and up) in the core.
vec3 flameColor(float t) {
    float x = clamp(t, 0.0, 1.0);
    vec3 red = vec3(0.55, 0.04, 0.0);
    vec3 orange = vec3(1.0, 0.32, 0.02);
    vec3 yellow = vec3(1.0, 0.70, 0.20);
    vec3 white = vec3(1.0, 0.93, 0.75);
    return x < 0.4 ? mix(red, orange, x / 0.4) : x < 0.75 ? mix(orange, yellow, (x - 0.4) / 0.35) : mix(yellow, white, (x - 0.75) / 0.25);
}

vec3 timingColor(int index) {
    if (index == 0) {
        return vec3(0.30, 0.72, 1.00);
    }
    if (index == 1) {
        return vec3(0.48, 0.90, 0.58);
    }
    if (index == 2) {
        return vec3(0.08, 0.72, 0.96);
    }
    if (index == 3) {
        return vec3(0.98, 0.53, 0.24);
    }
    if (index == 4) {
        return vec3(0.93, 0.80, 0.23);
    }
    if (index == 5) {
        return vec3(0.96, 0.35, 0.30);
    }
    return vec3(0.88, 0.88, 0.92);
}

// Radial falloff to the window corners; p is the framebuffer position over 0..1.
float vignette(vec2 p) {
    return 1.0 - 0.82 * smoothstep(0.30, 1.0, length(p - 0.5) * 1.41421356);
}

// What reaches a surface, in linear light: the moonlit ambient, the fire's glow (the emitted-light mip chain read at
// four widths, so one small flame lights the cave a long way off) and the cursor lantern.
// backdrop is the share of the pixel that is bare back wall, which gets half the glow and lantern light.
vec3 sceneLight(vec2 uv, vec2 p, float backdrop, out float warmth) {
    float dim = vignette(p);
    // Mips 1-4 (8 to 64 cells per texel), weights summing to 2: a flame lights its surroundings, not the whole cave.
    vec3 glow = textureLod(lightTex, uv, 1.0).rgb * 0.4 + textureLod(lightTex, uv, 2.0).rgb * 0.5 +
                textureLod(lightTex, uv, 3.0).rgb * 0.6 + textureLod(lightTex, uv, 4.0).rgb * 0.5;
    glow *= mix(1.0, dim, 0.5);
    if (lantern.z > 0.0) {
        float f = 1.0 - smoothstep(0.0, 1.0, length(uv * vec2(gridSize) - lantern.xy) / lantern.z);
        glow += kLanternColor * (2.4 * f * f);
    }
    glow *= 1.0 - 0.5 * backdrop;
    vec3 light = kAmbient * dim + glow;
    // The share of the light that is warm: it washes the blue out of stone, so lit rock reads orange-brown, not olive.
    warmth = dot(glow, vec3(0.30, 0.59, 0.11)) / max(dot(light, vec3(0.30, 0.59, 0.11)), 1e-4);
    return light;
}

void main() {
    uvec2 pixelHash = pcg2d(uvec2(gl_FragCoord.xy));
    // The simulation covers simRect; the rest of the window (the letterbox margin) is the same cave wall in the dark.
    vec2 uv = (gl_FragCoord.xy - vec2(simRect.xy)) / vec2(simRect.zw);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) {
        vec3 margin = caveAlbedo(ivec2(gl_FragCoord.xy));
        if (lighting != 0) {
            margin *= sqrt(kAmbient * vignette(gl_FragCoord.xy / vec2(fbSize)));
        }
        outColor = vec4(clamp(margin + (unitFloat(pixelHash.x) - 0.5) / 255.0, 0.0, 1.0), 1.0);
        return;
    }
    vec2 texel = 1.0 / vec2(gridSize);
    // Heat shimmer: above and inside hot gas the whole scene is looked up through a wavering offset, as if refracted
    // by the hot air. Lit frames only (lighting is 0 in every pixel test but the lighting case), and time enters only
    // through fxTime, so a frame is still a function of the simulation state.
    if (lighting != 0) {
        float hot = max(textureLod(temperatureTex, uv, 0.0).r, textureLod(temperatureTex, uv - vec2(0.0, 10.0) * texel, 0.0).r);
        float shimmer = smoothstep(0.12, 0.55, hot);
        if (shimmer > 0.0) {
            vec2 cellPos = uv * vec2(gridSize);
            uv += texel * shimmer * 1.6 * vec2(sin(cellPos.y * 0.31 - fxTime * 11.0 + sin(cellPos.x * 0.07)),
                                                0.5 * sin(cellPos.x * 0.23 + fxTime * 7.0));
            uv = clamp(uv, vec2(0.0), vec2(1.0) - 0.5 * texel);
        }
    }
    uint material = texture(materialTex, uv).r;
    float stress = material == 2u ? 1.0 : 0.0;
    Taps waterTaps = tapScalar(waterAmountTex, uv, texel, 0.0);
    float water = max(waterTaps.filtered * 0.9, waterTaps.dilated * 0.58);
    if (isnan(water) || isinf(water)) {
        atomicAdd(nanInfDetected, 1u);
        water = 0.0;
    }
    vec2 water_velocity = texture(waterVelocityTex, uv).xy;
    Taps smokeTaps = tapScalar(smokeDensityTex, uv, texel, 0.0);
    float smoke = clamp(max(smokeTaps.filtered, smokeTaps.dilated * 0.7), 0.0, 1.0);
    Taps sandTaps = tapScalar(sandOccupancyTex, uv, texel, kSandGain);
    float sandOccupancy = clamp(max(sandTaps.filtered * 1.02, sandTaps.dilated * 0.78), 0.0, 1.0);
    float temperature = clamp(texture(temperatureTex, uv).r, 0.0, 1.0);
    float flame = clamp(texture(fireEmissiveTex, uv).r, 0.0, 1.0);

    if (renderMode == 1) {
        outColor = vec4(vec3(texture(boundaryTex, uv).r), 1.0);
        return;
    }
    if (renderMode == 2) {
        outColor = vec4(vec3(water), 1.0);
        return;
    }
    if (renderMode == 3) {
        outColor = vec4(vec3(clamp(length(water_velocity), 0.0, 1.0)), 1.0);
        return;
    }
    if (renderMode == 4) {
        outColor = vec4(vec3(clamp(length(texture(gasVelocityTex, uv).xy), 0.0, 1.0)), 1.0);
        return;
    }
    if (renderMode == 5) {
        outColor = vec4(vec3(clamp(texture(gasPressureTex, uv).r, 0.0, 1.0)), 1.0);
        return;
    }
    if (renderMode == 6) {
        outColor = vec4(vec3(smoke), 1.0);
        return;
    }
    if (renderMode == 7) {
        outColor = vec4(vec3(temperature), 1.0);
        return;
    }
    if (renderMode == 8) {
        outColor = vec4(clamp(texture(fuelTex, uv).r, 0.0, 1.0), texture(combustionTex, uv).y, temperature, 1.0);
        return;
    }
    if (renderMode == 9) {
        vec3 debug = vec3(min(float(nanInfDetected) / 32.0, 1.0),
                          min(float(particleOverflowBlocked) / 32.0, 1.0),
                          min(float(pressureNonconverged + inactiveTileMisses + waterVelocityClamped) / 32.0, 1.0));
        float max_timing = 0.25;
        for (int i = 0; i < debugPassCount; ++i) {
            max_timing = max(max_timing, max(debugPassTimings[i], 0.0));
        }
        if (uv.y > 0.78 && uv.y < 0.98 && debugPassCount > 0) {
            float slot = clamp(uv.x, 0.0, 0.9999) * float(debugPassCount);
            int bar_index = clamp(int(floor(slot)), 0, debugPassCount - 1);
            float local_x = fract(slot);
            float bar_height = clamp(max(debugPassTimings[bar_index], 0.0) / max_timing, 0.0, 1.0);
            float local_y = (uv.y - 0.78) / 0.20;
            vec3 bar_color = timingColor(bar_index);
            if (debugReplayOk == 0) {
                bar_color = mix(bar_color, vec3(1.0, 0.16, 0.16), 0.6);
            }
            if (local_x > 0.08 && local_x < 0.92 && local_y < bar_height) {
                debug = mix(debug, bar_color, 0.92);
            }
        }
        outColor = vec4(debug, 1.0);
        return;
    }

    // Presentation only: nothing below feeds back into the simulation, and nothing depends on time.
    ivec2 cell = ivec2(uv * vec2(gridSize));
    vec3 color = caveAlbedo(cell);
    vec2 shade = material < 2u ? solidShade(cell) : vec2(0.0);
    color *= 1.0 - kCaveShadow * shade.x;

    if (material == 2u) {
        color = rockAlbedo(cell);
    }
    // Wood (3 intact, 256 + fuel burning, 512 + fuel charred; see wood_burn.comp): warm brown planks with grain that
    // char to dark grey ash as the fuel goes; burning cells glow (added with the fire below).
    float woodEmber = 0.0;
    if (material == 3u || material >= 256u) {
        uvec2 grain = pcg2d(uvec2(cell.x / 3, cell.y) + 707u);
        vec3 wood = vec3(0.42, 0.26, 0.13) * (0.80 + 0.35 * unitFloat(grain.x)) * (cell.y % 6 == 0 ? 0.75 : 1.0);
        float charred = material == 3u ? 0.0 : 1.0 - float(material & 255u) / 255.0;
        color = mix(wood, vec3(0.16, 0.15, 0.15) * (0.8 + 0.4 * unitFloat(grain.y)), clamp(charred * 1.6, 0.0, 1.0));
        if (material >= 256u && material < 512u) {
            woodEmber = (0.35 + 0.65 * unitFloat(pcg2d(uvec2(cell) + 909u).x)) * (1.0 - 0.6 * charred);
        }
    }

    // Sand: ochre grains (a per-cell shade plus the odd light or dark speck), a soft coverage edge and a pale rim
    // along exposed tops.
    float sandCover = smoothstep(0.04, 0.11, sandOccupancy);
    if (sandCover > 0.0) {
        float sandStress = clamp(max(stress * 0.65, sandOccupancy * 0.68), 0.0, 1.0);
        vec3 sand_color = mix(kSandRelaxed, kSandCompressed, sandStress);
        uvec2 grainHash = pcg2d(uvec2(cell));
        float speck = unitFloat(grainHash.y);
        sand_color *= 1.0 + (unitFloat(grainHash.x) - 0.5) * 0.30 + (speck > 0.93 ? 0.16 : (speck < 0.08 ? -0.18 : 0.0));
        // Water filling the pores darkens sand (less scattering at the grain surfaces: Lekner and Dorf 1988).
        sand_color *= 1.0 - kSandWetDarkening * texture(sandMoistureTex, uv).r;
        float sand_rim = clamp((sandTaps.centre - sandTaps.above) * 3.0, 0.0, 1.0);
        sand_color = mix(sand_color, vec3(1.0, 0.90, 0.62), sand_rim * 0.30);
        color = mix(color, sand_color, sandCover);
    }

    // Water: murky teal that darkens with depth, a thin bright free-surface line two cells deep, a faint static
    // shimmer of lighter cells near the surface, and foam only for genuinely fast flow. Coverage saturates quickly so
    // sparse (1-2 particle) water still reads.
    float water_cover = 1.0 - exp(-water * 26.0);
    // A gap of a few cells with water on all four sides is a void the particles left in a churning body, not air you
    // can see through: it reads as water instead of a dark hole (the particle density holes themselves are physics).
    float enclosed = 0.0;
    if (material == 0u && water_cover < 0.9) {
        enclosed = min(min(waterPresence(uv + texel * vec2(3.0, 0.0)), waterPresence(uv - texel * vec2(3.0, 0.0))),
                       min(waterPresence(uv + texel * vec2(0.0, 3.0)), waterPresence(uv - texel * vec2(0.0, 3.0))));
        water_cover = max(water_cover, enclosed);
    }
    float oilShare = 0.0;  // oil_field share, filtered; oil turns the liquid into a dark amber slick
    vec3 oilSheen = vec3(0.0);  // thin-film colour of the slick's surface
    float glint = 0.0;  // moonlight caught by the surface and a few ripples; added after the lighting
    if (water_cover > 0.004 && (material == 0u || sandCover > 0.0)) {
        // Depth: water above (three heights) and to both sides; thin streams, films and the band under the surface
        // stay shallow. Each tap is bilinear and half a cell off the column, so it averages a 2x2 block and one missing
        // cell (a particle gap above) does not draw a light streak down the body. Per-pixel jitter of the heights
        // turns the three steps into a smooth ramp.
        vec3 reach = 0.7 + 0.6 * vec3(pixelHash.y & 1023u, (pixelHash.y >> 10u) & 1023u, (pixelHash.y >> 20u) & 1023u) / 1023.0;
        vec2 half_cell = vec2(0.5, 0.0) * texel;
        float depth = (waterPresence(uv + half_cell + texel * vec2(0.0, 5.0 * reach.x)) +
                       waterPresence(uv - half_cell + texel * vec2(0.0, 12.0 * reach.y)) +
                       waterPresence(uv + half_cell + texel * vec2(0.0, 24.0 * reach.z))) / 3.0 *
                      waterPresence(uv + texel * vec2(7.0, 0.5)) * waterPresence(uv - texel * vec2(7.0, -0.5));
        float foam = smoothstep(35.0, 70.0, length(water_velocity));
        vec3 water_color = mix(kWaterShallow, kWaterDeep, depth * (1.0 - foam) * 0.85);
        // Saturated coverage one and two cells up, so particle jitter inside the body does not light up the rim.
        float above1 = 1.0 - exp(-waterTaps.above * 26.0);
        float above2 = 1.0 - exp(-textureLod(waterAmountTex, uv + vec2(0.0, 2.0 * texel.y), 0.0).r * 26.0);
        float surface = max(max(water_cover - above1, 0.0), 0.5 * max(water_cover - above2, 0.0)) * (1.0 - enclosed);
        water_color = mix(water_color, vec3(0.62, 0.88, 0.90), surface * 0.85);
        float shimmer = unitFloat(pcg2d(uvec2(cell) + 505u).x) > 0.9 ? (1.0 - depth) * (1.0 - depth) * water_cover : 0.0;
        water_color = mix(water_color, vec3(0.50, 0.82, 0.86), 0.35 * shimmer);
        glint = (0.55 * surface + 0.30 * shimmer) * (material == 0u ? 1.0 : 0.4);
        water_color = mix(water_color, vec3(0.70, 0.90, 0.92), foam * 0.25);
        // Oil: a dark amber slick. The share is filtered over the 3x3 neighbourhood and dilated, so the few oil
        // records of a thin film join into one continuous band instead of rust-coloured specks. Its surface carries
        // a thin-film sheen, an amber-biased rainbow along its top cells.
        Taps oilTaps = tapScalar(oilTex, uv, texel, 0.0);
        oilShare = smoothstep(0.04, 0.45, max(oilTaps.filtered * 1.2, oilTaps.dilated * 0.75));
        if (oilShare > 0.0) {
            // Amber where the slick is thin or near its top, near-black brown where it is deep.
            float top = 1.0 - waterPresence(uv + texel * vec2(0.5, 3.5));
            vec3 oil_color = mix(vec3(0.40, 0.23, 0.08), vec3(0.17, 0.095, 0.04), clamp(max(depth, oilShare) - 0.7 * top, 0.0, 1.0));
            // The hue drifts smoothly along the slick (low-frequency noise), not cell by cell.
            float film = float(cell.x) * 0.006 + 0.5 * valueNoise(vec2(cell) * 0.04) + 0.35 * oilTaps.filtered;
            oilSheen = 0.5 + 0.5 * cos(6.2832 * (film + vec3(0.0, 0.33, 0.67)));
            oilSheen = mix(vec3(0.95, 0.68, 0.28), oilSheen, 0.55);  // amber-biased rainbow
            oil_color = mix(oil_color, oilSheen * 0.6, top * 0.55);
            water_color = mix(water_color, oil_color, oilShare);
            glint = mix(glint, 0.5 * top, oilShare);
        }
        float water_alpha = water_cover * (material == 0u ? mix(0.88, 0.97, oilShare) : 0.40);
        color = mix(color, water_color, water_alpha);
    }
    // Sand, water and the back wall darken where they touch rock: contact occlusion.
    color *= 1.0 - kContactShade * shade.y;

    // Smoke: a grey haze that shows even when thin, slightly warmed where hot. It is albedo, so the fire's glow
    // lights it.
    float smoke_alpha = 0.85 * smoothstep(0.0, 0.15, smoke) * (1.0 - 0.45 * smoothstep(0.3, 0.9, temperature));
    vec3 smoke_color = mix(mix(vec3(0.36, 0.36, 0.40), vec3(0.58, 0.56, 0.58), smoke_alpha),
                           vec3(0.60, 0.40, 0.28), smoothstep(0.2, 0.8, temperature) * 0.30);
    color = mix(color, smoke_color, smoke_alpha);

    // Steam: a white plume over everything, a little self-lit after the lighting so it reads in the dark cave.
    float steam_alpha = 0.0;
    float steam = texture(steamTex, uv).r;
    if (steam > 0.0) {
        steam_alpha = 0.85 * smoothstep(0.0, 0.15, steam);
        color = mix(color, vec3(0.88, 0.90, 0.92), steam_alpha);
    }

    if (lighting != 0) {
        float warmth;
        // The back wall is further from the flames and the lantern than the matter in front of it: half their light.
        float backdrop = material < 2u ? (1.0 - max(water_cover, sandCover)) * (1.0 - smoke_alpha) * (1.0 - steam_alpha) : 0.0;
        vec3 light = sceneLight(uv, gl_FragCoord.xy / vec2(fbSize), backdrop, warmth);
        color = mix(color, vec3(dot(color, vec3(0.30, 0.59, 0.11))), 0.40 * warmth);
        color *= sqrt(light);
        color += mix(vec3(0.16, 0.30, 0.32), oilSheen * 0.35, oilShare) * glint + vec3(0.32, 0.33, 0.35) * steam_alpha;
    }

    // Fire: additive flame, a temperature glow that outlives the flame, soft-saturated instead of hard-clamped. The
    // bloom is eight fixed taps of the same temperature field at two reaches, so flames leave a smooth warm halo on
    // whatever is around them. Lit frames break the flame into rising tongues that
    // move with fxTime only; sparks and embers are particles (spark_step.comp) drawn over the frame by spark.vert.
    float heat = temperature * temperature;
    vec2 near = texel * 8.0, far = texel * 14.0;
    float bloom = 0.0625 * (texture(temperatureTex, uv + near).r + texture(temperatureTex, uv - near).r +
                            texture(temperatureTex, uv + vec2(near.x, -near.y)).r +
                            texture(temperatureTex, uv + vec2(-near.x, near.y)).r) +
                  0.1875 * (texture(temperatureTex, uv + vec2(far.x, 0.0)).r + texture(temperatureTex, uv - vec2(far.x, 0.0)).r +
                            texture(temperatureTex, uv + vec2(0.0, far.y)).r + texture(temperatureTex, uv - vec2(0.0, far.y)).r);
    bloom = clamp(bloom, 0.0, 1.0);
    float tongues = 1.0;
    if (lighting != 0) {
        vec2 cellPos = uv * vec2(gridSize);
        if (flame > 0.0 || heat > 0.01) {
            // Two octaves of noise stretched upward and scrolled up: tongues that lick and flicker. Thin flame
            // (the tip) is cut harder than the dense base.
            float n = 0.65 * valueNoise(vec2(cellPos.x * 0.11, cellPos.y * 0.045 - fxTime * 2.6)) +
                      0.35 * valueNoise(vec2(cellPos.x * 0.27, cellPos.y * 0.11 - fxTime * 5.5));
            tongues = smoothstep(0.62 - 0.55 * max(flame, heat), 0.95, n + 0.25) * 1.5;
        }
    }
    // Flame hue follows the gas temperature; its strength follows the burn, plus a dim glow from very hot gas.
    vec3 fire_add = flameColor(max(temperature, flame * 0.8)) * (flame * 1.6 * tongues) +
                    flameColor(temperature * 0.5) * (heat * heat * 1.6 * mix(1.0, tongues, 0.6)) +
                    vec3(1.0, 0.30, 0.03) * (bloom * 0.2 + bloom * bloom * 0.35) +
                    vec3(1.0, 0.30, 0.04) * (woodEmber * 1.2);
    color = 1.0 - (1.0 - color) * exp(-fire_add);

    // Sub-LSB dither against banding in the dark gradients.
    color += (unitFloat(pixelHash.x) - 0.5) / 255.0;
    outColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
