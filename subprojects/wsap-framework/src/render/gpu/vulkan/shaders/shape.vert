#version 450

/*
 * Shape vertex shader for Vulkan backend.
 * Generates quads for rounded rects, circles, and drop shadows.
 */

layout(push_constant) uniform PushConstants {
    vec2 viewport_size;
    uint channel;
} push;

layout(location = 0) in vec4 in_rect;
layout(location = 1) in vec4 in_color;
layout(location = 2) in float in_radius;
layout(location = 3) in uint in_mode;
layout(location = 4) in uint in_blur;

layout(location = 0) flat out vec4 out_rect;
layout(location = 1) flat out vec4 out_color;
layout(location = 2) flat out float out_radius;
layout(location = 3) flat out uint out_mode;
layout(location = 4) flat out uint out_blur;

void main()
{
    vec2 corner = vec2(float((0x16u >> gl_VertexIndex) & 1u),
                       float((0x34u >> gl_VertexIndex) & 1u));
    vec2 pos = in_rect.xy + corner * in_rect.zw;
    gl_Position = vec4(pos * (2.0 / push.viewport_size) - vec2(1.0), 0.0, 1.0);
    out_rect = in_rect;
    out_color = in_color;
    out_radius = in_radius;
    out_mode = in_mode;
    out_blur = in_blur;
}
