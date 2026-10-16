#import <CoreText/CoreText.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include "font/backend.h"
#include "font/internal.h"
#include "font/shaping.h"
#include "platform/platform.h"
#include "font/glyph/intern.h"
#include "wsap_vtcore/unicode/unicode.h"
#include "util/hash.h"

#include <hb-ot.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_FAMILY_LIST FONT_DEFAULT_FAMILY

char **font_list_families(size_t *count)
{
    *count = 0;
    @autoreleasepool {
        int32_t mono_trait = kCTFontMonoSpaceTrait;
        const void *trait_keys[] = { kCTFontSymbolicTrait };
        const void *trait_values[] = { CFNumberCreate(kCFAllocatorDefault,
            kCFNumberSInt32Type, &mono_trait) };
        if (!trait_values[0]) return NULL;
        CFDictionaryRef traits = CFDictionaryCreate(kCFAllocatorDefault,
            trait_keys, trait_values, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFRelease(trait_values[0]);
        if (!traits) return NULL;
        const void *keys[] = { kCTFontTraitsAttribute };
        const void *values[] = { traits };
        CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault,
            keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFRelease(traits);
        if (!attrs) return NULL;
        CTFontDescriptorRef query = CTFontDescriptorCreateWithAttributes(attrs);
        CFRelease(attrs);
        if (!query) return NULL;
        CTFontCollectionRef collection = CTFontCollectionCreateWithFontDescriptors(
            (__bridge CFArrayRef)@[(__bridge id)query], NULL);
        CFRelease(query);
        if (!collection) return NULL;
        CFArrayRef families = CTFontCollectionCreateMatchingFontDescriptors(collection);
        CFRelease(collection);
        CFIndex total = families ? CFArrayGetCount(families) : 0;
        char **names = calloc((size_t)total + 1, sizeof(*names));
        bool ok = names != NULL;
        NSMutableSet<NSString *> *seen = [NSMutableSet set];
        for (CFIndex i = 0; ok && i < total; i++) {
            CTFontDescriptorRef descriptor = CFArrayGetValueAtIndex(families, i);
            CFStringRef family = CTFontDescriptorCopyAttribute(descriptor,
                                                               kCTFontFamilyNameAttribute);
            if (!family) continue;
            NSString *ns_family = (__bridge NSString *)family;
            if ([seen containsObject:ns_family]) {
                CFRelease(family);
                continue;
            }
            [seen addObject:ns_family];
            const char *name = [ns_family UTF8String];
            names[*count] = name ? strdup(name) : NULL;
            CFRelease(family);
            if (!names[*count]) ok = false;
            else (*count)++;
        }
        if (families) CFRelease(families);
        if (!ok) {
            for (size_t i = 0; i < *count; i++) free(names[i]);
            free(names);
            names = NULL;
            *count = 0;
        }
        return names;
    }
}

enum { CT_CASCADE_MAX = 64 };

struct font_handle {
    CTFontRef regular;
    CTFontRef italic;
    CTFontRef bold;
    CTFontRef bold_italic;
    int pixel_size;
    CGFloat scale;
    uint32_t serial;
    hb_face_t *hb_face[FONT_VARIANT_COUNT];
    hb_font_t *hb_font[FONT_VARIANT_COUNT];
    CFArrayRef cascade_descriptors;
    CTFontRef cascade_fonts[CT_CASCADE_MAX];
    CFIndex cascade_loaded;
    pthread_mutex_t cascade_lock;
};

static _Atomic uint32_t ct_face_serial;

enum { CT_NO_QUANTIZE = 1, CT_SMOOTH = 2, CT_MONO = 4 };
static _Atomic unsigned ct_render_options;

static void ct_set_render_options(unsigned mask, unsigned value)
{
    unsigned old = atomic_load_explicit(&ct_render_options, memory_order_relaxed);
    unsigned next;
    do {
        next = (old & ~mask) | value;
        if (next == old)
            return;
    } while (!atomic_compare_exchange_weak_explicit(&ct_render_options, &old, next,
                memory_order_relaxed, memory_order_relaxed));
    font_bump_generation();
}

void font_set_hint_style(enum font_hint_style style)
{
    ct_set_render_options(CT_NO_QUANTIZE, style == FONT_HINT_NONE ? CT_NO_QUANTIZE : 0);
}

void font_set_aa_mode(enum font_aa_mode aa)
{
    ct_set_render_options(CT_MONO, aa == FONT_AA_MONO ? CT_MONO : 0);
}

void font_set_smoothing(bool enabled)
{
    ct_set_render_options(CT_SMOOTH, enabled ? CT_SMOOTH : 0);
}

static void ct_configure_context(CGContextRef ctx, bool color)
{
    unsigned options = atomic_load_explicit(&ct_render_options, memory_order_relaxed);
    bool aa = color || !(options & CT_MONO);
    bool smooth = aa && (options & CT_SMOOTH);
    CGContextSetAllowsFontSmoothing(ctx, true);
    CGContextSetShouldSmoothFonts(ctx, smooth);
    CGContextSetAllowsAntialiasing(ctx, aa);
    CGContextSetShouldAntialias(ctx, aa);
    CGContextSetAllowsFontSubpixelPositioning(ctx, true);
    CGContextSetShouldSubpixelPositionFonts(ctx, !(options & CT_MONO));
    CGContextSetAllowsFontSubpixelQuantization(ctx, true);
    CGContextSetShouldSubpixelQuantizeFonts(ctx, !(options & CT_NO_QUANTIZE));
}

static CGRect ct_device_rect(CGRect r, CGFloat scale)
{
    if (CGRectIsNull(r))
        return r;
    return CGRectMake(r.origin.x * scale, r.origin.y * scale,
                      r.size.width * scale, r.size.height * scale);
}

static CFStringRef ct_string(const uint32_t *cps, int count, int *cell_map)
{
    UniChar chars[FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN * 2];
    int used = 0;
    if (count < 1 || count > FONT_RUN_MAX)
        return NULL;
    for (int c = 0; c < count; c++) {
        uint32_t expanded[WSAP_VTCORE_GRAPHEME_MAX_LEN];
        int n = glyph_intern_expand(cps[c], expanded, WSAP_VTCORE_GRAPHEME_MAX_LEN);
        if (n < 1)
            return NULL;
        for (int i = 0; i < n; i++) {
            uint32_t cp = expanded[i];
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                return NULL;
            if (cp > 0xFFFF) {
                chars[used] = (UniChar)(0xD800 + ((cp - 0x10000) >> 10));
                if (cell_map) cell_map[used] = c;
                used++;
                cp = 0xDC00 + ((cp - 0x10000) & 0x3FF);
            }
            chars[used] = (UniChar)cp;
            if (cell_map) cell_map[used] = c;
            used++;
        }
    }
    return CFStringCreateWithCharacters(kCFAllocatorDefault, chars, used);
}

static CTLineRef ct_line(CTFontRef font, CFStringRef string)
{
    const void *keys[] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
    const void *values[] = { font, kCFBooleanTrue };
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!attrs)
        return NULL;
    CFAttributedStringRef text = CFAttributedStringCreate(kCFAllocatorDefault, string, attrs);
    CFRelease(attrs);
    if (!text)
        return NULL;
    CTLineRef line = CTLineCreateWithAttributedString(text);
    CFRelease(text);
    return line;
}

