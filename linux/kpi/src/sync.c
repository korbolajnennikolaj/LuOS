// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"


void lkpi_preempt_disable(void)
{
	int *c = luos_preempt_counter();
	(*c)++;
	barrier();
}

void lkpi_preempt_enable(void)
{
	int *c = luos_preempt_counter();
	barrier();
	if (unlikely(*c <= 0)) {
		*c = 0;
		WARN_ONCE(1, "preempt_enable without preempt_disable");
		return;
	}
	if (--(*c) == 0 && !luos_in_interrupt()) luos_preempt_resume();
}

int lkpi_preempt_count(void) { return *luos_preempt_counter(); }
bool lkpi_in_irq(void) { return luos_in_interrupt() != 0; }
bool lkpi_irqs_disabled(void) { return irqs_disabled_flags(arch_local_save_flags()); }
int lkpi_cpu_id(void) { return luos_cpu_id(); }
unsigned int lkpi_num_online_cpus(void) { return (unsigned int)luos_cpu_count(); }


#define MUTEX_FLAG_WAITERS 1UL
#define WAITER_PLAIN 0
#define WAITER_WW 1
#define WAITER_READ 2
#define WAITER_WRITE 3

static inline s64 mutex_me(void) { return (s64)(unsigned long)lkpi_current(); }

void __mutex_init(struct mutex *lock, const char *name, struct lock_class_key *key)
{
	atomic_long_set(&lock->owner, 0);
	raw_spin_lock_init(&lock->wait_lock);
	INIT_LIST_HEAD(&lock->wait_list);
}

int mutex_trylock(struct mutex *lock)
{
	s64 zero = 0;
	return atomic_long_try_cmpxchg_acquire(&lock->owner, &zero, mutex_me());
}

bool mutex_is_locked(struct mutex *lock) { return atomic_long_read(&lock->owner) != 0; }

static bool ww_should_die(struct mutex *lock, struct ww_acquire_ctx *ctx)
{
	struct ww_mutex *ww = container_of(lock, struct ww_mutex, base);
	struct ww_acquire_ctx *holder = READ_ONCE(ww->ctx);
	if (!ctx || !ctx->acquired || !holder || holder == ctx) return false;
	return holder->stamp < ctx->stamp;
}

static int __mutex_lock_common(struct mutex *lock, unsigned int state, struct ww_acquire_ctx *ww_ctx)
{
	s64 me = mutex_me();
	struct lkpi_waiter w = { .task = current, .type = ww_ctx ? WAITER_WW : WAITER_PLAIN };
	int ret = 0;

	raw_spin_lock(&lock->wait_lock);
	for (;;) {
		s64 o = atomic_long_read(&lock->owner);
		if (o == 0) {
			if (atomic_long_try_cmpxchg_acquire(&lock->owner, &o, me)) {
				raw_spin_unlock(&lock->wait_lock);
				return 0;
			}
			continue;
		}
		if (o & MUTEX_FLAG_WAITERS) break;
		if (atomic_long_try_cmpxchg(&lock->owner, &o, o | MUTEX_FLAG_WAITERS)) break;
	}
	list_add_tail(&w.node, &lock->wait_list);
	for (;;) {
		if (READ_ONCE(w.granted)) break;
		if (ww_ctx && ww_should_die(lock, ww_ctx)) {
			ret = -EDEADLK;
			break;
		}
		if (signal_pending_state(state, current)) {
			ret = -EINTR;
			break;
		}
		set_current_state(state);
		raw_spin_unlock(&lock->wait_lock);
		schedule();
		raw_spin_lock(&lock->wait_lock);
	}
	__set_current_state(TASK_RUNNING);
	if (!w.granted) {
		list_del(&w.node);
		if (list_empty(&lock->wait_list)) atomic_long_andnot(MUTEX_FLAG_WAITERS, &lock->owner);
	}
	raw_spin_unlock(&lock->wait_lock);
	return w.granted ? 0 : ret;
}

void mutex_lock(struct mutex *lock)
{
	if (!mutex_trylock(lock)) __mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, NULL);
}

