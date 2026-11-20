#version 450

/*
 * Copy fragment shader for presenting CPU pixels in Vulkan backend.
 * Directly outputs the pixel without blending.
 */

layout(set = 0, binding = 0) uniform sampler2D pixels;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = texelFetch(pixels, ivec2(in_uv), 0);
}