static hb_blob_t *ct_reference_table(hb_face_t *face, hb_tag_t tag, void *ud)
{
    (void)face;
    if (!tag)
        return NULL;
    CFDataRef data = CTFontCopyTable((CTFontRef)ud, (CTFontTableTag)tag,
                                     kCTFontTableOptionNoOptions);
    if (!data)
        return NULL;
    return hb_blob_create((const char *)CFDataGetBytePtr(data),
                          (unsigned)CFDataGetLength(data), HB_MEMORY_MODE_READONLY,
                          (void *)data, (hb_destroy_func_t)CFRelease);
}

static void copy_variations(CTFontRef font, hb_font_t *hb)
{
    CFDictionaryRef vars = CTFontCopyVariation(font);
    if (!vars)
        return;
    CFIndex n = CFDictionaryGetCount(vars);
    if (n > 0 && n <= 64) {
        const void *keys[64], *values[64];
        hb_variation_t out[64];
        unsigned count = 0;
        CFDictionaryGetKeysAndValues(vars, keys, values);
        for (CFIndex i = 0; i < n; i++) {
            int64_t tag = 0;
            double value = 0;
            if (CFNumberGetValue(keys[i], kCFNumberSInt64Type, &tag) &&
                CFNumberGetValue(values[i], kCFNumberDoubleType, &value))
                out[count++] = (hb_variation_t){ (hb_tag_t)tag, (float)value };
        }
        hb_font_set_variations(hb, out, count);
    }
    CFRelease(vars);
}

static void ct_open_hb(struct font_handle *face, enum font_variant variant,
                       CTFontRef font)
{
    face->hb_face[variant] = hb_face_create_for_tables(ct_reference_table,
        (void *)CFRetain(font), (hb_destroy_func_t)CFRelease);
    hb_face_set_upem(face->hb_face[variant], CTFontGetUnitsPerEm(font));
    hb_font_t *hb = hb_font_create(face->hb_face[variant]);
    int scale = (int)lround(CTFontGetSize(font) * face->scale * 64);
    hb_font_set_scale(hb, scale, scale);
    copy_variations(font, hb);
    hb_font_make_immutable(hb);
    face->hb_font[variant] = hb;
}

static void register_font_dir(NSString *dir)
{
    if (!dir)
        return;
    @autoreleasepool {
        NSArray<NSString *> *files =
            [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil];
        if (!files) return;
        CFMutableArrayRef urls = CFArrayCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeArrayCallBacks);
        if (!urls) return;
        for (NSString *f in files) {
            NSString *ext = f.pathExtension.lowercaseString;
            if (![ext isEqualToString:@"ttf"] && ![ext isEqualToString:@"otf"] &&
                ![ext isEqualToString:@"ttc"] && ![ext isEqualToString:@"dfont"])
                continue;
            NSURL *url = [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:f]];
            CFArrayAppendValue(urls, (__bridge CFURLRef)url);
        }
        if (CFArrayGetCount(urls)) {
            CTFontManagerRegisterFontURLs(urls, kCTFontManagerScopeProcess, true,
                ^bool(CFArrayRef errors, bool done) {
                    (void)done;
                    for (CFIndex i = 0; errors && i < CFArrayGetCount(errors); i++) {
                        CFErrorRef error = (CFErrorRef)CFArrayGetValueAtIndex(errors, i);
                        CFStringRef description = CFErrorCopyDescription(error);
                        if (description) {
                            NSLog(@"Unable to register bundled font: %@", description);
                            CFRelease(description);
                        }
                    }
                    return true;
                });
        }
        CFRelease(urls);
    }
}

static void register_bundled_fonts(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        char resources[PATH_MAX];
        if (platform_resource_dir(resources, sizeof(resources)) == 0) {
            register_font_dir([[NSString stringWithUTF8String:resources]
                               stringByAppendingPathComponent:@"fonts"]);
            return;
        }
#ifdef WSAP_DEV_FONTS_DIR
        register_font_dir(@WSAP_DEV_FONTS_DIR);
#endif
#ifdef WSAP_DATADIR
        register_font_dir(@WSAP_DATADIR "/wsap_term/fonts");
#endif
    });
}

static CTFontRef create_style_copy(CTFontRef base, CTFontSymbolicTraits traits)
{
    if (!base) return NULL;
    CTFontRef copy = CTFontCreateCopyWithSymbolicTraits(base, 0.0, NULL, traits, traits);
    if (copy) return copy;
    if ((traits & kCTFontTraitBold) && (traits & kCTFontTraitItalic)) {
        CTFontRef bold = CTFontCreateCopyWithSymbolicTraits(base, 0.0, NULL,
            kCTFontTraitBold, kCTFontTraitBold);
        CTFontRef target = bold ? bold : base;
        CGAffineTransform shear = CGAffineTransformMake(1, 0, tan(12.0 * M_PI / 180.0), 1, 0, 0);
        CTFontRef sheared = CTFontCreateCopyWithAttributes(target, 0.0, &shear, NULL);
        if (bold) CFRelease(bold);
        if (sheared) return sheared;
    }
    if (traits & kCTFontTraitItalic) {
        CGAffineTransform shear = CGAffineTransformMake(1, 0, tan(12.0 * M_PI / 180.0), 1, 0, 0);
        return CTFontCreateCopyWithAttributes(base, 0.0, &shear, NULL);
    }
    return (CTFontRef)CFRetain(base);
}

static void normalize_name(const char *src, char *dst, size_t cap)
{
    size_t n = 0;
    for (; *src && n + 1 < cap; src++) {
        if (*src >= 'a' && *src <= 'z')
            dst[n++] = *src;
        else if (*src >= 'A' && *src <= 'Z')
            dst[n++] = (char)(*src + ('a' - 'A'));
        else if (*src >= '0' && *src <= '9')
            dst[n++] = *src;
    }
    dst[n] = '\0';
}

