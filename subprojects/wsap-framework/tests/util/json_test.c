#include <assert.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "util/json.h"

static void values_test(void)
{
    struct json_writer j = {0};
    json_arr_begin(&j);
    json_uint(&j, UINT64_MAX);
    json_int(&j, INT64_MIN);
    json_double(&j, 1.2345678901234567);
    json_double(&j, INFINITY);
    json_double(&j, NAN);
    char text[] = {'a', 0, 'b', '\n'};
    json_strn(&j, text, sizeof(text));
    text[0] = 'z';
    json_str(&j, "İstanbul 😊");
    json_arr_end(&j);
    size_t len;
    char *out = json_take(&j, &len);
    assert(out && len == strlen(out));
    yyjson_doc *doc = yyjson_read(out, len, 0);
    assert(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_arr_size(root) == 7);
    assert(yyjson_get_uint(yyjson_arr_get(root, 0)) == UINT64_MAX);
    assert(yyjson_get_sint(yyjson_arr_get(root, 1)) == INT64_MIN);
    assert(yyjson_get_real(yyjson_arr_get(root, 2)) == 1.2345678901234567);
    assert(yyjson_is_null(yyjson_arr_get(root, 3)));
    assert(yyjson_is_null(yyjson_arr_get(root, 4)));
    assert(yyjson_get_len(yyjson_arr_get(root, 5)) == sizeof(text));
    assert(!memcmp(yyjson_get_str(yyjson_arr_get(root, 5)), "a\0b\n", sizeof(text)));
    assert(!strcmp(yyjson_get_str(yyjson_arr_get(root, 6)), "İstanbul 😊"));
    yyjson_doc_free(doc);
    free(out);
}

static void append_test(void)
{
    struct json_writer j = {0}, source = {0};
    json_obj_begin(&source);
    json_key(&source, "value"); json_int(&source, 42);
    json_obj_end(&source);
    json_arr_begin(&j);
    json_append(&j, &source);
    json_int(&source, 7);
    json_append(&j, &source);
    json_arr_end(&j);
    char *out = json_take(&j, NULL);
    assert(out && !strcmp(out, "[{\"value\":42},7]"));
    free(out);
}

static void errors_test(void)
{
    struct json_writer j = {0};
    size_t len = 123;
    json_str(&j, "\xff");
    assert(!json_take(&j, &len) && len == 0);
    json_obj_begin(&j);
    json_arr_end(&j);
    assert(!json_take(&j, NULL));
    json_obj_begin(&j);
    json_key(&j, "missing");
    json_obj_end(&j);
    assert(!json_take(&j, NULL));
    json_int(&j, 1);
    json_int(&j, 2);
    assert(!json_take(&j, NULL));
    json_int(&j, 3);
    char *out = json_take(&j, NULL);
    assert(out && !strcmp(out, "3"));
    free(out);
    struct json_writer source = {0};
    json_arr_begin(&source);
    json_append(&j, &source);
    assert(!json_take(&j, NULL));
    json_reset(&source);
}

int main(void)
{
    values_test();
    append_test();
    errors_test();
    if (setlocale(LC_NUMERIC, "tr_TR.UTF-8") ||
        setlocale(LC_NUMERIC, "de_DE.UTF-8"))
        values_test();
    return 0;
}
