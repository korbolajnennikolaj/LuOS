// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_DEVICE_H
#define _LKPI_DEVICE_H
#include <linux/llist.h>

#include <lkpi/fs.h>
#include <linux/idr.h>


struct module {
	const char *name;
	int state;
};
#define THIS_MODULE ((struct module *)0)
#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_VERSION(x)
#define MODULE_ALIAS(x)
#define MODULE_FIRMWARE(x)
#define MODULE_DEVICE_TABLE(type, name)
#define MODULE_SOFTDEP(x)
#define MODULE_INFO(tag, info)
#define MODULE_PARM_DESC(var, desc)
struct kernel_param;
struct kernel_param_ops {
	unsigned int flags;
	int (*set)(const char *val, const struct kernel_param *kp);
	int (*get)(char *buffer, const struct kernel_param *kp);
	void (*free)(void *arg);
};
struct kernel_param { const char *name; void *arg; const struct kernel_param_ops *ops; unsigned int lkpi_maxlen; };
#define __lkpi_param(id, pname, parg, pops, maxlen) \
	static const struct kernel_param __lkpi_param_##id __used __section("lkpi_params") __aligned(8) = { \
		KBUILD_MODNAME "." pname, (void *)(parg), pops, maxlen }
#define module_param_cb(name, ops, arg, perm) __lkpi_param(name, #name, arg, ops, 0)
#define module_param_cb_unsafe module_param_cb
#define module_param_named(name, value, type, perm) __lkpi_param(name, #name, &(value), &param_ops_##type, 0)
#define module_param(name, type, perm) module_param_named(name, name, type, perm)
#define module_param_named_unsafe module_param_named
#define module_param_unsafe module_param
#define module_param_string(name, string, len, perm) __lkpi_param(name, #name, string, &param_ops_string, len)
#define module_param_array(name, type, nump, perm) static void *__maybe_unused __lkpi_param_##name = (name)
#define module_param_array_named(name, array, type, nump, perm) static void *__maybe_unused __lkpi_param_##name = (array)
#define __MODULE_PARM_TYPE(name, _type)
extern const struct kernel_param_ops param_ops_int, param_ops_uint, param_ops_long, param_ops_ulong, param_ops_ullong,
	param_ops_short, param_ops_ushort, param_ops_byte, param_ops_bool, param_ops_invbool, param_ops_charp,
	param_ops_string, param_ops_hexint, param_ops_bool_enable_only, param_ops_bint;
int param_set_int(const char *val, const struct kernel_param *kp);
int param_get_int(char *buffer, const struct kernel_param *kp);
int param_set_uint(const char *val, const struct kernel_param *kp);
int param_get_uint(char *buffer, const struct kernel_param *kp);
int param_set_bool(const char *val, const struct kernel_param *kp);
int param_get_bool(char *buffer, const struct kernel_param *kp);
int param_set_bint(const char *val, const struct kernel_param *kp);
int param_get_bint(char *buffer, const struct kernel_param *kp);
int param_set_charp(const char *val, const struct kernel_param *kp);
int param_get_charp(char *buffer, const struct kernel_param *kp);
void lkpi_params_apply(const char *cmdline);
#define try_module_get(m) true
#define module_put(m) do { } while (0)
#define __module_get(m) do { } while (0)
#define module_is_live(m) true
#define request_module(...) (-ENOENT)
#define request_module_nowait(...) (-ENOENT)
#define symbol_get(x) (&(x))
#define symbol_put(x) do { } while (0)
#define symbol_request(x) (&(x))
#define module_name(m) "luos"
#define IS_MODULE_LKPI 0