static bool ct_font_name_matches(CTFontRef font, const char *want)
{
    char w[128];
    normalize_name(want, w, sizeof(w));
    if (!w[0])
        return true;

    CFStringRef names[] = {
        CTFontCopyPostScriptName(font),
        CTFontCopyFamilyName(font),
        CTFontCopyFullName(font),
        CTFontCopyDisplayName(font),
    };
    bool ok = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char buf[256], norm[128];
        if (names[i] &&
            CFStringGetCString(names[i], buf, sizeof(buf), kCFStringEncodingUTF8)) {
            normalize_name(buf, norm, sizeof(norm));
            if (!strcmp(norm, w))
                ok = true;
        }
        if (names[i])
            CFRelease(names[i]);
    }
    return ok;
}

static CTFontRef ct_font_from_descriptor(CFStringRef name, CFStringRef attribute, CGFloat size)
{
    const void *keys[] = { attribute };
    const void *values[] = { name };
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!attrs) return NULL;
    CTFontDescriptorRef query = CTFontDescriptorCreateWithAttributes(attrs);
    CFRelease(attrs);
    if (!query) return NULL;
    CTFontDescriptorRef match = CTFontDescriptorCreateMatchingFontDescriptor(query, NULL);
    CFRelease(query);
    if (!match) return NULL;
    CTFontRef font = CTFontCreateWithFontDescriptor(match, size, NULL);
    CFRelease(match);
    return font;
}

static CTFontRef ct_load_base_font(const char *family_list, CGFloat pt)
{

    char copy[256];
    strncpy(copy, family_list && family_list[0] ? family_list : DEFAULT_FAMILY_LIST, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *saveptr = NULL;
    for (char *name = strtok_r(copy, ",", &saveptr); name; name = strtok_r(NULL, ",", &saveptr)) {
        while (*name == ' ') name++;
        char *end = name + strlen(name) - 1;
        while (end > name && *end == ' ') *end-- = '\0';
        if (!*name)
            continue;

        char norm[128];
        normalize_name(name, norm, sizeof(norm));

        CTFontRef font = NULL;
        if (!strcmp(norm, "sfmono")) {
            CTFontRef ui = CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, pt, NULL);
            if (ui && ct_font_name_matches(ui, name))
                font = ui;
            else if (ui)
                CFRelease(ui);
        }
        if (!font) {
            CFStringRef cf_name = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
            if (cf_name) {
                font = ct_font_from_descriptor(cf_name, kCTFontNameAttribute, pt);
                if (!font)
                    font = ct_font_from_descriptor(cf_name, kCTFontFamilyNameAttribute, pt);
                CFRelease(cf_name);
            }
        }
        if (font)
            return font;
    }

    CTFontRef sys = CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, pt, NULL);
    if (sys)
        return sys;

    return CTFontCreateWithName(CFSTR("Menlo"), pt, NULL);
}

static font_handle_t *ct_face_open(const char *family_list, struct font_size size)
{
    int pt = font_size_resolve(size).pt;
    int pixel_size = font_size_px(size);

    CTFontRef base = ct_load_base_font(family_list, (CGFloat)pt);
    if (!base)
        return NULL;

    struct font_handle *face = calloc(1, sizeof(*face));
    if (!face) {
        CFRelease(base);
        return NULL;
    }

    face->regular = base;
    face->italic = create_style_copy(base, kCTFontTraitItalic);
    face->bold = create_style_copy(base, kCTFontTraitBold);
    face->bold_italic = create_style_copy(base, kCTFontTraitBold | kCTFontTraitItalic);
    face->pixel_size = pixel_size;
    face->scale = (CGFloat)pixel_size / pt;
    face->serial = atomic_fetch_add_explicit(&ct_face_serial, 1, memory_order_relaxed) + 1;
    face->cascade_descriptors = CTFontCopyDefaultCascadeListForLanguages(base, NULL);
    pthread_mutex_init(&face->cascade_lock, NULL);
    const CTFontRef styles[FONT_VARIANT_COUNT] = {
        [FONT_VARIANT_PLAIN] = face->regular,
        [FONT_VARIANT_BOLD] = face->bold,
        [FONT_VARIANT_ITALIC] = face->italic,
        [FONT_VARIANT_BOLD_ITALIC] = face->bold_italic,
    };
    for (int i = 0; i < FONT_VARIANT_COUNT; i++)
        ct_open_hb(face, (enum font_variant)i, styles[i]);
    return face;
}

static void ct_face_close(font_handle_t *face)
{
    if (!face)
        return;
    if (face->regular)     CFRelease(face->regular);
    if (face->italic)      CFRelease(face->italic);
    if (face->bold)        CFRelease(face->bold);
    if (face->bold_italic) CFRelease(face->bold_italic);
    for (int i = 0; i < FONT_VARIANT_COUNT; i++) {
        hb_font_destroy(face->hb_font[i]);
        hb_face_destroy(face->hb_face[i]);
    }
    if (face->cascade_descriptors)
        CFRelease(face->cascade_descriptors);
    for (CFIndex i = 0; i < face->cascade_loaded; i++) {
        if (face->cascade_fonts[i])
            CFRelease(face->cascade_fonts[i]);
    }
    pthread_mutex_destroy(&face->cascade_lock);
    free(face);
}

static bool ct_face_metrics(font_handle_t *face, struct font_metrics *out)
{
    if (!face || !face->regular || !out)
        return false;

    CTFontRef f = face->regular;
    CGFloat ascent = CTFontGetAscent(f) * face->scale;
    CGFloat descent = CTFontGetDescent(f) * face->scale;
    CGFloat leading = CTFontGetLeading(f) * face->scale;

    UniChar m = 'M';
    CGGlyph glyph = 0;
    CTFontGetGlyphsForCharacters(f, &m, &glyph, 1);

    CGSize adv = CGSizeZero;
    if (glyph)
        CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, &glyph, &adv, 1);
    int cell_w = (int)lround(adv.width * face->scale);
    if (cell_w <= 0) {
        UniChar fallback = '0';
        CGGlyph fallback_glyph = 0;
        CGSize fallback_advance = CGSizeZero;
        if (CTFontGetGlyphsForCharacters(f, &fallback, &fallback_glyph, 1) && fallback_glyph)
            CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal,
                                       &fallback_glyph, &fallback_advance, 1);
        cell_w = (int)lround(fallback_advance.width * face->scale);
    }
    if (cell_w <= 0)
        cell_w = 8;

    int asc = (int)ceil(ascent);
    int desc = (int)ceil(descent);
    int lead = leading > 0 ? (int)ceil(leading) : 0;
    int cell_h = asc + desc + lead;
    if (cell_h <= 0)
        cell_h = 16;

    *out = (struct font_metrics){
        .cell_w = cell_w,
        .cell_h = cell_h,
        .baseline = asc,
        .pixel_size = face->pixel_size,
        .ink_rows = cell_h,
    };
    return true;
}

