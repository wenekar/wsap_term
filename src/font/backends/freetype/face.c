#define _GNU_SOURCE
#include "font/backends/freetype/internal.h"

#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>
#include <hb-ft.h>

#include <fcntl.h>
#include <limits.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define INITIAL_FACE_CAP 16

__thread FT_Library ft_lib = NULL;

_Thread_local struct ft_face_slot *faces = NULL;
static _Thread_local int faces_cap = 0;
__thread int num_faces = 0;
int configured_faces;
__thread uint64_t face_stamp;
__thread int ft_applied_px = FONT_DEFAULT_PIXEL_SIZE;

static pthread_key_t faces_tls_key;
static pthread_once_t faces_tls_once = PTHREAD_ONCE_INIT;
static bool faces_tls_ready;

static void faces_tls_key_init(void)
{
    faces_tls_ready = pthread_key_create(&faces_tls_key, free) == 0;
}

/* Grows the per-thread face table on demand up to MAX_FACES. */
static bool faces_ensure(int need)
{
    if (need <= faces_cap)
        return true;
    int cap = faces_cap ? faces_cap : INITIAL_FACE_CAP;
    while (cap < need)
        cap *= 2;
    if (cap > MAX_FACES)
        cap = MAX_FACES;
    if (need > cap)
        return false;
    struct ft_face_slot *next = realloc(faces, (size_t)cap * sizeof(*next));
    if (!next)
        return false;
    memset(next + faces_cap, 0, (size_t)(cap - faces_cap) * sizeof(*next));
    faces = next;
    faces_cap = cap;
    pthread_once(&faces_tls_once, faces_tls_key_init);
    if (faces_tls_ready)
        pthread_setspecific(faces_tls_key, faces);
    return true;
}

void faces_release_table(void)
{
    free(faces);
    faces = NULL;
    faces_cap = 0;
    num_faces = 0;
    if (faces_tls_ready)
        pthread_setspecific(faces_tls_key, NULL);
}

const char embedded_cards_path[] = "embedded:playing-cards";

const enum font_variant font_style_order[3] = {
    FONT_VARIANT_BOLD_ITALIC, FONT_VARIANT_BOLD, FONT_VARIANT_ITALIC
};

enum cfg_style_state { CFG_STYLE_UNPROBED = 0, CFG_STYLE_FOUND = 1, CFG_STYLE_NONE = 2 };

struct cfg_style_spec {
    char *path;
    int index;
    uint8_t state;
};

static struct cfg_face_spec {
    char *path;
    int index;
    bool is_emoji;
    bool deferred;
    struct cfg_style_spec styles[FONT_VARIANT_COUNT];
} cfg_specs[MAX_FACES];
_Atomic unsigned ft_sync_epoch;

static pthread_mutex_t fc_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fc_query_mu = PTHREAD_MUTEX_INITIALIZER;
static int fc_pins;

static pthread_mutex_t cfg_style_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t style_probe_mu = PTHREAD_MUTEX_INITIALIZER;

struct ft_font_blob {
    struct ft_font_blob *next;
    char *path;
    const FT_Byte *data;
    size_t size;
    dev_t dev;
    ino_t ino;
    struct timespec mtime;
    unsigned refcount;
    bool active;
};

static pthread_mutex_t blob_mu = PTHREAD_MUTEX_INITIALIZER;
static struct ft_font_blob *blob_head;

static struct timespec stat_mtime(const struct stat *st)
{
#ifdef __APPLE__
    return st->st_mtimespec;
#else
    return st->st_mtim;
#endif
}

static bool ft_blob_matches(const struct ft_font_blob *b, const char *path,
                            const struct stat *st)
{
    struct timespec mt = stat_mtime(st);
    return b->size == (size_t)st->st_size && b->dev == st->st_dev &&
           b->ino == st->st_ino && b->mtime.tv_sec == mt.tv_sec &&
           b->mtime.tv_nsec == mt.tv_nsec && strcmp(b->path, path) == 0;
}