int mutex_lock_interruptible(struct mutex *lock)
{
	if (mutex_trylock(lock)) return 0;
	return __mutex_lock_common(lock, TASK_INTERRUPTIBLE, NULL);
}

int mutex_lock_killable(struct mutex *lock)
{
	if (mutex_trylock(lock)) return 0;
	return __mutex_lock_common(lock, TASK_KILLABLE, NULL);
}

void mutex_unlock(struct mutex *lock)
{
	s64 o = mutex_me();
	if (atomic_long_try_cmpxchg_release(&lock->owner, &o, 0)) return;

	raw_spin_lock(&lock->wait_lock);
	if (!list_empty(&lock->wait_list)) {
		struct lkpi_waiter *w = list_first_entry(&lock->wait_list, struct lkpi_waiter, node), *other;
		list_del_init(&w->node);
		s64 n = (s64)(unsigned long)w->task | (list_empty(&lock->wait_list) ? 0 : MUTEX_FLAG_WAITERS);
		atomic_long_set_release(&lock->owner, n);
		WRITE_ONCE(w->granted, 1);
		wake_up_process(w->task);
		list_for_each_entry(other, &lock->wait_list, node)
			if (other->type == WAITER_WW) wake_up_process(other->task);
	} else {
		atomic_long_set_release(&lock->owner, 0);
	}
	raw_spin_unlock(&lock->wait_lock);
}

int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
	if (atomic_add_unless(cnt, -1, 1)) return 0;
	mutex_lock(lock);
	if (!atomic_dec_and_test(cnt)) {
		mutex_unlock(lock);
		return 0;
	}
	return 1;
}

bool refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *lock)
{
	if (refcount_dec_not_one(r)) return false;
	mutex_lock(lock);
	if (!refcount_dec_and_test(r)) {
		mutex_unlock(lock);
		return false;
	}
	return true;
}

bool refcount_dec_and_lock(refcount_t *r, spinlock_t *lock)
{
	if (refcount_dec_not_one(r)) return false;
	spin_lock(lock);
	if (!refcount_dec_and_test(r)) {
		spin_unlock(lock);
		return false;
	}
	return true;
}

bool refcount_dec_and_lock_irqsave(refcount_t *r, spinlock_t *lock, unsigned long *flags)
{
	unsigned long f;
	if (refcount_dec_not_one(r)) return false;
	spin_lock_irqsave(lock, f);
	if (!refcount_dec_and_test(r)) {
		spin_unlock_irqrestore(lock, f);
		return false;
	}
	*flags = f;
	return true;
}


void ww_acquire_init(struct ww_acquire_ctx *ctx, struct ww_class *ww_class)
{
	ctx->task = current;
	ctx->stamp = atomic_long_inc_return(&ww_class->stamp);
	ctx->acquired = 0;
	ctx->wounded = 0;
	ctx->is_wait_die = ww_class->is_wait_die;
	ctx->ww_class = ww_class;
	ctx->contending_lock = NULL;
}

static int __ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx, unsigned int state)
{
	int ret;
	if (!ctx) {
		if (state == TASK_UNINTERRUPTIBLE) {
			mutex_lock(&lock->base);
			return 0;
		}
		return mutex_lock_interruptible(&lock->base);
	}
	if (READ_ONCE(lock->ctx) == ctx) return -EALREADY;
	if (!mutex_trylock(&lock->base)) {
		ret = __mutex_lock_common(&lock->base, state, ctx);
		if (ret) {
			if (ret == -EDEADLK) ctx->contending_lock = lock;
			return ret;
		}
	}
	WRITE_ONCE(lock->ctx, ctx);
	ctx->acquired++;
	ctx->contending_lock = NULL;
	return 0;
}

int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return __ww_mutex_lock(lock, ctx, TASK_UNINTERRUPTIBLE);
}

int ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return __ww_mutex_lock(lock, ctx, TASK_INTERRUPTIBLE);
}

void ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	__ww_mutex_lock(lock, ctx, TASK_UNINTERRUPTIBLE);
}

int ww_mutex_lock_slow_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return __ww_mutex_lock(lock, ctx, TASK_INTERRUPTIBLE);
}

