/* Measurement shim: refuse the CLI's SIGINT handler, so `mradm render` starts no Ctrl-C watcher
   thread (and does not wait for it to join); every other signal() call passes through. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>

typedef void (*handler_fn)(int);

handler_fn signal(int signum, handler_fn handler) {
    static handler_fn (*next)(int, handler_fn);
    if (signum == SIGINT) {
        errno = EINVAL;
        return SIG_ERR;
    }
    if (!next) {
        next = (handler_fn(*)(int, handler_fn))dlsym(RTLD_NEXT, "signal");
    }
    return next(signum, handler);
}
