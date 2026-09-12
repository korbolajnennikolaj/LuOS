#include "heap.h"

#include "kernel/scheduler/spinlock.h"

#include <stddef.h>

#define ALIGN 16UL
#define ALIGN_MASK (ALIGN - 1UL)
#define ALIGN_UP(x) (((x) + ALIGN_MASK) & ~ALIGN_MASK)

#define MIN_SPLIT (sizeof(block_t) + ALIGN)

typedef enum { RB_RED = 0, RB_BLACK = 1 } rb_color_t;

typedef struct rb_node {
    struct rb_node *left, *right, *parent;
    rb_color_t color;
} rb_node_t;

typedef struct rb_root {
    rb_node_t *node;
} rb_root_t;

typedef struct block {

    rb_node_t addr_rb;

    rb_node_t size_rb;

    size_t size;

    int is_free;
} block_t;

#define BLOCK_SIZE sizeof(block_t)

#define BLOCK_DATA(b) ((void*)((uintptr_t)(b) + BLOCK_SIZE))

#define DATA_BLOCK(p) ((block_t*)((uintptr_t)(p) - BLOCK_SIZE))

static rb_root_t addr_root;
static rb_root_t size_root;
static size_t heap_used = 0;
static size_t heap_total = 0;
static spinlock_t heap_lock = SPINLOCK_INIT;

size_t heap_get_used(void) { return heap_used; }
size_t heap_get_total(void) { return heap_total; }

static inline rb_node_t* rb_parent(rb_node_t *n) { return n->parent; }
static inline rb_color_t rb_color (rb_node_t *n) { return n ? n->color : RB_BLACK; }
static inline int rb_is_red(rb_node_t *n) { return n && n->color == RB_RED; }

static inline void rb_set_parent(rb_node_t *n, rb_node_t *p) { if (n) n->parent = p; }
static inline void rb_set_black (rb_node_t *n) { if (n) n->color = RB_BLACK; }
static inline void rb_set_red (rb_node_t *n) { if (n) n->color = RB_RED; }

static void rb_replace_child(rb_root_t *root, rb_node_t *p, rb_node_t *old_n, rb_node_t *new_n)
{
    if (!p)
        root->node = new_n;
    else if (p->left == old_n)
        p->left = new_n;
    else
        p->right = new_n;
    rb_set_parent(new_n, p);
}

static void rb_rotate_left(rb_root_t *root, rb_node_t *x)
{
    rb_node_t *y = x->right;
    rb_node_t *p = x->parent;

    x->right = y->left;
    rb_set_parent(y->left, x);

    y->left = x;
    x->parent = y;

    rb_replace_child(root, p, x, y);
}

static void rb_rotate_right(rb_root_t *root, rb_node_t *x)
{
    rb_node_t *y = x->left;
    rb_node_t *p = x->parent;

    x->left = y->right;
    rb_set_parent(y->right, x);

    y->right = x;
    x->parent = y;

    rb_replace_child(root, p, x, y);
}

static void rb_insert_fixup(rb_root_t *root, rb_node_t *n)
{
    while (rb_is_red(rb_parent(n))) {
        rb_node_t *p = rb_parent(n);
        rb_node_t *gp = rb_parent(p);

        if (p == gp->left) {
            rb_node_t *uncle = gp->right;
            if (rb_is_red(uncle)) {

                rb_set_black(p);
                rb_set_black(uncle);
                rb_set_red(gp);
                n = gp;
            } else {
                if (n == p->right) {

                    rb_rotate_left(root, p);
                    n = p;
                    p = rb_parent(n);
                }

                rb_set_black(p);
                rb_set_red(gp);
                rb_rotate_right(root, gp);
            }
        } else {
            rb_node_t *uncle = gp->left;
            if (rb_is_red(uncle)) {
                rb_set_black(p);
                rb_set_black(uncle);
                rb_set_red(gp);
                n = gp;
            } else {
                if (n == p->left) {
                    rb_rotate_right(root, p);
                    n = p;
                    p = rb_parent(n);
                }
                rb_set_black(p);
                rb_set_red(gp);
                rb_rotate_left(root, gp);
            }
        }
    }
    rb_set_black(root->node);
}