int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	if (!mutex_trylock(&lock->base)) return 0;
	if (ctx) {
		WRITE_ONCE(lock->ctx, ctx);
		ctx->acquired++;
	}
	return 1;
}

void ww_mutex_unlock(struct ww_mutex *lock)
{
	struct ww_acquire_ctx *ctx = lock->ctx;
	if (ctx) {
		if (ctx->acquired > 0) ctx->acquired--;
		WRITE_ONCE(lock->ctx, NULL);
	}
	mutex_unlock(&lock->base);
}


void __init_rwsem(struct rw_semaphore *sem, const char *name, struct lock_class_key *key)
{
	atomic_long_set(&sem->count, 0);
	raw_spin_lock_init(&sem->wait_lock);
	INIT_LIST_HEAD(&sem->wait_list);
	sem->owner = NULL;
}

static void rwsem_grant_locked(struct rw_semaphore *sem)
{
	while (!list_empty(&sem->wait_list)) {
		struct lkpi_waiter *w = list_first_entry(&sem->wait_list, struct lkpi_waiter, node);
		s64 c = atomic_long_read(&sem->count);
		if (w->type == WAITER_WRITE) {
			if (c != 0) break;
			atomic_long_set(&sem->count, -1);
			sem->owner = w->task;
		} else {
			if (c < 0) break;
			atomic_long_set(&sem->count, c + 1);
		}
		list_del_init(&w->node);
		WRITE_ONCE(w->granted, 1);
		wake_up_process(w->task);
		if (w->type == WAITER_WRITE) break;
	}
}

static int rwsem_wait(struct rw_semaphore *sem, int type, unsigned int state)
{
	struct lkpi_waiter w = { .task = current, .type = type };
	int ret = 0;
	list_add_tail(&w.node, &sem->wait_list);
	for (;;) {
		if (READ_ONCE(w.granted)) break;
		if (signal_pending_state(state, current)) {
			ret = -EINTR;
			break;
		}
		set_current_state(state);
		raw_spin_unlock(&sem->wait_lock);
		schedule();
		raw_spin_lock(&sem->wait_lock);
	}
	__set_current_state(TASK_RUNNING);
	if (!w.granted) {
		list_del(&w.node);
		rwsem_grant_locked(sem);
		return ret;
	}
	return 0;
}

static int __down_read(struct rw_semaphore *sem, unsigned int state)
{
	int ret = 0;
	raw_spin_lock(&sem->wait_lock);
	s64 c = atomic_long_read(&sem->count);
	if (c >= 0 && list_empty(&sem->wait_list)) atomic_long_set(&sem->count, c + 1);
	else ret = rwsem_wait(sem, WAITER_READ, state);
	raw_spin_unlock(&sem->wait_lock);
	return ret;
}

static int __down_write(struct rw_semaphore *sem, unsigned int state)
{
	int ret = 0;
	raw_spin_lock(&sem->wait_lock);
	if (atomic_long_read(&sem->count) == 0 && list_empty(&sem->wait_list)) {
		atomic_long_set(&sem->count, -1);
		sem->owner = current;
	} else {
		ret = rwsem_wait(sem, WAITER_WRITE, state);
	}
	raw_spin_unlock(&sem->wait_lock);
	return ret;
}

void down_read(struct rw_semaphore *sem) { __down_read(sem, TASK_UNINTERRUPTIBLE); }
int down_read_interruptible(struct rw_semaphore *sem) { return __down_read(sem, TASK_INTERRUPTIBLE); }
int down_read_killable(struct rw_semaphore *sem) { return __down_read(sem, TASK_KILLABLE); }
void down_write(struct rw_semaphore *sem) { __down_write(sem, TASK_UNINTERRUPTIBLE); }
int down_write_killable(struct rw_semaphore *sem) { return __down_write(sem, TASK_KILLABLE); }

int down_read_trylock(struct rw_semaphore *sem)
{
	int ok = 0;
	raw_spin_lock(&sem->wait_lock);
	s64 c = atomic_long_read(&sem->count);
	if (c >= 0 && list_empty(&sem->wait_list)) {
		atomic_long_set(&sem->count, c + 1);
		ok = 1;
	}
	raw_spin_unlock(&sem->wait_lock);
	return ok;
}

