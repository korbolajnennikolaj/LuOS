// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_SYNC_H
#define _LKPI_SYNC_H

#include <lkpi/base.h>

struct task_struct;


void lkpi_preempt_disable(void);
void lkpi_preempt_enable(void);
#define preempt_disable() lkpi_preempt_disable()
#define preempt_enable() lkpi_preempt_enable()
#define preempt_enable_no_resched() lkpi_preempt_enable()
#define preempt_disable_notrace() lkpi_preempt_disable()
#define preempt_enable_notrace() lkpi_preempt_enable()
#define migrate_disable() lkpi_preempt_disable()
#define migrate_enable() lkpi_preempt_enable()
#define preempt_count() lkpi_preempt_count()
#define preemptible() (lkpi_preempt_count() == 0 && !irqs_disabled())
#define local_bh_disable() lkpi_preempt_disable()
#define local_bh_enable() lkpi_preempt_enable()

static __always_inline unsigned long arch_local_save_flags(void)
{
	unsigned long flags;
	asm volatile("pushf ; pop %0" : "=rm"(flags) : : "memory");
	return flags;
}
static __always_inline void arch_local_irq_disable(void) { asm volatile("cli" ::: "memory"); }
static __always_inline void arch_local_irq_enable(void) { asm volatile("sti" ::: "memory"); }
static __always_inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags = arch_local_save_flags();
	arch_local_irq_disable();
	return flags;
}
static __always_inline void arch_local_irq_restore(unsigned long flags)
{
	if (flags & (1UL << 9)) arch_local_irq_enable();
}
#define local_irq_save(flags) do { typecheck(unsigned long, flags); flags = arch_local_irq_save(); } while (0)
#define local_irq_restore(flags) do { typecheck(unsigned long, flags); arch_local_irq_restore(flags); } while (0)
#define local_irq_disable() arch_local_irq_disable()
#define local_irq_enable() arch_local_irq_enable()
#define local_save_flags(flags) do { flags = arch_local_save_flags(); } while (0)
#define raw_local_irq_save local_irq_save
#define raw_local_irq_restore local_irq_restore
#define irqs_disabled_flags(flags) (!((flags) & (1UL << 9)))


typedef struct raw_spinlock {
	volatile unsigned int locked;
} raw_spinlock_t;

typedef struct spinlock {
	struct raw_spinlock rlock;
} spinlock_t;

#define __RAW_SPIN_LOCK_UNLOCKED(name) { .locked = 0 }
#define __RAW_SPIN_LOCK_INITIALIZER(name) __RAW_SPIN_LOCK_UNLOCKED(name)
#define __SPIN_LOCK_UNLOCKED(name) { .rlock = __RAW_SPIN_LOCK_UNLOCKED(name) }
#define __SPIN_LOCK_INITIALIZER(name) __SPIN_LOCK_UNLOCKED(name)
#define DEFINE_SPINLOCK(x) spinlock_t x = __SPIN_LOCK_UNLOCKED(x)
#define DEFINE_RAW_SPINLOCK(x) raw_spinlock_t x = __RAW_SPIN_LOCK_UNLOCKED(x)