static void rb_delete_fixup(rb_root_t *root, rb_node_t *x, rb_node_t *x_parent)
{
    while (x != root->node && !rb_is_red(x)) {
        if (x == (x_parent ? x_parent->left : (rb_node_t*)0)) {
            rb_node_t *w = x_parent->right;
            if (rb_is_red(w)) {
                rb_set_black(w);
                rb_set_red(x_parent);
                rb_rotate_left(root, x_parent);
                w = x_parent->right;
            }
            if (!rb_is_red(w ? w->left : (rb_node_t*)0) &&
                !rb_is_red(w ? w->right : (rb_node_t*)0)) {
                rb_set_red(w);
                x = x_parent;
                x_parent = rb_parent(x);
            } else {
                if (!rb_is_red(w ? w->right : (rb_node_t*)0)) {
                    rb_set_black(w ? w->left : (rb_node_t*)0);
                    rb_set_red(w);
                    if (w) rb_rotate_right(root, w);
                    w = x_parent ? x_parent->right : (rb_node_t*)0;
                }
                if (w) w->color = x_parent ? x_parent->color : RB_BLACK;
                rb_set_black(x_parent);
                rb_set_black(w ? w->right : (rb_node_t*)0);
                rb_rotate_left(root, x_parent);
                break;
            }
        } else {
            rb_node_t *w = x_parent ? x_parent->left : (rb_node_t*)0;
            if (rb_is_red(w)) {
                rb_set_black(w);
                rb_set_red(x_parent);
                rb_rotate_right(root, x_parent);
                w = x_parent ? x_parent->left : (rb_node_t*)0;
            }
            if (!rb_is_red(w ? w->right : (rb_node_t*)0) &&
                !rb_is_red(w ? w->left : (rb_node_t*)0)) {
                rb_set_red(w);
                x = x_parent;
                x_parent = rb_parent(x);
            } else {
                if (!rb_is_red(w ? w->left : (rb_node_t*)0)) {
                    rb_set_black(w ? w->right : (rb_node_t*)0);
                    rb_set_red(w);
                    if (w) rb_rotate_left(root, w);
                    w = x_parent ? x_parent->left : (rb_node_t*)0;
                }
                if (w) w->color = x_parent ? x_parent->color : RB_BLACK;
                rb_set_black(x_parent);
                rb_set_black(w ? w->left : (rb_node_t*)0);
                rb_rotate_right(root, x_parent);
                break;
            }
        }
    }
    rb_set_black(x);
}

static void rb_erase(rb_root_t *root, rb_node_t *z)
{
    rb_node_t *x, *x_parent;
    rb_color_t removed_color;

    if (!z->left && !z->right) {

        x = NULL;
        x_parent = z->parent;
        removed_color = z->color;
        rb_replace_child(root, z->parent, z, NULL);
    } else if (!z->left || !z->right) {

        x = z->left ? z->left : z->right;
        x_parent = z->parent;
        removed_color = z->color;
        rb_replace_child(root, z->parent, z, x);
        rb_set_parent(x, z->parent);
    } else {

        rb_node_t *succ = z->right;
        while (succ->left) succ = succ->left;

        removed_color = succ->color;
        x = succ->right;
        x_parent = succ->parent;

        if (succ->parent == z) {
            x_parent = succ;
        } else {
            rb_replace_child(root, succ->parent, succ, succ->right);
            rb_set_parent(succ->right, succ->parent);
            succ->right = z->right;
            rb_set_parent(z->right, succ);
        }

        rb_replace_child(root, z->parent, z, succ);
        succ->left = z->left;
        rb_set_parent(z->left, succ);
        succ->color = z->color;
        rb_set_parent(succ, z->parent);
    }

    if (removed_color == RB_BLACK)
        rb_delete_fixup(root, x, x_parent);
}

