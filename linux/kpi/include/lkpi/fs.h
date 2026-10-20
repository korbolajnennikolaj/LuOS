// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_FS_H
#define _LKPI_FS_H

#include <lkpi/mm.h>

struct inode;
struct file;
struct dentry;
struct vfsmount;
struct super_block;
struct poll_table_struct;
struct seq_file;
struct kiocb;
struct iov_iter;
struct dir_context;
struct pipe_inode_info;
struct file_lock;
struct fasync_struct;
struct module;

typedef struct { uid_t val; } kuid_t;
typedef struct { gid_t val; } kgid_t;
#define GLOBAL_ROOT_UID ((kuid_t){ 0 })
#define GLOBAL_ROOT_GID ((kgid_t){ 0 })
#define current_uid() GLOBAL_ROOT_UID
#define current_euid() GLOBAL_ROOT_UID
#define from_kuid(ns, uid) ((uid).val)
#define from_kuid_munged(ns, uid) ((uid).val)
#define uid_eq(a, b) ((a).val == (b).val)

#define FMODE_READ ((__force fmode_t)(1 << 0))
#define FMODE_WRITE ((__force fmode_t)(1 << 1))
#define FMODE_LSEEK ((__force fmode_t)(1 << 2))
#define FMODE_PREAD ((__force fmode_t)(1 << 3))
#define FMODE_PWRITE ((__force fmode_t)(1 << 4))
#define FMODE_EXEC ((__force fmode_t)(1 << 5))
#define FMODE_NOWAIT ((__force fmode_t)(1 << 27))
#define FMODE_UNSIGNED_OFFSET ((__force fmode_t)(1 << 13))
#define FMODE_ATOMIC_POS ((__force fmode_t)(1 << 15))

#define MAY_EXEC 0x00000001
#define MAY_WRITE 0x00000002
#define MAY_READ 0x00000004

#include <uapi/linux/stat.h>
#include <uapi/linux/fcntl.h>
#include <uapi/linux/fs.h>
#define S_IRWXUGO (S_IRWXU | S_IRWXG | S_IRWXO)
#define S_IALLUGO (S_ISUID | S_ISGID | S_ISVTX | S_IRWXUGO)
#define S_IRUGO (S_IRUSR | S_IRGRP | S_IROTH)
#define S_IWUGO (S_IWUSR | S_IWGRP | S_IWOTH)
#define S_IXUGO (S_IXUSR | S_IXGRP | S_IXOTH)
#define VERIFY_OCTAL_PERMISSIONS(perms) (perms)

#include <linux/kdev_t.h>