static bool valid_font_stat(const struct stat *st)
{
    return S_ISREG(st->st_mode) && st->st_size > 0 &&
           (uintmax_t)st->st_size <= (uintmax_t)LONG_MAX;
}

/* Unlinks `b` from the active blob cache when `blob_mu` is held. */
static void blob_unlink_locked(struct ft_font_blob *b)
{
    if (!b->active)
        return;
    struct ft_font_blob **pp = &blob_head;
    while (*pp && *pp != b)
        pp = &(*pp)->next;
    if (*pp == b)
        *pp = b->next;
    b->active = false;
}

/* Returns a shared read-only mapped buffer for `path`, creating it on cache miss. */
static struct ft_font_blob *ft_blob_acquire(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0 || !valid_font_stat(&st))
        return NULL;

    pthread_mutex_lock(&blob_mu);
    for (struct ft_font_blob *b = blob_head; b; b = b->next) {
        if (ft_blob_matches(b, path, &st)) {
            b->refcount++;
            pthread_mutex_unlock(&blob_mu);
            return b;
        }
    }
    pthread_mutex_unlock(&blob_mu);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || !valid_font_stat(&st)) {
        close(fd);
        return NULL;
    }
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return NULL;

    struct ft_font_blob *b = calloc(1, sizeof(*b));
    char *copy = b ? strdup(path) : NULL;
    if (!b || !copy) {
        free(b);
        free(copy);
        munmap(map, (size_t)st.st_size);
        return NULL;
    }
    *b = (struct ft_font_blob){
        .path = copy,
        .data = map,
        .size = (size_t)st.st_size,
        .dev = st.st_dev,
        .ino = st.st_ino,
        .mtime = stat_mtime(&st),
        .refcount = 1,
        .active = true,
    };

    pthread_mutex_lock(&blob_mu);
    for (struct ft_font_blob *cur = blob_head; cur; cur = cur->next) {
        if (ft_blob_matches(cur, path, &st)) {
            cur->refcount++;
            pthread_mutex_unlock(&blob_mu);
            munmap(map, b->size);
            free(copy);
            free(b);
            return cur;
        }
    }
    for (struct ft_font_blob *cur = blob_head; cur; cur = cur->next) {
        if (strcmp(cur->path, path) == 0) {
            blob_unlink_locked(cur);
            break;
        }
    }
    b->next = blob_head;
    blob_head = b;
    pthread_mutex_unlock(&blob_mu);
    return b;
}

static void ft_blob_release(struct ft_font_blob *b)
{
    if (!b)
        return;
    pthread_mutex_lock(&blob_mu);
    unsigned left = --b->refcount;
    if (left == 0)
        blob_unlink_locked(b);
    pthread_mutex_unlock(&blob_mu);
    if (left == 0) {
        munmap((void *)b->data, b->size);
        free(b->path);
        free(b);
    }
}

static bool ft_new_face_cached(const char *path, int index, FT_Face *out_face,
                               struct ft_font_blob **out_blob)
{
    *out_face = NULL;
    *out_blob = NULL;
    struct ft_font_blob *blob = ft_blob_acquire(path);
    if (!blob)
        return false;
    FT_Face face = NULL;
    if (FT_New_Memory_Face(ft_lib, blob->data, (FT_Long)blob->size, index, &face) != 0) {
        ft_blob_release(blob);
        return false;
    }
    *out_face = face;
    *out_blob = blob;
    return true;
}

