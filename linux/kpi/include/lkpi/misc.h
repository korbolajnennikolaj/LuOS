// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_MISC_H
#define _LKPI_MISC_H

#include <lkpi/pci.h>


struct notifier_block;
typedef int (*notifier_fn_t)(struct notifier_block *nb, unsigned long action, void *data);
struct notifier_block {
	notifier_fn_t notifier_call;
	struct notifier_block *next;
	int priority;
};
struct blocking_notifier_head { struct rw_semaphore rwsem; struct notifier_block *head; };
struct atomic_notifier_head { spinlock_t lock; struct notifier_block *head; };
struct raw_notifier_head { struct notifier_block *head; };
#define BLOCKING_NOTIFIER_HEAD(name) struct blocking_notifier_head name = { .rwsem = __RWSEM_INITIALIZER((name).rwsem), .head = NULL }
#define ATOMIC_NOTIFIER_HEAD(name) struct atomic_notifier_head name = { .lock = __SPIN_LOCK_UNLOCKED(name.lock), .head = NULL }
#define BLOCKING_INIT_NOTIFIER_HEAD(name) do { init_rwsem(&(name)->rwsem); (name)->head = NULL; } while (0)
#define ATOMIC_INIT_NOTIFIER_HEAD(name) do { spin_lock_init(&(name)->lock); (name)->head = NULL; } while (0)
int blocking_notifier_chain_register(struct blocking_notifier_head *nh, struct notifier_block *nb);
int blocking_notifier_chain_unregister(struct blocking_notifier_head *nh, struct notifier_block *nb);
int blocking_notifier_call_chain(struct blocking_notifier_head *nh, unsigned long val, void *v);
int atomic_notifier_chain_register(struct atomic_notifier_head *nh, struct notifier_block *nb);
int atomic_notifier_chain_unregister(struct atomic_notifier_head *nh, struct notifier_block *nb);
int atomic_notifier_call_chain(struct atomic_notifier_head *nh, unsigned long val, void *v);
#define NOTIFY_DONE 0x0000
#define NOTIFY_OK 0x0001
#define NOTIFY_STOP_MASK 0x8000
#define NOTIFY_BAD (NOTIFY_STOP_MASK | 0x0002)
#define NOTIFY_STOP (NOTIFY_OK | NOTIFY_STOP_MASK)
#define notifier_from_errno(err) ((err) ? (NOTIFY_STOP_MASK | (NOTIFY_OK - (err))) : NOTIFY_OK)
#define notifier_to_errno(ret) ((ret) & NOTIFY_STOP_MASK ? (NOTIFY_OK - ((ret) & ~NOTIFY_STOP_MASK)) : 0)
#define register_reboot_notifier(nb) 0
#define unregister_reboot_notifier(nb) 0
#define register_pm_notifier(nb) 0
#define unregister_pm_notifier(nb) 0
#define register_oom_notifier(nb) 0
#define unregister_oom_notifier(nb) 0
#define register_acpi_notifier(nb) 0
#define unregister_acpi_notifier(nb) 0