typedef int (*lkpi_initcall_t)(void);
typedef void (*lkpi_exitcall_t)(void);
struct lkpi_initcall { lkpi_initcall_t fn; const char *name; int level; };
#define __lkpi_initcall(fn, lvl) \
	static const struct lkpi_initcall __lkpi_initcall_##fn __used __section("lkpi_initcall") \
	__aligned(8) = { fn, #fn, lvl }
#define early_initcall(fn) __lkpi_initcall(fn, 1)
#define core_initcall(fn) __lkpi_initcall(fn, 1)
#define postcore_initcall(fn) __lkpi_initcall(fn, 2)
#define arch_initcall(fn) __lkpi_initcall(fn, 3)
#define subsys_initcall(fn) __lkpi_initcall(fn, 4)
#define subsys_initcall_sync(fn) __lkpi_initcall(fn, 4)
#define fs_initcall(fn) __lkpi_initcall(fn, 5)
#define rootfs_initcall(fn) __lkpi_initcall(fn, 5)
#define device_initcall(fn) __lkpi_initcall(fn, 6)
#define late_initcall(fn) __lkpi_initcall(fn, 7)
#define late_initcall_sync(fn) __lkpi_initcall(fn, 7)
#define module_init(fn) __lkpi_initcall(fn, 6)
#define module_exit(fn) static lkpi_exitcall_t __maybe_unused __lkpi_exit_##fn = fn
struct lkpi_setup { const char *str; int (*fn)(char *); };
#define __setup(str, fn) \
	static const struct lkpi_setup __lkpi_setup_##fn __used __section("lkpi_setup") __aligned(8) = { str, fn }
#define __exitcall(fn) static lkpi_exitcall_t __maybe_unused __lkpi_exitcall_##fn = fn
#define module_driver(__driver, __register, __unregister, ...) \
	static int __init __driver##_init(void) { return __register(&(__driver), ##__VA_ARGS__); } \
	module_init(__driver##_init); \
	static void __exit __driver##_exit(void) { __unregister(&(__driver), ##__VA_ARGS__); } \
	module_exit(__driver##_exit);
#define builtin_driver(__driver, __register, ...) \
	static int __init __driver##_init(void) { return __register(&(__driver), ##__VA_ARGS__); } \
	device_initcall(__driver##_init);
#define early_param(str, fn) __setup(str, fn)


struct attribute {
	const char *name;
	umode_t mode;
};
struct kobject;
struct bin_attribute;
struct attribute_group {
	const char *name;
	umode_t (*is_visible)(struct kobject *, struct attribute *, int);
	umode_t (*is_bin_visible)(struct kobject *, const struct bin_attribute *, int);
	struct attribute **attrs;
	const struct bin_attribute *const *bin_attrs;
};
struct bin_attribute {
	struct attribute attr;
	size_t size;
	void *private;
	ssize_t (*read)(struct file *, struct kobject *, const struct bin_attribute *, char *, loff_t, size_t);
	ssize_t (*write)(struct file *, struct kobject *, const struct bin_attribute *, char *, loff_t, size_t);
	int (*mmap)(struct file *, struct kobject *, const struct bin_attribute *attr, struct vm_area_struct *vma);
};
#define __ATTR(_name, _mode, _show, _store) { .attr = { .name = __stringify(_name), .mode = (_mode) }, \
	.show = _show, .store = _store }
#define __ATTR_RO_MODE(_name, _mode) { .attr = { .name = __stringify(_name), .mode = (_mode) }, .show = _name##_show }
#define __ATTR_RW_MODE(_name, _mode) { .attr = { .name = __stringify(_name), .mode = (_mode) }, \
	.show = _name##_show, .store = _name##_store }
#define __ATTR_RO(_name) { .attr = { .name = __stringify(_name), .mode = 0444 }, .show = _name##_show }
#define __ATTR_WO(_name) { .attr = { .name = __stringify(_name), .mode = 0200 }, .store = _name##_store }
#define __ATTR_RW(_name) __ATTR(_name, 0644, _name##_show, _name##_store)
#define __ATTR_NULL { .attr = { .name = NULL } }
#define ATTRIBUTE_GROUPS(_name) \
	static const struct attribute_group _name##_group = { .attrs = _name##_attrs }; \
	static const struct attribute_group *_name##_groups[] = { &_name##_group, NULL }
#define BIN_ATTR(_name, _mode, _read, _write, _size) \
	struct bin_attribute bin_attr_##_name = { .attr = { .name = __stringify(_name), .mode = _mode }, \
		.read = _read, .write = _write, .size = _size }
#define BIN_ATTR_RO(_name, _size) BIN_ATTR(_name, 0444, _name##_read, NULL, _size)
#define BIN_ATTR_RW(_name, _size) BIN_ATTR(_name, 0644, _name##_read, _name##_write, _size)
#define BIN_ATTR_ADMIN_RO(_name, _size) BIN_ATTR(_name, 0400, _name##_read, NULL, _size)
#define sysfs_attr_init(attr) do { } while (0)
#define sysfs_bin_attr_init(attr) do { } while (0)
struct sysfs_ops {
	ssize_t (*show)(struct kobject *kobj, struct attribute *attr, char *buf);
	ssize_t (*store)(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count);
};
struct kobj_type {
	void (*release)(struct kobject *kobj);
	const struct sysfs_ops *sysfs_ops;
	const struct attribute_group **default_groups;
};
struct kobj_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count);
};
struct kernfs_node;
struct kset;
struct kobject {
	const char *name;
	struct list_head entry;
	struct kobject *parent;
	struct kset *kset;
	const struct kobj_type *ktype;
	struct kref kref;
	unsigned int state_initialized:1;
	unsigned int state_in_sysfs:1;
	struct kernfs_node *sd;
};
static inline const char *kobject_name(const struct kobject *kobj) { return kobj->name; }
struct kobj_uevent_env;
struct kset_uevent_ops {
	int (* const filter)(const struct kobject *kobj);
	const char *(* const name)(const struct kobject *kobj);
	int (* const uevent)(const struct kobject *kobj, struct kobj_uevent_env *env);
};
struct kset {
	struct list_head list;
	spinlock_t list_lock;
	struct kobject kobj;
	const struct kset_uevent_ops *uevent_ops;
};
static inline struct kset *to_kset(struct kobject *kobj) { return kobj ? container_of(kobj, struct kset, kobj) : NULL; }
int kset_register(struct kset *kset);
void kset_unregister(struct kset *kset);
struct kset *kset_create_and_add(const char *name, const struct kset_uevent_ops *uevent_ops, struct kobject *parent_kobj);

