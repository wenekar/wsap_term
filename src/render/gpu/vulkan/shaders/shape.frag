#version 450

/*
 * Shape fragment shader for Vulkan backend.
 * Evaluates coverage for rounded boxes, circles, and drop shadows.
 */

layout(location = 0) flat in vec4 in_rect;
layout(location = 1) flat in vec4 in_color;
layout(location = 2) flat in float in_radius;
layout(location = 3) flat in uint in_mode;
layout(location = 4) flat in uint in_blur;

layout(location = 0) out vec4 out_color;

int shape_quantize(float v)
{
    if (!(v > 0.0))
        return 0;
    if (v >= 1.0)
        return 255;
    return int(v * 255.0 + 0.5);
}

int shape_corner_cov(float dx, float dy, float r)
{
    float d2 = dx * dx + dy * dy;
    float in_r = r - 1.0;
    float out_r = r + 1.0;
    if (in_r > 0.0 && d2 <= in_r * in_r)
        return 255;
    if (d2 >= out_r * out_r)
        return 0;
    return shape_quantize(r + 0.5 - sqrt(d2));
}

int shape_rrect_cov(float x, float y, float x0, float y0,
                    float w, float h, float r)
{
    float ctl_x = x0 + r;
    float ctl_y = y0 + r;
    float ctr_x = x0 + w - r - 1.0;
    float cbl_y = y0 + h - r - 1.0;
    float lo_x = min(ctl_x, ctr_x);
    float hi_x = max(ctl_x, ctr_x);
    float lo_y = min(ctl_y, cbl_y);
    float hi_y = max(ctl_y, cbl_y);
    if ((y >= lo_y && y <= hi_y) || (x >= lo_x && x <= hi_x))
        return 255;
    float cx = x < lo_x ? lo_x : hi_x;
    float cy = y < lo_y ? lo_y : hi_y;
    return shape_corner_cov(x - cx, y - cy, r);
}

int shape_circle_cov(float x, float y, float cx, float cy, float r)
{
    return shape_corner_cov(x - cx, y - cy, r);
}

float shape_phi(float z)
{
    float x = abs(z) * 0.70710678;
    float t = 1.0 / (1.0 + 0.3275911 * x);
    float poly = ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t
                   - 0.284496736) * t + 0.254829592) * t;
    float cdf = 0.5 * poly * exp(-x * x);
    return z < 0.0 ? cdf : 1.0 - cdf;
}

int shape_shadow_edge(int p, float sigma)
{
    return shape_quantize(shape_phi((float(p) + 0.5) / sigma));
}

float shape_shadow_slice(float r, int k, int n)
{
    float phi = (float(k) + 0.5) / float(n) * 1.5707963267948966;
    return r * sin(phi);
}

float shape_shadow_row_term(float y, float v, float r, int n, float sigma)
{
    float dy = y - (r - v);
    return exp(-dy * dy / (2.0 * sigma * sigma)) * (1.5707963267948966 / float(n));
}

float shape_shadow_col_term(float x, float v, float r, float sigma)
{
    float w = sqrt(max(0.0, r * r - (r - v) * (r - v)));
    return shape_phi((x - (r - w) + 0.5) / sigma);
}

int shape_shadow_tile(float sum, float x, float y, float r, float sigma)
{
    float base = shape_phi((x + 0.5) / sigma) * shape_phi((y + 0.5) / sigma);
    return shape_quantize(base - sum * 0.3989422804014327 / sigma);
}

int shadow_edge_at(int p, int extent, float sigma)
{
    if (p < -extent)
        return 0;
    if (p > extent)
        return 255;
    return shape_shadow_edge(p, sigma);
}

int shadow_slab(int p, int len, int extent, float sigma)
{
    int near_edge = shadow_edge_at(p, extent, sigma);
    int far_edge = shadow_edge_at(len - 1 - p, extent, sigma);
    return near_edge + far_edge > 255 ? near_edge + far_edge - 255 : 0;
}

int shadow_coverage(ivec2 pixel, vec4 painted, int radius, int blur)
{
    float sigma = float(blur) * 0.5;
    int extent = (blur * 3 + 1) / 2;
    ivec2 box_pos = ivec2(painted.xy) + extent;
    ivec2 box_size = max(ivec2(painted.zw) - 2 * extent, ivec2(0));
    int px = pixel.x - box_pos.x;
    int py = pixel.y - box_pos.y;
    if (px < -extent || px >= box_size.x + extent ||
        py < -extent || py >= box_size.y + extent)
        return 0;
    int span = 2 * (radius + extent);
    if (radius > 0 && box_size.x >= span && box_size.y >= span) {
        int cx = min(px + extent, box_size.x - 1 - px + extent);
        int cy = min(py + extent, box_size.y - 1 - py + extent);
        if (cx < radius + 2 * extent && cy < radius + 2 * extent) {
            if (cx < 0 || cy < 0)
                return 0;
            float x = float(cx - extent) + 0.5;
            float y = float(cy - extent) + 0.5;
            float r = float(radius);
            int n = radius * 4 < 32 ? 32 : radius * 4;
            float sum = 0.0;
            for (int k = 0; k < n; k++) {
                float v = shape_shadow_slice(r, k, n);
                sum += shape_shadow_row_term(y, v, r, n, sigma) *
                       shape_shadow_col_term(x, v, r, sigma);
            }
            return shape_shadow_tile(sum, x, y, r, sigma);
        }
    }
    int sx = shadow_slab(px, box_size.x, extent, sigma);
    if (sx == 0)
        return 0;
    int sy = shadow_slab(py, box_size.y, extent, sigma);
    return (sx * sy + 127) / 255;
}

void main()
{
    if (in_color.a <= 0.0)
        discard;
    vec2 p = floor(gl_FragCoord.xy);
    float cov;
    if (in_mode == 1u) {
        cov = float(shape_rrect_cov(p.x, p.y, in_rect.x, in_rect.y, in_rect.z, in_rect.w, in_radius)) / 255.0;
    } else if (in_mode == 2u) {
        float cx = in_rect.x + in_radius - 0.5;
        float cy = in_rect.y + in_radius - 0.5;
        cov = float(shape_circle_cov(p.x, p.y, cx, cy, in_radius)) / 255.0;
    } else {
        cov = float(shadow_coverage(ivec2(p), in_rect, int(in_radius), int(in_blur))) / 255.0;
    }
    if (cov <= 0.0)
        discard;
    float a = in_color.a * cov;
    out_color = vec4(in_color.rgb * a, a);
}