struct acpi_device;
struct acpi_device_id { __u8 id[16]; kernel_ulong_t driver_data; __u32 cls; __u32 cls_msk; };
typedef void *acpi_handle;
typedef u32 acpi_status;
#define AE_OK 0
#define ACPI_FAILURE(s) ((s) != AE_OK)
#define ACPI_SUCCESS(s) ((s) == AE_OK)
#define ACPI_HANDLE(dev) ((acpi_handle)NULL)
#define ACPI_COMPANION(dev) ((struct acpi_device *)NULL)
#define ACPI_HANDLE_FWNODE(fwnode) ((acpi_handle)NULL)
#define acpi_disabled 1
#define has_acpi_companion(dev) false
#define is_acpi_device_node(fw) false
#define acpi_video_register() 0
#define acpi_video_unregister() do { } while (0)
#define acpi_video_get_backlight_type() 0
#define acpi_video_backlight_use_native() true
#define acpi_lid_open() 1
#define acpi_target_system_state() 0
#define acpi_dev_present(hid, uid, hrv) false
#define acpi_check_dsm(h, guid, rev, funcs) false
#define acpi_evaluate_dsm(h, guid, rev, func, argv4) ((void *)NULL)
#define acpi_bus_get_device(h, d) (-ENODEV)
#define acpi_dev_get_first_match_dev(hid, uid, hrv) ((struct acpi_device *)NULL)
struct device_node { const char *name; const char *full_name; struct fwnode_handle *fwnode_lkpi; };
struct of_device_id { char name[32]; char type[32]; char compatible[128]; const void *data; };
struct fwnode_handle { struct fwnode_handle *secondary; const void *ops; struct device *dev; };
struct of_endpoint { unsigned int port; unsigned int id; const struct device_node *local_node; };
struct of_phandle_args { struct device_node *np; int args_count; uint32_t args[16]; };
struct property { char *name; int length; void *value; };
#define of_node_get(n) (n)
#define of_node_put(n) do { } while (0)
#define of_property_read_u32(np, name, out) (-EINVAL)
#define of_property_read_u32_array(np, name, out, sz) (-EINVAL)
#define of_property_read_string(np, name, out) (-EINVAL)
#define of_property_read_bool(np, name) false
#define of_property_present(np, name) false
#define of_find_property(np, name, len) ((struct property *)NULL)
#define of_get_property(np, name, len) ((const void *)NULL)
#define of_device_is_compatible(np, c) 0
#define of_device_is_available(np) false
#define of_match_device(m, d) ((const struct of_device_id *)NULL)
#define of_match_ptr(p) NULL
#define of_match_node(m, n) ((const struct of_device_id *)NULL)
#define of_device_get_match_data(d) ((const void *)NULL)
#define of_get_child_by_name(np, n) ((struct device_node *)NULL)
#define of_get_next_child(np, prev) ((struct device_node *)NULL)
#define of_get_next_available_child(np, prev) ((struct device_node *)NULL)
#define of_get_available_child_count(np) 0
#define for_each_child_of_node(parent, child) for (child = NULL; child; )
#define for_each_available_child_of_node(parent, child) for (child = NULL; child; )
#define of_graph_get_remote_node(np, port, ep) ((struct device_node *)NULL)
#define of_graph_get_port_by_id(np, id) ((struct device_node *)NULL)
#define of_graph_get_endpoint_by_regs(np, p, e) ((struct device_node *)NULL)
#define of_graph_get_remote_port_parent(np) ((struct device_node *)NULL)
#define of_graph_get_next_endpoint(np, prev) ((struct device_node *)NULL)
#define for_each_endpoint_of_node(parent, child) for (child = NULL; child; )
#define of_parse_phandle(np, name, idx) ((struct device_node *)NULL)
#define of_alias_get_id(np, stem) (-ENODEV)
#define of_fwnode_handle(node) ((struct fwnode_handle *)NULL)
#define to_of_node(fw) ((struct device_node *)NULL)
#define is_of_node(fw) false
#define of_node_full_name(np) "<no-node>"
#define of_node_name_eq(np, name) false
#define of_find_node_by_path(p) ((struct device_node *)NULL)
#define of_find_compatible_node(f, t, c) ((struct device_node *)NULL)
#define of_platform_device_create(np, b, p) ((struct platform_device *)NULL)
#define of_device_uevent_modalias(d, e) (-ENODEV)
#define of_device_modalias(d, b, l) (-ENODEV)
#define of_dma_configure(d, np, f) 0
#define of_reserved_mem_device_init(d) (-ENODEV)
#define of_reserved_mem_region_to_resource(np, idx, res) (-ENODEV)
#define of_machine_is_compatible(c) false
#define of_find_device_by_node(np) ((struct platform_device *)NULL)
#define of_chosen ((struct device_node *)NULL)
#define dev_fwnode_lkpi(dev) NULL
#define fwnode_handle_put(fw) do { } while (0)
#define fwnode_handle_get(fw) (fw)
#define fwnode_property_present(fw, n) false
#define fwnode_property_read_u32(fw, n, v) (-EINVAL)
#define fwnode_property_read_string(fw, n, v) (-EINVAL)
#define fwnode_graph_get_remote_endpoint(fw) ((struct fwnode_handle *)NULL)
#define fwnode_find_reference(fw, n, i) ERR_PTR(-ENOENT)
#define device_property_present(dev, n) false
#define device_property_read_u32(dev, n, v) (-EINVAL)
#define device_property_read_u32_array(dev, n, v, c) (-EINVAL)
#define device_property_read_string(dev, n, v) (-EINVAL)
#define device_property_read_bool(dev, n) false
#define device_get_match_data(dev) ((const void *)NULL)
#define device_get_named_child_node(dev, n) ((struct fwnode_handle *)NULL)
#define device_for_each_child_node(dev, child) for (child = NULL; child; )
#define dmi_check_system(list) 0
#define dmi_match(f, s) false
#define dmi_get_system_info(f) ((const char *)NULL)
#define dmi_first_match(list) ((const struct dmi_system_id *)NULL)
#define dmi_find_device(type, name, from) ((const struct dmi_device *)NULL)
enum dmi_field { DMI_NONE, DMI_BIOS_VENDOR, DMI_BIOS_VERSION, DMI_BIOS_DATE, DMI_BIOS_RELEASE, DMI_EC_FIRMWARE_RELEASE,
		 DMI_SYS_VENDOR, DMI_PRODUCT_NAME, DMI_PRODUCT_VERSION, DMI_PRODUCT_SERIAL, DMI_PRODUCT_UUID,
		 DMI_PRODUCT_SKU, DMI_PRODUCT_FAMILY, DMI_BOARD_VENDOR, DMI_BOARD_NAME, DMI_BOARD_VERSION,
		 DMI_BOARD_SERIAL, DMI_BOARD_ASSET_TAG, DMI_CHASSIS_VENDOR, DMI_CHASSIS_TYPE, DMI_CHASSIS_VERSION,
		 DMI_CHASSIS_SERIAL, DMI_CHASSIS_ASSET_TAG, DMI_STRING_MAX, DMI_OEM_STRING };