#define kobj_to_dev(kobj) container_of_const(kobj, struct device, kobj)
int kobject_set_name(struct kobject *kobj, const char *name, ...);
void kobject_init(struct kobject *kobj, const struct kobj_type *ktype);
int kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt, ...);
int kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype, struct kobject *parent, const char *fmt, ...);
struct kobject *kobject_get(struct kobject *kobj);
void kobject_put(struct kobject *kobj);
void kobject_del(struct kobject *kobj);
struct kobject *kobject_create_and_add(const char *name, struct kobject *parent);
enum kobject_action { KOBJ_ADD, KOBJ_REMOVE, KOBJ_CHANGE, KOBJ_MOVE, KOBJ_ONLINE, KOBJ_OFFLINE, KOBJ_BIND, KOBJ_UNBIND };
#define kobject_uevent(kobj, action) 0
#define kobject_uevent_env(kobj, action, envp) 0
#define UEVENT_NUM_ENVP 64
#define UEVENT_BUFFER_SIZE 2048
struct kobj_uevent_env {
	char *argv[3];
	char *envp[UEVENT_NUM_ENVP];
	int envp_idx;
	char buf[UEVENT_BUFFER_SIZE];
	int buflen;
};
__printf(2, 3) int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...);
#define sysfs_create_group(kobj, grp) 0
#define sysfs_remove_group(kobj, grp) do { } while (0)
#define sysfs_create_groups(kobj, grp) 0
#define sysfs_remove_groups(kobj, grp) do { } while (0)
#define sysfs_create_file(kobj, attr) 0
#define sysfs_remove_file(kobj, attr) do { } while (0)
#define sysfs_create_files(kobj, attr) 0
#define sysfs_remove_files(kobj, attr) do { } while (0)
#define sysfs_create_bin_file(kobj, attr) 0
#define sysfs_remove_bin_file(kobj, attr) do { } while (0)
#define sysfs_create_link(kobj, target, name) 0
#define sysfs_remove_link(kobj, name) do { } while (0)
#define sysfs_notify(kobj, dir, attr) do { } while (0)
#define sysfs_update_group(kobj, grp) 0
#define sysfs_merge_group(kobj, grp) 0
#define sysfs_unmerge_group(kobj, grp) do { } while (0)
#define sysfs_add_file_to_group(kobj, attr, grp) 0
#define sysfs_remove_file_from_group(kobj, attr, grp) do { } while (0)
#define sysfs_create_mount_point(kobj, name) 0
#define sysfs_remove_mount_point(kobj, name) do { } while (0)
extern struct kobject *kernel_kobj;


struct device;
struct device_driver;
struct device_type;
struct bus_type;
struct class;
struct fwnode_handle;
struct device_node;
struct dev_pm_ops;
struct iommu_group;
struct dev_pm_domain;

typedef void (*dr_release_t)(struct device *dev, void *res);
typedef int (*dr_match_t)(struct device *dev, void *res, void *match_data);

struct device_dma_parameters {
	unsigned int max_segment_size;
	unsigned int min_align_mask;
	unsigned long segment_boundary_mask;
};

struct dev_pm_info {
	unsigned int can_wakeup:1;
	unsigned int async_suspend:1;
	bool is_prepared:1;
	bool is_suspended:1;
	bool runtime_auto:1;
	int runtime_status;
	atomic_t usage_count;
	int autosuspend_delay;
};

struct device {
	struct kobject kobj;
	struct device *parent;
	const char *init_name;
	const struct device_type *type;
	const struct bus_type *bus;
	struct device_driver *driver;
	void *platform_data;
	void *driver_data;
	struct mutex mutex;
	struct dev_pm_info power;
	struct dev_pm_domain *pm_domain;
	u64 *dma_mask;
	u64 coherent_dma_mask;
	u64 bus_dma_limit;
	struct device_dma_parameters *dma_parms;
	struct list_head dma_pools;
	struct list_head devres_head;
	spinlock_t devres_lock;
	struct device_node *of_node;
	struct fwnode_handle *fwnode;
	int numa_node;
	dev_t devt;
	u32 id;
	const struct class *class;
	const struct attribute_group **groups;
	void (*release)(struct device *dev);
	struct iommu_group *iommu_group;
	bool offline_disabled:1;
	bool offline:1;
	bool of_node_reused:1;
	bool state_synced:1;
	bool can_match:1;
	bool dma_coherent:1;
	bool dma_skip_sync:1;
	bool dma_iommu:1;
	struct list_head lkpi_node;
	bool lkpi_added;
};

struct device_type {
	const char *name;
	const struct attribute_group **groups;
	int (*uevent)(const struct device *dev, struct kobj_uevent_env *env);
	char *(*devnode)(const struct device *dev, umode_t *mode, kuid_t *uid, kgid_t *gid);
	void (*release)(struct device *dev);
	const struct dev_pm_ops *pm;
};

struct class {
	const char *name;
	const struct attribute_group **class_groups;
	const struct attribute_group **dev_groups;
	int (*dev_uevent)(const struct device *dev, struct kobj_uevent_env *env);
	char *(*devnode)(const struct device *dev, umode_t *mode);
	void (*class_release)(const struct class *class);
	void (*dev_release)(struct device *dev);
	const struct dev_pm_ops *pm;
};

struct device_driver {
	const char *name;
	const struct bus_type *bus;
	struct module *owner;
	const char *mod_name;
	bool suppress_bind_attrs;
	int probe_type;
	const struct of_device_id *of_match_table;
	const struct acpi_device_id *acpi_match_table;
	int (*probe)(struct device *dev);
	void (*sync_state)(struct device *dev);
	int (*remove)(struct device *dev);
	void (*shutdown)(struct device *dev);
	int (*suspend)(struct device *dev, int state);
	int (*resume)(struct device *dev);
	const struct attribute_group **groups;
	const struct attribute_group **dev_groups;
	const struct dev_pm_ops *pm;
};
#define PROBE_DEFAULT_STRATEGY 0
#define PROBE_PREFER_ASYNCHRONOUS 1
#define PROBE_FORCE_SYNCHRONOUS 2

