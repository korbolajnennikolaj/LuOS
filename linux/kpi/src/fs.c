// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"


struct lkpi_dentry {
        struct dentry d;
        refcount_t ref;
        char *name;
};

static struct dentry *dentry_alloc(struct super_block *sb, struct inode *inode, const char *name)
{
        struct lkpi_dentry *ld = kzalloc(sizeof(*ld), GFP_KERNEL);
        if (!ld) return NULL;
        ld->name = kstrdup(name ? name : "", GFP_KERNEL);
        refcount_set(&ld->ref, 1);
        ld->d.d_inode = inode;
        ld->d.d_sb = sb;
        ld->d.d_name.name = (const unsigned char *)ld->name;
        ld->d.d_name.len = ld->name ? strlen(ld->name) : 0;
        if (ld->name) strscpy((char *)ld->d.d_iname, ld->name, sizeof(ld->d.d_iname));
        return &ld->d;
}

static struct dentry *dget(struct dentry *d)
{
        if (d) refcount_inc(&container_of(d, struct lkpi_dentry, d)->ref);
        return d;
}

static void dput(struct dentry *d)
{
        if (!d) return;
        struct lkpi_dentry *ld = container_of(d, struct lkpi_dentry, d);
        if (!refcount_dec_and_test(&ld->ref)) return;
        if (d->d_sb && d->d_sb->s_d_op && d->d_sb->s_d_op->d_release) d->d_sb->s_d_op->d_release(d);
        if (d->d_inode) iput(d->d_inode);
        kfree(ld->name);
        kfree(ld);
}

char *dynamic_dname(char *buffer, int buflen, const char *fmt, ...)
{
        va_list args;
        char temp[64];
        int sz;
        va_start(args, fmt);
        sz = vsnprintf(temp, sizeof(temp), fmt, args) + 1;
        va_end(args);
        if (sz > (int)sizeof(temp) || sz > buflen) return ERR_PTR(-ENAMETOOLONG);
        buffer += buflen - sz;
        return memcpy(buffer, temp, sz);
}


static atomic_long_t next_ino = ATOMIC_LONG_INIT(1000);

static void address_space_init(struct address_space *m, struct inode *host)
{
        m->host = host;
        xa_init(&m->i_pages);
        m->gfp_mask = GFP_HIGHUSER_MOVABLE;
        m->i_mmap = RB_ROOT_CACHED;
        spin_lock_init(&m->i_private_lock);
        init_rwsem(&m->i_mmap_rwsem);
        INIT_LIST_HEAD(&m->lkpi_vmas);
        spin_lock_init(&m->lkpi_vma_lock);
}

static struct inode *inode_new(struct super_block *sb)
{
        struct inode *inode = kzalloc(sizeof(*inode), GFP_KERNEL);
        if (!inode) return NULL;
        inode->i_sb = sb;
        inode->i_ino = atomic_long_inc_return(&next_ino);
        inode->i_mode = S_IFREG | 0600;
        atomic_set(&inode->i_count, 1);
        spin_lock_init(&inode->i_lock);
        init_rwsem(&inode->i_rwsem);
        address_space_init(&inode->i_data, inode);
        inode->i_mapping = &inode->i_data;
        return inode;
}

struct inode *alloc_anon_inode(struct super_block *sb)
{
        struct inode *inode = inode_new(sb);
        return inode ? inode : ERR_PTR(-ENOMEM);
}

static void mapping_truncate(struct address_space *mapping, pgoff_t start, pgoff_t end)
{
        struct page *page;
        unsigned long index;
        xa_for_each_range(&mapping->i_pages, index, page, start, end) {
                xa_erase(&mapping->i_pages, index);
                page->mapping = NULL;
                put_page(page);
        }
}

void iput(struct inode *inode)
{
        if (!inode || !atomic_dec_and_test(&inode->i_count)) return;
        if (inode->i_sb && inode->i_sb->s_op && inode->i_sb->s_op->evict_inode) inode->i_sb->s_op->evict_inode(inode);
        mapping_truncate(&inode->i_data, 0, ULONG_MAX);
        xa_destroy(&inode->i_data.i_pages);
        kfree(inode);
}