static CTFontRef get_style_font(const font_handle_t *face, uint16_t style)
{
    if (!face)
        return NULL;
    bool bold = (style & FONT_STYLE_BOLD) != 0;
    bool italic = (style & FONT_STYLE_ITALIC) != 0;
    if (bold && italic && face->bold_italic) return face->bold_italic;
    if (bold && face->bold)                 return face->bold;
    if (italic && face->italic)             return face->italic;
    return face->regular;
}

struct ct_scratch {
    uint8_t *pixels;
    CGContextRef context;
    size_t row_bytes;
    int width, height;
    bool color;
};

static __thread struct ct_scratch ct_gray_scratch;
static __thread struct ct_scratch ct_color_scratch;
static __thread struct ct_scratch ct_bounds_scratch;

static void ct_worker_destroy(void);

static pthread_key_t ct_worker_key;
static pthread_once_t ct_worker_key_once = PTHREAD_ONCE_INIT;

static void ct_thread_exit_cleanup(void *val)
{
    (void)val;
    ct_worker_destroy();
}

static void ct_init_worker_key(void)
{
    pthread_key_create(&ct_worker_key, ct_thread_exit_cleanup);
}

static void ct_ensure_worker_cleanup(void)
{
    pthread_once(&ct_worker_key_once, ct_init_worker_key);
    if (!pthread_getspecific(ct_worker_key))
        pthread_setspecific(ct_worker_key, (void *)1);
}

static CGContextRef ct_scratch_get(struct ct_scratch *scratch, bool color,
                                   int width, int height, size_t *row_bytes)
{
    ct_ensure_worker_cleanup();
    if (width <= 0 || height <= 0)
        return NULL;
    int capacity = 64;
    while (capacity < width && capacity <= INT_MAX / 2)
        capacity *= 2;
    if (!scratch->context || scratch->height != height || scratch->width < width ||
        scratch->color != color) {
        if (scratch->context) CGContextRelease(scratch->context);
        free(scratch->pixels);
        memset(scratch, 0, sizeof(*scratch));
        scratch->width = capacity;
        scratch->height = height;
        scratch->color = color;
        size_t bpp = color ? 4 : 1;
        scratch->row_bytes = ((size_t)capacity * bpp + 15) & ~(size_t)15;
        scratch->pixels = calloc(scratch->row_bytes, (size_t)height);
        if (!scratch->pixels) {
            memset(scratch, 0, sizeof(*scratch));
            return NULL;
        }
        CGColorSpaceRef space = color ? CGColorSpaceCreateWithName(kCGColorSpaceSRGB)
                                      : NULL;
        scratch->context = CGBitmapContextCreate(scratch->pixels, capacity, height, 8,
            scratch->row_bytes, space,
            color ? (CGBitmapInfo)(kCGBitmapByteOrder32Host | kCGImageAlphaPremultipliedFirst)
                  : (CGBitmapInfo)kCGImageAlphaOnly);
        if (space) CGColorSpaceRelease(space);
        if (!scratch->context) {
            free(scratch->pixels);
            memset(scratch, 0, sizeof(*scratch));
            return NULL;
        }
    }
    *row_bytes = scratch->row_bytes;
    CGContextSaveGState(scratch->context);
    CGContextSetTextMatrix(scratch->context, CGAffineTransformIdentity);
    CGContextClearRect(scratch->context, CGRectMake(0, 0, scratch->width, scratch->height));
    return scratch->context;
}

static void ct_scratch_put(struct ct_scratch *scratch)
{
    CGContextRestoreGState(scratch->context);
}

static void ct_scratch_destroy(struct ct_scratch *scratch)
{
    if (scratch->context) CGContextRelease(scratch->context);
    free(scratch->pixels);
    memset(scratch, 0, sizeof(*scratch));
}

static void ct_configure_synthetic_bold(CGContextRef ctx, CTFontRef font, CGFloat scale)
{
    CGContextSetTextDrawingMode(ctx, kCGTextFillStroke);
    CGContextSetLineWidth(ctx, fmax(0.5 / scale, CTFontGetSize(font) * 0.04));
}

static bool ct_rasterize_ink(CTFontRef font, CGGlyph gid, CTLineRef line,
        CGRect bb, int advance, CGFloat units, bool color, uint16_t style,
        const struct font_metrics *m, struct glyph *out)
{
    int cols = m->cell_w * ((style & FONT_STYLE_WIDE) ? 2 : 1);
    int rows = m->cell_h < glyph_rows(out) ? m->cell_h : glyph_rows(out);
    bool ok = false;
    if (cols <= 0 || rows <= 0 || CGRectIsNull(bb))
        goto done;
    if (CGRectIsEmpty(bb)) {
        ok = glyph_prepare(out, (unsigned)cols);
        goto done;
    }
    int left = (int)floor(CGRectGetMinX(bb));
    int above = (int)ceil(CGRectGetMaxY(bb));
    int ink_w = (int)ceil(CGRectGetMaxX(bb)) - left;
    int ink_h = above - (int)floor(CGRectGetMinY(bb));
    int limit = m->ink_rows > 0 && m->ink_rows < rows ? m->ink_rows : rows;
    double scale = color || ink_h > limit ? (double)limit / ink_h : 1;
    int dst_x = left, dst_y = m->baseline - above;
    if (scale != 1 || color) {
        dst_x = (cols - (int)ceil(ink_w * scale)) / 2;
        dst_y = (rows - (int)ceil(ink_h * scale)) / 2;
    } else {
        if (advance > 0 && advance < cols) dst_x += (cols - advance) / 2;
        if (dst_y < 0) dst_y = 0;
        if (dst_y + ink_h > rows) dst_y = rows - ink_h;
    }
    int span_l = dst_x < 0 ? dst_x : 0;
    int span_r = dst_x + (int)ceil(ink_w * scale);
    if (span_r < cols) span_r = cols;
    int max_span = 4 * cols > 60 ? 4 * cols : 60;
    if (span_r - span_l > max_span) {
        scale *= (double)max_span / (span_r - span_l);
        dst_x = (cols - (int)ceil(ink_w * scale)) / 2;
        span_l = dst_x < 0 ? dst_x : 0;
        span_r = dst_x + (int)ceil(ink_w * scale);
        if (span_r < cols) span_r = cols;
        if (span_r - span_l > max_span) span_r = span_l + max_span;
    }
    int span = span_r - span_l;
    if (!glyph_prepare(out, (unsigned)span) ||
        (color && !glyph_color_plane(out)))
        goto done;
    struct ct_scratch *scratch = color ? &ct_color_scratch : &ct_gray_scratch;
    size_t row_bytes;
    CGContextRef ctx = ct_scratch_get(scratch, color, span, rows, &row_bytes);
    if (!ctx) goto done;
    uint8_t *pixels = scratch->pixels;
    ct_configure_context(ctx, color);
    if (color) CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
    else CGContextSetGrayFillColor(ctx, 1, 1);
    CTFontRef draw_font = font;
    if (!draw_font && line) {
        CFArrayRef line_runs = CTLineGetGlyphRuns(line);
        if (CFArrayGetCount(line_runs)) {
            CTRunRef run = CFArrayGetValueAtIndex(line_runs, 0);
            draw_font = CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
        }
    }
    if ((style & FONT_STYLE_BOLD) && draw_font &&
        !(CTFontGetSymbolicTraits(draw_font) & kCTFontTraitBold))
        ct_configure_synthetic_bold(ctx, draw_font, units);
    CGContextTranslateCTM(ctx, (dst_x - span_l) - left * scale,
                          rows - dst_y - above * scale);
    CGContextScaleCTM(ctx, scale * units, scale * units);
    if (line) {
        CGContextSetTextPosition(ctx, 0, 0);
        CTLineDraw(line, ctx);
    } else {
        CGPoint pos = CGPointZero;
        CTFontDrawGlyphs(font, &gid, &pos, 1, ctx);
    }
    ct_scratch_put(scratch);
    out->x_offset = (int16_t)span_l;
    out->has_color = color;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < span; x++) {
            uint8_t a;
            if (color) {
                const uint32_t *p = (const uint32_t *)(pixels + (size_t)y * row_bytes);
                uint32_t px = p[x];
                out->color[(size_t)y * span + x] = px;
                a = (uint8_t)(px >> 24);
            } else {
                a = pixels[(size_t)y * row_bytes + x];
                out->alpha[y][x] = a;
            }
            if (a) glyph_mark(out, y, x);
        }
    }
    ok = true;