#define ADDR_ENTRY(n) ((block_t*)((uintptr_t)(n) - __builtin_offsetof(block_t, addr_rb)))

static void addr_insert(block_t *b)
{
    rb_node_t **link = &addr_root.node;
    rb_node_t *parent = NULL;

    while (*link) {
        block_t *cur = ADDR_ENTRY(*link);
        parent = *link;
        if ((uintptr_t)b < (uintptr_t)cur)
            link = &(*link)->left;
        else
            link = &(*link)->right;
    }

    b->addr_rb.left = NULL;
    b->addr_rb.right = NULL;
    b->addr_rb.parent = parent;
    b->addr_rb.color = RB_RED;
    *link = &b->addr_rb;

    rb_insert_fixup(&addr_root, &b->addr_rb);
}

static void addr_erase(block_t *b)
{
    rb_erase(&addr_root, &b->addr_rb);
}

static block_t* addr_find_prev(uintptr_t addr)
{
    rb_node_t *n = addr_root.node;
    block_t *res = NULL;
    while (n) {
        block_t *cur = ADDR_ENTRY(n);
        if ((uintptr_t)cur < addr) {
            res = cur;
            n = n->right;
        } else {
            n = n->left;
        }
    }
    return res;
}

static block_t* addr_find_next(uintptr_t addr)
{
    rb_node_t *n = addr_root.node;
    block_t *res = NULL;
    while (n) {
        block_t *cur = ADDR_ENTRY(n);
        if ((uintptr_t)cur > addr) {
            res = cur;
            n = n->left;
        } else {
            n = n->right;
        }
    }
    return res;
}

static __attribute__((unused)) block_t* addr_find(uintptr_t addr)
{
    rb_node_t *n = addr_root.node;
    while (n) {
        block_t *cur = ADDR_ENTRY(n);
        uintptr_t ca = (uintptr_t)cur;
        if (addr == ca) return cur;
        n = (addr < ca) ? n->left : n->right;
    }
    return NULL;
}

#define SIZE_ENTRY(n) ((block_t*)((uintptr_t)(n) - __builtin_offsetof(block_t, size_rb)))

static inline int size_cmp(block_t *a, block_t *b)
{
    if (a->size != b->size)
        return (a->size < b->size) ? -1 : 1;
    if ((uintptr_t)a != (uintptr_t)b)
        return ((uintptr_t)a < (uintptr_t)b) ? -1 : 1;
    return 0;
}

static void size_insert(block_t *b)
{
    rb_node_t **link = &size_root.node;
    rb_node_t *parent = NULL;

    while (*link) {
        block_t *cur = SIZE_ENTRY(*link);
        parent = *link;
        link = (size_cmp(b, cur) < 0) ? &(*link)->left : &(*link)->right;
    }

    b->size_rb.left = NULL;
    b->size_rb.right = NULL;
    b->size_rb.parent = parent;
    b->size_rb.color = RB_RED;
    *link = &b->size_rb;

    rb_insert_fixup(&size_root, &b->size_rb);
}

static void size_erase(block_t *b)
{
    rb_erase(&size_root, &b->size_rb);
}

static block_t* size_find_best(size_t needed)
{
    rb_node_t *n = size_root.node;
    block_t *res = NULL;
    while (n) {
        block_t *cur = SIZE_ENTRY(n);
        if (cur->size >= needed) {
            res = cur;
            n = n->left;
        } else {
            n = n->right;
        }
    }
    return res;
}

static void block_split(block_t *b, size_t size)
{
    size_t remaining = b->size - size - BLOCK_SIZE;
    block_t *tail = (block_t*)((uintptr_t)b + BLOCK_SIZE + size);

    tail->size = remaining;
    tail->is_free = 1;

    b->size = size;

    addr_insert(tail);
    size_insert(tail);
}