static struct file *file_alloc(struct inode *inode, struct dentry *dentry, unsigned int flags,
                               const struct file_operations *fops)
{
        struct file *f = kzalloc(sizeof(*f), GFP_KERNEL);
        if (!f) return NULL;
        atomic_long_set(&f->f_ref, 1);
        spin_lock_init(&f->f_lock);
        mutex_init(&f->f_pos_lock);
        INIT_LIST_HEAD(&f->lkpi_node);
        f->f_flags = flags;
        switch (flags & O_ACCMODE) {
        case O_RDONLY: f->f_mode = FMODE_READ; break;
        case O_WRONLY: f->f_mode = FMODE_WRITE; break;
        default: f->f_mode = FMODE_READ | FMODE_WRITE; break;
        }
        f->f_mode |= FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE;
        f->f_op = fops;
        f->f_inode = inode;
        f->f_mapping = inode ? inode->i_mapping : NULL;
        f->f_path.dentry = dentry;
        return f;
}

static void __fput(struct file *file)
{
        if (file->f_op && file->f_op->release) file->f_op->release(file->f_inode, file);
        if (file->f_path.dentry) dput(file->f_path.dentry);
        else if (file->f_inode) iput(file->f_inode);
        kfree(file);
}

void fput(struct file *file)
{
        if (!file || !atomic_long_dec_and_test(&file->f_ref)) return;
        if (lkpi_in_irq() || preempt_count() > 0 || irqs_disabled()) {
                lkpi_file_release_deferred(file);
                return;
        }
        __fput(file);
}

struct deferred_fput {
        struct work_struct work;
        struct file *file;
};

static void deferred_fput_fn(struct work_struct *work)
{
        struct deferred_fput *d = container_of(work, struct deferred_fput, work);
        __fput(d->file);
        kfree(d);
}

void lkpi_file_release_deferred(struct file *file)
{
        struct deferred_fput *d = kmalloc(sizeof(*d), GFP_ATOMIC);
        if (!d) {
                __fput(file);
                return;
        }
        INIT_WORK(&d->work, deferred_fput_fn);
        d->file = file;
        queue_work(system_unbound_wq, &d->work);
}

struct file *fget(unsigned int fd) { return luos_fd_get_lx((int)fd); }
struct file *fget_raw(unsigned int fd) { return fget(fd); }

int get_unused_fd_flags(unsigned flags) { return luos_fd_reserve((flags & O_CLOEXEC) != 0); }
void put_unused_fd(unsigned int fd) { luos_fd_unreserve((int)fd); }
void fd_install(unsigned int fd, struct file *file) { luos_fd_install_lx((int)fd, file); }
int close_fd(unsigned int fd) { return -EINVAL; }

int receive_fd(struct file *file, int __user *ufd, unsigned int o_flags)
{
        int fd = get_unused_fd_flags(o_flags);
        if (fd < 0) return fd;
        if (ufd && put_user(fd, ufd)) {
                put_unused_fd(fd);
                return -EFAULT;
        }
        fd_install(fd, get_file(file));
        return fd;
}

static struct inode *anon_inode;

struct file *anon_inode_getfile(const char *name, const struct file_operations *fops, void *priv, int flags)
{
        ihold(anon_inode);
        struct dentry *d = dentry_alloc(NULL, anon_inode, name);
        if (!d) {
                iput(anon_inode);
                return ERR_PTR(-ENOMEM);
        }
        struct file *f = file_alloc(anon_inode, d, flags & (O_ACCMODE | O_NONBLOCK), fops);
        if (!f) {
                dput(d);
                return ERR_PTR(-ENOMEM);
        }
        f->private_data = priv;
        return f;
}

int anon_inode_getfd(const char *name, const struct file_operations *fops, void *priv, int flags)
{
        int fd = get_unused_fd_flags(flags);
        if (fd < 0) return fd;
        struct file *f = anon_inode_getfile(name, fops, priv, flags);
        if (IS_ERR(f)) {
                put_unused_fd(fd);
                return PTR_ERR(f);
        }
        fd_install(fd, f);
        return fd;
}

struct file *alloc_file_pseudo(struct inode *inode, struct vfsmount *mnt, const char *name, int flags,
                               const struct file_operations *fops)
{
        struct dentry *d = dentry_alloc(mnt ? mnt->mnt_sb : NULL, inode, name);
        if (!d) return ERR_PTR(-ENOMEM);
        struct file *f = file_alloc(inode, d, flags, fops);
        if (!f) {
                d->d_inode = NULL;
                dput(d);
                return ERR_PTR(-ENOMEM);
        }
        return f;
}

struct file *alloc_file_clone(struct file *base, int flags, const struct file_operations *fops)
{
        struct file *f = file_alloc(base->f_inode, dget(base->f_path.dentry), flags, fops);
        if (!f) {
                dput(base->f_path.dentry);
                return ERR_PTR(-ENOMEM);
        }
        if (!base->f_path.dentry && base->f_inode) ihold(base->f_inode);
        f->f_mapping = base->f_mapping;
        return f;
}