struct address_space_operations;
struct address_space {
	struct inode *host;
	struct xarray i_pages;
	gfp_t gfp_mask;
	unsigned long flags;
	atomic_t i_mmap_writable;
	struct rb_root_cached i_mmap;
	unsigned long nrpages;
	pgoff_t writeback_index;
	const struct address_space_operations *a_ops;
	spinlock_t i_private_lock;
	struct rw_semaphore i_mmap_rwsem;
	void *private_data;
	struct list_head lkpi_vmas;
	spinlock_t lkpi_vma_lock;
};
#define AS_UNEVICTABLE 3
static inline void mapping_set_unevictable(struct address_space *mapping) { set_bit(AS_UNEVICTABLE, &mapping->flags); }
static inline void mapping_clear_unevictable(struct address_space *mapping) { clear_bit(AS_UNEVICTABLE, &mapping->flags); }
static inline bool mapping_unevictable(struct address_space *mapping) { return test_bit(AS_UNEVICTABLE, &mapping->flags); }
static inline gfp_t mapping_gfp_mask(struct address_space *mapping) { return mapping->gfp_mask; }
static inline void mapping_set_gfp_mask(struct address_space *m, gfp_t mask) { m->gfp_mask = mask; }
static inline gfp_t mapping_gfp_constraint(struct address_space *mapping, gfp_t gfp_mask) { return mapping_gfp_mask(mapping) & gfp_mask; }
#define mapping_set_large_folios(m) do { } while (0)
#define mapping_set_folio_order_range(m, min, max) do { } while (0)
#define mapping_large_folio_support(m) false
#define check_move_unevictable_folios(fbatch) do { } while (0)
#define check_move_unevictable_pages(pvec) do { } while (0)
struct folio *shmem_read_folio_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp);
struct page *shmem_read_mapping_page_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp);
static inline struct page *shmem_read_mapping_page(struct address_space *mapping, pgoff_t index)
{
	return shmem_read_mapping_page_gfp(mapping, index, mapping_gfp_mask(mapping));
}
static inline struct folio *shmem_read_folio(struct address_space *mapping, pgoff_t index)
{
	return shmem_read_folio_gfp(mapping, index, mapping_gfp_mask(mapping));
}
void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end);
struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags);
struct file *shmem_file_setup_with_mnt(struct vfsmount *mnt, const char *name, loff_t size, unsigned long flags);
struct vfsmount *shmem_kernel_mount_lkpi(void);
int shmem_zero_setup(struct vm_area_struct *vma);
#define shmem_mapping(m) true
#define VM_NORESERVE_SHMEM 0
void truncate_inode_pages(struct address_space *mapping, loff_t lstart);
void truncate_inode_pages_final(struct address_space *mapping);
void invalidate_mapping_pages_lkpi(struct address_space *mapping);
#define invalidate_mapping_pages(mapping, start, end) ({ invalidate_mapping_pages_lkpi(mapping); 0UL; })
struct folio *filemap_get_folio(struct address_space *mapping, pgoff_t index);
#define filemap_release_folio(f, gfp) true
#define folio_batch_lkpi_dummy 0

#define PAGEVEC_SIZE 31
struct folio_batch {
	unsigned char nr;
	unsigned char i;
	bool percpu_pvec_drained;
	struct folio *folios[PAGEVEC_SIZE];
};
static inline void folio_batch_init(struct folio_batch *fbatch) { fbatch->nr = 0; fbatch->i = 0; fbatch->percpu_pvec_drained = false; }
static inline void folio_batch_reinit(struct folio_batch *fbatch) { fbatch->nr = 0; fbatch->i = 0; }
static inline unsigned int folio_batch_count(struct folio_batch *fbatch) { return fbatch->nr; }
static inline unsigned int folio_batch_space(struct folio_batch *fbatch) { return PAGEVEC_SIZE - fbatch->nr; }
static inline unsigned folio_batch_add(struct folio_batch *fbatch, struct folio *folio)
{
	fbatch->folios[fbatch->nr++] = folio;
	return folio_batch_space(fbatch);
}
static inline struct folio *folio_batch_next(struct folio_batch *fbatch)
{
	if (fbatch->i == fbatch->nr) return NULL;
	return fbatch->folios[fbatch->i++];
}
void folio_batch_release(struct folio_batch *fbatch);
#define __folio_batch_release folio_batch_release