static __always_inline void arch_spin_lock(raw_spinlock_t *l)
{
	while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
		while (READ_ONCE(l->locked)) cpu_relax();
}
static __always_inline bool arch_spin_trylock(raw_spinlock_t *l)
{
	return !__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE);
}
static __always_inline void arch_spin_unlock(raw_spinlock_t *l)
{
	__atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

#define raw_spin_lock_init(l) do { (l)->locked = 0; } while (0)
#define raw_spin_lock(l) do { preempt_disable(); arch_spin_lock(l); } while (0)
#define raw_spin_unlock(l) do { arch_spin_unlock(l); preempt_enable(); } while (0)
#define raw_spin_trylock(l) ({ preempt_disable(); arch_spin_trylock(l) ? 1 : ({ preempt_enable(); 0; }); })
#define raw_spin_lock_irq(l) do { local_irq_disable(); raw_spin_lock(l); } while (0)
#define raw_spin_unlock_irq(l) do { raw_spin_unlock(l); local_irq_enable(); } while (0)
#define raw_spin_lock_irqsave(l, flags) do { local_irq_save(flags); raw_spin_lock(l); } while (0)
#define raw_spin_unlock_irqrestore(l, flags) do { raw_spin_unlock(l); local_irq_restore(flags); } while (0)
#define raw_spin_is_locked(l) (READ_ONCE((l)->locked) != 0)

static __always_inline raw_spinlock_t *spinlock_check(spinlock_t *lock) { return &lock->rlock; }
#define spin_lock_init(l) do { raw_spin_lock_init(&(l)->rlock); } while (0)
#define spin_lock(l) raw_spin_lock(&(l)->rlock)
#define spin_lock_bh(l) raw_spin_lock(&(l)->rlock)
#define spin_lock_nested(l, sub) raw_spin_lock(&(l)->rlock)
#define spin_lock_nest_lock(l, nl) raw_spin_lock(&(l)->rlock)
#define spin_unlock(l) raw_spin_unlock(&(l)->rlock)
#define spin_unlock_bh(l) raw_spin_unlock(&(l)->rlock)
#define spin_trylock(l) raw_spin_trylock(&(l)->rlock)
#define spin_lock_irq(l) raw_spin_lock_irq(&(l)->rlock)
#define spin_unlock_irq(l) raw_spin_unlock_irq(&(l)->rlock)
#define spin_lock_irqsave(l, flags) raw_spin_lock_irqsave(spinlock_check(l), flags)
#define spin_lock_irqsave_nested(l, flags, sub) raw_spin_lock_irqsave(spinlock_check(l), flags)
#define spin_unlock_irqrestore(l, flags) raw_spin_unlock_irqrestore(&(l)->rlock, flags)
#define spin_trylock_irqsave(l, flags) ({ local_irq_save(flags); spin_trylock(l) ? 1 : ({ local_irq_restore(flags); 0; }); })
#define spin_trylock_irq(l) ({ local_irq_disable(); spin_trylock(l) ? 1 : ({ local_irq_enable(); 0; }); })
#define spin_is_locked(l) raw_spin_is_locked(&(l)->rlock)
#define assert_spin_locked(l) do { } while (0)
#define assert_raw_spin_locked(l) do { } while (0)
#define spin_is_contended(l) 0

typedef struct { raw_spinlock_t l; } rwlock_t;
#define __RW_LOCK_UNLOCKED(name) { .l = __RAW_SPIN_LOCK_UNLOCKED(name) }
#define DEFINE_RWLOCK(x) rwlock_t x = __RW_LOCK_UNLOCKED(x)
#define rwlock_init(rw) raw_spin_lock_init(&(rw)->l)
#define read_lock(rw) raw_spin_lock(&(rw)->l)
#define read_unlock(rw) raw_spin_unlock(&(rw)->l)
#define write_lock(rw) raw_spin_lock(&(rw)->l)
#define write_unlock(rw) raw_spin_unlock(&(rw)->l)
#define read_lock_irq(rw) raw_spin_lock_irq(&(rw)->l)
#define read_unlock_irq(rw) raw_spin_unlock_irq(&(rw)->l)
#define write_lock_irq(rw) raw_spin_lock_irq(&(rw)->l)
#define write_unlock_irq(rw) raw_spin_unlock_irq(&(rw)->l)
#define read_lock_irqsave(rw, f) raw_spin_lock_irqsave(&(rw)->l, f)
#define read_unlock_irqrestore(rw, f) raw_spin_unlock_irqrestore(&(rw)->l, f)
#define write_lock_irqsave(rw, f) raw_spin_lock_irqsave(&(rw)->l, f)
#define write_unlock_irqrestore(rw, f) raw_spin_unlock_irqrestore(&(rw)->l, f)
#define read_lock_bh read_lock
#define read_unlock_bh read_unlock
#define write_lock_bh write_lock
#define write_unlock_bh write_unlock


struct mutex {
	atomic_long_t owner;
	raw_spinlock_t wait_lock;
	struct list_head wait_list;
};

#define __MUTEX_INITIALIZER(lockname) { \
	.owner = ATOMIC_LONG_INIT(0), \
	.wait_lock = __RAW_SPIN_LOCK_UNLOCKED(lockname.wait_lock), \
	.wait_list = LIST_HEAD_INIT(lockname.wait_list) }
#define DEFINE_MUTEX(mutexname) struct mutex mutexname = __MUTEX_INITIALIZER(mutexname)

