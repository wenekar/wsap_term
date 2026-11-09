#include <metal_stdlib>
using namespace metal;

/*
 * The shaders of the Metal backend. render.m builds this source at run time.
 * tools/embed_metal.py puts the file, with its quoted includes, in a string.
 *
 * Every pipeline writes premultiplied color. The blend is
 * out = src + dst * (1 - src.a).
 *
 * The structs below must match the structs of render.m. Each draw is one
 * quad of six vertices for each instance.
 */

#define SHAPE_FN static inline
#define SHAPE_SQRT(x) sqrt(x)
#define SHAPE_EXP(x) exp(x)
#include "paint/shape_math.h"

/* The glyph instance modes. */
#define MODE_SOLID 0u
#define MODE_ALPHA 1u
#define MODE_COLOR 2u
#define MODE_IMAGE 3u
#define MODE_SUBPX 4u

/* The shape instance modes. */
#define SHAPE_ROUNDED 1u
#define SHAPE_CIRCLE  2u
#define SHAPE_SHADOW  3u

struct Uniforms {
    float2 viewport_size;
};

static inline float2 quad_corner(uint vertex_id)
{
    constexpr float2 corners[] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1)
    };
    return corners[vertex_id];
}

static inline float4 pixel_to_ndc(float2 pos, float2 viewport)
{
    return float4(pos.x / (viewport.x * 0.5f) - 1.0f,
                  1.0f - pos.y / (viewport.y * 0.5f), 0.0f, 1.0f);
}

/* ---- Glyphs, solid fills, images, and the CPU pixel copy ---------------- */

struct GlyphInstance {
    float4 rect;    /* x, y, width, height in pixels. */
    float4 uv;      /* The texel rect: x0, y0, x1, y1. */
    float4 color;   /* Straight color. The alpha is the opacity. */
    uint mode;
};

struct GlyphVertexOut {
    float4 position [[position]];
    float2 uv;
    float4 color [[flat]];
    uint mode [[flat]];
};

vertex GlyphVertexOut glyph_vertex(uint vertex_id [[vertex_id]],
                                   uint instance_id [[instance_id]],
                                   const device GlyphInstance *instances [[buffer(0)]],
                                   constant Uniforms &uniforms [[buffer(1)]])
{
    GlyphInstance in = instances[instance_id];
    float2 corner = quad_corner(vertex_id);
    GlyphVertexOut out;
    out.position = pixel_to_ndc(in.rect.xy + corner * in.rect.zw, uniforms.viewport_size);
    out.uv = mix(in.uv.xy, in.uv.zw, corner);
    out.color = in.color;
    out.mode = in.mode;
    return out;
}

/*
 * The atlas holds BGRA texels. An alpha glyph keeps its coverage in the
 * alpha channel. A subpixel glyph keeps red, green, and blue coverage in the
 * color channels. A color glyph keeps premultiplied color. A subpixel draw
 * runs three times, once for each color write mask. `channel` picks the
 * coverage of the pass.
 */
fragment float4 glyph_fragment(GlyphVertexOut in [[stage_in]],
                               texture2d<float, access::read> atlas [[texture(0)]],
                               texture2d<float, access::read> image [[texture(1)]],
                               constant uint &channel [[buffer(0)]])
{
    float4 c = in.color;
    if (in.mode == MODE_SOLID)
        return float4(c.rgb * c.a, c.a);
    if (in.mode == MODE_ALPHA) {
        float a = c.a * atlas.read(uint2(in.uv)).a;
        return float4(c.rgb * a, a);
    }
    if (in.mode == MODE_COLOR)
        return atlas.read(uint2(in.uv)) * c.a;
    if (in.mode == MODE_IMAGE)
        return float4(image.read(uint2(in.uv)).rgb, 1.0f);
    float3 cov = atlas.read(uint2(in.uv)).rgb;
    float a = c.a * cov[channel];
    return float4(c.rgb * a, a);
}

/* Copies the CPU pixels as they are. The pipeline has no blend. */
fragment float4 copy_fragment(GlyphVertexOut in [[stage_in]],
                              texture2d<float, access::read> pixels [[texture(0)]])
{
    return pixels.read(uint2(in.uv));
}

/* ---- UI shapes ---------------------------------------------------------- */