struct dmi_strmatch { unsigned char slot:7; unsigned char exact_match:1; char substr[79]; };
struct dmi_system_id {
	int (*callback)(const struct dmi_system_id *);
	const char *ident;
	struct dmi_strmatch matches[4];
	void *driver_data;
};
struct dmi_device { int type; const char *name; void *device_data; };
#define DMI_MATCH(a, b) { .slot = a, .substr = b }
#define DMI_EXACT_MATCH(a, b) { .slot = a, .substr = b, .exact_match = 1 }
#define DMI_DEV_TYPE_OEM_STRING 0x81


#include <uapi/linux/i2c.h>
#define I2C_NAME_SIZE 20
struct i2c_adapter;
struct i2c_algorithm {
	int (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	int (*xfer_atomic)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	int (*smbus_xfer)(struct i2c_adapter *adap, u16 addr, unsigned short flags, char read_write, u8 command,
			  int size, union i2c_smbus_data *data);
	int (*smbus_xfer_atomic)(struct i2c_adapter *adap, u16 addr, unsigned short flags, char read_write, u8 command,
				 int size, union i2c_smbus_data *data);
	u32 (*functionality)(struct i2c_adapter *adap);
};
#define master_xfer xfer
#define master_xfer_atomic xfer_atomic
struct i2c_algo_bit_data {
	void *data;
	void (*setsda)(void *data, int state);
	void (*setscl)(void *data, int state);
	int (*getsda)(void *data);
	int (*getscl)(void *data);
	int (*pre_xfer)(struct i2c_adapter *);
	void (*post_xfer)(struct i2c_adapter *);
	int udelay;
	int timeout;
	bool can_do_atomic;
};
struct i2c_lock_operations {
	void (*lock_bus)(struct i2c_adapter *adapter, unsigned int flags);
	int (*trylock_bus)(struct i2c_adapter *adapter, unsigned int flags);
	void (*unlock_bus)(struct i2c_adapter *adapter, unsigned int flags);
};
struct i2c_adapter_quirks { u64 flags; int max_num_msgs; u16 max_write_len, max_read_len, max_comb_1st_msg_len, max_comb_2nd_msg_len; };
struct i2c_adapter {
	struct module *owner;
	unsigned int class;
	const struct i2c_algorithm *algo;
	void *algo_data;
	const struct i2c_lock_operations *lock_ops;
	struct rt_mutex_lkpi { struct mutex m; } bus_lock;
	struct mutex userspace_clients_lock;
	int timeout;
	int retries;
	struct device dev;
	unsigned long locked_flags;
	int nr;
	char name[48];
	const struct i2c_adapter_quirks *quirks;
};
struct i2c_client {
	unsigned short flags;
	unsigned short addr;
	char name[I2C_NAME_SIZE];
	struct i2c_adapter *adapter;
	struct device dev;
	int irq;
};
enum i2c_alert_protocol { I2C_PROTOCOL_SMBUS_ALERT, I2C_PROTOCOL_SMBUS_HOST_NOTIFY };
struct i2c_board_info { char type[I2C_NAME_SIZE]; unsigned short flags; unsigned short addr; void *platform_data; int irq; };
struct i2c_device_id { char name[I2C_NAME_SIZE]; kernel_ulong_t driver_data; };
struct i2c_driver {
	unsigned int class;
	int (*probe)(struct i2c_client *client);
	void (*remove)(struct i2c_client *client);
	void (*shutdown)(struct i2c_client *client);
	void (*alert)(struct i2c_client *client, enum i2c_alert_protocol protocol, unsigned int data);
	int (*command)(struct i2c_client *client, unsigned int cmd, void *arg);
	struct device_driver driver;
	const struct i2c_device_id *id_table;
	int (*detect)(struct i2c_client *client, struct i2c_board_info *info);
	const unsigned short *address_list;
	struct list_head clients;
	u32 flags;
};
#define to_i2c_driver(d) container_of_const(d, struct i2c_driver, driver)
static inline bool i2c_client_has_driver(struct i2c_client *client) { return !IS_ERR_OR_NULL(client) && client->dev.driver; }
static inline int i2c_register_driver(struct module *owner, struct i2c_driver *driver) { return 0; }
static inline void i2c_del_driver(struct i2c_driver *driver) { }
#define i2c_add_driver(driver) i2c_register_driver(THIS_MODULE, driver)
#define I2C_BOARD_INFO(dev_type, dev_addr) .type = dev_type, .addr = (dev_addr)
#define I2C_LOCK_ROOT_ADAPTER 0x01
#define I2C_LOCK_SEGMENT 0x02
static inline void *i2c_get_adapdata(const struct i2c_adapter *adap) { return dev_get_drvdata(&adap->dev); }
static inline void i2c_set_adapdata(struct i2c_adapter *adap, void *data) { dev_set_drvdata(&adap->dev, data); }
#define to_i2c_adapter(d) container_of(d, struct i2c_adapter, dev)
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int i2c_add_adapter(struct i2c_adapter *adap);
int devm_i2c_add_adapter(struct device *dev, struct i2c_adapter *adapter);
void i2c_del_adapter(struct i2c_adapter *adap);
int i2c_bit_add_bus(struct i2c_adapter *adap);
int i2c_bit_add_numbered_bus(struct i2c_adapter *adap);
extern const struct i2c_algorithm i2c_bit_algo;
static inline int i2c_add_numbered_adapter(struct i2c_adapter *adap) { return i2c_add_adapter(adap); }
static inline u8 i2c_8bit_addr_from_msg(const struct i2c_msg *msg) { return (msg->addr << 1) | (msg->flags & I2C_M_RD); }
int i2c_recover_bus(struct i2c_adapter *adap);
#define I2C_AQ_COMB BIT(0)
#define I2C_AQ_COMB_WRITE_FIRST BIT(1)
#define I2C_AQ_COMB_READ_SECOND BIT(2)
#define I2C_AQ_COMB_SAME_ADDR BIT(3)
#define I2C_AQ_NO_CLK_STRETCH BIT(4)
#define I2C_AQ_NO_ZERO_LEN_READ BIT(5)
#define I2C_AQ_NO_ZERO_LEN_WRITE BIT(6)
#define I2C_AQ_NO_ZERO_LEN (I2C_AQ_NO_ZERO_LEN_READ | I2C_AQ_NO_ZERO_LEN_WRITE)
#define I2C_AQ_NO_REP_START BIT(7)
struct i2c_adapter *i2c_get_adapter(int nr);
void i2c_put_adapter(struct i2c_adapter *adap);
struct i2c_adapter *of_find_i2c_adapter_by_node(struct device_node *node);
struct i2c_client *i2c_new_client_device(struct i2c_adapter *adap, struct i2c_board_info const *info);
void i2c_unregister_device(struct i2c_client *client);
s32 i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command);
s32 i2c_smbus_write_byte_data(const struct i2c_client *client, u8 command, u8 value);
static inline void i2c_lock_bus(struct i2c_adapter *adapter, unsigned int flags) { mutex_lock(&adapter->bus_lock.m); }
static inline void i2c_unlock_bus(struct i2c_adapter *adapter, unsigned int flags) { mutex_unlock(&adapter->bus_lock.m); }
#define i2c_mark_adapter_suspended(a) do { } while (0)
#define i2c_mark_adapter_resumed(a) do { } while (0)
extern const struct bus_type i2c_bus_type;