done:
    return ok;
}
static bool ct_rasterize_cluster(CTFontRef font, CGFloat units, uint32_t cp,
                                 uint16_t style, const struct font_metrics *m,
                                 struct glyph *out)
{
    CFStringRef string = ct_string(&cp, 1, NULL);
    if (!string)
        return false;
    CTLineRef line = ct_line(font, string);
    CFRelease(string);
    if (!line)
        return false;
    bool color = false;
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    for (CFIndex i = 0; i < CFArrayGetCount(runs); i++) {
        CTRunRef run = CFArrayGetValueAtIndex(runs, i);
        CTFontRef run_font = CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
        color |= (CTFontGetSymbolicTraits(run_font) & kCTFontTraitColorGlyphs) != 0;
    }
    struct ct_scratch *bounds_scratch = &ct_bounds_scratch;
    size_t row_bytes;
    CGContextRef bounds_context = ct_scratch_get(bounds_scratch, color, 1, 1, &row_bytes);
    if (!bounds_context) {
        CFRelease(line);
        return false;
    }
    ct_configure_context(bounds_context, color);
    CGRect bb = ct_device_rect(CTLineGetImageBounds(line, bounds_context), units);
    ct_scratch_put(bounds_scratch);
    int advance = (int)lround(CTLineGetTypographicBounds(line, NULL, NULL, NULL) * units);
    bool ok = ct_rasterize_ink(NULL, 0, line, bb, advance, units, color, style, m, out);
    CFRelease(line);
    return ok;
}

static bool ct_rasterize_glyph(font_handle_t *face, uint32_t cp, uint16_t style,
                              const struct font_metrics *m, struct glyph *out)
{
    if (!face || !m || !out)
        return false;
    if (cp <= ' ')
        return true;
    CTFontRef font = get_style_font(face, style);
    if (wsap_vtcore_grapheme_is_handle(cp))
        return ct_rasterize_cluster(font, face->scale, cp, style, m, out);
    UniChar chars[2];
    CFIndex count;
    if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        chars[0] = (UniChar)cp;
        count = 1;
    } else if (cp <= 0x10FFFF) {
        uint32_t value = cp - 0x10000;
        chars[0] = (UniChar)(0xD800 + (value >> 10));
        chars[1] = (UniChar)(0xDC00 + (value & 0x3FF));
        count = 2;
    } else {
        return false;
    }
    CTFontRef matched = NULL;
    CGGlyph gids[2] = {0};
    if (CTFontGetGlyphsForCharacters(font, chars, gids, count) && gids[0]) {
        matched = (CTFontRef)CFRetain(font);
    } else {
        pthread_mutex_lock(&face->cascade_lock);
        for (CFIndex i = 0; i < face->cascade_loaded; i++) {
            CTFontRef fb = face->cascade_fonts[i];
            if (fb && CTFontGetGlyphsForCharacters(fb, chars, gids, count) && gids[0]) {
                matched = (CTFontRef)CFRetain(fb);
                break;
            }
        }
        if (!matched && face->cascade_descriptors) {
            CFIndex total = CFArrayGetCount(face->cascade_descriptors);
            while (!matched && face->cascade_loaded < total &&
                   face->cascade_loaded < CT_CASCADE_MAX) {
                CTFontDescriptorRef desc = CFArrayGetValueAtIndex(
                    face->cascade_descriptors, face->cascade_loaded);
                CTFontRef fb = CTFontCreateWithFontDescriptor(desc, CTFontGetSize(face->regular), NULL);
                face->cascade_fonts[face->cascade_loaded++] = fb;
                if (fb && CTFontGetGlyphsForCharacters(fb, chars, gids, count) && gids[0]) {
                    matched = (CTFontRef)CFRetain(fb);
                    break;
                }
            }
        }
        pthread_mutex_unlock(&face->cascade_lock);
        if (!matched) {
            UniChar copy[2] = { chars[0], count == 2 ? chars[1] : 0 };
            CFStringRef string = CFStringCreateWithCharacters(kCFAllocatorDefault, copy, count);
            if (!string) return false;
            matched = CTFontCreateForString(font, string, CFRangeMake(0, count));
            CFRelease(string);
        }
    }
    if (!matched)
        return false;
    bool ok = false;
    if (gids[0] || (CTFontGetGlyphsForCharacters(matched, chars, gids, count) && gids[0])) {
        CGRect bb;
        CGSize advance;
        CTFontGetBoundingRectsForGlyphs(matched, kCTFontOrientationHorizontal, gids, &bb, 1);
        if ((style & FONT_STYLE_BOLD) && !(CTFontGetSymbolicTraits(matched) & kCTFontTraitBold)) {
            CGFloat lw = fmax(0.5 / face->scale, CTFontGetSize(matched) * 0.04) * face->scale;
            CGFloat pad = ceil(lw * 0.5);
            bb = CGRectInset(bb, -pad, -pad);
        }
        CTFontGetAdvancesForGlyphs(matched, kCTFontOrientationHorizontal, gids, &advance, 1);
        bool color = (CTFontGetSymbolicTraits(matched) & kCTFontTraitColorGlyphs) != 0;
        ok = ct_rasterize_ink(matched, gids[0], NULL, ct_device_rect(bb, face->scale),
                              (int)lround(advance.width * face->scale), face->scale,
                              color, style, m, out);
    }
    CFRelease(matched);
    return ok;
}

