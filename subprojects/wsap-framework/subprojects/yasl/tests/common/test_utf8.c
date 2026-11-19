#include "common/utf8.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char latin1[256];
    for (int i = 1; i <= 255; i++) latin1[i - 1] = (char)i;
    latin1[255] = '\0';
    size_t len = 0, back_len = 0;
    char *utf8 = yasl_private_utf8_from_latin1(latin1, 255, &len);
    assert(utf8 && yasl_private_utf8_validate(utf8, len));
    char *back = yasl_private_utf8_to_latin1(utf8, &back_len);
    assert(back && back_len == 255 && memcmp(back, latin1, 256) == 0);
    free(back);
    free(utf8);
    assert(!yasl_private_utf8_to_latin1("😀", &len));
    assert(!yasl_private_utf8_to_latin1("\xc3", &len));
    assert(!yasl_private_utf8_validate("\xc3", 1));
    assert(!yasl_private_utf8_validate("\xe2\x82", 2));
    assert(!yasl_private_utf8_validate("\xf0\x9f\x98", 3));
    assert(!yasl_private_utf8_validate("\xed\xa0\x80", 3)); /* Surrogate. */
    assert(yasl_private_utf8_validate("😀", sizeof("😀") - 1));
    assert(yasl_private_utf8_validate(NULL, 0));
    return 0;
}
