// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "kernel/proc/futex.h"

#include "kernel/scheduler/scheduler.h"

#include <stddef.h>

#define FUTEX_BUCKETS 64

struct futex_waiter {
    struct futex_waiter *next;
    struct mm *mm;
    uint64_t uaddr;
    uint32_t bitset;
    volatile int woken;
    struct task *task;
};

struct futex_bucket {
    struct kmutex lock;
    struct futex_waiter *head;
};

static struct futex_bucket buckets[FUTEX_BUCKETS];

static struct futex_bucket *bucket_for(struct mm *mm, uint64_t uaddr) {
    uint64_t h = (uaddr >> 2) ^ ((uint64_t)(uintptr_t)mm >> 6);
    h ^= h >> 17;
    h *= 0x9E3779B97F4A7C15ULL;
    return &buckets[(h >> 40) % FUTEX_BUCKETS];
}

static void unlink_waiter(struct futex_bucket *b, struct futex_waiter *w) {
    struct futex_waiter **pp = &b->head;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp == w) *pp = w->next;
    w->next = NULL;
}

int futex_wait(struct process *p, uint64_t uaddr, uint32_t val, uint64_t timeout_ms, uint32_t bitset) {
    if (uaddr & 3) return -LX_EINVAL;
    if (!bitset) return -LX_EINVAL;

    struct futex_bucket *b = bucket_for(&p->mm, uaddr);
    struct futex_waiter w = { NULL, &p->mm, uaddr, bitset, 0, current_task() };

    kmutex_lock(&b->lock);
    uint32_t cur;
    if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) {
        kmutex_unlock(&b->lock);
        return -LX_EFAULT;
    }
    if (cur != val) {
        kmutex_unlock(&b->lock);
        return -LX_EAGAIN;
    }
    w.next = b->head;
    b->head = &w;
    kmutex_unlock(&b->lock);

    scheduler_wait(&w, &w.woken, timeout_ms);

    if (__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) return 0;
    kmutex_lock(&b->lock);
    if (__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) {
        kmutex_unlock(&b->lock);
        return 0;
    }
    unlink_waiter(b, &w);
    kmutex_unlock(&b->lock);
    if (w.task && w.task->kill_pending) return -LX_EINTR;
    return timeout_ms ? -LX_ETIMEDOUT : -LX_EINTR;
}

int futex_wake(struct process *p, uint64_t uaddr, int count, uint32_t bitset) {
    if (count <= 0) return 0;
    struct futex_bucket *b = bucket_for(&p->mm, uaddr);
    int n = 0;

    kmutex_lock(&b->lock);
    struct futex_waiter **pp = &b->head;
    while (*pp && n < count) {
        struct futex_waiter *w = *pp;
        if (w->mm == &p->mm && w->uaddr == uaddr && (w->bitset & bitset)) {
            *pp = w->next;
            w->next = NULL;
            __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
            scheduler_wake(w);
            n++;
            continue;
        }
        pp = &w->next;
    }
    kmutex_unlock(&b->lock);
    return n;
}

int futex_requeue(struct process *p, uint64_t uaddr, int wake, int requeue, uint64_t uaddr2, int cmp,
                  uint32_t cmpval) {
    (void)uaddr2;
    if (cmp) {
        uint32_t cur;
        if (copy_from_user(&cur, uaddr, sizeof(cur)) != 0) return -LX_EFAULT;
        if (cur != cmpval) return -LX_EAGAIN;
    }
    int woken = futex_wake(p, uaddr, wake, 0xFFFFFFFFu);
    if (requeue > 0) woken += futex_wake(p, uaddr, requeue, 0xFFFFFFFFu);
    return woken;
}