static void fc_trim_heap(void)
{
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

static bool fc_ensure_locked(void)
{
    if (fc_ready)
        return true;
    if (!FcInit())
        return false;
#ifdef WSAP_DEV_FONTS_DIR
    FcConfigAppFontAddDir(NULL, (const FcChar8 *)WSAP_DEV_FONTS_DIR);
#endif
#ifdef WSAP_DATADIR
    FcConfigAppFontAddDir(NULL, (const FcChar8 *)(WSAP_DATADIR "/wsap_term/fonts"));
#endif
    fc_ready = true;
    return true;
}

static void fc_release_locked(void)
{
    if (fc_pins > 0 || fallback_ready())
        return;
    if (fc_ready) {
        FcFini();
        fc_ready = false;
    }
    fc_trim_heap();
}

bool fc_pin(void)
{
    pthread_mutex_lock(&fc_mu);
    bool ok = fc_ensure_locked();
    if (ok)
        fc_pins++;
    pthread_mutex_unlock(&fc_mu);
    return ok;
}

void fc_unpin(void)
{
    pthread_mutex_lock(&fc_mu);
    if (fc_pins > 0)
        fc_pins--;
    fc_release_locked();
    pthread_mutex_unlock(&fc_mu);
}

void fc_fini(void)
{
    pthread_mutex_lock(&fc_mu);
    fc_pins = 0;
    if (fc_ready) {
        FcFini();
        fc_ready = false;
    }
    fc_trim_heap();
    pthread_mutex_unlock(&fc_mu);
}

void fc_query_lock(void)
{
    pthread_mutex_lock(&fc_query_mu);
}

void fc_query_unlock(void)
{
    pthread_mutex_unlock(&fc_query_mu);
}

bool size_face(FT_Face face, int px)
{
    if (FT_Set_Pixel_Sizes(face, 0, px) == 0)
        return true;
    if (face->num_fixed_sizes <= 0)
        return false;
    int best = 0;
    for (int i = 1; i < face->num_fixed_sizes; i++) {
        if (abs(face->available_sizes[i].height - px) <
            abs(face->available_sizes[best].height - px))
            best = i;
    }
    return FT_Select_Size(face, best) == 0;
}

FcPattern *fc_font_match(FcPattern *pat)
{
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    return FcFontMatch(NULL, pat, &result);
}

static FT_Face load_verified_style_face(const char *path, int index, int slant,
                                      int weight, struct ft_font_blob **out_blob)
{
    FT_Face styled = NULL;
    struct ft_font_blob *blob = NULL;
    if (!ft_new_face_cached(path, index, &styled, &blob))
        return NULL;
    if ((slant != FC_SLANT_ROMAN && !(styled->style_flags & FT_STYLE_FLAG_ITALIC)) ||
        (weight != FC_WEIGHT_REGULAR && !(styled->style_flags & FT_STYLE_FLAG_BOLD)) ||
        !size_face(styled, ft_applied_px)) {
        FT_Done_Face(styled);
        ft_blob_release(blob);
        return NULL;
    }
    *out_blob = blob;
    return styled;
}

static FT_Face probe_style_face(const char *family_name, int slant, int weight,
                                struct ft_font_blob **out_blob,
                                char **out_path, int *out_index)
{
    *out_blob = NULL;
    *out_path = NULL;
    *out_index = 0;
    if (!fc_pin())
        return NULL;
    fc_query_lock();
    FcPattern *pat = FcPatternCreate();
    FcPattern *match = NULL;
    if (pat) {
        FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family_name);
        FcPatternAddInteger(pat, FC_SLANT, slant);
        FcPatternAddInteger(pat, FC_WEIGHT, weight);
        match = fc_font_match(pat);
        FcPatternDestroy(pat);
    }
    char *matched_path = NULL;
    int index = 0;
    if (match) {
        FcChar8 *file = NULL;
        int mslant = 0, mweight = 0;
        FcPatternGetInteger(match, FC_SLANT, 0, &mslant);
        FcPatternGetInteger(match, FC_WEIGHT, 0, &mweight);
        bool slant_ok = slant == FC_SLANT_ROMAN
            ? mslant == FC_SLANT_ROMAN
            : (mslant == FC_SLANT_ITALIC || mslant == FC_SLANT_OBLIQUE);
        bool weight_ok = weight == FC_WEIGHT_REGULAR ||
                         mweight >= FC_WEIGHT_DEMIBOLD;
        if (slant_ok && weight_ok &&
            FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
            FcPatternGetInteger(match, FC_INDEX, 0, &index);
            matched_path = strdup((const char *)file);
        }
        FcPatternDestroy(match);
    }
    fc_query_unlock();
    fc_unpin();
    if (!matched_path)
        return NULL;
    FT_Face styled = load_verified_style_face(matched_path, index, slant, weight, out_blob);
    if (styled) {
        *out_path = matched_path;
        *out_index = index;
    } else {
        free(matched_path);
    }
    return styled;
}