struct inode_operations;
struct file_operations;
struct inode {
	umode_t i_mode;
	unsigned int i_flags;
	unsigned long i_ino;
	dev_t i_rdev;
	loff_t i_size;
	struct super_block *i_sb;
	struct address_space *i_mapping;
	struct address_space i_data;
	const struct inode_operations *i_op;
	const struct file_operations *i_fop;
	void *i_private;
	atomic_t i_count;
	spinlock_t i_lock;
	struct rw_semaphore i_rwsem;
	struct timespec64 __i_atime, __i_mtime, __i_ctime;
	u32 i_generation;
	kuid_t i_uid;
	kgid_t i_gid;
};
static inline unsigned iminor(const struct inode *inode) { return MINOR(inode->i_rdev); }
static inline unsigned imajor(const struct inode *inode) { return MAJOR(inode->i_rdev); }
static inline loff_t i_size_read(const struct inode *inode) { return READ_ONCE(inode->i_size); }
static inline void i_size_write(struct inode *inode, loff_t i_size) { WRITE_ONCE(inode->i_size, i_size); }
#define inode_lock(i) down_write(&(i)->i_rwsem)
#define inode_unlock(i) up_write(&(i)->i_rwsem)
#define inode_lock_shared(i) down_read(&(i)->i_rwsem)
#define inode_unlock_shared(i) up_read(&(i)->i_rwsem)
#define ihold(i) atomic_inc(&(i)->i_count)
void iput(struct inode *inode);
struct inode *alloc_anon_inode(struct super_block *sb);
#define inode_init_once(i) do { } while (0)
static inline void inode_set_bytes(struct inode *inode, loff_t bytes) { }
#define simple_inode_init_ts(i) do { } while (0)
#define inode_set_atime_to_ts(i, t) (t)
#define inode_set_mtime_to_ts(i, t) (t)
#define inode_set_ctime_to_ts(i, t) (t)
#define inode_set_ctime_current(i) ((struct timespec64){0, 0})
#define current_time(i) ((struct timespec64){0, 0})
#define file_inode(f) ((f)->f_inode)
#define file_dentry(f) ((f)->f_path.dentry)
struct qstr { u32 hash; u32 len; const unsigned char *name; };
#define QSTR_INIT(n, l) { .len = l, .name = n }
#define QSTR(n) (struct qstr)QSTR_INIT(n, strlen(n))

struct dentry {
	struct inode *d_inode;
	struct qstr d_name;
	unsigned char d_iname[32];
	struct super_block *d_sb;
	void *d_fsdata;
};
#define d_inode(d) ((d)->d_inode)
struct path {
	struct vfsmount *mnt;
	struct dentry *dentry;
};
struct vfsmount { struct dentry *mnt_root; struct super_block *mnt_sb; int mnt_flags; };
struct super_operations {
	struct inode *(*alloc_inode)(struct super_block *sb);
	void (*destroy_inode)(struct inode *);
	void (*free_inode)(struct inode *);
	int (*statfs)(struct dentry *, void *);
	void (*evict_inode)(struct inode *);
};
struct dentry_operations {
	int (*d_revalidate)(struct inode *, const struct qstr *, struct dentry *, unsigned int);
	int (*d_delete)(const struct dentry *);
	void (*d_release)(struct dentry *);
	void (*d_prune)(struct dentry *);
	char *(*d_dname)(struct dentry *, char *, int);
};
struct super_block {
	unsigned long s_magic;
	const struct super_operations *s_op;
	const struct dentry_operations *s_d_op;
	void *s_fs_info;
	struct dentry *s_root;
	struct file_system_type *s_type;
};
struct fs_context;
struct fs_context_operations {
	void (*free)(struct fs_context *fc);
	int (*get_tree)(struct fs_context *fc);
	int (*reconfigure)(struct fs_context *fc);
};
struct fs_context {
	const struct fs_context_operations *ops;
	struct file_system_type *fs_type;
	void *fs_private;
	void *sget_key;
	struct dentry *root;
	unsigned int sb_flags;
};
struct pseudo_fs_context {
	const struct super_operations *ops;
	const void *const *xattr;
	const struct dentry_operations *dops;
	unsigned long magic;
};
struct pseudo_fs_context *init_pseudo(struct fs_context *fc, unsigned long magic);
struct file_system_type {
	const char *name;
	int fs_flags;
	int (*init_fs_context)(struct fs_context *);
	const void *parameters;
	struct dentry *(*mount)(struct file_system_type *, int, const char *, void *);
	void (*kill_sb)(struct super_block *);
	struct module *owner;
	struct file_system_type *next;
};
#define FS_REQUIRES_DEV 1
#define FS_USERNS_MOUNT 8
void kill_anon_super(struct super_block *sb);
void kill_litter_super(struct super_block *sb);
struct vfsmount *kern_mount(struct file_system_type *type);
void kern_unmount(struct vfsmount *mnt);
int register_filesystem(struct file_system_type *fs);
int unregister_filesystem(struct file_system_type *fs);
__printf(3, 4) char *dynamic_dname(char *buffer, int buflen, const char *fmt, ...);
#define d_set_d_op(d, op) do { } while (0)
#define compat_ptr_ioctl NULL
#define compat_ptr(x) ((void __user *)(unsigned long)(x))
#define ptr_to_compat(p) ((u32)(unsigned long)(p))