void __mutex_init(struct mutex *lock, const char *name, struct lock_class_key *key);
#define mutex_init(mutex) __mutex_init((mutex), #mutex, NULL)
#define mutex_init_with_key(mutex, key) __mutex_init((mutex), #mutex, key)
void mutex_lock(struct mutex *lock);
int __must_check mutex_lock_interruptible(struct mutex *lock);
int __must_check mutex_lock_killable(struct mutex *lock);
int mutex_trylock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);
bool mutex_is_locked(struct mutex *lock);
#define mutex_lock_nested(lock, subclass) mutex_lock(lock)
#define mutex_lock_interruptible_nested(lock, subclass) mutex_lock_interruptible(lock)
#define mutex_lock_killable_nested(lock, subclass) mutex_lock_killable(lock)
#define mutex_lock_nest_lock(lock, nest_lock) mutex_lock(lock)
#define mutex_lock_io(lock) mutex_lock(lock)
#define mutex_trylock_nested(lock, sub) mutex_trylock(lock)
#define mutex_destroy(lock) do { (void)(lock); } while (0)
static inline int mutex_trylock_recursive(struct mutex *lock) { return mutex_trylock(lock); }
int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock);
#define devm_mutex_init(dev, mutex) ({ mutex_init(mutex); 0; })

struct rw_semaphore {
	atomic_long_t count;
	raw_spinlock_t wait_lock;
	struct list_head wait_list;
	struct task_struct *owner;
};
#define __RWSEM_INITIALIZER(name) { \
	.count = ATOMIC_LONG_INIT(0), \
	.wait_lock = __RAW_SPIN_LOCK_UNLOCKED(name.wait_lock), \
	.wait_list = LIST_HEAD_INIT((name).wait_list) }
#define DECLARE_RWSEM(name) struct rw_semaphore name = __RWSEM_INITIALIZER(name)
void __init_rwsem(struct rw_semaphore *sem, const char *name, struct lock_class_key *key);
#define init_rwsem(sem) __init_rwsem((sem), #sem, NULL)
void down_read(struct rw_semaphore *sem);
int down_read_interruptible(struct rw_semaphore *sem);
int down_read_killable(struct rw_semaphore *sem);
int down_read_trylock(struct rw_semaphore *sem);
void up_read(struct rw_semaphore *sem);
void down_write(struct rw_semaphore *sem);
int down_write_killable(struct rw_semaphore *sem);
int down_write_trylock(struct rw_semaphore *sem);
void up_write(struct rw_semaphore *sem);
void downgrade_write(struct rw_semaphore *sem);
#define down_read_nested(sem, sub) down_read(sem)
#define down_write_nested(sem, sub) down_write(sem)
#define down_write_nest_lock(sem, nl) down_write(sem)
static inline int rwsem_is_locked(struct rw_semaphore *sem) { return atomic_long_read(&sem->count) != 0; }
static inline int rwsem_is_contended(struct rw_semaphore *sem) { return !list_empty(&sem->wait_list); }
#define rwsem_assert_held(sem) do { } while (0)
#define rwsem_assert_held_write(sem) do { } while (0)

struct semaphore {
	raw_spinlock_t lock;
	unsigned int count;
	struct list_head wait_list;
};
#define __SEMAPHORE_INITIALIZER(name, n) { .lock = __RAW_SPIN_LOCK_UNLOCKED((name).lock), .count = n, \
	.wait_list = LIST_HEAD_INIT((name).wait_list) }
#define DEFINE_SEMAPHORE(name, n) struct semaphore name = __SEMAPHORE_INITIALIZER(name, n)
void sema_init(struct semaphore *sem, int val);
void down(struct semaphore *sem);
int down_interruptible(struct semaphore *sem);
int down_killable(struct semaphore *sem);
int down_trylock(struct semaphore *sem);
int down_timeout(struct semaphore *sem, long timeout);
void up(struct semaphore *sem);


typedef struct seqcount { unsigned sequence; } seqcount_t;
#define SEQCNT_ZERO(name) { .sequence = 0 }
#define seqcount_init(s) do { (s)->sequence = 0; } while (0)
typedef seqcount_t seqcount_raw_spinlock_t;
typedef seqcount_t seqcount_spinlock_t;
typedef seqcount_t seqcount_rwlock_t;
typedef seqcount_t seqcount_mutex_t;
typedef seqcount_t seqcount_ww_mutex_t;
#define seqcount_spinlock_init(s, lock) seqcount_init(s)
#define seqcount_raw_spinlock_init(s, lock) seqcount_init(s)
#define seqcount_mutex_init(s, lock) seqcount_init(s)
#define seqcount_ww_mutex_init(s, lock) seqcount_init(s)
#define SEQCNT_SPINLOCK_ZERO(name, lock) SEQCNT_ZERO(name)
#define SEQCNT_WW_MUTEX_ZERO(name, lock) SEQCNT_ZERO(name)

