#version 430

layout(location = 0) in vec2 uv;
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
uniform usampler2D boundaryTex;
uniform ivec2 gridSize;
uniform int renderMode;
uniform int debugReplayOk;
uniform int debugPassCount;
uniform float debugPassTimings[7];

layout(std430, binding = 0) buffer DebugCounters {
    uint nanInfDetected;
    uint particleOverflowBlocked;
    uint pressureNonconverged;
    uint inactiveTileMisses;
};

float filteredScalar(sampler2D tex, vec2 coord, vec2 texel) {
    float center = texture(tex, coord).r * 4.0;
    float cardinals = texture(tex, coord + vec2(texel.x, 0.0)).r +
                      texture(tex, coord - vec2(texel.x, 0.0)).r +
                      texture(tex, coord + vec2(0.0, texel.y)).r +
                      texture(tex, coord - vec2(0.0, texel.y)).r;
    float diagonals = texture(tex, coord + texel).r +
                      texture(tex, coord + vec2(texel.x, -texel.y)).r +
                      texture(tex, coord + vec2(-texel.x, texel.y)).r +
                      texture(tex, coord - texel).r;
    return (center + cardinals + diagonals * 0.5) / 10.0;
}

float dilatedScalar(sampler2D tex, vec2 coord, vec2 texel) {
    float value = texture(tex, coord).r;
    value = max(value, texture(tex, coord + vec2(texel.x, 0.0)).r);
    value = max(value, texture(tex, coord - vec2(texel.x, 0.0)).r);
    value = max(value, texture(tex, coord + vec2(0.0, texel.y)).r);
    value = max(value, texture(tex, coord - vec2(0.0, texel.y)).r);
    return value;
}

vec3 flameColor(float t) {
    vec3 ember = vec3(0.82, 0.18, 0.02);
    vec3 body = vec3(1.0, 0.58, 0.06);
    vec3 core = vec3(1.0, 0.95, 0.72);
    float clamped_t = clamp(t, 0.0, 1.0);
    return mix(mix(ember, body, sqrt(clamped_t)), core, clamped_t * clamped_t);
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

void main() {
    vec2 fullTexel = 1.0 / vec2(gridSize);
    uint material = texture(materialTex, uv).r;
    float stress = material == 2u ? 1.0 : 0.0;
    float waterFiltered = filteredScalar(waterAmountTex, uv, fullTexel);
    float waterDilated = dilatedScalar(waterAmountTex, uv, fullTexel);
    float water = max(waterFiltered * 0.9, waterDilated * 0.58);
    if (isnan(water) || isinf(water)) {
        atomicAdd(nanInfDetected, 1u);
        water = 0.0;
    }
    vec2 water_velocity = texture(waterVelocityTex, uv).xy;
    float water_speed = clamp(length(water_velocity) * 1.8, 0.0, 1.0);
    float smoke = clamp(max(filteredScalar(smokeDensityTex, uv, fullTexel),
                            dilatedScalar(smokeDensityTex, uv, fullTexel) * 0.7),
                        0.0,
                        1.0);
    vec2 gas_velocity = texture(gasVelocityTex, uv).xy;
    vec2 combustion = texture(combustionTex, uv).xy;
    float fuel = clamp(texture(fuelTex, uv).r, 0.0, 1.0);
    float sandOccupancy = clamp(max(filteredScalar(sandOccupancyTex, uv, fullTexel) * 1.02,
                                    dilatedScalar(sandOccupancyTex, uv, fullTexel) * 0.78),
                                0.0,
                                1.0);
    float temperature = clamp(texture(temperatureTex, uv).r, 0.0, 1.0);
    float gas_pressure = clamp(texture(gasPressureTex, uv).r, 0.0, 1.0);
    float flame = clamp(texture(fireEmissiveTex, uv).r, 0.0, 1.0);
    float boundary = texture(boundaryTex, uv).r;

    if (renderMode == 1) {
        outColor = vec4(vec3(boundary), 1.0);
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
        outColor = vec4(vec3(clamp(length(gas_velocity), 0.0, 1.0)), 1.0);
        return;
    }
    if (renderMode == 5) {
        outColor = vec4(vec3(gas_pressure), 1.0);
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
        outColor = vec4(fuel, combustion.y, temperature, 1.0);
        return;
    }
    if (renderMode == 9) {
        vec3 debug = vec3(min(float(nanInfDetected) / 32.0, 1.0),
                          min(float(particleOverflowBlocked) / 32.0, 1.0),
                          min(float(pressureNonconverged + inactiveTileMisses) / 32.0, 1.0));
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

    vec3 color = vec3(0.09, 0.11, 0.14);

    if (sandOccupancy > 0.06) {
        vec3 relaxed = vec3(0.95, 0.78, 0.34);
        vec3 compressed = vec3(0.63, 0.41, 0.16);
        float sandStress = clamp(max(stress * 0.65, sandOccupancy * 0.68), 0.0, 1.0);
        color = mix(relaxed, compressed, sandStress);
    } else if (material == 2u) {
        color = vec3(0.70, 0.74, 0.79);
    }

    float water_strength = smoothstep(0.02, 0.18, water);
    vec3 water_color = mix(vec3(0.06, 0.22, 0.38), vec3(0.18, 0.78, 1.0), water_strength);
    water_color = mix(water_color, vec3(0.88, 0.97, 1.0), water_speed * water_strength * 0.45);
    float water_blend = water_strength * mix(0.58, 0.95, clamp(water_speed * 1.4, 0.0, 1.0));

    if (material == 0u) {
        color = mix(color, water_color, water_blend);
    } else if (sandOccupancy > 0.06) {
        color = mix(color, water_color, water_blend * 0.24);
    }

    vec3 fire_color = flameColor(flame);
    color += fire_color * flame * 1.02;
    color += vec3(1.0, 0.32, 0.04) * temperature * flame * 0.12;

    float smoke_alpha = clamp(smoke * 0.58, 0.0, 0.58);
    vec3 smoke_color = mix(vec3(0.34, 0.38, 0.44), vec3(0.70, 0.74, 0.78), smoke_alpha);
    color = mix(color, smoke_color, smoke_alpha);
    color += vec3(0.12, 0.18, 0.22) * water_strength * 0.14;
    color = clamp(color, 0.0, 1.0);

    outColor = vec4(color, 1.0);
}