static bool slot_matches_cfg_locked(int slot_idx, const struct ft_face_slot *f)
{
    return slot_idx >= 0 && slot_idx < configured_faces &&
           f->path && cfg_specs[slot_idx].path &&
           cfg_specs[slot_idx].index == f->index &&
           strcmp(cfg_specs[slot_idx].path, f->path) == 0;
}

static FT_Face load_style_face(int slot_idx, const struct ft_face_slot *f,
                               enum font_variant variant, FT_Face regular,
                               int slant, int weight,
                               struct ft_font_blob **out_blob)
{
    *out_blob = NULL;
    if (!regular || !regular->family_name)
        return NULL;

    pthread_mutex_lock(&cfg_style_mu);
    bool is_cfg = slot_matches_cfg_locked(slot_idx, f);
    uint8_t state = is_cfg ? cfg_specs[slot_idx].styles[variant].state : CFG_STYLE_UNPROBED;
    char *cached_path = (is_cfg && state == CFG_STYLE_FOUND && cfg_specs[slot_idx].styles[variant].path)
        ? strdup(cfg_specs[slot_idx].styles[variant].path) : NULL;
    int cached_index = is_cfg ? cfg_specs[slot_idx].styles[variant].index : 0;
    pthread_mutex_unlock(&cfg_style_mu);

    if (is_cfg && state == CFG_STYLE_NONE)
        return NULL;
    if (cached_path) {
        FT_Face styled = load_verified_style_face(cached_path, cached_index, slant, weight, out_blob);
        free(cached_path);
        return styled;
    }

    if (!is_cfg) {
        char *unused_path = NULL;
        int unused_index = 0;
        FT_Face styled = probe_style_face(regular->family_name, slant, weight,
                                          out_blob, &unused_path, &unused_index);
        free(unused_path);
        return styled;
    }

    pthread_mutex_lock(&style_probe_mu);
    pthread_mutex_lock(&cfg_style_mu);
    is_cfg = slot_matches_cfg_locked(slot_idx, f);
    state = is_cfg ? cfg_specs[slot_idx].styles[variant].state : CFG_STYLE_UNPROBED;
    cached_path = (is_cfg && state == CFG_STYLE_FOUND && cfg_specs[slot_idx].styles[variant].path)
        ? strdup(cfg_specs[slot_idx].styles[variant].path) : NULL;
    cached_index = is_cfg ? cfg_specs[slot_idx].styles[variant].index : 0;
    pthread_mutex_unlock(&cfg_style_mu);

    FT_Face styled = NULL;
    if (is_cfg && state == CFG_STYLE_NONE) {
        pthread_mutex_unlock(&style_probe_mu);
        return NULL;
    }
    if (cached_path) {
        styled = load_verified_style_face(cached_path, cached_index, slant, weight, out_blob);
        free(cached_path);
        pthread_mutex_unlock(&style_probe_mu);
        return styled;
    }

    char *found_path = NULL;
    int found_index = 0;
    styled = probe_style_face(regular->family_name, slant, weight,
                              out_blob, &found_path, &found_index);
    pthread_mutex_lock(&cfg_style_mu);
    if (slot_matches_cfg_locked(slot_idx, f) &&
        cfg_specs[slot_idx].styles[variant].state == CFG_STYLE_UNPROBED) {
        if (styled && found_path) {
            cfg_specs[slot_idx].styles[variant].path = found_path;
            cfg_specs[slot_idx].styles[variant].index = found_index;
            cfg_specs[slot_idx].styles[variant].state = CFG_STYLE_FOUND;
            found_path = NULL;
        } else {
            cfg_specs[slot_idx].styles[variant].state = CFG_STYLE_NONE;
        }
    }
    pthread_mutex_unlock(&cfg_style_mu);
    pthread_mutex_unlock(&style_probe_mu);
    free(found_path);
    return styled;
}