int down_write_trylock(struct rw_semaphore *sem)
{
	int ok = 0;
	raw_spin_lock(&sem->wait_lock);
	if (atomic_long_read(&sem->count) == 0) {
		atomic_long_set(&sem->count, -1);
		sem->owner = current;
		ok = 1;
	}
	raw_spin_unlock(&sem->wait_lock);
	return ok;
}

void up_read(struct rw_semaphore *sem)
{
	raw_spin_lock(&sem->wait_lock);
	s64 c = atomic_long_read(&sem->count);
	if (WARN_ON_ONCE(c <= 0)) c = 1;
	atomic_long_set(&sem->count, c - 1);
	if (c == 1) rwsem_grant_locked(sem);
	raw_spin_unlock(&sem->wait_lock);
}

void up_write(struct rw_semaphore *sem)
{
	raw_spin_lock(&sem->wait_lock);
	atomic_long_set(&sem->count, 0);
	sem->owner = NULL;
	rwsem_grant_locked(sem);
	raw_spin_unlock(&sem->wait_lock);
}

void downgrade_write(struct rw_semaphore *sem)
{
	raw_spin_lock(&sem->wait_lock);
	atomic_long_set(&sem->count, 1);
	sem->owner = NULL;
	rwsem_grant_locked(sem);
	raw_spin_unlock(&sem->wait_lock);
}


void sema_init(struct semaphore *sem, int val)
{
	raw_spin_lock_init(&sem->lock);
	sem->count = val;
	INIT_LIST_HEAD(&sem->wait_list);
}

static int __down(struct semaphore *sem, unsigned int state, long timeout)
{
	unsigned long flags;
	int ret = 0;
	raw_spin_lock_irqsave(&sem->lock, flags);
	if (sem->count > 0) {
		sem->count--;
		raw_spin_unlock_irqrestore(&sem->lock, flags);
		return 0;
	}
	struct lkpi_waiter w = { .task = current };
	list_add_tail(&w.node, &sem->wait_list);
	for (;;) {
		if (READ_ONCE(w.granted)) break;
		if (signal_pending_state(state, current)) {
			ret = -EINTR;
			break;
		}
		if (timeout <= 0) {
			ret = -ETIME;
			break;
		}
		set_current_state(state);
		raw_spin_unlock_irqrestore(&sem->lock, flags);
		timeout = schedule_timeout(timeout);
		raw_spin_lock_irqsave(&sem->lock, flags);
	}
	__set_current_state(TASK_RUNNING);
	if (!w.granted) list_del(&w.node);
	raw_spin_unlock_irqrestore(&sem->lock, flags);
	return w.granted ? 0 : ret;
}

void down(struct semaphore *sem) { __down(sem, TASK_UNINTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT); }
int down_interruptible(struct semaphore *sem) { return __down(sem, TASK_INTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT); }
int down_killable(struct semaphore *sem) { return __down(sem, TASK_KILLABLE, MAX_SCHEDULE_TIMEOUT); }
int down_timeout(struct semaphore *sem, long timeout) { return __down(sem, TASK_UNINTERRUPTIBLE, timeout); }

int down_trylock(struct semaphore *sem)
{
	unsigned long flags;
	int busy = 1;
	raw_spin_lock_irqsave(&sem->lock, flags);
	if (sem->count > 0) {
		sem->count--;
		busy = 0;
	}
	raw_spin_unlock_irqrestore(&sem->lock, flags);
	return busy;
}

void up(struct semaphore *sem)
{
	unsigned long flags;
	raw_spin_lock_irqsave(&sem->lock, flags);
	if (list_empty(&sem->wait_list)) {
		sem->count++;
	} else {
		struct lkpi_waiter *w = list_first_entry(&sem->wait_list, struct lkpi_waiter, node);
		list_del_init(&w->node);
		WRITE_ONCE(w->granted, 1);
		wake_up_process(w->task);
	}
	raw_spin_unlock_irqrestore(&sem->lock, flags);
}


void init_completion(struct completion *x)
{
	x->done = 0;
	raw_spin_lock_init(&x->lock);
	INIT_LIST_HEAD(&x->waiters);
}

