#version 450

/*
 * Glyph fragment shader for Vulkan backend.
 * Renders solid fills, alpha glyphs, color glyphs, images, and subpixel text.
 *
 * Every mode writes premultiplied color, except for BLEND_NONE, which
 * writes the straight color. An image keeps the premultiplied color of its
 * texels, scaled by the tint and the opacity of the instance. The blend
 * state of the draw is the one of BLEND_NONE or of the mode, so the result
 * is the expression of blend_mode_px() in render/cpu/scene.c.
 */

layout(push_constant) uniform PushConstants {
    vec2 viewport_size;
    uint channel;
} push;

/* The texture of the draw: the atlas, or one UI image. */
layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 in_uv;
layout(location = 1) flat in vec4 in_uv_rect;
layout(location = 2) flat in vec4 in_color;
layout(location = 3) flat in uint in_mode;
layout(location = 4) flat in uint in_blend;

layout(location = 0) out vec4 out_color;

#define MODE_SOLID 0u
#define MODE_ALPHA 1u
#define MODE_COLOR 2u
#define MODE_IMAGE 3u
#define MODE_SUBPX 4u
#define MODE_IMAGE_LINEAR 5u

#define BLEND_NONE 4u

/* The texel of `uv`, clamped to the uv sub-rect. */
vec4 texel_in(ivec2 uv, ivec2 lo, ivec2 hi)
{
    return texelFetch(tex, clamp(uv, lo, hi), 0);
}

/*
 * The sample rule of an image op: destination pixel i of n samples the
 * source coordinate (i + 0.5) * src / n. A nearest sample takes the texel
 * that holds the coordinate. A linear sample mixes four texels by the rest
 * of the way between their centers. The CPU painter uses the same rule.
 * Every tap stays inside the uv sub-rect, so a texel of the image next to
 * the sub-rect cannot bleed into the sample.
 */
vec4 image_sample(vec2 uv, bool linear, vec4 uv_rect)
{
    ivec2 lo = ivec2(uv_rect.xy);
    ivec2 hi = ivec2(uv_rect.zw) - ivec2(1);
    if (!linear)
        return texel_in(ivec2(floor(uv)), lo, hi);
    vec2 p = clamp(uv - 0.5, vec2(lo), vec2(hi));
    vec2 f = fract(p);
    ivec2 i = ivec2(p);
    vec4 top = mix(texel_in(i, lo, hi), texel_in(i + ivec2(1, 0), lo, hi), f.x);
    vec4 bottom = mix(texel_in(i + ivec2(0, 1), lo, hi), texel_in(i + ivec2(1, 1), lo, hi), f.x);
    return mix(top, bottom, f.y);
}

void main()
{
    /* A solid draw at alpha 0 still runs: MULTIPLY zeroes the color, and
     * NONE replaces the pixel. Other modes draw nothing at alpha 0. */
    if (in_color.a <= 0.0 && in_mode != MODE_SOLID)
        discard;

    /* A NONE draw replaces the pixel, so it writes a straight color. */
    float dim = in_blend == BLEND_NONE ? 1.0 : in_color.a;

    if (in_mode == MODE_SOLID) {
        out_color = vec4(in_color.rgb * dim, in_color.a);
        return;
    }

    if (in_mode == MODE_IMAGE || in_mode == MODE_IMAGE_LINEAR) {
        vec4 t = image_sample(in_uv, in_mode == MODE_IMAGE_LINEAR, in_uv_rect);
        float a = t.a * in_color.a;
        if (a <= 0.0)
            discard;
        out_color = vec4(t.rgb * in_color.rgb * dim, a);
        return;
    }

    vec4 t = texelFetch(tex, ivec2(in_uv), 0);
    if (in_mode == MODE_COLOR) {
        float a = t.a * in_color.a;
        if (a <= 0.0)
            discard;
        out_color = t * dim;
    } else {
        float cov = (in_mode == MODE_ALPHA) ? t.a : t[push.channel];
        float a = in_color.a * cov;
        if (a <= 0.0)
            discard;
        out_color = vec4(in_color.rgb * a, a);
    }
}