#include <uapi/linux/fb.h>
int fb_get_options(const char *name, char **option);
#define fb_modesetting_disabled(drvname) false
struct fb_info;
struct fb_videomode;


#define CEC_PHYS_ADDR_INVALID 0xffff
#define cec_get_edid_phys_addr(edid, size, offset) CEC_PHYS_ADDR_INVALID
#define overflowuid 65534
#define overflowgid 65534
struct cpuinfo_x86 {
	u8 x86;
	u8 x86_vendor;
	u8 x86_model;
	u8 x86_stepping;
	int x86_clflush_size;
	int x86_cache_alignment;
	u32 x86_capability[24];
	char x86_vendor_id[16];
	char x86_model_id[64];
};
extern struct cpuinfo_x86 boot_cpu_data;
#define X86_VENDOR_INTEL 0
#define X86_VENDOR_AMD 2
#define X86_VENDOR_HYGON 9
#define cpu_data(cpu) boot_cpu_data
#define xen_domain() 0
#define xen_pv_domain() 0
#define xen_initial_domain() 0
#define hypervisor_is_type(t) ((t) == X86_HYPER_NATIVE && !boot_cpu_has(X86_FEATURE_HYPERVISOR))
#define X86_HYPER_NATIVE 0
#define X86_HYPER_VMWARE 1
#define X86_HYPER_MS_HYPERV 2
#define X86_HYPER_XEN_PV 3
#define X86_HYPER_XEN_HVM 4
#define X86_HYPER_KVM 5
#define X86_HYPER_JAILHOUSE 6
#define X86_HYPER_ACRN 7
#define X86_HYPER_BHYVE 8
#define boot_cpu_has(bit) lkpi_boot_cpu_has(bit)
bool lkpi_boot_cpu_has(int bit);
#define static_cpu_has(bit) boot_cpu_has(bit)
#define cpu_feature_enabled(bit) boot_cpu_has(bit)
#define X86_FEATURE_CLFLUSH (0 * 32 + 19)
#define X86_FEATURE_PAT (0 * 32 + 16)
#define X86_FEATURE_XMM2 (0 * 32 + 26)
#define X86_FEATURE_XMM4_1 (4 * 32 + 19)
#define X86_FEATURE_HYPERVISOR (4 * 32 + 31)
#define X86_FEATURE_AVX (4 * 32 + 28)
#define X86_FEATURE_PAT_LKPI 0
#define kgdb_active_lkpi 0
#define in_dbg_master() 0
#define kmsg_dump_register(d) 0
#define kmsg_dump_unregister(d) 0
struct kmsg_dumper { int dummy; };
struct kmsg_dump_detail;
enum kmsg_dump_reason { KMSG_DUMP_UNDEF, KMSG_DUMP_PANIC, KMSG_DUMP_OOPS, KMSG_DUMP_EMERG, KMSG_DUMP_SHUTDOWN };