struct ShapeInstance {
    float4 rect;    /* The shape rect in pixels. It is also the quad. */
    float4 color;   /* Straight color. The alpha is the opacity. */
    float radius;
    uint mode;
    uint blur;      /* The shadow blur in pixels. */
};

struct ShapeVertexOut {
    float4 position [[position]];
    float4 rect [[flat]];
    float4 color [[flat]];
    float radius [[flat]];
    uint mode [[flat]];
    uint blur [[flat]];
};

vertex ShapeVertexOut shape_vertex(uint vertex_id [[vertex_id]],
                                   uint instance_id [[instance_id]],
                                   const device ShapeInstance *instances [[buffer(0)]],
                                   constant Uniforms &uniforms [[buffer(1)]])
{
    ShapeInstance in = instances[instance_id];
    ShapeVertexOut out;
    out.position = pixel_to_ndc(in.rect.xy + quad_corner(vertex_id) * in.rect.zw,
                                uniforms.viewport_size);
    out.rect = in.rect;
    out.color = in.color;
    out.radius = in.radius;
    out.mode = in.mode;
    out.blur = in.blur;
    return out;
}

/* The shadow of a straight edge, as the CPU mask table holds it. */
static inline int shadow_edge_at(int p, int extent, float sigma)
{
    if (p < -extent)
        return 0;
    if (p > extent)
        return 255;
    return shape_shadow_edge(p, sigma);
}

static inline int shadow_slab(int p, int len, int extent, float sigma)
{
    int near_edge = shadow_edge_at(p, extent, sigma);
    int far_edge = shadow_edge_at(len - 1 - p, extent, sigma);
    return near_edge + far_edge > 255 ? near_edge + far_edge - 255 : 0;
}

/* The coverage of one pixel of a shadow. This is ui_shadow_coverage(). */
static inline int shadow_coverage(int2 pixel, float4 painted, int radius, int blur)
{
    float sigma = float(blur) * 0.5f;
    int extent = (blur * 3 + 1) / 2;
    int2 box_pos = int2(painted.xy) + extent;
    int2 box_size = max(int2(painted.zw) - 2 * extent, int2(0));
    int px = pixel.x - box_pos.x;
    int py = pixel.y - box_pos.y;
    int span = 2 * (radius + extent);
    if (radius > 0 && box_size.x >= span && box_size.y >= span) {
        int cx = min(px + extent, box_size.x - 1 - px + extent);
        int cy = min(py + extent, box_size.y - 1 - py + extent);
        if (cx < radius + 2 * extent && cy < radius + 2 * extent) {
            if (cx < 0 || cy < 0)
                return 0;
            float x = float(cx - extent) + 0.5f;
            float y = float(cy - extent) + 0.5f;
            float r = float(radius);
            int n = radius * 4 < 32 ? 32 : radius * 4;
            float sum = 0.0f;
            for (int k = 0; k < n; k++) {
                float v = shape_shadow_slice(r, k, n);
                sum += shape_shadow_row_term(y, v, r, n, sigma) *
                       shape_shadow_col_term(x, v, r, sigma);
            }
            return shape_shadow_tile(sum, x, y, r, sigma);
        }
    }
    int sx = shadow_slab(px, box_size.x, extent, sigma);
    int sy = shadow_slab(py, box_size.y, extent, sigma);
    return (sx * sy + 127) / 255;
}

fragment float4 shape_fragment(ShapeVertexOut in [[stage_in]])
{
    /* The shape math takes pixel indexes, not pixel centers. */
    float2 p = floor(in.position.xy);
    float cov;
    if (in.mode == SHAPE_ROUNDED) {
        cov = float(shape_rrect_cov(p.x, p.y, in.rect.x, in.rect.y, in.rect.z, in.rect.w,
                                    in.radius)) / 255.0f;
    } else if (in.mode == SHAPE_CIRCLE) {
        float cx = in.rect.x + in.radius - 0.5f;
        float cy = in.rect.y + in.radius - 0.5f;
        cov = float(shape_circle_cov(p.x, p.y, cx, cy, in.radius)) / 255.0f;
    } else {
        cov = float(shadow_coverage(int2(p), in.rect, int(in.radius), int(in.blur))) / 255.0f;
    }
    float a = in.color.a * cov;
    return float4(in.color.rgb * a, a);
}
