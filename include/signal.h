#ifndef SIGNAL_H
#define SIGNAL_H

typedef volatile int sig_atomic_t;

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIGINT 2
#define SIGTERM 15
#define SIGABRT 6
#define SIGFPE 8
#define SIGSEGV 11
#define SIGILL 4

static inline sighandler_t signal(int sig, sighandler_t handler) {
    (void)sig;
    (void)handler;
    return SIG_DFL;
}

static inline int raise(int sig) {
    (void)sig;
    return 0;
}

#endif