struct bus_type {
	const char *name;
	const char *dev_name;
	const struct attribute_group **bus_groups;
	const struct attribute_group **dev_groups;
	const struct attribute_group **drv_groups;
	int (*match)(struct device *dev, const struct device_driver *drv);
	int (*uevent)(const struct device *dev, struct kobj_uevent_env *env);
	int (*probe)(struct device *dev);
	void (*remove)(struct device *dev);
	void (*shutdown)(struct device *dev);
	const struct cpumask *(*irq_get_affinity)(struct device *dev, unsigned int irq_vec);
	int (*online)(struct device *dev);
	int (*offline)(struct device *dev);
	int (*dma_configure)(struct device *dev);
	void (*dma_cleanup)(struct device *dev);
	int (*num_vf)(struct device *dev);
	const struct dev_pm_ops *pm;
	bool need_parent_lock;
};

#define DEVICE_ATTR(_name, _mode, _show, _store) struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)
#define DEVICE_ATTR_RO(_name) struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#define DEVICE_ATTR_RW(_name) struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
#define DEVICE_ATTR_WO(_name) struct device_attribute dev_attr_##_name = __ATTR_WO(_name)
#define DEVICE_ATTR_ADMIN_RO(_name) struct device_attribute dev_attr_##_name = __ATTR(_name, 0400, _name##_show, NULL)
#define DEVICE_ATTR_ADMIN_RW(_name) struct device_attribute dev_attr_##_name = __ATTR(_name, 0600, _name##_show, _name##_store)
struct device_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device *dev, struct device_attribute *attr, char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
};
#define device_create_file(dev, attr) 0
#define device_remove_file(dev, attr) do { } while (0)
#define device_create_bin_file(dev, attr) 0
#define device_remove_bin_file(dev, attr) do { } while (0)
#define device_add_group(dev, grp) 0
#define device_remove_group(dev, grp) do { } while (0)
#define device_add_groups(dev, grp) 0
#define device_remove_groups(dev, grp) do { } while (0)
#define devm_device_add_group(dev, grp) 0
#define devm_device_add_groups(dev, grp) 0
#define dev_groups_lkpi 0

static inline const char *dev_name(const struct device *dev)
{
	if (dev->init_name) return dev->init_name;
	return kobject_name(&dev->kobj) ? kobject_name(&dev->kobj) : "";
}
__printf(2, 3) int dev_set_name(struct device *dev, const char *name, ...);
static inline const char *dev_driver_string(const struct device *dev)
{
	return dev->driver ? dev->driver->name : (dev->bus ? dev->bus->name : (dev->class ? dev->class->name : ""));
}
static inline const char *dev_bus_name(const struct device *dev) { return dev->bus ? dev->bus->name : ""; }
static inline void *dev_get_drvdata(const struct device *dev) { return dev->driver_data; }
static inline void dev_set_drvdata(struct device *dev, void *data) { dev->driver_data = data; }
static inline struct device_node *dev_of_node(struct device *dev) { return dev->of_node; }
static inline struct fwnode_handle *dev_fwnode(const struct device *dev) { return dev->fwnode; }
static inline int dev_to_node_lkpi(struct device *dev) { return NUMA_NO_NODE; }
static inline bool device_is_registered(struct device *dev) { return dev->lkpi_added; }
static inline void device_lock(struct device *dev) { mutex_lock(&dev->mutex); }
static inline int device_trylock(struct device *dev) { return mutex_trylock(&dev->mutex); }
static inline void device_unlock(struct device *dev) { mutex_unlock(&dev->mutex); }
#define device_lock_assert(dev) do { } while (0)
#define dev_pm_set_driver_flags(dev, flags) do { } while (0)
#define device_set_wakeup_capable(dev, c) do { } while (0)
#define device_set_wakeup_enable(dev, e) 0
#define device_init_wakeup(dev, e) 0
#define device_wakeup_enable(dev) 0
#define device_may_wakeup(dev) false
#define dev_is_removable(dev) false
#define device_link_add(c, s, f) ((struct device_link *)NULL)
#define device_link_del(l) do { } while (0)
#define device_link_remove(c, s) do { } while (0)
struct device_link;
#define DL_FLAG_STATELESS 1
#define DL_FLAG_AUTOREMOVE_CONSUMER 2
#define DL_FLAG_PM_RUNTIME 4

void device_initialize(struct device *dev);
int device_add(struct device *dev);
int device_register(struct device *dev);
void device_del(struct device *dev);
void device_unregister(struct device *dev);
struct device *get_device(struct device *dev);
void put_device(struct device *dev);
__printf(5, 6) struct device *device_create(const struct class *cls, struct device *parent, dev_t devt,
					     void *drvdata, const char *fmt, ...);
void device_destroy(const struct class *cls, dev_t devt);
int device_rename(struct device *dev, const char *new_name);
int class_register(const struct class *class);
void class_unregister(const struct class *class);
struct class *class_create(const char *name);
void class_destroy(const struct class *cls);
#define class_create_file(cls, attr) 0
#define class_remove_file(cls, attr) do { } while (0)
struct class_attribute_string { int dummy; };
#define CLASS_ATTR_STRING(_name, _mode, _str) struct class_attribute_string class_attr_##_name = { 0 }
#define class_create_file_string(c, a) 0
int driver_register(struct device_driver *drv);
void driver_unregister(struct device_driver *drv);
int device_attach(struct device *dev);
int device_driver_attach(const struct device_driver *drv, struct device *dev);
void device_release_driver(struct device *dev);
int bus_register(const struct bus_type *bus);
void bus_unregister(const struct bus_type *bus);
int bus_for_each_dev(const struct bus_type *bus, struct device *start, void *data, int (*fn)(struct device *dev, void *data));
int bus_for_each_drv(const struct bus_type *bus, struct device_driver *start, void *data,
		     int (*fn)(struct device_driver *drv, void *data));
