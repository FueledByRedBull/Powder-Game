#version 430

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

uniform usampler2D materialTex;
uniform sampler2D stressTex;
uniform sampler2D waterPressureTex;
uniform sampler2D smokeDensityTex;
uniform sampler2D fireTex;
uniform sampler2D heatTex;
uniform ivec2 gridSize;
uniform ivec2 smokeSize;
uniform ivec2 heatSize;

vec3 flameColor(float t) {
    vec3 ember = vec3(0.56, 0.10, 0.02);
    vec3 core = vec3(1.0, 0.93, 0.74);
    return mix(ember, core, sqrt(clamp(t, 0.0, 1.0)));
}

void main() {
    uint material = texture(materialTex, uv).r;
    float stress = clamp(texture(stressTex, uv).r, 0.0, 1.0);
    float pressure = texture(waterPressureTex, uv).r;
    if (isnan(pressure) || isinf(pressure)) {
        pressure = 0.0;
    }
    float smoke = clamp(texture(smokeDensityTex, uv).r, 0.0, 1.0);

    vec2 fire_state = texture(fireTex, uv).xy;
    float heat = clamp(texture(heatTex, uv).r, 0.0, 1.0);
    float flame = clamp(fire_state.y * 1.5 + heat * 0.8, 0.0, 1.0);

    // Light cool gray-blue background for higher contrast across all materials.
    vec3 color = vec3(0.83, 0.87, 0.90);

    if (material == 1u) {
        vec3 relaxed = vec3(0.89, 0.77, 0.44);
        vec3 compressed = vec3(0.57, 0.45, 0.24);
        color = mix(relaxed, compressed, stress);
    } else if (material == 2u) {
        color = vec3(0.32, 0.34, 0.38);
    }

    float water_signal = max(pressure, 0.0) + 0.22 * abs(pressure);
    float water_strength = smoothstep(0.003, 0.11, water_signal);
    vec3 water_color = mix(vec3(0.04, 0.09, 0.16), vec3(0.13, 0.41, 0.86), water_strength);

    if (material == 0u) {
        color = mix(color, water_color, water_strength);
    } else if (material == 1u) {
        color = mix(color, water_color, water_strength * 0.25);
    }

    vec3 fire_color = flameColor(flame);
    color += fire_color * flame * 0.8;
    color = mix(color, vec3(0.04, 0.04, 0.05), clamp(smoke * 0.82, 0.0, 0.82));

    outColor = vec4(color, 1.0);
}
