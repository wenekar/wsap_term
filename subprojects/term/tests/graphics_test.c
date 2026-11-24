#include <wsap_vtcore/wsap_vtcore.h>
#include <assert.h>
#include <string.h>

struct receiver {
    struct wsap_vtcore_graphics last;
    unsigned calls;
    uint8_t pixels[128];
    char replies[256];
    size_t replied;
};

static enum wsap_vtcore_graphics_result receive(void *ud,
    const struct wsap_vtcore_graphics *cmd, struct wsap_vtcore_vector2i *extent)
{
    struct receiver *r = ud;
    if (cmd->action == WSAP_VTCORE_GRAPHICS_ERASE)
        return WSAP_VTCORE_GRAPHICS_OK;
    r->last = *cmd;
    r->last.data = NULL;
    r->calls++;
    if (cmd->len) {
        assert(cmd->len <= sizeof r->pixels);
        memcpy(r->pixels, cmd->data, cmd->len);
    }
    if (cmd->action == WSAP_VTCORE_GRAPHICS_PLACE && cmd->image_id == 99)
        return WSAP_VTCORE_GRAPHICS_NOT_FOUND;
    if (cmd->action == WSAP_VTCORE_GRAPHICS_PLACE ||
        cmd->action == WSAP_VTCORE_GRAPHICS_TRANSMIT_AND_PLACE)
        *extent = (struct wsap_vtcore_vector2i){2, 1};
    return WSAP_VTCORE_GRAPHICS_OK;
}

static void reply(void *ud, const char *bytes, size_t len)
{
    struct receiver *r = ud;
    assert(len < sizeof(r->replies) - r->replied);
    memcpy(r->replies + r->replied, bytes, len);
    r->replied += len;
    r->replies[r->replied] = '\0';
}

static void expect_reply(struct receiver *r, const char *expected)
{
    assert(r->replied == strlen(expected));
    assert(memcmp(r->replies, expected, r->replied) == 0);
    r->replied = 0;
}

static struct wsap_vtcore *graphics_term(struct receiver *r)
{
    struct wsap_vtcore *t = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = {10, 6}, .callbacks = {.ud = r, .reply = reply},
    });
    assert(t);
    return t;
}

static void feed(struct wsap_vtcore *t, const char *str)
{
    assert(wsap_vtcore_feed(t, str, strlen(str), 0));
}

static void test_kitty(void)
{
    struct receiver r = {0};
    struct wsap_vtcore *t = graphics_term(&r);
    const char query[] = "\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\\033[c";
    feed(t, query);
    expect_reply(&r, "\033[?62;6;21;22;28c");
    wsap_vtcore_set_graphics(t, receive, &r);
    for (size_t i = 0; i < sizeof query - 1; i++)
        assert(wsap_vtcore_feed(t, query + i, 1, 0));
    assert(r.calls == 1 && r.last.action == WSAP_VTCORE_GRAPHICS_QUERY);
    expect_reply(&r, "\033_Gi=31;OK\033\\\033[?62;4;6;21;22;28c");

    feed(t, "\033_Ga=T,f=24,s=2,v=1,i=4294967295,p=7,z=-3,C=1,m=1\033\\"
            "\033_Gm=1;/wAA\033\\");
    assert(r.calls == 1);
    expect_reply(&r, "");
    feed(t, "X\033_G;AP8A\033\\");
    assert(r.calls == 2 && r.last.action == WSAP_VTCORE_GRAPHICS_TRANSMIT_AND_PLACE);
    assert(r.last.format == WSAP_VTCORE_GRAPHICS_RGB && r.last.len == 6);
    assert(r.last.size.x == 2 && r.last.size.y == 1);
    assert(r.last.cursor.x == 1 && r.last.cursor.y == 0 && r.last.z_index == -3);
    assert(memcmp(r.pixels, (uint8_t[]){255, 0, 0, 0, 255, 0}, 6) == 0);
    assert(wsap_vtcore_cursor(t).pos.x == 1);
    expect_reply(&r, "\033_Gi=4294967295,p=7;OK\033\\");

    feed(t, "\033_Ga=p,i=4294967295,c=2,r=1,q=1\033\\");
    assert(r.calls == 3 && r.last.action == WSAP_VTCORE_GRAPHICS_PLACE);
    assert(r.last.placement_size.x == 2 && r.last.placement_size.y == 1);
    assert(wsap_vtcore_cursor(t).pos.x == 3 && wsap_vtcore_cursor(t).pos.y == 1);
    expect_reply(&r, "");
    feed(t, "\033_Ga=p,i=99,q=1\033\\");
    expect_reply(&r, "\033_Gi=99;ENOENT:Image not found\033\\");
    feed(t, "\033_Ga=d,d=I,i=4294967295,p=7,q=2\033\\");
    assert(r.last.action == WSAP_VTCORE_GRAPHICS_DELETE && r.last.delete_mode == 'I');
    expect_reply(&r, "");

    feed(t, "\033[?1049h\033_Gf=100;"
         "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC\033\\");
    assert(r.last.action == WSAP_VTCORE_GRAPHICS_TRANSMIT && r.last.alternate);
    assert(r.last.format == WSAP_VTCORE_GRAPHICS_PNG && r.last.len == 69);
    assert(r.last.size.x == 1 && r.last.size.y == 1);
    assert(memcmp(r.pixels, "\x89" "PNG", 4) == 0);
    wsap_vtcore_free(t);
}