typedef int (*filldir_t)(struct dir_context *, const char *, int, loff_t, u64, unsigned);
typedef void *fl_owner_t;

struct file_operations {
	struct module *owner;
	unsigned int fop_flags;
	loff_t (*llseek)(struct file *, loff_t, int);
	ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
	ssize_t (*read_iter)(struct kiocb *, struct iov_iter *);
	ssize_t (*write_iter)(struct kiocb *, struct iov_iter *);
	int (*iopoll)(struct kiocb *kiocb, void *, unsigned int flags);
	int (*iterate_shared)(struct file *, struct dir_context *);
	__poll_t (*poll)(struct file *, struct poll_table_struct *);
	long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
	long (*compat_ioctl)(struct file *, unsigned int, unsigned long);
	int (*mmap)(struct file *, struct vm_area_struct *);
	int (*open)(struct inode *, struct file *);
	int (*flush)(struct file *, fl_owner_t id);
	int (*release)(struct inode *, struct file *);
	int (*fsync)(struct file *, loff_t, loff_t, int datasync);
	int (*fasync)(int, struct file *, int);
	int (*lock)(struct file *, int, struct file_lock *);
	unsigned long (*get_unmapped_area)(struct file *, unsigned long, unsigned long, unsigned long, unsigned long);
	int (*check_flags)(int);
	int (*flock)(struct file *, int, struct file_lock *);
	ssize_t (*splice_write)(struct pipe_inode_info *, struct file *, loff_t *, size_t, unsigned int);
	ssize_t (*splice_read)(struct file *, loff_t *, struct pipe_inode_info *, size_t, unsigned int);
	void (*splice_eof)(struct file *file);
	int (*setlease)(struct file *, int, struct file_lock **, void **);
	long (*fallocate)(struct file *file, int mode, loff_t offset, loff_t len);
	void (*show_fdinfo)(struct seq_file *m, struct file *f);
	ssize_t (*copy_file_range)(struct file *, loff_t, struct file *, loff_t, size_t, unsigned int);
	int (*mmap_prepare)(void *desc);
};
#define FOP_UNSIGNED_OFFSET ((unsigned int)(1 << 5))
#define FOP_BUFFER_RASYNC 0
#define FOP_BUFFER_WASYNC 0
#define FOP_MMAP_SYNC 0
#define FOP_DIO_PARALLEL_WRITE 0
#define FOP_HUGE_PAGES 0
#define FOP_ASYNC_LOCK 0
#define FOP_DONTCACHE 0

struct file {
	atomic_long_t f_ref;
	spinlock_t f_lock;
	fmode_t f_mode;
	const struct file_operations *f_op;
	struct address_space *f_mapping;
	void *private_data;
	struct inode *f_inode;
	unsigned int f_flags;
	struct path f_path;
	loff_t f_pos;
	struct mutex f_pos_lock;
	u64 f_version;
	void *f_security;
	struct fown_struct_lkpi { int dummy; } f_owner;
	struct list_head lkpi_node;
	void *luos_file;
};
#define get_file(f) ({ atomic_long_inc(&(f)->f_ref); (f); })
static inline bool file_ref_get(atomic_long_t *ref) { return atomic_long_inc_not_zero(ref); }
#define get_file_rcu(fpp) get_file(*(fpp))
#define file_count(f) atomic_long_read(&(f)->f_ref)
void fput(struct file *file);
struct file *fget(unsigned int fd);
struct file *fget_raw(unsigned int fd);
int get_unused_fd_flags(unsigned flags);
void put_unused_fd(unsigned int fd);
void fd_install(unsigned int fd, struct file *file);
int close_fd(unsigned int fd);
struct fd { unsigned long word; };
#define fd_file(f) ((struct file *)((f).word & ~3UL))
#define fd_empty(f) unlikely(!(f).word)
static inline struct fd fdget(unsigned int fd) { struct fd f = { (unsigned long)fget(fd) }; return f; }
static inline void fdput(struct fd fd) { if (fd_file(fd)) fput(fd_file(fd)); }
DEFINE_CLASS(fd, struct fd, fdput(_T), fdget(fd), int fd)
int receive_fd(struct file *file, int __user *ufd, unsigned int o_flags);
#define FD_ADD(flags, getfile) ({ \
	int __fd = get_unused_fd_flags(flags); struct file *__f; \
	if (__fd >= 0) { __f = (getfile); if (IS_ERR(__f)) { put_unused_fd(__fd); __fd = PTR_ERR(__f); } else fd_install(__fd, __f); } \
	__fd; })
