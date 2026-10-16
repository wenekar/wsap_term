#include "terminal/drop.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char *path = terminal_quote_path("/tmp/a b's.txt");
    assert(path && strcmp(path, "'/tmp/a b'\\''s.txt'") == 0);
    free(path);
    assert(!terminal_quote_path("/tmp/line\nbreak"));

    const char *uris = "# files\r\nfile:///tmp/a%20b.txt\r\n"
                       "file://localhost/tmp/b%27s.txt\n"
                       "https://example.com/nope\nfile://remote/tmp/nope\n";
    path = terminal_quote_uri_list(uris, strlen(uris));
    assert(path && strcmp(path, "'/tmp/a b.txt' '/tmp/b'\\''s.txt'") == 0);
    free(path);
    return 0;
}