struct file *file_clone_open(struct file *file)
{
        struct file *f = alloc_file_clone(file, file->f_flags, file->f_op);
        if (IS_ERR(f)) return f;
        if (f->f_op && f->f_op->open) {
                int ret = f->f_op->open(f->f_inode, f);
                if (ret) {
                        f->f_op = NULL;
                        fput(f);
                        return ERR_PTR(ret);
                }
        }
        return f;
}

loff_t noop_llseek(struct file *file, loff_t offset, int whence) { return file->f_pos; }
loff_t no_llseek(struct file *file, loff_t offset, int whence) { return -ESPIPE; }

loff_t fixed_size_llseek(struct file *file, loff_t offset, int whence, loff_t size)
{
        loff_t pos;
        switch (whence) {
        case SEEK_SET: pos = offset; break;
        case SEEK_CUR: pos = file->f_pos + offset; break;
        case SEEK_END: pos = size + offset; break;
        default: return -EINVAL;
        }
        if (pos < 0) return -EINVAL;
        file->f_pos = pos;
        return pos;
}

loff_t default_llseek(struct file *file, loff_t offset, int whence)
{
        return fixed_size_llseek(file, offset, whence, file->f_inode ? i_size_read(file->f_inode) : 0);
}

loff_t generic_file_llseek(struct file *file, loff_t offset, int whence) { return default_llseek(file, offset, whence); }

int nonseekable_open(struct inode *inode, struct file *filp)
{
        filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);
        return 0;
}

int stream_open(struct inode *inode, struct file *filp) { return nonseekable_open(inode, filp); }

int simple_open(struct inode *inode, struct file *file)
{
        if (inode->i_private) file->private_data = inode->i_private;
        return 0;
}

ssize_t simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos, const void *from, size_t available)
{
        loff_t pos = *ppos;
        if (pos < 0) return -EINVAL;
        if (pos >= (loff_t)available || !count) return 0;
        if (count > available - pos) count = available - pos;
        if (copy_to_user(to, (const char *)from + pos, count)) return -EFAULT;
        *ppos = pos + count;
        return count;
}

ssize_t simple_write_to_buffer(void *to, size_t available, loff_t *ppos, const void __user *from, size_t count)
{
        loff_t pos = *ppos;
        if (pos < 0) return -EINVAL;
        if (pos >= (loff_t)available || !count) return 0;
        if (count > available - pos) count = available - pos;
        if (copy_from_user((char *)to + pos, from, count)) return -EFAULT;
        *ppos = pos + count;
        return count;
}

int fasync_helper(int fd, struct file *filp, int on, struct fasync_struct **fapp) { return 0; }
void kill_fasync(struct fasync_struct **fp, int sig, int band) { }


struct pseudo_fs_context *init_pseudo(struct fs_context *fc, unsigned long magic)
{
        struct pseudo_fs_context *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
        if (ctx) {
                ctx->magic = magic;
                fc->fs_private = ctx;
        }
        return ctx;
}

struct vfsmount *kern_mount(struct file_system_type *type)
{
        struct vfsmount *mnt = kzalloc(sizeof(*mnt), GFP_KERNEL);
        struct super_block *sb = kzalloc(sizeof(*sb), GFP_KERNEL);
        if (!mnt || !sb) {
                kfree(mnt);
                kfree(sb);
                return ERR_PTR(-ENOMEM);
        }
        sb->s_type = type;
        if (type->init_fs_context) {
                struct fs_context fc = { .fs_type = type };
                int ret = type->init_fs_context(&fc);
                if (ret) {
                        kfree(mnt);
                        kfree(sb);
                        return ERR_PTR(ret);
                }
                struct pseudo_fs_context *ctx = fc.fs_private;
                if (ctx) {
                        sb->s_op = ctx->ops;
                        sb->s_d_op = ctx->dops;
                        sb->s_magic = ctx->magic;
                        kfree(ctx);
                }
        }
        mnt->mnt_sb = sb;
        return mnt;
}

void kern_unmount(struct vfsmount *mnt)
{
        if (IS_ERR_OR_NULL(mnt)) return;
        if (mnt->mnt_sb && mnt->mnt_sb->s_type && mnt->mnt_sb->s_type->kill_sb) mnt->mnt_sb->s_type->kill_sb(mnt->mnt_sb);
        kfree(mnt->mnt_sb);
        kfree(mnt);
}

int simple_pin_fs(void *type, struct vfsmount **mount, int *count)
{
        if (!*mount) {
                struct vfsmount *mnt = kern_mount(type);
                if (IS_ERR(mnt)) return PTR_ERR(mnt);
                *mount = mnt;
        }
        (*count)++;
        return 0;
}