void ensure_primary_face(int i)
{
    if (i < 0 || i >= num_faces || !faces[i].deferred)
        return;
    struct ft_face_slot *f = &faces[i];
    if (!f->path)
        return;
    FT_Face face = NULL;
    struct ft_font_blob *blob = NULL;
    if (!ft_new_face_cached(f->path, f->index, &face, &blob))
        return;
    if (!size_face(face, ft_applied_px) || !face->size) {
        FT_Done_Face(face);
        ft_blob_release(blob);
        return;
    }
    f->styles[0] = face;
    f->blobs[0] = blob;
    f->deferred = false;
    f->last_used = ++face_stamp;
}

void ensure_styled_face(int i, enum font_variant variant)
{
    if (i < 0 || i >= num_faces || !variant || (unsigned)variant >= FONT_VARIANT_COUNT)
        return;
    ensure_primary_face(i);
    struct ft_face_slot *f = &faces[i];
    uint8_t bit = (uint8_t)(1u << variant);
    if ((f->styles_checked & bit) || f->is_emoji)
        return;
    f->styles_checked |= bit;
    f->styles[variant] = load_style_face(i, f, variant, f->styles[FONT_VARIANT_PLAIN],
        variant & FONT_VARIANT_ITALIC ? FC_SLANT_ITALIC : FC_SLANT_ROMAN,
        variant & FONT_VARIANT_BOLD ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR,
        &f->blobs[variant]);
}

static bool style_covers(int i, const uint32_t *cps, int len,
                         enum font_variant variant)
{
    if (i < 0 || i >= num_faces || (unsigned)variant >= FONT_VARIANT_COUNT)
        return false;
    ensure_styled_face(i, variant);
    FT_Face ft = faces[i].styles[variant];
    if (!ft)
        return false;
    for (int k = 0; k < len; k++)
        if (!FT_Get_Char_Index(ft, cps[k]))
            return false;
    return true;
}

enum font_variant pick_face_style(int i, const uint32_t *cps, int len,
                                  enum font_variant want)
{
    for (int a = 0; a < 3; a++)
        if ((font_style_order[a] & want) == font_style_order[a] &&
            style_covers(i, cps, len, font_style_order[a]))
            return font_style_order[a];
    return FONT_VARIANT_PLAIN;
}

hb_font_t *face_hb_font(int i, enum font_variant variant)
{
    if (i < 0 || i >= num_faces || (unsigned)variant >= FONT_VARIANT_COUNT)
        return NULL;
    ensure_primary_face(i);
    if (variant && !faces[i].styles[variant]) {
        ensure_styled_face(i, variant);
        if (!faces[i].styles[variant])
            variant = FONT_VARIANT_PLAIN;
    }
    if (!faces[i].styles[variant])
        return NULL;
    if (!faces[i].hb[variant]) {
        faces[i].hb[variant] =
            hb_ft_font_create_referenced(faces[i].styles[variant]);
        if (faces[i].hb[variant])
            hb_ft_font_set_load_flags(faces[i].hb[variant],
                                      font_ft_load_flags());
    }
    return faces[i].hb[variant];
}

static void pattern_face_attrs(FcPattern *p, int *index, bool *is_emoji)
{
    FcPatternGetInteger(p, FC_INDEX, 0, index);
    if (pattern_is_emoji(p))
        *is_emoji = true;
}

