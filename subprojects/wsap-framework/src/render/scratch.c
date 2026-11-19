#include "render/scratch.h"

#include <stdlib.h>

static _Thread_local uint32_t *scratch_words;
static _Thread_local int scratch_cap;

void render_thread_scratch_release(void)
{
    free(scratch_words);
    scratch_words = NULL;
    scratch_cap = 0;
}

uint32_t *render_thread_scratch(int words)
{
    if (words <= scratch_cap)
        return scratch_words;
    int cap = scratch_cap ? scratch_cap : 256;
    while (cap < words)
        cap *= 2;
    uint32_t *p = realloc(scratch_words, (size_t)cap * sizeof(*p));
    if (!p)
        return NULL;
    scratch_words = p;
    scratch_cap = cap;
    return p;
}

bool render_thread_scratch_reserve(int words)
{
    return words <= scratch_cap || render_thread_scratch(words) != NULL;
}
