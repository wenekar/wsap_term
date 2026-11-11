#version 450

/*
 * Glyph vertex shader for Vulkan backend.
 * Generates quad vertices from gl_VertexIndex and maps instance to screen NDC.
 */

layout(push_constant) uniform PushConstants {
    vec2 viewport_size;
    uint channel;
} push;

layout(location = 0) in vec4 in_rect;
layout(location = 1) in vec4 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in uint in_mode;

layout(location = 0) out vec2 out_uv;
layout(location = 1) flat out vec4 out_color;
layout(location = 2) flat out uint out_mode;

void main()
{
    vec2 corner = vec2(float((0x16u >> gl_VertexIndex) & 1u),
                       float((0x34u >> gl_VertexIndex) & 1u));
    vec2 pos = in_rect.xy + corner * in_rect.zw;
    gl_Position = vec4(pos * (2.0 / push.viewport_size) - vec2(1.0), 0.0, 1.0);
    out_uv = mix(in_uv.xy, in_uv.zw, corner);
    out_color = in_color;
    out_mode = in_mode;
}
