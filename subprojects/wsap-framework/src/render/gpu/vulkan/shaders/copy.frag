#version 450

/*
 * Presents CPU pixels: fetches the texel of each pixel and writes it without
 * blending. The copy pipeline pairs this shader with glyph.vert, and the
 * quad's uv rect is the whole texture, so one texel maps to one pixel.
 */

layout(set = 0, binding = 0) uniform sampler2D pixels;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = texelFetch(pixels, ivec2(in_uv), 0);
}
