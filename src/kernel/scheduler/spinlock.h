#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct spinlock {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock_init(spinlock_t *lock) {
    lock->locked = 0;
}

static inline void spin_lock(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        while (lock->locked)
            asm volatile("pause");
    }
}

static inline int spin_trylock(spinlock_t *lock) {
    return !__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE);
}

static inline void spin_unlock(spinlock_t *lock) {
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags));
    asm volatile("cli");
    spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    if (flags & (1 << 9))
        asm volatile("sti");
}

#endif