static inline unsigned __read_seqcount_begin(const seqcount_t *s)
{
	unsigned ret;
	while ((ret = READ_ONCE(s->sequence)) & 1) cpu_relax();
	return ret;
}
static inline unsigned raw_read_seqcount_begin(const seqcount_t *s) { unsigned r = __read_seqcount_begin(s); smp_rmb(); return r; }
#define read_seqcount_begin(s) raw_read_seqcount_begin(s)
static inline unsigned raw_read_seqcount(const seqcount_t *s) { unsigned r = READ_ONCE(s->sequence); smp_rmb(); return r; }
#define raw_seqcount_begin(s) (raw_read_seqcount(s) & ~1u)
static inline int __read_seqcount_retry(const seqcount_t *s, unsigned start) { return unlikely(READ_ONCE(s->sequence) != start); }
static inline int read_seqcount_retry(const seqcount_t *s, unsigned start) { smp_rmb(); return __read_seqcount_retry(s, start); }
static inline void raw_write_seqcount_begin(seqcount_t *s) { s->sequence++; smp_wmb(); }
static inline void raw_write_seqcount_end(seqcount_t *s) { smp_wmb(); s->sequence++; }
#define write_seqcount_begin(s) raw_write_seqcount_begin(s)
#define write_seqcount_end(s) raw_write_seqcount_end(s)
#define write_seqcount_begin_nested(s, sub) raw_write_seqcount_begin(s)
static inline void write_seqcount_invalidate(seqcount_t *s) { smp_wmb(); s->sequence += 2; }

typedef struct {
	seqcount_t seqcount;
	spinlock_t lock;
} seqlock_t;
#define __SEQLOCK_UNLOCKED(lockname) { .seqcount = SEQCNT_ZERO(lockname), .lock = __SPIN_LOCK_UNLOCKED(lockname) }
#define DEFINE_SEQLOCK(sl) seqlock_t sl = __SEQLOCK_UNLOCKED(sl)
#define seqlock_init(sl) do { seqcount_init(&(sl)->seqcount); spin_lock_init(&(sl)->lock); } while (0)
static inline unsigned read_seqbegin(const seqlock_t *sl) { return read_seqcount_begin(&sl->seqcount); }
static inline unsigned read_seqretry(const seqlock_t *sl, unsigned start) { return read_seqcount_retry(&sl->seqcount, start); }
static inline void write_seqlock(seqlock_t *sl) { spin_lock(&sl->lock); write_seqcount_begin(&sl->seqcount); }
static inline void write_sequnlock(seqlock_t *sl) { write_seqcount_end(&sl->seqcount); spin_unlock(&sl->lock); }
#define write_seqlock_irqsave(lock, flags) do { spin_lock_irqsave(&(lock)->lock, flags); write_seqcount_begin(&(lock)->seqcount); } while (0)
#define write_sequnlock_irqrestore(lock, flags) do { write_seqcount_end(&(lock)->seqcount); spin_unlock_irqrestore(&(lock)->lock, flags); } while (0)
static inline void write_seqlock_irq(seqlock_t *sl) { spin_lock_irq(&sl->lock); write_seqcount_begin(&sl->seqcount); }
static inline void write_sequnlock_irq(seqlock_t *sl) { write_seqcount_end(&sl->seqcount); spin_unlock_irq(&sl->lock); }
static inline void read_seqlock_excl(seqlock_t *sl) { spin_lock(&sl->lock); }
static inline void read_sequnlock_excl(seqlock_t *sl) { spin_unlock(&sl->lock); }


struct callback_head;
typedef void (*rcu_callback_t)(struct callback_head *head);