struct device *bus_find_device_by_name(const struct bus_type *bus, struct device *start, const char *name);
#define device_for_each_child(dev, data, fn) 0
#define device_find_child(dev, data, match) ((struct device *)NULL)
#define dev_has_sync_state(dev) false
#define dev_set_removable(dev, r) do { } while (0)
static inline void device_set_node(struct device *dev, struct fwnode_handle *fwnode) { dev->fwnode = fwnode; }
#define dev_dma_attr_lkpi 0

void *__devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp, int nid, const char *name);
#define devres_alloc(release, size, gfp) __devres_alloc_node(release, size, gfp, NUMA_NO_NODE, #release)
#define devres_alloc_node(release, size, gfp, nid) __devres_alloc_node(release, size, gfp, nid, #release)
void devres_free(void *res);
void devres_add(struct device *dev, void *res);
void *devres_find(struct device *dev, dr_release_t release, dr_match_t match, void *match_data);
void *devres_remove(struct device *dev, dr_release_t release, dr_match_t match, void *match_data);
int devres_destroy(struct device *dev, dr_release_t release, dr_match_t match, void *match_data);
int devres_release(struct device *dev, dr_release_t release, dr_match_t match, void *match_data);
void *devres_open_group(struct device *dev, void *id, gfp_t gfp);
void devres_close_group(struct device *dev, void *id);
void devres_remove_group(struct device *dev, void *id);
int devres_release_group(struct device *dev, void *id);
void devres_release_all(struct device *dev);
void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp);
static inline void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp) { return devm_kmalloc(dev, size, gfp | __GFP_ZERO); }
static inline void *devm_kmalloc_array(struct device *dev, size_t n, size_t size, gfp_t flags)
{
	size_t bytes;
	if (unlikely(check_mul_overflow(n, size, &bytes))) return NULL;
	return devm_kmalloc(dev, bytes, flags);
}
#define devm_kcalloc(dev, n, size, flags) devm_kmalloc_array(dev, n, size, (flags) | __GFP_ZERO)
void devm_kfree(struct device *dev, const void *p);
char *devm_kstrdup(struct device *dev, const char *s, gfp_t gfp);
const char *devm_kstrdup_const(struct device *dev, const char *s, gfp_t gfp);
__printf(3, 4) char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...);
void *devm_kmemdup(struct device *dev, const void *src, size_t len, gfp_t gfp);
int __devm_add_action(struct device *dev, void (*action)(void *), void *data, const char *name);
#define devm_add_action(dev, action, data) __devm_add_action(dev, action, data, #action)
static inline int __devm_add_action_or_reset(struct device *dev, void (*action)(void *), void *data, const char *name)
{
	int ret = __devm_add_action(dev, action, data, name);
	if (ret) action(data);
	return ret;
}
#define devm_add_action_or_reset(dev, action, data) __devm_add_action_or_reset(dev, action, data, #action)
void devm_remove_action(struct device *dev, void (*action)(void *), void *data);
void devm_release_action(struct device *dev, void (*action)(void *), void *data);
void __iomem *devm_ioremap(struct device *dev, resource_size_t offset, resource_size_t size);
void __iomem *devm_ioremap_wc(struct device *dev, resource_size_t offset, resource_size_t size);
void devm_iounmap(struct device *dev, void __iomem *addr);
void __iomem *devm_ioremap_resource(struct device *dev, const struct resource *res);
struct resource *devm_request_mem_region(struct device *dev, resource_size_t start, resource_size_t n, const char *name);
#define devm_request_region devm_request_mem_region
#define devm_release_mem_region(dev, start, n) do { } while (0)
int devm_arch_phys_wc_add(struct device *dev, unsigned long base, unsigned long size);
int devm_arch_io_reserve_memtype_wc(struct device *dev, resource_size_t start, resource_size_t size);
void *devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags);