struct ct_raster {
    uint8_t *alpha;
    CTFontRef font;
    int w, h;
    int left, above;
    uint32_t serial;
    uint32_t use_pass;
    uint16_t gid;
    uint8_t variant;
    uint8_t options;
    bool valid;
};

#define CT_RASTER_CACHE 4096
#define CT_RASTER_PROBES 8

static __thread struct ct_raster *ct_rasters;
static __thread uint32_t ct_use_pass;
static __thread hb_buffer_t *ct_buffer;

static void ct_worker_destroy(void)
{
    if (ct_rasters) {
        for (int i = 0; i < CT_RASTER_CACHE; i++) {
            free(ct_rasters[i].alpha);
            if (ct_rasters[i].font) CFRelease(ct_rasters[i].font);
        }
        free(ct_rasters);
        ct_rasters = NULL;
    }
    hb_buffer_destroy(ct_buffer);
    ct_buffer = NULL;
    ct_scratch_destroy(&ct_gray_scratch);
    ct_scratch_destroy(&ct_color_scratch);
    ct_scratch_destroy(&ct_bounds_scratch);
    ct_use_pass = 0;
}

static const struct ct_raster *ct_raster_gid(const font_handle_t *face,
                                             enum font_variant variant,
                                             CTFontRef font, CGGlyph gid)
{
    if (!ct_rasters && !(ct_rasters = calloc(CT_RASTER_CACHE, sizeof(*ct_rasters))))
        return NULL;
    uint32_t h = (face->serial * 2654435761u) ^ (gid * 40503u) ^
                 ((uint32_t)variant << 28) ^ (uint32_t)CFHash(font);
    h ^= h >> 15;
    struct ct_raster *r = NULL;
    unsigned options = atomic_load_explicit(&ct_render_options, memory_order_relaxed);
    for (uint32_t p = 0; p < CT_RASTER_PROBES; p++) {
        struct ct_raster *e = &ct_rasters[(h + p) & (CT_RASTER_CACHE - 1)];
        if (e->valid && e->serial == face->serial && e->gid == gid && e->variant == variant &&
            e->options == options && (e->font == font || CFEqual(e->font, font))) {
            e->use_pass = ct_use_pass;
            return e;
        }
        if (!r && (!e->valid || e->use_pass != ct_use_pass))
            r = e;
    }
    if (!r)
        return NULL;
    free(r->alpha);
    if (r->font) CFRelease(r->font);
    *r = (struct ct_raster){
        .serial = face->serial, .use_pass = ct_use_pass, .gid = gid, .variant = (uint8_t)variant,
        .font = (CTFontRef)CFRetain(font), .options = (uint8_t)options,
    };

    CGRect bb = CGRectNull;
    CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &gid, &bb, 1);
    bb = ct_device_rect(bb, face->scale);
    if (!CGRectIsNull(bb) && bb.size.width > 0 && bb.size.height > 0) {
        bool synth_bold = (variant & FONT_VARIANT_BOLD) &&
                          !(CTFontGetSymbolicTraits(font) & kCTFontTraitBold);
        int pad = 0;
        if (synth_bold) {
            CGFloat lw = fmax(0.5 / face->scale, CTFontGetSize(font) * 0.04) * face->scale;
            pad = (int)ceil(lw * 0.5);
        }
        r->left = (int)floor(bb.origin.x) - pad;
        r->above = (int)ceil(bb.origin.y + bb.size.height) + pad;
        r->w = (int)ceil(bb.origin.x + bb.size.width) + pad - r->left;
        r->h = r->above - ((int)floor(bb.origin.y) - pad);
        size_t row_bytes;
        r->alpha = calloc((size_t)r->w, (size_t)r->h);
        if (!r->alpha)
            return NULL;
        CGContextRef ctx = ct_scratch_get(&ct_gray_scratch, false,
                                          r->w, r->h, &row_bytes);
        if (!ctx) return NULL;
        uint8_t *scratch = ct_gray_scratch.pixels;
        ct_configure_context(ctx, false);
        CGContextSetGrayFillColor(ctx, 1.0, 1.0);
        if (synth_bold)
            ct_configure_synthetic_bold(ctx, font, face->scale);
        CGContextTranslateCTM(ctx, -r->left, r->h - r->above);
        CGContextScaleCTM(ctx, face->scale, face->scale);
        CGPoint pos = CGPointZero;
        CTFontDrawGlyphs(font, &gid, &pos, 1, ctx);
        ct_scratch_put(&ct_gray_scratch);
        for (int y = 0; y < r->h; y++) {
            memcpy(r->alpha + (size_t)y * r->w,
                   scratch + (size_t)y * row_bytes, (size_t)r->w);
        }
    }
    r->valid = true;
    return r;
}

static void ct_blit_direct(struct glyph *g, const struct ct_raster *src, const struct box *dst)
{
    int sw = src->w, sh = src->h;
    int r0 = dst->y < 0 ? -dst->y : 0;
    int rows = glyph_rows(g);
    int r1 = dst->y + sh > rows ? rows - dst->y : sh;
    int c0 = dst->x < 0 ? -dst->x : 0;
    int c1 = dst->x + sw > (int)g->width ? (int)g->width - dst->x : sw;
    if (r0 >= r1 || c0 >= c1)
        return;

    for (int r = r0; r < r1; r++) {
        int y = dst->y + r;
        const uint8_t *src_row = src->alpha + (size_t)r * sw;
        uint8_t *alpha = g->alpha[y];
        for (int c = c0; c < c1; c++) {
            uint8_t a = src_row[c];
            int x = dst->x + c;
            if (a > alpha[x])
                alpha[x] = a;
            if (alpha[x])
                glyph_mark(g, y, x);
        }
    }
}