static void test_recovery(void)
{
    struct receiver r = {0};
    struct wsap_vtcore *t = graphics_term(&r);
    wsap_vtcore_set_graphics(t, receive, &r);
    feed(t, "\033_Gs=2,v=1,f=24,i=4,m=1;AAAA\033\\\033_Gm=0;!!!\033\\");
    expect_reply(&r, "\033_Gi=4;EINVAL:Invalid graphics command\033\\");
    feed(t, "\033_Gt=f,i=9;AAAA\033\\");
    expect_reply(&r, "\033_Gi=9;ENOTSUP:Unsupported graphics command\033\\");
    feed(t, "\033_Ga=T,f=24,s=2,v=1,m=1;AAAA\033\\\033_Gm=0;AAA\030");
    feed(t, "\033_Gm=0;AAAA\033\\");
    assert(r.calls == 0);

    char long_apc[5000];
    memset(long_apc, 'A', sizeof long_apc);
    feed(t, "\033_Gs=1,v=1,f=24;AAAA");
    assert(wsap_vtcore_feed(t, long_apc, sizeof long_apc, 0));
    feed(t, "\033\\Z");
    assert(r.calls == 0 && wsap_vtcore_cursor(t).pos.x == 1);

    feed(t, "\033_Ga=T,s=2,v=1,f=24,m=1;AAAA\033\\\033c\033_Gm=0;AAAA\033\\");
    assert(r.calls == 1 && r.last.action == WSAP_VTCORE_GRAPHICS_RESET);
    feed(t, "\033_Ga=q,i=31,s=1,v=1,f=24;AAAA");
    wsap_vtcore_set_graphics(t, NULL, NULL);
    wsap_vtcore_set_graphics(t, receive, &r);
    feed(t, "\033\\");
    assert(r.calls == 1);
    expect_reply(&r, "");
    wsap_vtcore_free(t);
}

static void test_sixel(void)
{
    struct receiver r = {0};
    struct wsap_vtcore *t = graphics_term(&r);
    wsap_vtcore_set_graphics(t, receive, &r);
    feed(t, "\033[2;3H\033P0;1q\"2;1;2;7#1;2;100;0;0!2@\r\n");
    feed(t, "-$#2;1;0;50;100@\033");
    assert(r.calls == 0);
    feed(t, "\\");
    assert(r.calls == 1 && r.last.action == WSAP_VTCORE_GRAPHICS_SIXEL);
    assert(r.last.format == WSAP_VTCORE_GRAPHICS_RGBA && r.last.len == 56);
    assert(r.last.size.x == 2 && r.last.size.y == 7);
    assert(r.last.pixel_aspect.x == 1 && r.last.pixel_aspect.y == 2);
    assert(r.last.cursor.x == 2 && r.last.cursor.y == 1);
    const uint8_t expected[56] = {255, 0, 0, 255, 255, 0, 0, 255, [50] = 255, [51] = 255};
    assert(memcmp(r.pixels, expected, sizeof expected) == 0);
    feed(t, "\033Pq!4097~\033\\\033Pq!2\033\\\033Pq~\030\033Pq~\007");
    assert(r.calls == 1);
    feed(t, "\033P;2q?\x9c");
    assert(r.calls == 2 && r.last.len == 4 && r.pixels[3] == 255);
    wsap_vtcore_set_graphics(t, NULL, NULL);
    feed(t, "\033Pq~\r\n~\033\\Z");
    assert(r.calls == 2 && wsap_vtcore_cursor(t).pos.x == 3 && wsap_vtcore_cursor(t).pos.y == 1);
    wsap_vtcore_free(t);
}

int main(void)
{
    test_kitty();
    test_recovery();
    test_sixel();
    return 0;
}
