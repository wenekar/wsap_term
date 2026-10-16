#ifndef UTIL_JSON_H
#define UTIL_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/geom.h"

#include <yyjson.h>

struct json_writer {
    yyjson_mut_doc *doc;
    yyjson_mut_val **stack;
    yyjson_mut_val *key;
    int depth;
    int depth_cap;
    bool failed;
};

void json_obj_begin(struct json_writer *j);
void json_obj_end(struct json_writer *j);
void json_arr_begin(struct json_writer *j);
void json_arr_end(struct json_writer *j);
void json_key(struct json_writer *j, const char *key);

void json_str(struct json_writer *j, const char *s);
void json_strn(struct json_writer *j, const char *s, size_t len);
void json_int(struct json_writer *j, int64_t v);
void json_uint(struct json_writer *j, uint64_t v);
void json_hex(struct json_writer *j, uint64_t v);
void json_double(struct json_writer *j, double v);
void json_bool(struct json_writer *j, bool v);
void json_null(struct json_writer *j);
void json_append(struct json_writer *j, struct json_writer *source);

void json_rect(struct json_writer *j, struct rect2i r);
void json_vec(struct json_writer *j, struct vector2i v);
void json_insets(struct json_writer *j, struct insets2i i);

char *json_take(struct json_writer *j, size_t *len);
void json_reset(struct json_writer *j);

#endif
