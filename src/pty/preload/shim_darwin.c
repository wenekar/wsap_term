#include "pty/preload/shim.h"

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define FP_INTERPOSE(name)                                                 \
    __attribute__((used, section("__DATA,__interpose")))                   \
    static const struct { const void *replacement, *original; }            \
    fp_interpose_##name = { (const void *)fp_##name, (const void *)name };

FP_INTERPOSE(isatty)
FP_INTERPOSE(tcgetattr)
FP_INTERPOSE(tcsetattr)
FP_INTERPOSE(tcdrain)
FP_INTERPOSE(tcflush)
FP_INTERPOSE(tcflow)
FP_INTERPOSE(tcsendbreak)
FP_INTERPOSE(tcgetpgrp)
FP_INTERPOSE(tcsetpgrp)
FP_INTERPOSE(ttyname)
FP_INTERPOSE(ttyname_r)
FP_INTERPOSE(ioctl)
