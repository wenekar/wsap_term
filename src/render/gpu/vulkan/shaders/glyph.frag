#version 450

/*
 * Glyph fragment shader for Vulkan backend.
 * Renders solid fills, alpha glyphs, color glyphs, images, and subpixel text.
 */

layout(push_constant) uniform PushConstants {
    vec2 viewport_size;
    uint channel;
} push;

layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(set = 0, binding = 1) uniform sampler2D image_tex;

layout(location = 0) in vec2 in_uv;
layout(location = 1) flat in vec4 in_color;
layout(location = 2) flat in uint in_mode;

layout(location = 0) out vec4 out_color;

#define MODE_SOLID 0u
#define MODE_ALPHA 1u
#define MODE_COLOR 2u
#define MODE_IMAGE 3u
#define MODE_SUBPX 4u

void main()
{
    if (in_color.a <= 0.0)
        discard;

    if (in_mode == MODE_SOLID) {
        out_color = vec4(in_color.rgb * in_color.a, in_color.a);
        return;
    }

    if (in_mode == MODE_IMAGE) {
        out_color = vec4(texelFetch(image_tex, ivec2(in_uv), 0).rgb, 1.0);
        return;
    }

    vec4 t = texelFetch(atlas, ivec2(in_uv), 0);
    if (in_mode == MODE_COLOR) {
        float a = t.a * in_color.a;
        if (a <= 0.0)
            discard;
        out_color = t * in_color.a;
    } else {
        float cov = (in_mode == MODE_ALPHA) ? t.a : t[push.channel];
        float a = in_color.a * cov;
        if (a <= 0.0)
            discard;
        out_color = vec4(in_color.rgb * a, a);
    }
}
