#include "model.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    struct model *m = model_new((struct wsap_vtcore_vector2i){ .x = 10, .y = 3 }, 10, NULL);
    model_feed(m, "A\r\nB\r\nC\r\nD", strlen("A\r\nB\r\nC\r\nD"), 0);
    assert(model_history(m) == 1);
    assert(model_row(m, -1) == NULL);
    assert(model_row(m, 4) == NULL);

    const struct model_cell *oldest = model_row(m, 0);
    const struct model_cell *next = model_row(m, 1);
    const struct model_cell *last = model_row(m, 3);
    assert(oldest[0].cp[0] == 'A');
    assert(next[0].cp[0] == 'B');
    assert(last[0].cp[0] == 'D');
    assert(oldest != next && next != last);

    model_feed(m, "E", 1, 0);
    assert(model_row(m, 3)[1].cp[0] == 'E');
    assert(model_resize(m, (struct wsap_vtcore_vector2i){ .x = 12, .y = 3 }));
    assert(model_row(m, 3)[0].cp[0] == 'D');
    assert(model_row(m, 3)[1].cp[0] == 'E');
    model_free(m);
    return 0;
}