static long wait_for_common(struct completion *x, long timeout, unsigned int state)
{
	unsigned long flags;
	raw_spin_lock_irqsave(&x->lock, flags);
	if (!x->done) {
		struct lkpi_waiter w = { .task = current };
		INIT_LIST_HEAD(&w.node);
		do {
			if (signal_pending_state(state, current)) {
				timeout = -ERESTARTSYS;
				break;
			}
			if (list_empty(&w.node)) list_add_tail(&w.node, &x->waiters);
			set_current_state(state);
			raw_spin_unlock_irqrestore(&x->lock, flags);
			timeout = schedule_timeout(timeout);
			raw_spin_lock_irqsave(&x->lock, flags);
		} while (!x->done && timeout > 0);
		list_del_init(&w.node);
		__set_current_state(TASK_RUNNING);
		if (!x->done) {
			raw_spin_unlock_irqrestore(&x->lock, flags);
			return timeout;
		}
	}
	if (x->done != UINT_MAX) x->done--;
	raw_spin_unlock_irqrestore(&x->lock, flags);
	return timeout > 0 ? timeout : 1;
}

void wait_for_completion(struct completion *x) { wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_UNINTERRUPTIBLE); }
void wait_for_completion_io(struct completion *x) { wait_for_completion(x); }

int wait_for_completion_interruptible(struct completion *x)
{
	long t = wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_INTERRUPTIBLE);
	return t == -ERESTARTSYS ? (int)t : 0;
}

int wait_for_completion_killable(struct completion *x)
{
	long t = wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_KILLABLE);
	return t == -ERESTARTSYS ? (int)t : 0;
}

unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout)
{
	long t = wait_for_common(x, (long)timeout, TASK_UNINTERRUPTIBLE);
	return t > 0 ? (unsigned long)t : 0;
}

unsigned long wait_for_completion_io_timeout(struct completion *x, unsigned long timeout)
{
	return wait_for_completion_timeout(x, timeout);
}

long wait_for_completion_interruptible_timeout(struct completion *x, unsigned long timeout)
{
	return wait_for_common(x, (long)timeout, TASK_INTERRUPTIBLE);
}

long wait_for_completion_killable_timeout(struct completion *x, unsigned long timeout)
{
	return wait_for_common(x, (long)timeout, TASK_KILLABLE);
}

bool try_wait_for_completion(struct completion *x)
{
	unsigned long flags;
	bool ret = false;
	if (!READ_ONCE(x->done)) return false;
	raw_spin_lock_irqsave(&x->lock, flags);
	if (x->done) {
		if (x->done != UINT_MAX) x->done--;
		ret = true;
	}
	raw_spin_unlock_irqrestore(&x->lock, flags);
	return ret;
}

bool completion_done(struct completion *x) { return READ_ONCE(x->done) != 0; }

void complete(struct completion *x)
{
	unsigned long flags;
	raw_spin_lock_irqsave(&x->lock, flags);
	if (x->done != UINT_MAX) x->done++;
	if (!list_empty(&x->waiters)) {
		struct lkpi_waiter *w = list_first_entry(&x->waiters, struct lkpi_waiter, node);
		list_del_init(&w->node);
		wake_up_process(w->task);
	}
	raw_spin_unlock_irqrestore(&x->lock, flags);
}

void complete_all(struct completion *x)
{
	unsigned long flags;
	raw_spin_lock_irqsave(&x->lock, flags);
	x->done = UINT_MAX;
	while (!list_empty(&x->waiters)) {
		struct lkpi_waiter *w = list_first_entry(&x->waiters, struct lkpi_waiter, node);
		list_del_init(&w->node);
		wake_up_process(w->task);
	}
	raw_spin_unlock_irqrestore(&x->lock, flags);
}


struct rcu_sync {
	struct completion done;
	atomic_t left;
};

static void rcu_cpu_fn(void *arg)
{
	struct rcu_sync *s = arg;
	if (atomic_dec_and_test(&s->left)) complete(&s->done);
}

