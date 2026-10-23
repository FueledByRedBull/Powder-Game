#version 430

// One instance per spark record (spark_step.comp): a streak along its velocity, drawn over the lit composite. Pass 0
// is the glow of the streak (blended as the same soft saturation the fire uses); pass 1 is a wider soft quad that
// multiplies the scene a little, so a spark lights what is around it. Dead records collapse outside the clip volume.
layout(location = 0) in vec4 motion;  // x, y, vx, vy in cells and cells/s
layout(location = 1) in vec4 life;    // age, life, drag time, seed * 2 + ember

uniform ivec2 gridSize;
uniform ivec4 simRect;  // x, y, width, height of the simulation in the framebuffer
uniform ivec2 fbSize;
uniform int pass;

out vec2 local;       // across the streak -1..1 (pass 0: along it 0..1 in x); the quad's -1..1 square in pass 1
flat out vec3 color;  // emitted colour times intensity

// Same blackbody-like ramp as composite.frag: deep red, orange, yellow, yellow-white.
vec3 flameColor(float t) {
    float x = clamp(t, 0.0, 1.0);
    vec3 red = vec3(0.55, 0.04, 0.0);
    vec3 orange = vec3(1.0, 0.32, 0.02);
    vec3 yellow = vec3(1.0, 0.70, 0.20);
    vec3 white = vec3(1.0, 0.93, 0.75);
    return x < 0.4 ? mix(red, orange, x / 0.4) : x < 0.75 ? mix(orange, yellow, (x - 0.4) / 0.35) : mix(yellow, white, (x - 0.75) / 0.25);
}

void main() {
    if (life.y <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        local = vec2(0.0);
        color = vec3(0.0);
        return;
    }
    uint tag = uint(life.w);
    bool ember = (tag & 1u) != 0u;
    float seed = float(tag >> 1u) * (1.0 / 32768.0);
    float a = life.x / life.y;  // 0 newborn, 1 gone
    // Cools from yellow (sparks) or orange (embers) through red; fades in at once and out over the last 40%.
    float temperature = (ember ? 0.55 + 0.25 * seed : 0.8 + 0.2 * seed) * pow(1.0 - a, 1.4);
    float flicker = 0.75 + 0.25 * sin(life.x * 47.0 + 6.2832 * seed);
    float intensity = (ember ? 1.5 : 1.3) * smoothstep(0.0, 0.05, a) * (1.0 - smoothstep(0.6, 1.0, a)) * flicker;
    color = flameColor(temperature) * intensity;

    vec2 scale = vec2(simRect.zw) / vec2(gridSize);  // pixels per cell
    vec2 headCell = motion.xy;
    float speed = length(motion.zw);
    vec2 dir = speed > 1e-3 ? motion.zw / speed : vec2(0.0, 1.0);
    vec2 headPx = vec2(simRect.xy) + headCell * scale;
    vec2 tailPx = vec2(simRect.xy) + (headCell - dir * min(speed * 0.022, 7.0)) * scale;
    vec2 along = headPx - tailPx;
    float length_px = length(along);
    vec2 axis = length_px > 1e-3 ? along / length_px : vec2(0.0, 1.0);
    vec2 across = vec2(-axis.y, axis.x);
    float end = float(gl_VertexID & 1);  // 0 tail, 1 head
    float side = float(gl_VertexID >> 1) * 2.0 - 1.0;
    vec2 px;
    if (pass == 0) {
        float half_width = max(0.55 * min(scale.x, scale.y), 0.9);
        px = mix(tailPx - axis * half_width, headPx + axis * half_width, end) + across * (side * half_width);
        local = vec2(end, side);
    } else {
        float radius = 6.0 * min(scale.x, scale.y);
        px = headPx + (axis * (end * 2.0 - 1.0) + across * side) * radius;
        local = vec2(end * 2.0 - 1.0, side);
        color *= 0.35 * (ember ? 1.0 : 0.8);
    }
    gl_Position = vec4(px / vec2(fbSize) * 2.0 - 1.0, 0.0, 1.0);
}