void simple_release_fs(struct vfsmount **mount, int *count)
{
        if (--(*count) == 0) {
                kern_unmount(*mount);
                *mount = NULL;
        }
}

void kill_anon_super(struct super_block *sb) { }
void kill_litter_super(struct super_block *sb) { }
int register_filesystem(struct file_system_type *fs) { return 0; }
int unregister_filesystem(struct file_system_type *fs) { return 0; }


static const struct file_operations shmem_fops = { };
static struct vfsmount shmem_mnt;

struct vfsmount *shmem_kernel_mount_lkpi(void) { return &shmem_mnt; }

struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags)
{
        struct inode *inode = inode_new(NULL);
        if (!inode) return ERR_PTR(-ENOMEM);
        inode->i_size = size;
        struct file *f = alloc_file_pseudo(inode, NULL, name, O_RDWR, &shmem_fops);
        if (IS_ERR(f)) iput(inode);
        return f;
}

struct file *shmem_file_setup_with_mnt(struct vfsmount *mnt, const char *name, loff_t size, unsigned long flags)
{
        return shmem_file_setup(name, size, flags);
}

int shmem_zero_setup(struct vm_area_struct *vma) { return -ENOSYS; }

struct folio *shmem_read_folio_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp)
{
        struct page *page, *old;
        for (;;) {
                xa_lock(&mapping->i_pages);
                page = xa_load(&mapping->i_pages, index);
                if (page) get_page(page);
                xa_unlock(&mapping->i_pages);
                if (page) return page_folio(page);

                page = alloc_page((gfp & ~__GFP_HIGHMEM) | __GFP_ZERO);
                if (!page) return ERR_PTR(-ENOMEM);
                page->mapping = mapping;
                page->index = index;
                old = xa_cmpxchg(&mapping->i_pages, index, NULL, page, GFP_KERNEL);
                if (xa_is_err(old)) {
                        __free_page(page);
                        return ERR_PTR(xa_err(old));
                }
                if (old) {
                        __free_page(page);
                        continue;
                }
                mapping->nrpages++;
                get_page(page);
                return page_folio(page);
        }
}

struct page *shmem_read_mapping_page_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp)
{
        struct folio *folio = shmem_read_folio_gfp(mapping, index, gfp);
        return IS_ERR(folio) ? ERR_CAST(folio) : &folio->page;
}

void shmem_truncate_range(struct inode *inode, loff_t lstart, loff_t lend)
{
        pgoff_t start = PAGE_ALIGN(lstart) >> PAGE_SHIFT;
        pgoff_t end = lend < 0 ? ULONG_MAX : (pgoff_t)(lend >> PAGE_SHIFT);
        mapping_truncate(inode->i_mapping, start, end);
}

void truncate_inode_pages(struct address_space *mapping, loff_t lstart)
{
        mapping_truncate(mapping, PAGE_ALIGN(lstart) >> PAGE_SHIFT, ULONG_MAX);
}

void truncate_inode_pages_final(struct address_space *mapping) { mapping_truncate(mapping, 0, ULONG_MAX); }
void invalidate_mapping_pages_lkpi(struct address_space *mapping) { }

struct folio *filemap_get_folio(struct address_space *mapping, pgoff_t index)
{
        struct page *page;
        xa_lock(&mapping->i_pages);
        page = xa_load(&mapping->i_pages, index);
        if (page) get_page(page);
        xa_unlock(&mapping->i_pages);
        return page ? page_folio(page) : ERR_PTR(-ENOENT);
}

void folio_batch_release(struct folio_batch *fbatch)
{
        for (unsigned int i = 0; i < fbatch->nr; i++) folio_put(fbatch->folios[i]);
        folio_batch_reinit(fbatch);
}


struct chrdev_region {
        struct list_head node;
        unsigned int major, baseminor, count;
        const struct file_operations *fops;
        const char *name;
};

static LIST_HEAD(chrdevs);
static DEFINE_MUTEX(chrdev_lock);
static unsigned int next_dyn_major = 234;

static int chrdev_add(unsigned int major, unsigned int baseminor, unsigned int count, const char *name,
                      const struct file_operations *fops)
{
        struct chrdev_region *r = kzalloc(sizeof(*r), GFP_KERNEL);
        if (!r) return -ENOMEM;
        mutex_lock(&chrdev_lock);
        if (!major) major = next_dyn_major--;
        r->major = major;
        r->baseminor = baseminor;
        r->count = count;
        r->fops = fops;
        r->name = name;
        list_add(&r->node, &chrdevs);
        mutex_unlock(&chrdev_lock);
        return major;
}