static void block_merge_right(block_t *b, block_t *next)
{
    if (next->is_free)
        size_erase(next);
    addr_erase(next);
    b->size += BLOCK_SIZE + next->size;
}

void heap_init(uintptr_t addr, size_t size)
{
    addr_root.node = NULL;
    size_root.node = NULL;
    heap_used = 0;
    heap_total = size - BLOCK_SIZE;

    block_t *b = (block_t*)addr;
    b->size = heap_total;
    b->is_free = 1;

    addr_insert(b);
    size_insert(b);
}

static void* kmalloc_impl(size_t size)
{
    if (size == 0) return NULL;

    size = ALIGN_UP(size);

    block_t *b = size_find_best(size);
    if (!b) return NULL;

    size_erase(b);

    if (b->size >= size + BLOCK_SIZE + MIN_SPLIT)
        block_split(b, size);

    b->is_free = 0;
    heap_used += b->size;

    return BLOCK_DATA(b);
}

void* kmalloc(size_t size)
{
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    void *p = kmalloc_impl(size);
    spin_unlock_irqrestore(&heap_lock, flags);
    return p;
}

static void kfree_impl(void* ptr)
{
    if (!ptr) return;

    block_t *b = DATA_BLOCK(ptr);
    if (b->is_free) return;

    heap_used -= b->size;
    b->is_free = 1;

    uintptr_t b_end = (uintptr_t)b + BLOCK_SIZE + b->size;
    block_t *next = addr_find_next((uintptr_t)b);

    if (next && (uintptr_t)next == b_end && next->is_free)
        block_merge_right(b, next);

    block_t *prev = addr_find_prev((uintptr_t)b);
    if (prev) {
        uintptr_t prev_end = (uintptr_t)prev + BLOCK_SIZE + prev->size;
        if (prev_end == (uintptr_t)b && prev->is_free) {

            size_erase(prev);
            addr_erase(b);
            prev->size += BLOCK_SIZE + b->size;
            b = prev;
        }
    }

    size_insert(b);
}

void kfree(void* ptr)
{
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    kfree_impl(ptr);
    spin_unlock_irqrestore(&heap_lock, flags);
}

void* krealloc(void* ptr, size_t size)
{
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }

    size = ALIGN_UP(size);

    uint64_t flags = spin_lock_irqsave(&heap_lock);

    block_t *b = DATA_BLOCK(ptr);

    if (b->size >= size) {

        if (b->size >= size + BLOCK_SIZE + MIN_SPLIT) {
            size_t old_size = b->size;
            block_split(b, size);
            heap_used -= (old_size - b->size);
        }
        spin_unlock_irqrestore(&heap_lock, flags);
        return ptr;
    }

    uintptr_t b_end = (uintptr_t)b + BLOCK_SIZE + b->size;
    block_t *next = addr_find_next((uintptr_t)b);

    if (next && (uintptr_t)next == b_end && next->is_free) {
        size_t combined = b->size + BLOCK_SIZE + next->size;
        if (combined >= size) {
            size_erase(next);
            addr_erase(next);

            heap_used -= b->size;
            b->size = combined;
            if (b->size >= size + BLOCK_SIZE + MIN_SPLIT)
                block_split(b, size);
            heap_used += b->size;
            spin_unlock_irqrestore(&heap_lock, flags);
            return ptr;
        }
    }

    void *new_ptr = kmalloc_impl(size);
    if (!new_ptr) { spin_unlock_irqrestore(&heap_lock, flags); return NULL; }

    unsigned char *src = (unsigned char*)ptr;
    unsigned char *dst = (unsigned char*)new_ptr;
    size_t copy_sz = b->size;
    for (size_t i = 0; i < copy_sz; i++)
        dst[i] = src[i];

    kfree_impl(ptr);
    spin_unlock_irqrestore(&heap_lock, flags);
    return new_ptr;
}