static char *resolve_font_path(const char *name_or_pattern, int *index, bool *is_emoji)
{
    *index = 0;
    *is_emoji = false;
    if (!name_or_pattern || !name_or_pattern[0])
        return NULL;
    if (access(name_or_pattern, R_OK) == 0) {
        if (fc_pin()) {
            fc_query_lock();
            int count = 0;
            FcPattern *query = FcFreeTypeQuery((const FcChar8 *)name_or_pattern,
                                               0, NULL, &count);
            if (query) {
                pattern_face_attrs(query, index, is_emoji);
                FcPatternDestroy(query);
            }
            fc_query_unlock();
            fc_unpin();
        }
        return strdup(name_or_pattern);
    }

    if (!fc_pin())
        return NULL;
    fc_query_lock();
    FcPattern *pat = FcNameParse((const FcChar8 *)name_or_pattern);
    char *font_path = NULL;
    if (pat) {
        FcPattern *font = fc_font_match(pat);
        FcChar8 *file = NULL;
        if (font && FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch && file) {
            pattern_face_attrs(font, index, is_emoji);
            if (access((const char *)file, R_OK) == 0)
                font_path = strdup((const char *)file);
        }
        if (font)
            FcPatternDestroy(font);
        FcPatternDestroy(pat);
    }
    fc_query_unlock();
    fc_unpin();
    return font_path;
}

static void release_face_hb(int i)
{
    for (int s = 0; s < FONT_VARIANT_COUNT; s++) {
        if (faces[i].hb[s])
            hb_font_destroy(faces[i].hb[s]);
        faces[i].hb[s] = NULL;
    }
}

static void release_face(int i)
{
    unsigned gen = faces[i].slot_reuse + 1;
    release_face_hb(i);
    for (int s = 0; s < FONT_VARIANT_COUNT; s++) {
        if (faces[i].styles[s])
            FT_Done_Face(faces[i].styles[s]);
        ft_blob_release(faces[i].blobs[s]);
    }
    free(faces[i].path);
    faces[i] = (struct ft_face_slot){0};
    faces[i].slot_reuse = gen;
}

static int insert_face(FT_Face face, struct ft_font_blob *blob, const char *path,
                       int index, bool is_emoji)
{
    if (!size_face(face, ft_applied_px) || !face->size) {
        FT_Done_Face(face);
        ft_blob_release(blob);
        return -1;
    }

    int slot = num_faces;
    if (num_faces == MAX_FACES) {
        if (configured_faces >= MAX_FACES) {
            FT_Done_Face(face);
            ft_blob_release(blob);
            return -1;
        }
        slot = configured_faces;
        for (int i = configured_faces + 1; i < num_faces; i++)
            if (faces[i].last_used < faces[slot].last_used)
                slot = i;
        release_face(slot);
    } else {
        if (!faces_ensure(num_faces + 1)) {
            FT_Done_Face(face);
            ft_blob_release(blob);
            return -1;
        }
        slot = num_faces++;
    }

    faces[slot] = (struct ft_face_slot){
        .styles = { face },
        .blobs = { blob },
        .is_emoji = is_emoji,
        .path = strdup(path),
        .index = index,
        .last_used = ++face_stamp,
        .slot_reuse = faces[slot].slot_reuse,
    };
    return slot;
}

int ft_load_face(const char *path, int index, bool is_emoji)
{
    if (!path)
        return -1;

    FT_Face face = NULL;
    struct ft_font_blob *blob = NULL;
    if (!ft_new_face_cached(path, index, &face, &blob))
        return -1;

    return insert_face(face, blob, path, index, is_emoji);
}

int load_embedded_face(const unsigned char *data, size_t len, bool is_emoji)
{
    FT_Face face = NULL;
    if (FT_New_Memory_Face(ft_lib, data, (FT_Long)len, 0, &face))
        return -1;

    return insert_face(face, NULL, embedded_cards_path, 0, is_emoji);
}

static int defer_face(const char *path, int index, bool is_emoji)
{
    if (!path)
        return -1;
    if (num_faces == MAX_FACES)
        return ft_load_face(path, index, is_emoji);
    if (!faces_ensure(num_faces + 1))
        return -1;
    int slot = num_faces++;
    faces[slot] = (struct ft_face_slot){
        .is_emoji = is_emoji,
        .path = strdup(path),
        .index = index,
        .last_used = ++face_stamp,
        .slot_reuse = faces[slot].slot_reuse,
        .deferred = true,
    };
    return slot;
}