static void chrdev_del(unsigned int major, unsigned int baseminor, unsigned int count)
{
        struct chrdev_region *r, *n;
        mutex_lock(&chrdev_lock);
        list_for_each_entry_safe(r, n, &chrdevs, node) {
                if (r->major == major && r->baseminor == baseminor && r->count == count) {
                        list_del(&r->node);
                        kfree(r);
                }
        }
        mutex_unlock(&chrdev_lock);
}

int __register_chrdev(unsigned int major, unsigned int baseminor, unsigned int count, const char *name,
                      const struct file_operations *fops)
{
        int ret = chrdev_add(major, baseminor, count, name, fops);
        if (ret < 0) return ret;
        return major ? 0 : ret;
}

void __unregister_chrdev(unsigned int major, unsigned int baseminor, unsigned int count, const char *name)
{
        chrdev_del(major, baseminor, count);
}

int register_chrdev(unsigned int major, const char *name, const struct file_operations *fops)
{
        return __register_chrdev(major, 0, 1U << MINORBITS, name, fops);
}

void unregister_chrdev(unsigned int major, const char *name) { __unregister_chrdev(major, 0, 1U << MINORBITS, name); }

int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count, const char *name)
{
        mutex_lock(&chrdev_lock);
        unsigned int major = next_dyn_major--;
        mutex_unlock(&chrdev_lock);
        *dev = MKDEV(major, baseminor);
        return 0;
}

int register_chrdev_region(dev_t from, unsigned count, const char *name) { return 0; }
void unregister_chrdev_region(dev_t from, unsigned count) { }

void cdev_init(struct cdev *cdev, const struct file_operations *fops)
{
        memset(cdev, 0, sizeof(*cdev));
        INIT_LIST_HEAD(&cdev->list);
        cdev->ops = fops;
}

int cdev_add(struct cdev *p, dev_t dev, unsigned count)
{
        p->dev = dev;
        p->count = count;
        int ret = chrdev_add(MAJOR(dev), MINOR(dev), count, "cdev", p->ops);
        return ret < 0 ? ret : 0;
}

void cdev_del(struct cdev *p) { chrdev_del(MAJOR(p->dev), MINOR(p->dev), p->count); }

static const struct file_operations *chrdev_lookup(unsigned int major, unsigned int minor)
{
        struct chrdev_region *r;
        const struct file_operations *fops = NULL;
        mutex_lock(&chrdev_lock);
        list_for_each_entry(r, &chrdevs, node) {
                if (r->major == major && minor >= r->baseminor && minor - r->baseminor < r->count) {
                        fops = r->fops;
                        break;
                }
        }
        mutex_unlock(&chrdev_lock);
        return fops;
}


static void seq_reserve(struct seq_file *m, size_t extra)
{
        if (m->count + extra + 1 <= m->size) return;
        size_t ns = max_t(size_t, m->size * 2, m->count + extra + 1);
        ns = max_t(size_t, ns, 4096);
        char *nb = kvmalloc(ns, GFP_KERNEL);
        if (!nb) return;
        if (m->buf) {
                memcpy(nb, m->buf, m->count);
                kvfree(m->buf);
        }
        m->buf = nb;
        m->size = ns;
}

void seq_vprintf(struct seq_file *m, const char *fmt, va_list args)
{
        va_list copy;
        va_copy(copy, args);
        int len = vsnprintf(NULL, 0, fmt, copy);
        va_end(copy);
        if (len < 0) return;
        seq_reserve(m, len);
        if (m->count + len + 1 > m->size) return;
        vsnprintf(m->buf + m->count, m->size - m->count, fmt, args);
        m->count += len;
}

void seq_printf(struct seq_file *m, const char *fmt, ...)
{
        va_list args;
        va_start(args, fmt);
        seq_vprintf(m, fmt, args);
        va_end(args);
}

void seq_write(struct seq_file *m, const void *data, size_t len)
{
        seq_reserve(m, len);
        if (m->count + len + 1 > m->size) return;
        memcpy(m->buf + m->count, data, len);
        m->count += len;
}

void seq_puts(struct seq_file *m, const char *s) { seq_write(m, s, strlen(s)); }
void seq_putc(struct seq_file *m, char c) { seq_write(m, &c, 1); }

int seq_open(struct file *file, const struct seq_operations *op)
{
        struct seq_file *m = kzalloc(sizeof(*m), GFP_KERNEL);
        if (!m) return -ENOMEM;
        mutex_init(&m->lock);
        m->op = op;
        m->file = file;
        file->private_data = m;
        return 0;
}