struct dentry *debugfs_lookup_lkpi(void);
#define debugfs_create_dir(name, parent) ((struct dentry *)ERR_PTR(-ENODEV))
#define debugfs_create_file(name, mode, parent, data, fops) ((struct dentry *)ERR_PTR(-ENODEV))
#define debugfs_create_file_unsafe debugfs_create_file
#define debugfs_create_file_size(name, mode, parent, data, fops, size) ((struct dentry *)ERR_PTR(-ENODEV))
#define debugfs_create_symlink(name, parent, target) ((struct dentry *)ERR_PTR(-ENODEV))
#define debugfs_create_bool(name, mode, parent, value) do { } while (0)
#define debugfs_create_u8(name, mode, parent, value) do { } while (0)
#define debugfs_create_u16(name, mode, parent, value) do { } while (0)
#define debugfs_create_u32(name, mode, parent, value) do { } while (0)
#define debugfs_create_u64(name, mode, parent, value) do { } while (0)
#define debugfs_create_x32(name, mode, parent, value) do { } while (0)
#define debugfs_create_x64(name, mode, parent, value) do { } while (0)
#define debugfs_create_ulong(name, mode, parent, value) do { } while (0)
#define debugfs_create_atomic_t(name, mode, parent, value) do { } while (0)
#define debugfs_create_regset32(name, mode, parent, regset) do { } while (0)
#define debugfs_create_blob(name, mode, parent, blob) ((struct dentry *)ERR_PTR(-ENODEV))
#define debugfs_remove(d) do { (void)(d); } while (0)
#define debugfs_remove_recursive(d) do { (void)(d); } while (0)
#define debugfs_lookup(name, parent) ((struct dentry *)NULL)
#define debugfs_lookup_and_remove(name, parent) do { } while (0)
#define debugfs_initialized() false
#define debugfs_real_fops(f) ((const struct file_operations *)NULL)
struct debugfs_blob_wrapper { void *data; unsigned long size; };
struct debugfs_reg32 { char *name; unsigned long offset; };
struct debugfs_regset32 { const struct debugfs_reg32 *regs; int nregs; void __iomem *base; struct device *dev; };
#define DEFINE_DEBUGFS_ATTRIBUTE(__fops, __get, __set, __fmt) static const struct file_operations __fops = { }
#define DEFINE_SIMPLE_ATTRIBUTE(__fops, __get, __set, __fmt) static const struct file_operations __fops = { }