static void load_named_face_ex(const char *name, bool defer)
{
    int index;
    bool is_emoji;
    char *path = resolve_font_path(name, &index, &is_emoji);
    if (!path) return;
    bool duplicate = false;
    for (int i = 0; i < num_faces; i++)
        if (faces[i].path && faces[i].index == index && !strcmp(path, faces[i].path))
            duplicate = true;
    if (!duplicate) {
        if (defer)
            defer_face(path, index, is_emoji);
        else
            ft_load_face(path, index, is_emoji);
    }
    free(path);
}

void load_named_face(const char *name)
{
    load_named_face_ex(name, false);
}

void defer_named_face(const char *name)
{
    load_named_face_ex(name, true);
}

void release_all_faces(void)
{
    for (int i = 0; i < num_faces; i++)
        release_face(i);
    num_faces = 0;
}

static void cfg_specs_clear_locked(void)
{
    for (int i = 0; i < MAX_FACES; i++) {
        free(cfg_specs[i].path);
        for (int s = 0; s < FONT_VARIANT_COUNT; s++)
            free(cfg_specs[i].styles[s].path);
        cfg_specs[i] = (struct cfg_face_spec){0};
    }
}

void face_cfg_specs_destroy(void)
{
    pthread_mutex_lock(&cfg_style_mu);
    cfg_specs_clear_locked();
    pthread_mutex_unlock(&cfg_style_mu);
}

void face_snapshot_configured(void)
{
    pthread_mutex_lock(&cfg_style_mu);
    cfg_specs_clear_locked();
    configured_faces = num_faces;
    for (int i = 0; i < configured_faces; i++)
        cfg_specs[i] = (struct cfg_face_spec){
            .path = strdup(faces[i].path),
            .index = faces[i].index,
            .is_emoji = faces[i].is_emoji,
            .deferred = faces[i].deferred,
        };
    pthread_mutex_unlock(&cfg_style_mu);
}

void face_reload_configured(void)
{
    for (int i = 0; i < configured_faces; i++) {
        if (cfg_specs[i].deferred)
            defer_face(cfg_specs[i].path, cfg_specs[i].index,
                       cfg_specs[i].is_emoji);
        else
            ft_load_face(cfg_specs[i].path, cfg_specs[i].index,
                         cfg_specs[i].is_emoji);
    }
}

struct font_metrics ft_measure_cell(FT_Face face)
{
    if (!face || !face->size)
        return font_thread_metrics;
    FT_Size_Metrics m = face->size->metrics;
    int advance = (int)(m.max_advance >> 6);
    int load_flags = font_ft_load_flags();
    if (!FT_Load_Char(face, 'M', load_flags))
        advance = ft_px_round(face->glyph->advance.x);

    int line = ft_px_round(FT_MulFix(face->height, m.y_scale));
    int ascent = ft_px_round(FT_MulFix(face->ascender, m.y_scale));
    int descent = ft_px_round(FT_MulFix(face->descender, m.y_scale));
    if (descent > 0)
        descent = -descent;

    struct font_metrics t = { .pixel_size = ft_applied_px };
    t.cell_w = advance < 1 ? 1 : advance;
    t.cell_h = line < 1 ? 1 : line;

    t.baseline = ascent + (t.cell_h - (ascent - descent)) / 2;
    if (t.baseline < 0) t.baseline = 0;
    if (t.baseline > t.cell_h) t.baseline = t.cell_h;

    if (!FT_Load_Char(face, '(', load_flags | FT_LOAD_RENDER) && face->glyph->bitmap.rows)
        t.ink_rows = (int)face->glyph->bitmap.rows;
    else
        t.ink_rows = t.cell_h;
    return t;
}

void ft_size_faces(int px)
{
    if (px == ft_applied_px)
        return;
    ft_applied_px = px;
    for (int i = 0; i < num_faces; i++) {
        for (int s = 0; s < FONT_VARIANT_COUNT; s++)
            if (faces[i].styles[s])
                size_face(faces[i].styles[s], px);
        release_face_hb(i);
    }
}

void face_activate(const struct font_metrics *m)
{
    if (m) {
        ft_size_faces(m->pixel_size);
        font_thread_metrics_set(m);
    }
}