static void seq_fill(struct seq_file *m)
{
        loff_t pos = 0;
        void *p = m->op->start(m, &pos);
        while (p && !IS_ERR(p)) {
                if (m->op->show(m, p) < 0) break;
                p = m->op->next(m, p, &pos);
        }
        m->op->stop(m, p);
}

ssize_t seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos)
{
        struct seq_file *m = file->private_data;
        ssize_t ret;
        mutex_lock(&m->lock);
        if (*ppos == 0) {
                m->count = 0;
                seq_fill(m);
        }
        ret = simple_read_from_buffer(buf, size, ppos, m->buf, m->count);
        mutex_unlock(&m->lock);
        return ret;
}

loff_t seq_lseek(struct file *file, loff_t offset, int whence)
{
        if (whence != SEEK_SET || offset != 0) return -EINVAL;
        file->f_pos = 0;
        return 0;
}

int seq_release(struct inode *inode, struct file *file)
{
        struct seq_file *m = file->private_data;
        if (m) {
                kvfree(m->buf);
                kfree(m);
        }
        return 0;
}

struct single_ops {
        struct seq_operations ops;
        int (*show)(struct seq_file *, void *);
};

static void *single_start(struct seq_file *m, loff_t *pos) { return *pos ? NULL : SEQ_START_TOKEN; }
static void *single_next(struct seq_file *m, void *v, loff_t *pos) { ++*pos; return NULL; }
static void single_stop(struct seq_file *m, void *v) { }

static int single_show(struct seq_file *m, void *v)
{
        struct single_ops *so = container_of(m->op, struct single_ops, ops);
        return so->show(m, m->private);
}

int single_open(struct file *file, int (*show)(struct seq_file *, void *), void *data)
{
        struct single_ops *so = kzalloc(sizeof(*so), GFP_KERNEL);
        if (!so) return -ENOMEM;
        so->ops.start = single_start;
        so->ops.next = single_next;
        so->ops.stop = single_stop;
        so->ops.show = single_show;
        so->show = show;
        int ret = seq_open(file, &so->ops);
        if (ret) {
                kfree(so);
                return ret;
        }
        ((struct seq_file *)file->private_data)->private = data;
        return 0;
}

int single_release(struct inode *inode, struct file *file)
{
        struct seq_file *m = file->private_data;
        const struct seq_operations *op = m ? m->op : NULL;
        seq_release(inode, file);
        kfree(op ? container_of(op, struct single_ops, ops) : NULL);
        return 0;
}


struct eventfd_ctx *eventfd_ctx_fdget(int fd) { return ERR_PTR(-EBADF); }
void eventfd_ctx_put(struct eventfd_ctx *ctx) { }
void eventfd_signal(struct eventfd_ctx *ctx) { }


static void vma_link(struct vm_area_struct *vma)
{
        struct address_space *m = vma->vm_file ? vma->vm_file->f_mapping : NULL;
        INIT_LIST_HEAD(&vma->lkpi_node);
        if (!m || !m->lkpi_vmas.next) return;
        spin_lock(&m->lkpi_vma_lock);
        list_add(&vma->lkpi_node, &m->lkpi_vmas);
        spin_unlock(&m->lkpi_vma_lock);
}

static void vma_unlink(struct vm_area_struct *vma)
{
        struct address_space *m = vma->vm_file ? vma->vm_file->f_mapping : NULL;
        if (!m || list_empty(&vma->lkpi_node)) return;
        spin_lock(&m->lkpi_vma_lock);
        list_del_init(&vma->lkpi_node);
        spin_unlock(&m->lkpi_vma_lock);
}

static int vma_fault_one(struct vm_area_struct *vma, unsigned long addr, int write)
{
        struct vm_fault vmf = {
                .vma = vma,
                .gfp_mask = GFP_KERNEL,
                .address = addr & PAGE_MASK,
                .real_address = addr,
                .pgoff = vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT),
                .flags = FAULT_FLAG_USER | (write ? FAULT_FLAG_WRITE : 0),
        };
        vm_fault_t ret = vma->vm_ops->fault(&vmf);
        if (ret & VM_FAULT_ERROR) return -1;
        if (ret & VM_FAULT_NOPAGE) return 0;
        if (vmf.page) {
                vm_fault_t r = vmf_insert_page(vma, vmf.address, vmf.page);
                put_page(vmf.page);
                return r == VM_FAULT_NOPAGE ? 0 : -1;
        }
        return 0;
}

#define LKPI_FAULT_AROUND 32

static void lkpi_mm_ref(struct mm_struct *mm);
static void lkpi_mm_put(struct mm_struct *mm);