struct file *anon_inode_getfile(const char *name, const struct file_operations *fops, void *priv, int flags);
int anon_inode_getfd(const char *name, const struct file_operations *fops, void *priv, int flags);
struct file *alloc_file_pseudo(struct inode *inode, struct vfsmount *mnt, const char *name, int flags,
			       const struct file_operations *fops);
struct file *alloc_file_clone(struct file *base, int flags, const struct file_operations *fops);
loff_t noop_llseek(struct file *file, loff_t offset, int whence);
loff_t no_llseek(struct file *file, loff_t offset, int whence);
loff_t default_llseek(struct file *file, loff_t offset, int whence);
loff_t generic_file_llseek(struct file *file, loff_t offset, int whence);
loff_t fixed_size_llseek(struct file *file, loff_t offset, int whence, loff_t size);
int nonseekable_open(struct inode *inode, struct file *filp);
int stream_open(struct inode *inode, struct file *filp);
int simple_open(struct inode *inode, struct file *file);
ssize_t simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos, const void *from, size_t available);
ssize_t simple_write_to_buffer(void *to, size_t available, loff_t *ppos, const void __user *from, size_t count);
int simple_pin_fs(void *type, struct vfsmount **mount, int *count);
void simple_release_fs(struct vfsmount **mount, int *count);
#define fops_get(fops) (fops)
#define fops_put(fops) do { } while (0)
#define replace_fops(f, fops) do { (f)->f_op = (fops); } while (0)
int fasync_helper(int fd, struct file *filp, int on, struct fasync_struct **fapp);
void kill_fasync(struct fasync_struct **fp, int sig, int band);
#define file_mnt_idmap(f) NULL
#define file_accessed(f) do { } while (0)
#define iterate_fd(files, n, f, p) 0
struct file *file_clone_open(struct file *file);
#define f_path_dentry_name(f) ((f)->f_path.dentry ? (f)->f_path.dentry->d_name.name : (const unsigned char *)"")

int register_chrdev(unsigned int major, const char *name, const struct file_operations *fops);
void unregister_chrdev(unsigned int major, const char *name);
int __register_chrdev(unsigned int major, unsigned int baseminor, unsigned int count, const char *name,
		      const struct file_operations *fops);
void __unregister_chrdev(unsigned int major, unsigned int baseminor, unsigned int count, const char *name);
int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count, const char *name);
int register_chrdev_region(dev_t from, unsigned count, const char *name);
void unregister_chrdev_region(dev_t from, unsigned count);
struct cdev {
	struct module *owner;
	const struct file_operations *ops;
	dev_t dev;
	unsigned int count;
	struct list_head list;
};
void cdev_init(struct cdev *cdev, const struct file_operations *fops);
int cdev_add(struct cdev *p, dev_t dev, unsigned count);
void cdev_del(struct cdev *p);