#define rcu_read_lock() preempt_disable()
#define rcu_read_unlock() preempt_enable()
#define rcu_read_lock_bh() preempt_disable()
#define rcu_read_unlock_bh() preempt_enable()
#define rcu_read_lock_sched() preempt_disable()
#define rcu_read_unlock_sched() preempt_enable()
#define rcu_read_lock_held() 1
#define rcu_read_lock_any_held() 1
#define rcu_read_lock_sched_held() 1
#define __rcu_dereference_check(p, local, c, space) ((typeof(*p) __force *)READ_ONCE(p))
#define rcu_dereference_raw(p) ((typeof(*p) __force *)READ_ONCE(p))
#define rcu_dereference(p) rcu_dereference_raw(p)
#define rcu_dereference_bh(p) rcu_dereference_raw(p)
#define rcu_dereference_sched(p) rcu_dereference_raw(p)
#define rcu_dereference_check(p, c) rcu_dereference_raw(p)
#define rcu_dereference_bh_check(p, c) rcu_dereference_raw(p)
#define rcu_dereference_protected(p, c) ((typeof(*p) __force *)(p))
#define rcu_dereference_raw_check(p) rcu_dereference_raw(p)
#define rcu_access_pointer(p) ((typeof(*p) __force *)READ_ONCE(p))
#define rcu_assign_pointer(p, v) do { smp_wmb(); WRITE_ONCE((p), (typeof(p))(v)); } while (0)
#define RCU_INIT_POINTER(p, v) do { WRITE_ONCE((p), (typeof(p))(v)); } while (0)
#define RCU_INITIALIZER(v) (typeof(*(v)) __force __rcu *)(v)
#define rcu_replace_pointer(rcu_ptr, ptr, c) ({ \
	typeof(ptr) __tmp = rcu_dereference_protected((rcu_ptr), (c)); \
	rcu_assign_pointer((rcu_ptr), (ptr)); __tmp; })
#define rcu_pointer_handoff(p) (p)
#define unrcu_pointer(p) ((typeof(*p) __force *)(p))
void synchronize_rcu(void);
void synchronize_rcu_expedited(void);
void call_rcu(struct callback_head *head, rcu_callback_t func);
void rcu_barrier(void);
void kvfree_call_rcu(struct callback_head *head, void *ptr);
#define synchronize_rcu_tasks() synchronize_rcu()
#define synchronize_net() synchronize_rcu()
#define kfree_rcu(ptr, rhf) kvfree_call_rcu(&((ptr)->rhf), (void *)(ptr))
#define kvfree_rcu(ptr, rhf) kvfree_call_rcu(&((ptr)->rhf), (void *)(ptr))
#define kfree_rcu_mightsleep(ptr) do { synchronize_rcu(); kfree(ptr); } while (0)
#define rcu_head_init(h) do { } while (0)
#define rcu_head_after_call_rcu(h, f) false
#define init_rcu_head(h) do { } while (0)
#define destroy_rcu_head(h) do { } while (0)
#define init_rcu_head_on_stack(h) do { } while (0)
#define destroy_rcu_head_on_stack(h) do { } while (0)
#define rcu_is_watching() true
#define rcu_sleep_check() do { } while (0)
#define cond_synchronize_rcu(c) synchronize_rcu()
#define get_state_synchronize_rcu() 0UL
#define poll_state_synchronize_rcu(c) true

struct srcu_struct {
	atomic_t readers[2];
	unsigned int idx;
	struct mutex lock;
};
#define DEFINE_SRCU(name) struct srcu_struct name = { .lock = __MUTEX_INITIALIZER(name.lock) }
#define DEFINE_STATIC_SRCU(name) static struct srcu_struct name = { .lock = __MUTEX_INITIALIZER(name.lock) }
int init_srcu_struct(struct srcu_struct *ssp);
void cleanup_srcu_struct(struct srcu_struct *ssp);
int srcu_read_lock(struct srcu_struct *ssp);
void srcu_read_unlock(struct srcu_struct *ssp, int idx);
void synchronize_srcu(struct srcu_struct *ssp);
#define srcu_dereference(p, ssp) rcu_dereference_raw(p)
#define srcu_dereference_check(p, ssp, c) rcu_dereference_raw(p)
#define srcu_read_lock_held(ssp) 1
#define srcu_barrier(ssp) synchronize_srcu(ssp)