int lkpi_vma_fault(void *ctx, void *proc, luos_u64 addr, int write)
{
        struct vm_area_struct *vma = ctx;
        if (!vma->vm_ops || !vma->vm_ops->fault) return -1;
        if (write && !(vma->vm_flags & VM_WRITE)) return -1;
        if (vma_fault_one(vma, addr, write)) return -1;
        if (!(vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP))) return 0;
        for (int i = 1; i < LKPI_FAULT_AROUND; i++) {
                unsigned long a = (addr & PAGE_MASK) + ((unsigned long)i << PAGE_SHIFT);
                if (a >= vma->vm_end || luos_user_is_mapped(proc, a)) break;
                if (vma_fault_one(vma, a, write)) break;
        }
        return 0;
}

void *lkpi_vma_split(void *ctx, luos_u64 old_start, luos_u64 new_start, luos_u64 new_end)
{
        struct vm_area_struct *vma = ctx;
        struct vm_area_struct *n = kmemdup(vma, sizeof(*vma), GFP_KERNEL);
        if (!n) return NULL;
        n->vm_start = new_start;
        n->vm_end = new_end;
        n->vm_pgoff = vma->vm_pgoff + ((new_start - vma->vm_start) >> PAGE_SHIFT);
        vma->vm_end = new_start;
        if (n->vm_file) get_file(n->vm_file);
        lkpi_mm_ref(n->vm_mm);
        vma_link(n);
        if (n->vm_ops && n->vm_ops->open) n->vm_ops->open(n);
        return n;
}

void lkpi_vma_close(void *ctx)
{
        struct vm_area_struct *vma = ctx;
        if (!vma) return;
        if (vma->vm_ops && vma->vm_ops->close) vma->vm_ops->close(vma);
        vma_unlink(vma);
        if (vma->vm_file) fput(vma->vm_file);
        lkpi_mm_put(vma->vm_mm);
        kfree(vma);
}


#define LX_PROT_READ 1
#define LX_PROT_WRITE 2
#define LX_PROT_EXEC 4
#define LX_MAP_SHARED 1

static long lx_errno(long r)
{
        if (r == -ERESTARTSYS || r == -ERESTARTNOINTR || r == -ERESTARTNOHAND || r == -ERESTART_RESTARTBLOCK) return -EINTR;
        if (r == -ENOTSUPP) return -EOPNOTSUPP;
        return r;
}

int lkpi_chrdev_open(unsigned int major, unsigned int minor, unsigned int flags, void **lx_file)
{
        const struct file_operations *fops = chrdev_lookup(major, minor);
        if (!fops) return -ENXIO;
        struct inode *inode = inode_new(NULL);
        if (!inode) return -ENOMEM;
        inode->i_mode = S_IFCHR | 0666;
        inode->i_rdev = MKDEV(major, minor);
        struct file *f = file_alloc(inode, NULL, flags, fops);
        if (!f) {
                iput(inode);
                return -ENOMEM;
        }
        if (fops->open) {
                int ret = fops->open(inode, f);
                if (ret) {
                        f->f_op = NULL;
                        fput(f);
                        return lx_errno(ret);
                }
        }
        *lx_file = f;
        return 0;
}

long lkpi_file_read(void *lx, u64 ubuf, u64 n)
{
        struct file *f = lx;
        if (!f->f_op || !f->f_op->read) return -EINVAL;
        return lx_errno(f->f_op->read(f, (char __user *)ubuf, n, &f->f_pos));
}

long lkpi_file_write(void *lx, u64 ubuf, u64 n)
{
        struct file *f = lx;
        if (!f->f_op || !f->f_op->write) return -EINVAL;
        return lx_errno(f->f_op->write(f, (const char __user *)ubuf, n, &f->f_pos));
}

long drm_legacy_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

long lkpi_file_ioctl(void *lx, unsigned int cmd, unsigned long arg)
{
        struct file *f = lx;
        long ret;
        if (!f->f_op || !f->f_op->unlocked_ioctl) return -ENOTTY;
        ret = drm_legacy_ioctl(f, cmd, arg);
        if (ret != -ENOENT) return ret;
        u64 t0 = ktime_get();
        ret = lx_errno(f->f_op->unlocked_ioctl(f, cmd, arg));
        u64 dt = ktime_get() - t0;
        if (dt >= 10 * NSEC_PER_MSEC && ((cmd >> 8) & 0xff) == 'd'  )
                printk(KERN_DEBUG "slow drm ioctl %#x took %llu ms (ret %ld)\n", cmd, dt / NSEC_PER_MSEC, ret);
        return ret;
}