__printf(3, 4) void _dev_printk(const char *level, const struct device *dev, const char *fmt, ...);
#define dev_printk(level, dev, fmt, ...) _dev_printk(level, dev, fmt, ##__VA_ARGS__)
#define dev_emerg(dev, fmt, ...) _dev_printk(KERN_EMERG, dev, fmt, ##__VA_ARGS__)
#define dev_alert(dev, fmt, ...) _dev_printk(KERN_ALERT, dev, fmt, ##__VA_ARGS__)
#define dev_crit(dev, fmt, ...) _dev_printk(KERN_CRIT, dev, fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...) _dev_printk(KERN_ERR, dev, fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...) _dev_printk(KERN_WARNING, dev, fmt, ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...) _dev_printk(KERN_NOTICE, dev, fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...) _dev_printk(KERN_INFO, dev, fmt, ##__VA_ARGS__)
#ifdef DEBUG
#define dev_dbg(dev, fmt, ...) _dev_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__)
#else
#define dev_dbg(dev, fmt, ...) ({ if (0) _dev_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__); })
#endif
#define dev_vdbg(dev, fmt, ...) do { } while (0)
#define dev_err_once(dev, fmt, ...) dev_err(dev, fmt, ##__VA_ARGS__)
#define dev_warn_once(dev, fmt, ...) dev_warn(dev, fmt, ##__VA_ARGS__)
#define dev_info_once(dev, fmt, ...) dev_info(dev, fmt, ##__VA_ARGS__)
#define dev_notice_once(dev, fmt, ...) dev_notice(dev, fmt, ##__VA_ARGS__)
#define dev_dbg_once(dev, fmt, ...) dev_dbg(dev, fmt, ##__VA_ARGS__)
#define dev_err_ratelimited dev_err
#define dev_warn_ratelimited dev_warn
#define dev_info_ratelimited dev_info
#define dev_dbg_ratelimited dev_dbg
#define dev_notice_ratelimited dev_notice
#define dev_WARN(dev, format, arg...) WARN(1, "%s: " format, dev_name(dev), ##arg)
#define dev_WARN_ONCE(dev, condition, format, arg...) WARN_ONCE(condition, "%s: " format, dev_name(dev), ##arg)
__printf(3, 4) int dev_err_probe(const struct device *dev, int err, const char *fmt, ...);
#define dev_warn_probe(dev, err, fmt, ...) ({ dev_warn(dev, fmt, ##__VA_ARGS__); (err); })
#define dev_err_ptr_probe(dev, err, fmt, ...) ERR_PTR(dev_err_probe(dev, err, fmt, ##__VA_ARGS__))
#define dev_level_once_lkpi 0

#define RPM_ACTIVE 0
#define RPM_RESUMING 1
#define RPM_SUSPENDED 2
#define RPM_SUSPENDING 3
struct dev_pm_ops {
	int (*prepare)(struct device *dev);
	void (*complete)(struct device *dev);
	int (*suspend)(struct device *dev);
	int (*resume)(struct device *dev);
	int (*freeze)(struct device *dev);
	int (*thaw)(struct device *dev);
	int (*poweroff)(struct device *dev);
	int (*restore)(struct device *dev);
	int (*suspend_late)(struct device *dev);
	int (*resume_early)(struct device *dev);
	int (*freeze_late)(struct device *dev);
	int (*thaw_early)(struct device *dev);
	int (*poweroff_late)(struct device *dev);
	int (*restore_early)(struct device *dev);
	int (*suspend_noirq)(struct device *dev);
	int (*resume_noirq)(struct device *dev);
	int (*freeze_noirq)(struct device *dev);
	int (*thaw_noirq)(struct device *dev);
	int (*poweroff_noirq)(struct device *dev);
	int (*restore_noirq)(struct device *dev);
	int (*runtime_suspend)(struct device *dev);
	int (*runtime_resume)(struct device *dev);
	int (*runtime_idle)(struct device *dev);
};
struct dev_pm_domain {
	struct dev_pm_ops ops;
	int (*start)(struct device *dev);
	void (*detach)(struct device *dev, bool power_off);
	int (*activate)(struct device *dev);
	void (*sync)(struct device *dev);
	void (*dismiss)(struct device *dev);
	int (*set_performance_state)(struct device *dev, unsigned int state);
};
#define SET_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn)
#define SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) .suspend = suspend_fn, .resume = resume_fn, \
	.freeze = suspend_fn, .thaw = resume_fn, .poweroff = suspend_fn, .restore = resume_fn,
#define SET_RUNTIME_PM_OPS(s, r, i)
#define RUNTIME_PM_OPS(s, r, i) .runtime_suspend = s, .runtime_resume = r, .runtime_idle = i,
#define DEFINE_SIMPLE_DEV_PM_OPS(name, suspend_fn, resume_fn) \
	static const struct dev_pm_ops name = { SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) }
#define SIMPLE_DEV_PM_OPS DEFINE_SIMPLE_DEV_PM_OPS
#define pm_sleep_ptr(_ptr) (_ptr)
#define pm_ptr(_ptr) (_ptr)
#define pm_runtime_get_sync(dev) 0
#define pm_runtime_get(dev) 0
#define pm_runtime_get_noresume(dev) do { } while (0)
#define pm_runtime_resume_and_get(dev) 0
#define pm_runtime_get_if_in_use(dev) 1
#define pm_runtime_get_if_active(dev) 1
#define pm_runtime_put(dev) 0
#define pm_runtime_put_sync(dev) 0
#define pm_runtime_put_noidle(dev) do { } while (0)
#define pm_runtime_put_autosuspend(dev) 0
#define pm_runtime_put_sync_autosuspend(dev) 0
#define pm_runtime_mark_last_busy(dev) do { } while (0)
#define pm_runtime_use_autosuspend(dev) do { } while (0)
#define pm_runtime_dont_use_autosuspend(dev) do { } while (0)
#define pm_runtime_set_autosuspend_delay(dev, d) do { } while (0)
#define pm_runtime_set_active(dev) 0
#define pm_runtime_set_suspended(dev) 0
#define pm_runtime_allow(dev) do { } while (0)
#define pm_runtime_forbid(dev) do { } while (0)
#define pm_runtime_enable(dev) do { } while (0)
#define pm_runtime_disable(dev) do { } while (0)
#define devm_pm_runtime_enable(dev) 0
#define pm_runtime_suspended(dev) false
#define pm_runtime_active(dev) true
#define pm_runtime_status_suspended(dev) false
#define pm_runtime_enabled(dev) false
#define pm_runtime_suspend(dev) 0
#define pm_runtime_resume(dev) 0
#define pm_runtime_idle(dev) 0
#define pm_runtime_autosuspend(dev) 0
#define pm_runtime_get_noidle_lkpi 0
#define pm_suspend_ignore_children(dev, e) do { } while (0)
#define pm_runtime_no_callbacks(dev) do { } while (0)
#define pm_runtime_irq_safe(dev) do { } while (0)
#define pm_suspend_target_state 0
#define pm_suspend_via_firmware() false
#define pm_resume_via_firmware() false
#define pm_suspend_default_s2idle() false
#define PM_EVENT_SUSPEND 0x0002
#define PM_EVENT_FREEZE 0x0001
#define PM_EVENT_HIBERNATE 0x0004
#define PM_EVENT_RESUME 0x0010
#define PM_EVENT_PRETHAW 0x0008
typedef struct pm_message { int event; } pm_message_t;
#define PMSG_SUSPEND ((struct pm_message){ .event = PM_EVENT_SUSPEND })
#define PMSG_FREEZE ((struct pm_message){ .event = PM_EVENT_FREEZE })