extern unsigned long __per_cpu_offset[NR_CPUS];
#define DEFINE_PER_CPU(type, name) __attribute__((section("lkpi_percpu"))) __typeof__(type) name
#define DEFINE_PER_CPU_ALIGNED(type, name) DEFINE_PER_CPU(type, name) ____cacheline_aligned
#define DEFINE_PER_CPU_SHARED_ALIGNED(type, name) DEFINE_PER_CPU(type, name) ____cacheline_aligned
#define DECLARE_PER_CPU(type, name) extern __typeof__(type) name
#define per_cpu_ptr(ptr, cpu) ((typeof(ptr))((unsigned long)(ptr) + __per_cpu_offset[(cpu)]))
#define raw_cpu_ptr(ptr) per_cpu_ptr(ptr, lkpi_cpu_id())
#define this_cpu_ptr(ptr) raw_cpu_ptr(ptr)
#define per_cpu(var, cpu) (*per_cpu_ptr(&(var), cpu))
#define __this_cpu_read(pcp) (*this_cpu_ptr(&(pcp)))
#define __this_cpu_write(pcp, val) do { *this_cpu_ptr(&(pcp)) = (val); } while (0)
#define this_cpu_read(pcp) ({ typeof(pcp) __v; unsigned long __f; local_irq_save(__f); __v = __this_cpu_read(pcp); local_irq_restore(__f); __v; })
#define this_cpu_write(pcp, val) do { unsigned long __f; local_irq_save(__f); __this_cpu_write(pcp, val); local_irq_restore(__f); } while (0)
#define __this_cpu_add(pcp, val) do { *this_cpu_ptr(&(pcp)) += (val); } while (0)
#define __this_cpu_sub(pcp, val) do { *this_cpu_ptr(&(pcp)) -= (val); } while (0)
#define __this_cpu_add_return(pcp, val) ({ typeof(pcp) *__p = this_cpu_ptr(&(pcp)); *__p += (val); *__p; })
#define __this_cpu_inc_return(pcp) __this_cpu_add_return(pcp, 1)
#define __this_cpu_dec_return(pcp) __this_cpu_add_return(pcp, -1)
#define __this_cpu_inc(pcp) __this_cpu_add(pcp, 1)
#define __this_cpu_dec(pcp) __this_cpu_sub(pcp, 1)
#define this_cpu_add(pcp, val) do { unsigned long __f; local_irq_save(__f); __this_cpu_add(pcp, val); local_irq_restore(__f); } while (0)
#define this_cpu_sub(pcp, val) do { unsigned long __f; local_irq_save(__f); __this_cpu_sub(pcp, val); local_irq_restore(__f); } while (0)
#define this_cpu_inc(pcp) this_cpu_add(pcp, 1)
#define this_cpu_dec(pcp) this_cpu_sub(pcp, 1)
#define get_cpu_var(var) (*({ preempt_disable(); this_cpu_ptr(&var); }))
#define put_cpu_var(var) do { (void)&(var); preempt_enable(); } while (0)
#define get_cpu_ptr(var) ({ preempt_disable(); this_cpu_ptr(var); })
#define put_cpu_ptr(var) do { (void)(var); preempt_enable(); } while (0)
typedef struct { } local_lock_t;
#define INIT_LOCAL_LOCK(lockname) { }
#define local_lock(lock) preempt_disable()
#define local_unlock(lock) preempt_enable()
#define local_lock_irqsave(lock, flags) local_irq_save(flags)
#define local_unlock_irqrestore(lock, flags) local_irq_restore(flags)
#define local_lock_init(l) do { } while (0)


struct swait_queue_head;
struct completion {
	unsigned int done;
	raw_spinlock_t lock;
	struct list_head waiters;
};
#define COMPLETION_INITIALIZER(work) { 0, __RAW_SPIN_LOCK_UNLOCKED((work).lock), LIST_HEAD_INIT((work).waiters) }
#define COMPLETION_INITIALIZER_ONSTACK(work) (*({ init_completion(&work); &work; }))
#define DECLARE_COMPLETION(work) struct completion work = COMPLETION_INITIALIZER(work)
#define DECLARE_COMPLETION_ONSTACK(work) DECLARE_COMPLETION(work)
void init_completion(struct completion *x);
static inline void reinit_completion(struct completion *x) { x->done = 0; }
void wait_for_completion(struct completion *x);
void wait_for_completion_io(struct completion *x);
int wait_for_completion_interruptible(struct completion *x);
int wait_for_completion_killable(struct completion *x);
unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout);
unsigned long wait_for_completion_io_timeout(struct completion *x, unsigned long timeout);
long wait_for_completion_interruptible_timeout(struct completion *x, unsigned long timeout);
long wait_for_completion_killable_timeout(struct completion *x, unsigned long timeout);
bool try_wait_for_completion(struct completion *x);
bool completion_done(struct completion *x);
void complete(struct completion *x);
void complete_all(struct completion *x);
#define complete_on_current_cpu complete