int lkpi_file_poll(void *lx)
{
        struct file *f = lx;
        if (!f->f_op || !f->f_op->poll) return EPOLLIN | EPOLLOUT | EPOLLRDNORM | EPOLLWRNORM;
        return (int)f->f_op->poll(f, NULL);
}

long lkpi_file_llseek(void *lx, s64 off, int whence)
{
        struct file *f = lx;
        if (!f->f_op || !f->f_op->llseek) return -ESPIPE;
        return lx_errno(f->f_op->llseek(f, off, whence));
}

struct lkpi_mm {
        struct list_head node;
        struct mm_struct mm;
        int vmas;
};
static LIST_HEAD(lkpi_mms);
static DEFINE_SPINLOCK(lkpi_mm_lock);

static struct mm_struct *lkpi_mm_get(void *proc)
{
        struct lkpi_mm *m, *fresh = kzalloc(sizeof(*fresh), GFP_KERNEL);
        spin_lock(&lkpi_mm_lock);
        list_for_each_entry(m, &lkpi_mms, node) {
                if (m->mm.luos_mm != proc) continue;
                m->vmas++;
                spin_unlock(&lkpi_mm_lock);
                kfree(fresh);
                return &m->mm;
        }
        if (fresh) {
                init_rwsem(&fresh->mm.mmap_lock);
                atomic_set(&fresh->mm.mm_users, 1);
                atomic_set(&fresh->mm.mm_count, 1);
                fresh->mm.luos_mm = proc;
                fresh->vmas = 1;
                list_add(&fresh->node, &lkpi_mms);
        }
        spin_unlock(&lkpi_mm_lock);
        return fresh ? &fresh->mm : NULL;
}

static void lkpi_mm_ref(struct mm_struct *mm)
{
        if (!mm) return;
        spin_lock(&lkpi_mm_lock);
        container_of(mm, struct lkpi_mm, mm)->vmas++;
        spin_unlock(&lkpi_mm_lock);
}

static void lkpi_mm_put(struct mm_struct *mm)
{
        struct lkpi_mm *m;
        if (!mm) return;
        m = container_of(mm, struct lkpi_mm, mm);
        spin_lock(&lkpi_mm_lock);
        if (--m->vmas) m = NULL;
        else list_del(&m->node);
        spin_unlock(&lkpi_mm_lock);
        kfree(m);
}

long lkpi_file_mmap(void *lx, void *proc, u64 start, u64 len, u32 prot, u32 flags, u64 pgoff, void **ctx)
{
        struct file *f = lx;
        if (!f->f_op || !f->f_op->mmap) return -ENODEV;
        struct vm_area_struct *vma = kzalloc(sizeof(*vma), GFP_KERNEL);
        if (!vma) return -ENOMEM;
        vm_flags_t vf = VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;
        if (prot & LX_PROT_READ) vf |= VM_READ;
        if (prot & LX_PROT_WRITE) vf |= VM_WRITE;
        if (prot & LX_PROT_EXEC) vf |= VM_EXEC;
        if ((flags & 0xf) == LX_MAP_SHARED) vf |= VM_SHARED | VM_MAYSHARE;
        vma->vm_start = start;
        vma->vm_end = start + len;
        vma->vm_pgoff = pgoff;
        vm_flags_init(vma, vf);
        vma->vm_page_prot = vm_get_page_prot(vf);
        vma->vm_file = get_file(f);
        vma->luos_proc = proc;
        vma->vm_mm = lkpi_mm_get(proc);
        INIT_LIST_HEAD(&vma->lkpi_node);
        int ret = f->f_op->mmap(f, vma);
        if (ret) {
                if (vma->vm_file) fput(vma->vm_file);
                lkpi_mm_put(vma->vm_mm);
                kfree(vma);
                return lx_errno(ret);
        }
        vma_link(vma);
        *ctx = vma;
        return 0;
}

void lkpi_file_put(void *lx) { fput(lx); }
void lkpi_file_get(void *lx) { get_file((struct file *)lx); }

int lkpi_file_info(void *lx, u64 *rdev, u32 *mode, u64 *size)
{
        struct file *f = lx;
        struct inode *inode = f->f_inode;
        if (!inode) return -EINVAL;
        *rdev = huge_encode_dev(inode->i_rdev);
        *mode = inode->i_mode;
        *size = i_size_read(inode);
        return 0;
}

void lkpi_fs_init(void)
{
        anon_inode = inode_new(NULL);
        anon_inode->i_mode = S_IRUSR | S_IWUSR;
        luos_mm_set_device_hooks(lkpi_vma_fault, lkpi_vma_split, lkpi_vma_close);
}