struct eventfd_ctx;
struct eventfd_ctx *eventfd_ctx_fdget(int fd);
void eventfd_ctx_put(struct eventfd_ctx *ctx);
void eventfd_signal(struct eventfd_ctx *ctx);
#define eventfd_signal_mask(ctx, mask) eventfd_signal(ctx)

struct seq_buf { char *buffer; size_t size; size_t len; };
#define DECLARE_SEQ_BUF(NAME, SIZE) char __ ## NAME ## _buffer[SIZE] = ""; \
	struct seq_buf NAME = { .buffer = __ ## NAME ## _buffer, .size = SIZE }
static inline void seq_buf_init(struct seq_buf *s, char *buf, unsigned int size) { s->buffer = buf; s->size = size; s->len = 0; if (size) buf[0] = 0; }
__printf(2, 3) int seq_buf_printf(struct seq_buf *s, const char *fmt, ...);
int seq_buf_puts(struct seq_buf *s, const char *str);
static inline const char *seq_buf_str(struct seq_buf *s) { return s->buffer; }
static inline bool seq_buf_has_overflowed(struct seq_buf *s) { return s->len > s->size; }
static inline unsigned int seq_buf_used(struct seq_buf *s) { return min(s->len, s->size); }

typedef unsigned int depot_stack_handle_t;
#define stack_depot_save(entries, nr, flags) 0
#define stack_depot_print(h) do { } while (0)
#define stack_depot_snprint(h, buf, size, spaces) 0
#define stack_depot_init() do { } while (0)
#define stack_trace_save(store, size, skip) 0
#define stack_trace_snprint(buf, size, entries, nr, spaces) 0
#define stack_trace_print(entries, nr, spaces) do { } while (0)

struct component_ops {
	int (*bind)(struct device *comp, struct device *master, void *master_data);
	void (*unbind)(struct device *comp, struct device *master, void *master_data);
};
struct component_master_ops {
	int (*bind)(struct device *master);
	void (*unbind)(struct device *master);
};
struct component_match;
#define component_add(dev, ops) 0
#define component_del(dev, ops) do { } while (0)
#define component_bind_all(m, d) 0
#define component_unbind_all(m, d) do { } while (0)
#define component_master_add_with_match(p, ops, m) 0
#define component_master_del(p, ops) do { } while (0)
#define component_compare_dev(d, data) 0
#define component_match_add(p, m, c, d) do { } while (0)

struct cgroup_dmem_region;
struct dmem_cgroup_pool_state;
#define dmem_cgroup_register_region(size, fmt, ...) ((struct dmem_cgroup_region *)NULL)
#define dmem_cgroup_unregister_region(r) do { } while (0)
#define dmem_cgroup_try_charge(r, s, ret, lim) 0
#define dmem_cgroup_uncharge(p, s) do { } while (0)
#define dmem_cgroup_pool_state_put(p) do { } while (0)
#define dmem_cgroup_state_evict_valuable(l, t, i, f) true
struct dmem_cgroup_region;

#define screen_info_lkpi 0
bool video_is_primary_device(struct device *dev);
#define video_is_primary_device video_is_primary_device
bool sysfb_disable(struct device *dev);
#define sysfb_handles_screen_info(si, t) false
#define sysfb_primary_display_lkpi 0
#define dev_is_sysfb_lkpi 0

#define is_kdump_kernel() false
#define reboot_mode_lkpi 0
#define system_freezable_lkpi 0


struct virtio_device_id {
	__u32 device;
	__u32 vendor;
};
#define VIRTIO_DEV_ANY_ID 0xffffffff

#define kernel_halt lkpi_kernel_halt

#endif