enum irqreturn { IRQ_NONE = 0, IRQ_HANDLED = 1, IRQ_WAKE_THREAD = 2 };
typedef enum irqreturn irqreturn_t;
#define IRQ_RETVAL(x) ((x) ? IRQ_HANDLED : IRQ_NONE)
typedef irqreturn_t (*irq_handler_t)(int, void *);
#define IRQF_SHARED 0x00000080
#define IRQF_PROBE_SHARED 0x00000100
#define IRQF_TIMER 0x00000200
#define IRQF_PERCPU 0x00000400
#define IRQF_NOBALANCING 0x00000800
#define IRQF_IRQPOLL 0x00001000
#define IRQF_ONESHOT 0x00002000
#define IRQF_NO_SUSPEND 0x00004000
#define IRQF_NO_THREAD 0x00010000
#define IRQF_TRIGGER_NONE 0x00000000
#define IRQF_TRIGGER_RISING 0x00000001
#define IRQF_TRIGGER_HIGH 0x00000004
#define IRQF_TRIGGER_LOW 0x00000008
#define IRQF_NO_AUTOEN 0x00080000
int request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn, unsigned long flags,
			 const char *name, void *dev);
static inline int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags, const char *name, void *dev)
{
	return request_threaded_irq(irq, handler, NULL, flags, name, dev);
}
const void *free_irq(unsigned int irq, void *dev_id);
int devm_request_threaded_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
			      unsigned long irqflags, const char *devname, void *dev_id);
#define devm_request_irq(dev, irq, h, f, n, d) devm_request_threaded_irq(dev, irq, h, NULL, f, n, d)
void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id);
void disable_irq(unsigned int irq);
void disable_irq_nosync(unsigned int irq);
void enable_irq(unsigned int irq);
void synchronize_irq(unsigned int irq);
#define irq_set_affinity_hint(irq, m) 0
#define irq_set_affinity_and_hint(irq, m) 0
#define IRQ_AFFINITY_MAX_SETS 4
struct irq_affinity {
	unsigned int pre_vectors;
	unsigned int post_vectors;
	unsigned int nr_sets;
	unsigned int set_size[IRQ_AFFINITY_MAX_SETS];
	void (*calc_sets)(struct irq_affinity *, unsigned int nvecs);
	void *priv;
};
#define irq_update_affinity_hint(irq, m) 0
#define irq_set_irq_wake(irq, on) 0
#define enable_irq_wake(irq) 0
#define disable_irq_wake(irq) 0
struct tasklet_struct {
	struct tasklet_struct *next;
	unsigned long state;
	atomic_t count;
	bool use_callback;
	union {
		void (*func)(unsigned long data);
		void (*callback)(struct tasklet_struct *t);
	};
	unsigned long data;
	struct work_struct lkpi_work;
};
enum { TASKLET_STATE_SCHED, TASKLET_STATE_RUN };
void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long), unsigned long data);
void tasklet_setup(struct tasklet_struct *t, void (*callback)(struct tasklet_struct *));
void tasklet_schedule(struct tasklet_struct *t);
#define tasklet_hi_schedule tasklet_schedule
void tasklet_kill(struct tasklet_struct *t);
static inline bool tasklet_trylock(struct tasklet_struct *t) { return !test_and_set_bit(TASKLET_STATE_RUN, &t->state); }
void tasklet_unlock(struct tasklet_struct *t);
void tasklet_unlock_wait(struct tasklet_struct *t);
void tasklet_unlock_spin_wait(struct tasklet_struct *t);
static inline void tasklet_disable_nosync(struct tasklet_struct *t)
{
	atomic_inc(&t->count);
	smp_mb__after_atomic();
}
static inline void tasklet_disable_in_atomic(struct tasklet_struct *t)
{
	tasklet_disable_nosync(t);
	tasklet_unlock_spin_wait(t);
	smp_mb();
}
static inline void tasklet_disable(struct tasklet_struct *t)
{
	tasklet_disable_nosync(t);
	tasklet_unlock_wait(t);
	smp_mb();
}
static inline void tasklet_enable(struct tasklet_struct *t)
{
	smp_mb__before_atomic();
	atomic_dec(&t->count);
}
#define from_tasklet(var, callback_tasklet, tasklet_fieldname) container_of(callback_tasklet, typeof(*var), tasklet_fieldname)
struct __call_single_node {
	struct llist_node llist;
	union {
		unsigned int u_flags;
		atomic_t a_flags;
	};
	u16 src, dst;
};
#define IRQ_WORK_PENDING BIT(0)
#define IRQ_WORK_BUSY BIT(1)
#define IRQ_WORK_LAZY BIT(2)
#define IRQ_WORK_HARD_IRQ BIT(3)
#define IRQ_WORK_CLAIMED (IRQ_WORK_PENDING | IRQ_WORK_BUSY)
struct irq_work {
	struct __call_single_node node;
	void (*func)(struct irq_work *);
	struct work_struct lkpi_work;
};
#define __IRQ_WORK_INIT(_func, _flags) (struct irq_work){ .node = { .u_flags = (_flags), }, .func = (_func), }
#define IRQ_WORK_INIT(_func) __IRQ_WORK_INIT(_func, 0)
#define IRQ_WORK_INIT_LAZY(_func) __IRQ_WORK_INIT(_func, IRQ_WORK_LAZY)
#define IRQ_WORK_INIT_HARD(_func) __IRQ_WORK_INIT(_func, IRQ_WORK_HARD_IRQ)
#define DEFINE_IRQ_WORK(name, _f) struct irq_work name = IRQ_WORK_INIT(_f)
static inline void init_irq_work(struct irq_work *work, void (*func)(struct irq_work *)) { *work = IRQ_WORK_INIT(func); }
static inline bool irq_work_is_pending(struct irq_work *work) { return atomic_read(&work->node.a_flags) & IRQ_WORK_PENDING; }
static inline bool irq_work_is_busy(struct irq_work *work) { return atomic_read(&work->node.a_flags) & IRQ_WORK_BUSY; }
static inline bool irq_work_is_hard(struct irq_work *work) { return atomic_read(&work->node.a_flags) & IRQ_WORK_HARD_IRQ; }
bool irq_work_queue(struct irq_work *work);
#define irq_work_queue_on(work, cpu) irq_work_queue(work)
void irq_work_sync(struct irq_work *work);
#define irq_work_run() do { } while (0)
#define irq_work_needs_cpu() false


