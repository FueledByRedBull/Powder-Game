#version 430

layout(location = 0) out vec4 outColor;

uniform int pass;

in vec2 local;
flat in vec3 color;

void main() {
    if (pass == 0) {
        // Brightest at the head, a third at the tail, soft across.
        float weight = (1.0 - local.y * local.y) * mix(0.3, 1.0, clamp(local.x, 0.0, 1.0));
        // Blended as (1 - dst) * src + dst, which is composite.frag's 1 - (1 - dst) exp(-add).
        outColor = vec4(1.0 - exp(-color * weight), 1.0);
    } else {
        // Blended as dst * (1 + src): light on the scene, strongest at the spark.
        float falloff = max(1.0 - dot(local, local), 0.0);
        outColor = vec4(color * falloff * falloff, 1.0);
    }
}