static void ct_blit_scaled(struct glyph *g, const struct ct_raster *src, const struct box *dst)
{
    int sw = src->w, sh = src->h;
    bool mono = atomic_load_explicit(&ct_render_options, memory_order_relaxed) & CT_MONO;
    int rows = glyph_rows(g);
    for (int r = 0; r < dst->h; r++) {
        int y = dst->y + r;
        if (y < 0 || y >= rows)
            continue;
        int sy = r * sh / dst->h, sy_end = (r + 1) * sh / dst->h;
        if (sy_end <= sy) sy_end = sy + 1;
        if (sy_end > sh) sy_end = sh;

        for (int c = 0; c < dst->w; c++) {
            int x = dst->x + c;
            if (x < 0 || x >= (int)g->width)
                continue;
            int sx = c * sw / dst->w, sx_end = (c + 1) * sw / dst->w;
            if (sx_end <= sx) sx_end = sx + 1;
            if (sx_end > sw) sx_end = sw;

            unsigned sum = 0, count = 0;
            for (int yy = sy; yy < sy_end; yy++) {
                const uint8_t *row = src->alpha + (size_t)yy * sw;
                for (int xx = sx; xx < sx_end; xx++) {
                    sum += row[xx];
                    count++;
                }
            }
            if (!count)
                continue;
            uint8_t a = (uint8_t)(sum / count);
            if (mono) a = a >= 128 ? 255 : 0;
            if (a > g->alpha[y][x])
                g->alpha[y][x] = a;
            if (g->alpha[y][x])
                glyph_mark(g, y, x);
        }
    }
}

static void ct_blit(struct glyph *g, const struct ct_raster *src, const struct box *dst)
{
    if (!src || !src->alpha || dst->w <= 0 || dst->h <= 0 || src->w <= 0 || src->h <= 0)
        return;
    if (dst->w == src->w && dst->h == src->h)
        ct_blit_direct(g, src, dst);
    else
        ct_blit_scaled(g, src, dst);
}

struct ct_ink {
    const struct ct_raster *ras;
    struct box box;
};

static bool ct_fit_cells(const struct ct_ink *ink, int drawn, int n_cells,
                         struct glyph *cells)
{
    if (!drawn)
        return false;
    int cw = GLYPH_CELL_W, rows = GLYPH_CELL_H;
    int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
    for (int i = 0; i < drawn; i++) {
        const struct box *b = &ink[i].box;
        if (b->x < left) left = b->x;
        if (b->y < top) top = b->y;
        if (b->x + b->w > right) right = b->x + b->w;
        if (b->y + b->h > bottom) bottom = b->y + b->h;
    }
    int run_w = right - left, run_h = bottom - top;
    if (run_w <= 0 || run_h <= 0)
        return false;
    struct box fit = { left, font_thread_metrics.baseline + top, run_w, run_h };
    if (run_h > rows) {
        fit.w = run_w * rows / run_h > 0 ? run_w * rows / run_h : 1;
        fit.h = rows;
        fit.x = (n_cells * cw - fit.w) / 2;
        fit.y = 0;
    } else {
        if (fit.y + fit.h > rows) fit.y = rows - fit.h;
        if (fit.y < 0) fit.y = 0;
    }
    for (int i = 0; i < n_cells; i++)
        if (!glyph_reset(&cells[i], (unsigned)cw))
            return false;
    uint8_t covered[FONT_RUN_MAX] = {0};
    for (int i = 0; i < drawn; i++) {
        const struct box *b = &ink[i].box;
        struct box dst = {
            fit.x + (b->x - left) * fit.w / run_w,
            fit.y + (b->y - top) * fit.h / run_h,
            b->w * fit.w / run_w > 0 ? b->w * fit.w / run_w : 1,
            b->h * fit.h / run_h > 0 ? b->h * fit.h / run_h : 1,
        };
        if (dst.x < 0)
            dst.x = 0;
        int first = dst.x / cw;
        int last = (dst.x + dst.w - 1) / cw;
        if (last >= n_cells) last = n_cells - 1;
        for (int c = first; c <= last; c++) {
            struct box slice = dst;
            slice.x -= c * cw;
            ct_blit(&cells[c], ink[i].ras, &slice);
            covered[c] = 1;
        }
    }
    for (int c = 0; c < n_cells; c++) {
        bool inked = false;
        for (int y = 0; covered[c] && y < rows && !inked; y++)
            inked = glyph_row_inked(&cells[c], y);
        if (covered[c] && !inked)
            return false;
    }
    return true;
}

static enum font_run_result ct_shape_fallback(font_handle_t *face,
        const struct font_window *w, const struct font_metrics *m, struct glyph *cells)
{
    int cell_map[FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN * 2];
    CFStringRef string = ct_string(w->cps, w->len, cell_map);
    if (!string)
        return FONT_RUN_UNAVAILABLE;
    CFIndex units = CFStringGetLength(string);
    CTLineRef line = ct_line(get_style_font(face, w->style), string);
    CFRelease(string);
    if (!line)
        return FONT_RUN_UNAVAILABLE;
    struct ct_ink ink[FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN];
    CGFloat primary[FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN * 2];
    CGFloat secondary_offsets[FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN * 2];
    bool offset_valid[FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN * 2] = {0};
    if (units > FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN * 2) {
        CFRelease(line);
        return FONT_RUN_UNAVAILABLE;
    }
    int drawn = 0;
    enum font_run_result result = FONT_RUN_UNAVAILABLE;
    ct_use_pass++;
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    for (CFIndex i = 0; i < CFArrayGetCount(runs); i++) {
        CTRunRef run = CFArrayGetValueAtIndex(runs, i);
        CTFontRef font = CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
        if (!font || (CTFontGetSymbolicTraits(font) & kCTFontTraitColorGlyphs))
            goto done;
        CFIndex count = CTRunGetGlyphCount(run);
        if (count > FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN)
            goto done;
        CGGlyph gids[FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN];
        CGPoint positions[FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN];
        CGSize advances[FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN];
        CFIndex indices[FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN];
        CTRunGetGlyphs(run, CFRangeMake(0, count), gids);
        CTRunGetPositions(run, CFRangeMake(0, count), positions);
        CTRunGetAdvances(run, CFRangeMake(0, count), advances);
        CTRunGetStringIndices(run, CFRangeMake(0, count), indices);
        bool rtl = (CTRunGetStatus(run) & kCTRunStatusRightToLeft) != 0;
        for (CFIndex k = 0; k < count; k++) {
            CFIndex index = indices[k];
            if (index < 0 || index >= units || !gids[k])
                goto done;
            int c = cell_map[index] - w->core;
            if (c < 0 || c >= w->core_len)
                continue;
            const struct ct_raster *r = ct_raster_gid(face, font_variant_of(w->style), font, gids[k]);
            if (!r)
                goto done;
            if (!r->w || !r->h)
                continue;
            if (drawn == FONT_RUN_MAX * WSAP_VTCORE_GRAPHEME_MAX_LEN)
                goto done;
            CGFloat anchor, secondary;
            if (offset_valid[index]) {
                anchor = primary[index];
                secondary = secondary_offsets[index];
            } else {
                anchor = CTLineGetOffsetForStringIndex(line, index, &secondary);
                primary[index] = anchor;
                secondary_offsets[index] = secondary;
                offset_valid[index] = true;
            }
            if (rtl) {
                anchor -= advances[k].width;
                secondary -= advances[k].width;
            }
            if (fabs(positions[k].x - secondary) < fabs(positions[k].x - anchor))
                anchor = secondary;
            ink[drawn++] = (struct ct_ink){
                .ras = r,
                .box = {
                    c * m->cell_w + (int)lround((positions[k].x - anchor) * face->scale) +
                        r->left,
                    -(int)lround(positions[k].y * face->scale) - r->above,
                    r->w, r->h,
                },
            };
        }
    }
    if (ct_fit_cells(ink, drawn, w->core_len, cells))
        result = FONT_RUN_SHAPED;
done:
    CFRelease(line);
    return result;
}