typedef struct refcount_struct { atomic_t refs; } refcount_t;
#define REFCOUNT_INIT(n) { .refs = ATOMIC_INIT(n) }
#define REFCOUNT_MAX INT_MAX
#define REFCOUNT_SATURATED (INT_MIN / 2)
static inline void refcount_set(refcount_t *r, int n) { atomic_set(&r->refs, n); }
static inline void refcount_set_release(refcount_t *r, int n) { atomic_set_release(&r->refs, n); }
static inline unsigned int refcount_read(const refcount_t *r) { return atomic_read(&r->refs); }
static inline bool refcount_add_not_zero(int i, refcount_t *r) { return atomic_add_unless(&r->refs, i, 0); }
static inline bool refcount_inc_not_zero(refcount_t *r) { return refcount_add_not_zero(1, r); }
static inline void refcount_add(int i, refcount_t *r)
{
	int old = atomic_fetch_add(i, &r->refs);
	WARN_ONCE(old == 0, "refcount_t: addition on 0; use-after-free");
}
static inline void refcount_inc(refcount_t *r) { refcount_add(1, r); }
static inline bool refcount_sub_and_test(int i, refcount_t *r)
{
	int old = atomic_fetch_sub_release(i, &r->refs);
	if (old == i) { smp_mb(); return true; }
	WARN_ONCE(old < i, "refcount_t: underflow; use-after-free");
	return false;
}
static inline bool refcount_dec_and_test(refcount_t *r) { return refcount_sub_and_test(1, r); }
static inline void refcount_dec(refcount_t *r) { refcount_sub_and_test(1, r); }
static inline bool refcount_dec_if_one(refcount_t *r) { int one = 1; return atomic_try_cmpxchg(&r->refs, &one, 0); }
static inline bool refcount_dec_not_one(refcount_t *r) { return atomic_add_unless(&r->refs, -1, 1); }
bool refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *lock);
bool refcount_dec_and_lock(refcount_t *r, spinlock_t *lock);
bool refcount_dec_and_lock_irqsave(refcount_t *r, spinlock_t *lock, unsigned long *flags);


struct ww_class {
	atomic_long_t stamp;
	const char *acquire_name;
	const char *mutex_name;
	unsigned int is_wait_die;
};
struct ww_acquire_ctx {
	struct task_struct *task;
	unsigned long stamp;
	unsigned int acquired;
	unsigned short wounded;
	unsigned short is_wait_die;
	struct ww_class *ww_class;
	void *contending_lock;
};
struct ww_mutex {
	struct mutex base;
	struct ww_acquire_ctx *ctx;
	struct ww_class *ww_class;
};
#define __WW_CLASS_INITIALIZER(ww_class, _is_wait_die) { .stamp = ATOMIC_LONG_INIT(0), \
	.acquire_name = #ww_class "_acquire", .mutex_name = #ww_class "_mutex", .is_wait_die = _is_wait_die }
#define DEFINE_WD_CLASS(classname) struct ww_class classname = __WW_CLASS_INITIALIZER(classname, 1)
#define DEFINE_WW_CLASS(classname) struct ww_class classname = __WW_CLASS_INITIALIZER(classname, 0)
static inline void ww_mutex_init(struct ww_mutex *lock, struct ww_class *ww_class)
{
	mutex_init(&lock->base);
	lock->ctx = NULL;
	lock->ww_class = ww_class;
}
void ww_acquire_init(struct ww_acquire_ctx *ctx, struct ww_class *ww_class);
static inline void ww_acquire_done(struct ww_acquire_ctx *ctx) { }
static inline void ww_acquire_fini(struct ww_acquire_ctx *ctx) { }
int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
int __must_check ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
void ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
int ww_mutex_lock_slow_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
void ww_mutex_unlock(struct ww_mutex *lock);
static inline bool ww_mutex_is_locked(struct ww_mutex *lock) { return mutex_is_locked(&lock->base); }
static inline void ww_mutex_destroy(struct ww_mutex *lock) { }

#endif