struct firmware {
	size_t size;
	const u8 *data;
	void *priv;
};
int request_firmware(const struct firmware **fw, const char *name, struct device *device);
int firmware_request_nowarn(const struct firmware **fw, const char *name, struct device *device);
int request_firmware_direct(const struct firmware **fw, const char *name, struct device *device);
int request_firmware_nowait(struct module *module, bool uevent, const char *name, struct device *device, gfp_t gfp,
			    void *context, void (*cont)(const struct firmware *fw, void *context));
int request_firmware_into_buf(const struct firmware **firmware_p, const char *name, struct device *device, void *buf,
			      size_t size);
void release_firmware(const struct firmware *fw);


struct platform_device_id { char name[20]; unsigned long driver_data; };
struct mfd_cell;
struct platform_device {
	const char *name;
	int id;
	bool id_auto;
	struct device dev;
	u64 platform_dma_mask;
	struct device_dma_parameters dma_parms;
	u32 num_resources;
	struct resource *resource;
	const struct platform_device_id *id_entry;
	struct mfd_cell *mfd_cell;
};
#define to_platform_device(x) container_of((x), struct platform_device, dev)
#define PLATFORM_DEVID_NONE (-1)
#define PLATFORM_DEVID_AUTO (-2)
struct platform_driver {
	int (*probe)(struct platform_device *);
	void (*remove)(struct platform_device *);
	void (*shutdown)(struct platform_device *);
	struct device_driver driver;
	const struct platform_device_id *id_table;
	bool prevent_deferred_probe;
};
int __platform_driver_register(struct platform_driver *drv, struct module *owner);
#define platform_driver_register(drv) __platform_driver_register(drv, THIS_MODULE)
void platform_driver_unregister(struct platform_driver *drv);
struct platform_device *platform_device_alloc(const char *name, int id);
int platform_device_add(struct platform_device *pdev);
void platform_device_del(struct platform_device *pdev);
void platform_device_put(struct platform_device *pdev);
int platform_device_register(struct platform_device *pdev);
void platform_device_unregister(struct platform_device *pdev);
struct platform_device *platform_device_register_resndata(struct device *parent, const char *name, int id,
							   const struct resource *res, unsigned int num,
							   const void *data, size_t size);
#define platform_device_register_simple(name, id, res, num) platform_device_register_resndata(NULL, name, id, res, num, NULL, 0)
#define platform_device_register_data(parent, name, id, data, size) platform_device_register_resndata(parent, name, id, NULL, 0, data, size)
int platform_device_add_data(struct platform_device *pdev, const void *data, size_t size);
int platform_device_add_resources(struct platform_device *pdev, const struct resource *res, unsigned int num);
struct resource *platform_get_resource(struct platform_device *dev, unsigned int type, unsigned int num);
int platform_get_irq(struct platform_device *dev, unsigned int num);
static inline void *platform_get_drvdata(const struct platform_device *pdev) { return dev_get_drvdata(&pdev->dev); }
static inline void platform_set_drvdata(struct platform_device *pdev, void *data) { dev_set_drvdata(&pdev->dev, data); }
#define dev_get_platdata(dev) ((dev)->platform_data)
#define module_platform_driver(__platform_driver) \
	static int __init __platform_driver##_init(void) { return platform_driver_register(&(__platform_driver)); } \
	module_init(__platform_driver##_init)
extern const struct bus_type platform_bus_type;

#endif
