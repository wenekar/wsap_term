#include <metal_stdlib>
using namespace metal;

static inline float2 quad_corner(uint vertex_id)
{
    constexpr float2 corners[] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1)
    };
    return corners[vertex_id];
}

struct Uniforms {
    float2 viewport_size;
};

struct UiShapeVertexIn {
    float4 rect;
    float4 color;
    float radius;
    uint mode;
    uint alpha;
};

struct UiShapeVertexOut {
    float4 position [[position]];
    float2 v_pix;
    float4 v_rect;
    float4 v_color;
    float v_radius;
    uint v_mode;
    uint v_alpha;
};

vertex UiShapeVertexOut ui_shape_vertex(uint vertex_id [[vertex_id]],
                                        uint instance_id [[instance_id]],
                                        const device UiShapeVertexIn *vertices [[buffer(0)]],
                                        constant Uniforms &uniforms [[buffer(1)]])
{
    UiShapeVertexOut out;
    UiShapeVertexIn in = vertices[instance_id];
    float2 pos = in.rect.xy + quad_corner(vertex_id) * in.rect.zw;
    out.v_pix = pos;
    out.v_rect = in.rect;
    out.v_color = in.color;
    out.v_radius = in.radius;
    out.v_mode = in.mode;
    out.v_alpha = in.alpha;

    float2 ndc = float2(
        (pos.x / (uniforms.viewport_size.x * 0.5f)) - 1.0f,
        1.0f - (pos.y / (uniforms.viewport_size.y * 0.5f))
    );
    out.position = float4(ndc, 0.0f, 1.0f);
    return out;
}

static inline float sd_rounded_box(float2 p, float2 half_size, float r)
{
    float2 q = abs(p) - half_size + r;
    return length(max(q, float2(0.0f))) + min(max(q.x, q.y), 0.0f) - r;
}

static inline float erf_approx(float x)
{
    float s = sign(x);
    float a = abs(x);
    float t = 1.0f / (1.0f + 0.3275911f * a);
    float y = 1.0f - (((((1.061405429f * t - 1.453152027f) * t)
        + 1.421413741f) * t - 0.284496736f) * t + 0.254829592f)
        * t * exp(-a * a);
    return s * y;
}

static inline float normal_cdf(float z)
{
    return 0.5f * (1.0f + erf_approx(z * 0.70710678f));
}

fragment float4 ui_shape_fragment(UiShapeVertexOut in [[stage_in]])
{
    float4 col = in.v_color;
    float alpha = 1.0f;
    float cov = 1.0f;

    if (in.v_mode == 3u) {
        float blur = float(in.v_alpha);
        float sigma = blur * 0.5f;
        float extent = ceil(1.5f * blur);
        float2 half_size = max(in.v_rect.zw * 0.5f - extent, float2(0.0f));
        float2 center = in.v_rect.xy + in.v_rect.zw * 0.5f;
        float d = sd_rounded_box(in.v_pix - center, half_size,
                                 min(in.v_radius, min(half_size.x, half_size.y)));
        alpha = 1.0f;
        cov = sigma > 0.0f ? clamp(normal_cdf(-d / sigma), 0.0f, 1.0f) : step(d, 0.0f);
    } else if (in.v_mode == 2u) {
        float2 center = in.v_rect.xy + in.v_radius + 0.5f;
        float d = length(in.v_pix - center);
        cov = clamp(in.v_radius + 0.5f - d, 0.0f, 1.0f);
    } else if (in.v_mode == 1u) {
        float r = min(in.v_radius, min(in.v_rect.z, in.v_rect.w) * 0.5f);
        float2 center = in.v_rect.xy + in.v_rect.zw * 0.5f;
        float2 q = abs(in.v_pix - center) - in.v_rect.zw * 0.5f + r;
        float d = length(max(q, float2(0.0f))) + min(max(q.x, q.y), 0.0f) - r;
        cov = clamp(0.5f - d, 0.0f, 1.0f);
    }

    return float4(col.rgb, col.a * alpha * cov);
}

struct GlyphVertexIn {
    float4 rect;
    float4 uv;
    float4 color;
    uint mode;
};

struct GlyphVertexOut {
    float4 position [[position]];
    float2 uv;
    float4 color;
    uint mode;
};

vertex GlyphVertexOut glyph_vertex(uint vertex_id [[vertex_id]],
                                        uint instance_id [[instance_id]],
                                   const device GlyphVertexIn *vertices [[buffer(0)]],
                                   constant Uniforms &uniforms [[buffer(1)]])
{
    GlyphVertexOut out;
    GlyphVertexIn in = vertices[instance_id];
    float2 corner = quad_corner(vertex_id);
    float2 pos = in.rect.xy + corner * in.rect.zw;
    out.uv = mix(in.uv.xy, in.uv.zw, corner);
    out.color = in.color;
    out.mode = in.mode;

    float2 ndc = float2(
        (pos.x / (uniforms.viewport_size.x * 0.5f)) - 1.0f,
        1.0f - (pos.y / (uniforms.viewport_size.y * 0.5f))
    );
    out.position = float4(ndc, 0.0f, 1.0f);
    return out;
}

fragment float4 glyph_fragment(GlyphVertexOut in [[stage_in]],
                               texture2d<float> atlasTex [[texture(0)]],
                               texture2d<float> asciiTex [[texture(1)]],
                               sampler imageSampler [[sampler(0)]])
{
    constexpr sampler texel(coord::pixel, filter::nearest, address::clamp_to_edge);
    if (in.mode == 0) {
        return in.color;
    } else if (in.mode == 4) {
        float a = asciiTex.sample(texel, in.uv).r;
        return float4(in.color.rgb, in.color.a * a);
    } else if (in.mode == 1) {
        float a = atlasTex.sample(texel, in.uv).a;
        return float4(in.color.rgb, in.color.a * a);
    } else if (in.mode == 2) {
        float4 c = atlasTex.sample(texel, in.uv);
        return float4(c.rgb, c.a * in.color.a);
    } else if (in.mode == 5) {
        float4 mask = atlasTex.sample(texel, in.uv);
        return float4(in.color.rgb * mask.rgb, in.color.a * dot(mask.rgb, float3(0.299f, 0.587f, 0.114f)));
    } else {
        return float4(atlasTex.sample(imageSampler, in.uv).rgb * in.color.rgb, in.color.a);
    }
}