static uint64_t ct_scan_run(const uint32_t *cps, int len, uint16_t style,
                            uint64_t *hash, int *dead_at)
{
    uint64_t h = hash64_step(HASH64_BASIS, style);
    for (int i = 0; i < len; i++) {
        h = hash64_step(h, cps[i]);
    }
    if (hash)
        *hash = h;
    if (dead_at)
        *dead_at = -1;
    return UINT64_MAX;
}

static enum font_run_result ct_shape_run(font_handle_t *face, const struct font_window *w,
                                         const struct font_metrics *m, struct glyph *cells)
{
    if (!face || !m || !w || !cells || w->len < 1 || w->len > FONT_RUN_MAX ||
        w->core < 0 || w->core_len < 1 || w->core + w->core_len > w->len)
        return FONT_RUN_UNAVAILABLE;
    face_activate(m);
    enum font_variant variant = font_variant_of(w->style);
    CTFontRef font = get_style_font(face, w->style);
    hb_font_t *hb = face->hb_font[variant];
    if (!font || !hb || (CTFontGetSymbolicTraits(font) & kCTFontTraitColorGlyphs))
        return FONT_RUN_UNAVAILABLE;

    for (int i = 0; i < w->len; i++)
        if (wsap_vtcore_grapheme_is_handle(w->cps[i]))
            return ct_shape_fallback(face, w, m, cells);

    if (!ct_buffer && !(ct_buffer = hb_buffer_create()))
        return FONT_RUN_UNAVAILABLE;
    hb_buffer_t *buf = ct_buffer;
    hb_buffer_clear_contents(buf);
    hb_buffer_add_codepoints(buf, w->cps, w->len, 0, w->len);
    if (w->script) {
        hb_buffer_set_script(buf, (hb_script_t)w->script);
        hb_buffer_set_direction(buf, hb_script_get_horizontal_direction((hb_script_t)w->script));
    }
    hb_buffer_guess_segment_properties(buf);
    font_shape_text(hb, buf, w->cps, w->len);

    unsigned count = 0;
    const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &count);
    const hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &count);
    if (!count || count > FONT_RUN_MAX)
        return FONT_RUN_UNAVAILABLE;

    bool plain = true;
    int seen = 0;
    for (unsigned k = 0; k < count; k++) {
        if (!info[k].codepoint)
            return ct_shape_fallback(face, w, m, cells);
        int c = (int)info[k].cluster - w->core;
        if (c < 0 || c >= w->core_len)
            continue;
        hb_codepoint_t nominal = 0;
        hb_font_get_nominal_glyph(hb, w->cps[info[k].cluster], &nominal);
        if (info[k].codepoint != nominal || pos[k].x_offset || pos[k].y_offset)
            plain = false;
        seen++;
    }
    if (plain && seen == w->core_len)
        return FONT_RUN_UNCHANGED;

    struct ct_ink ink[FONT_RUN_MAX];
    int drawn = 0;
    ct_use_pass++;
    int pen_x = 0;
    uint32_t last_cluster = UINT32_MAX;
    for (unsigned k = 0; k < count; k++) {
        uint32_t cl = info[k].cluster;
        if (cl != last_cluster) {
            pen_x = 0;
            last_cluster = cl;
        }
        int c = (int)cl - w->core;
        if (c < 0 || c >= w->core_len)
            continue;
        const struct ct_raster *r = ct_raster_gid(face, variant, font, (CGGlyph)info[k].codepoint);
        if (!r)
            return FONT_RUN_UNAVAILABLE;
        if (!r->w || !r->h) {
            pen_x += (int)(pos[k].x_advance >> 6);
            continue;
        }
        ink[drawn++] = (struct ct_ink){
            .ras = r,
            .box = {
                c * m->cell_w + pen_x + (pos[k].x_offset >> 6) + r->left,
                -(pos[k].y_offset >> 6) - r->above,
                r->w, r->h,
            },
        };
        pen_x += (int)(pos[k].x_advance >> 6);
    }
    return ct_fit_cells(ink, drawn, w->core_len, cells) ? FONT_RUN_SHAPED
                                                        : FONT_RUN_UNAVAILABLE;
}

static hb_face_t *ct_layout_face(font_handle_t *face, uint16_t style)
{
    return face ? hb_face_reference(face->hb_face[font_variant_of(style)]) : NULL;
}

static void ct_font_registered_changed(CFNotificationCenterRef center, void *observer,
                                    CFStringRef name, const void *object,
                                    CFDictionaryRef userInfo)
{
    (void)center; (void)observer; (void)name; (void)object; (void)userInfo;
    font_bump_generation();
}

static bool ct_init(void)
{
    register_bundled_fonts();
    CFNotificationCenterAddObserver(
        CFNotificationCenterGetLocalCenter(),
        NULL,
        ct_font_registered_changed,
        kCTFontManagerRegisteredFontsChangedNotification,
        NULL,
        CFNotificationSuspensionBehaviorDeliverImmediately);
    return true;
}
static void ct_destroy(void)
{
    CFNotificationCenterRemoveObserver(
        CFNotificationCenterGetLocalCenter(),
        NULL,
        kCTFontManagerRegisteredFontsChangedNotification,
        NULL);
    ct_worker_destroy();
}

const struct font_backend font_backend_coretext = {
    .name = "coretext",
    .init = ct_init,
    .destroy = ct_destroy,
    .thread_detach = ct_worker_destroy,
    .open = ct_face_open,
    .close = ct_face_close,
    .measure = ct_face_metrics,
    .rasterize_glyph = ct_rasterize_glyph,
    .scan_run = ct_scan_run,
    .shape_run = ct_shape_run,
    .layout_face = ct_layout_face,
};