void synchronize_rcu(void)
{
	int n = luos_cpu_count();
	smp_mb();
	if (n <= 1) return;
	struct rcu_sync s;
	init_completion(&s.done);
	atomic_set(&s.left, n);
	for (int cpu = 0; cpu < n; cpu++) luos_run_on_cpu(cpu, rcu_cpu_fn, &s);
	wait_for_completion(&s.done);
	smp_mb();
}

void synchronize_rcu_expedited(void) { synchronize_rcu(); }

static DEFINE_RAW_SPINLOCK(rcu_cb_lock);
static struct callback_head *rcu_cb_head;
static struct callback_head **rcu_cb_tail = &rcu_cb_head;
static struct task_struct *rcu_thread;

#define RCU_KVFREE_MAX_OFFSET 4096UL

void call_rcu(struct callback_head *head, rcu_callback_t func)
{
	unsigned long flags;
	head->func = func;
	head->next = NULL;
	raw_spin_lock_irqsave(&rcu_cb_lock, flags);
	*rcu_cb_tail = head;
	rcu_cb_tail = &head->next;
	raw_spin_unlock_irqrestore(&rcu_cb_lock, flags);
	if (rcu_thread) wake_up_process(rcu_thread);
}

void kvfree_call_rcu(struct callback_head *head, void *ptr)
{
	unsigned long off;
	if (!head) {
		synchronize_rcu();
		kvfree(ptr);
		return;
	}
	off = (unsigned long)head - (unsigned long)ptr;
	if (off >= RCU_KVFREE_MAX_OFFSET) {
		synchronize_rcu();
		kvfree(ptr);
		return;
	}
	call_rcu(head, (rcu_callback_t)off);
}

static int rcu_thread_fn(void *arg)
{
	for (;;) {
		unsigned long flags;
		struct callback_head *list;

		set_current_state(TASK_UNINTERRUPTIBLE);
		raw_spin_lock_irqsave(&rcu_cb_lock, flags);
		list = rcu_cb_head;
		rcu_cb_head = NULL;
		rcu_cb_tail = &rcu_cb_head;
		raw_spin_unlock_irqrestore(&rcu_cb_lock, flags);
		if (!list) {
			schedule();
			continue;
		}
		__set_current_state(TASK_RUNNING);
		synchronize_rcu();
		while (list) {
			struct callback_head *next = list->next;
			unsigned long f = (unsigned long)list->func;
			if (f < RCU_KVFREE_MAX_OFFSET) kvfree((char *)list - f);
			else list->func(list);
			list = next;
		}
	}
	return 0;
}

struct rcu_barrier_cb {
	struct callback_head head;
	struct completion done;
};

static void rcu_barrier_func(struct callback_head *head)
{
	complete(&container_of(head, struct rcu_barrier_cb, head)->done);
}

void rcu_barrier(void)
{
	struct rcu_barrier_cb cb;
	init_completion(&cb.done);
	call_rcu(&cb.head, rcu_barrier_func);
	wait_for_completion(&cb.done);
}


int init_srcu_struct(struct srcu_struct *ssp)
{
	atomic_set(&ssp->readers[0], 0);
	atomic_set(&ssp->readers[1], 0);
	ssp->idx = 0;
	mutex_init(&ssp->lock);
	return 0;
}

void cleanup_srcu_struct(struct srcu_struct *ssp) { }

int srcu_read_lock(struct srcu_struct *ssp)
{
	int idx = READ_ONCE(ssp->idx) & 1;
	atomic_inc(&ssp->readers[idx]);
	smp_mb();
	return idx;
}

void srcu_read_unlock(struct srcu_struct *ssp, int idx)
{
	smp_mb();
	atomic_dec(&ssp->readers[idx & 1]);
}

void synchronize_srcu(struct srcu_struct *ssp)
{
	mutex_lock(&ssp->lock);
	smp_mb();
	unsigned int idx = ssp->idx & 1;
	WRITE_ONCE(ssp->idx, ssp->idx + 1);
	smp_mb();
	while (atomic_read(&ssp->readers[idx])) schedule_timeout_uninterruptible(1);
	smp_mb();
	mutex_unlock(&ssp->lock);
}

void lkpi_sync_init(void)
{
	rcu_thread = kthread_run(rcu_thread_fn, NULL, "rcu");
}
