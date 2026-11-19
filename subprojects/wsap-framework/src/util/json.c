#include "util/json.h"
#include "util/grow.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ready(struct json_writer *j)
{
    if (j->failed)
        return false;
    if (!j->doc)
        j->doc = yyjson_mut_doc_new(NULL);
    if (!j->doc)
        j->failed = true;
    return !j->failed;
}

static bool add(struct json_writer *j, yyjson_mut_val *value)
{
    if (j->failed)
        return false;
    bool ok = value != NULL;
    if (ok && !j->depth) {
        ok = yyjson_mut_doc_get_root(j->doc) == NULL;
        if (ok)
            yyjson_mut_doc_set_root(j->doc, value);
    } else if (ok) {
        yyjson_mut_val *parent = j->stack[j->depth - 1];
        if (yyjson_mut_is_arr(parent))
            ok = yyjson_mut_arr_append(parent, value);
        else
            ok = j->key && yyjson_mut_obj_add(parent, j->key, value);
    }
    j->key = NULL;
    j->failed = !ok;
    return ok;
}

static void open_(struct json_writer *j, bool object)
{
    if (!ready(j))
        return;
    if (j->depth == INT_MAX ||
        !UTIL_GROW(j->stack, j->depth_cap, j->depth + 1)) {
        j->failed = true;
        return;
    }
    yyjson_mut_val *value = object ? yyjson_mut_obj(j->doc) : yyjson_mut_arr(j->doc);
    if (add(j, value))
        j->stack[j->depth++] = value;
}

static void close_(struct json_writer *j, bool object)
{
    if (j->failed)
        return;
    if (!j->depth || j->key ||
        yyjson_mut_is_obj(j->stack[j->depth - 1]) != object) {
        j->failed = true;
        return;
    }
    j->depth--;
}

void json_obj_begin(struct json_writer *j) { open_(j, true); }
void json_obj_end(struct json_writer *j) { close_(j, true); }
void json_arr_begin(struct json_writer *j) { open_(j, false); }
void json_arr_end(struct json_writer *j) { close_(j, false); }

void json_key(struct json_writer *j, const char *key)
{
    if (!ready(j))
        return;
    if (!j->depth || j->key || !yyjson_mut_is_obj(j->stack[j->depth - 1])) {
        j->failed = true;
        return;
    }
    j->key = yyjson_mut_strcpy(j->doc, key ? key : "");
    if (!j->key)
        j->failed = true;
}

void json_strn(struct json_writer *j, const char *s, size_t len)
{
    if (ready(j))
        add(j, s ? yyjson_mut_strncpy(j->doc, s, len) : yyjson_mut_null(j->doc));
}

void json_str(struct json_writer *j, const char *s)
{
    json_strn(j, s, s ? strlen(s) : 0);
}

void json_int(struct json_writer *j, int64_t v)
{
    if (ready(j)) add(j, yyjson_mut_sint(j->doc, v));
}

void json_uint(struct json_writer *j, uint64_t v)
{
    if (ready(j)) add(j, yyjson_mut_uint(j->doc, v));
}

void json_hex(struct json_writer *j, uint64_t v)
{
    char text[19];
    snprintf(text, sizeof(text), "0x%" PRIx64, v);
    json_str(j, text);
}

void json_double(struct json_writer *j, double v)
{
    if (ready(j))
        add(j, isfinite(v) ? yyjson_mut_real(j->doc, v) : yyjson_mut_null(j->doc));
}

void json_bool(struct json_writer *j, bool v)
{
    if (ready(j)) add(j, yyjson_mut_bool(j->doc, v));
}

void json_null(struct json_writer *j)
{
    if (ready(j)) add(j, yyjson_mut_null(j->doc));
}

void json_append(struct json_writer *j, struct json_writer *source)
{
    if (j == source) {
        j->failed = true;
        return;
    }
    if (ready(j)) {
        yyjson_mut_val *root = source->doc ? yyjson_mut_doc_get_root(source->doc) : NULL;
        if (source->failed || source->depth || source->key)
            j->failed = true;
        else
            add(j, root ? yyjson_mut_val_mut_copy(j->doc, root) : yyjson_mut_null(j->doc));
    }
    json_reset(source);
}

void json_rect(struct json_writer *j, struct rect2i r)
{
    json_arr_begin(j);
    json_int(j, r.x);
    json_int(j, r.y);
    json_int(j, r.width);
    json_int(j, r.height);
    json_arr_end(j);
}

void json_vec(struct json_writer *j, struct vector2i v)
{
    json_arr_begin(j);
    json_int(j, v.x);
    json_int(j, v.y);
    json_arr_end(j);
}

void json_insets(struct json_writer *j, struct insets2i i)
{
    json_obj_begin(j);
    json_key(j, "t"); json_int(j, i.t);
    json_key(j, "r"); json_int(j, i.r);
    json_key(j, "b"); json_int(j, i.b);
    json_key(j, "l"); json_int(j, i.l);
    json_obj_end(j);
}

char *json_take(struct json_writer *j, size_t *len)
{
    if (len)
        *len = 0;
    char *out = NULL;
    if (!j->failed && j->doc && !j->depth && !j->key)
        out = yyjson_mut_write(j->doc, 0, len);
    json_reset(j);
    return out;
}

void json_reset(struct json_writer *j)
{
    yyjson_mut_doc_free(j->doc);
    free(j->stack);
    *j = (struct json_writer){0};
}