typedef struct poll_table_struct {
	void (*_qproc)(struct file *, struct wait_queue_head *, struct poll_table_struct *);
	__poll_t _key;
	void *lkpi_waiter;
} poll_table;
static inline void poll_wait(struct file *filp, struct wait_queue_head *wait_address, poll_table *p)
{
	if (p && p->_qproc && wait_address) p->_qproc(filp, wait_address, p);
}
static inline bool poll_does_not_wait(const poll_table *p) { return p == NULL || p->_qproc == NULL; }
static inline __poll_t poll_requested_events(const poll_table *p) { return p ? p->_key : ~(__poll_t)0; }
#define EPOLLIN ((__force __poll_t)0x00000001)
#define EPOLLPRI ((__force __poll_t)0x00000002)
#define EPOLLOUT ((__force __poll_t)0x00000004)
#define EPOLLERR ((__force __poll_t)0x00000008)
#define EPOLLHUP ((__force __poll_t)0x00000010)
#define EPOLLNVAL ((__force __poll_t)0x00000020)
#define EPOLLRDNORM ((__force __poll_t)0x00000040)
#define EPOLLRDBAND ((__force __poll_t)0x00000080)
#define EPOLLWRNORM ((__force __poll_t)0x00000100)
#define EPOLLWRBAND ((__force __poll_t)0x00000200)
#define EPOLLMSG ((__force __poll_t)0x00000400)
#define EPOLLRDHUP ((__force __poll_t)0x00002000)
#define POLLFREE ((__force __poll_t)0x4000)


struct seq_operations;
struct seq_file {
	char *buf;
	size_t size;
	size_t from;
	size_t count;
	size_t pad_until;
	loff_t index;
	loff_t read_pos;
	struct mutex lock;
	const struct seq_operations *op;
	int poll_event;
	const struct file *file;
	void *private;
};
struct seq_operations {
	void *(*start)(struct seq_file *m, loff_t *pos);
	void (*stop)(struct seq_file *m, void *v);
	void *(*next)(struct seq_file *m, void *v, loff_t *pos);
	int (*show)(struct seq_file *m, void *v);
};
__printf(2, 3) void seq_printf(struct seq_file *m, const char *fmt, ...);
__printf(2, 0) void seq_vprintf(struct seq_file *m, const char *fmt, va_list args);
void seq_puts(struct seq_file *m, const char *s);
void seq_putc(struct seq_file *m, char c);
void seq_write(struct seq_file *seq, const void *data, size_t len);
#define seq_has_overflowed(m) false
#define SEQ_START_TOKEN ((void *)1)
int seq_open(struct file *file, const struct seq_operations *op);
ssize_t seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos);
loff_t seq_lseek(struct file *file, loff_t offset, int whence);
int seq_release(struct inode *inode, struct file *file);
int single_open(struct file *file, int (*show)(struct seq_file *, void *), void *data);
int single_release(struct inode *inode, struct file *file);
#define DEFINE_SHOW_ATTRIBUTE(__name) \
static int __name##_open(struct inode *inode, struct file *file) { return single_open(file, __name##_show, inode->i_private); } \
static const struct file_operations __name##_fops = { .open = __name##_open, .read = seq_read, .llseek = seq_lseek, .release = single_release }
#define DEFINE_SEQ_ATTRIBUTE(__name) DEFINE_SHOW_ATTRIBUTE(__name)
#define DEFINE_SHOW_STORE_ATTRIBUTE(__name) \
static int __name##_open(struct inode *inode, struct file *file) { return single_open(file, __name##_show, inode->i_private); } \
static const struct file_operations __name##_fops = { .open = __name##_open, .read = seq_read, .write = __name##_write, \
	.llseek = seq_lseek, .release = single_release }
static inline void seq_pad(struct seq_file *m, char c) { }
#define seq_hex_dump(m, p, t, r, g, b, l, a) do { } while (0)
#define seq_put_decimal_ull(m, d, n) seq_printf(m, "%s%llu", d, (unsigned long long)(n))
#define seq_put_decimal_ll(m, d, n) seq_printf(m, "%s%lld", d, (long long)(n))


static inline void vma_set_file_impl(struct vm_area_struct *vma, struct file *file)
{
	if (file) get_file(file);
	if (vma->vm_file) fput(vma->vm_file);
	vma->vm_file = file;
}
#define vma_set_file(vma, file) vma_set_file_impl(vma, file)
#define mapping_map_writable(m) 0
#define mapping_unmap_writable(m) do { } while (0)

#endif
