#ifndef SETJMP_H
#define SETJMP_H

#include <stdint.h>

typedef uint64_t jmp_buf[8];
typedef uint64_t sigjmp_buf[8];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#define sigsetjmp(env, save) setjmp(env)
#define siglongjmp(env, val) longjmp(env, val)

#endif