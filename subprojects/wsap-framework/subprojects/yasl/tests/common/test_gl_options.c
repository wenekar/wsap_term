#include "common/gl_options.h"
#include <assert.h>
#include <stddef.h>
#include <errno.h>
int main(void)
{
    struct yasl_private_gl_options o;
    assert(yasl_private_gl_parse_options(NULL,&o));assert(o.major==3&&o.minor==2&&o.red_bits==8);
    const int es[]={YASL_PRIVATE_GL_API,YASL_PRIVATE_GL_OPENGL_ES,YASL_PRIVATE_GL_MAJOR,3,YASL_PRIVATE_GL_MINOR,0,YASL_PRIVATE_GL_END};
    assert(yasl_private_gl_parse_options(es,&o)&&o.api==YASL_PRIVATE_GL_OPENGL_ES&&o.minor==0);
    const int invalid[]={99,0,YASL_PRIVATE_GL_END};
    assert(!yasl_private_gl_parse_options(invalid,&o)&&errno==EINVAL);
    const int negative[]={YASL_PRIVATE_GL_DEPTH_BITS,-1,YASL_PRIVATE_GL_END};
    assert(!yasl_private_gl_parse_options(negative,&o));
    return 0;
}
