// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_HW_H
#define _LKPI_HW_H

#include <lkpi/misc.h>
#include <linux/uuid.h>


typedef u32 acpi_object_type;
typedef u64 acpi_size;
typedef u64 acpi_io_address;
typedef char *acpi_string;
#define ACPI_TYPE_ANY 0x00
#define ACPI_TYPE_INTEGER 0x01
#define ACPI_TYPE_STRING 0x02
#define ACPI_TYPE_DEVICE 0x06
#define ACPI_TYPE_BUFFER 0x03
#define ACPI_TYPE_PACKAGE 0x04
#define ACPI_TYPE_LOCAL_REFERENCE 0x14
#define ACPI_ALLOCATE_BUFFER ((acpi_size)-1)
#define ACPI_FULL_PATHNAME 0
#define ACPI_SINGLE_NAME 1
#define AE_ERROR 0x0001
#define AE_NOT_FOUND 0x0005
#define AE_NOT_EXIST 0x0006
#define AE_SUPPORT 0x000F
#define ACPI_STATE_S0 0
#define ACPI_STATE_S3 3
#define ACPI_STATE_S4 4
#define ACPI_VIDEO_CLASS "video"
union acpi_object {
	acpi_object_type type;
	struct { acpi_object_type type; u64 value; } integer;
	struct { acpi_object_type type; u32 length; char *pointer; } string;
	struct { acpi_object_type type; u32 length; u8 *pointer; } buffer;
	struct { acpi_object_type type; u32 count; union acpi_object *elements; } package;
	struct { acpi_object_type type; acpi_object_type actual_type; acpi_handle handle; } reference;
	struct { acpi_object_type type; u32 proc_id; acpi_io_address pblk_address; u32 pblk_length; } processor;
	struct { acpi_object_type type; u32 system_level; u32 resource_order; } power_resource;
};
struct acpi_buffer { acpi_size length; void *pointer; };
struct acpi_object_list { u32 count; union acpi_object *pointer; };
struct acpi_bus_event { struct list_head node; char device_class[20]; char bus_id[15]; u32 type; u32 data; };
struct acpi_bus_type {
	struct list_head list;
	const char *name;
	bool (*match)(struct device *dev);
	struct acpi_device *(*find_companion)(struct device *dev);
	void (*setup)(struct device *dev);
};
struct acpi_resource;
#define ACPI_FREE(p) kfree(p)
#define register_acpi_bus_type(t) 0
#define unregister_acpi_bus_type(t) 0
#define to_acpi_device_node(fw) ((struct acpi_device *)NULL)
#define acpi_fwnode_handle(adev) ((struct fwnode_handle *)NULL)
#define acpi_device_handle(adev) ((acpi_handle)NULL)
#define acpi_find_child_device(adev, addr, check) ((struct acpi_device *)NULL)
#define acpi_get_handle(parent, path, ret) ((acpi_status)AE_NOT_FOUND)
#define acpi_get_name(h, type, buf) ((acpi_status)AE_NOT_FOUND)
#define acpi_evaluate_object(h, path, args, buf) ((acpi_status)AE_NOT_FOUND)
#define acpi_evaluate_integer(h, path, args, val) ((acpi_status)AE_NOT_FOUND)
#define acpi_evaluate_dsm_typed(h, guid, rev, func, argv4, type) ((union acpi_object *)NULL)
#define acpi_video_register_backlight() do { } while (0)
#define acpi_backlight_native 3
#define acpi_handle_info(h, fmt, ...) pr_info(fmt, ##__VA_ARGS__)
#define acpi_handle_debug(h, fmt, ...) pr_debug(fmt, ##__VA_ARGS__)
#define acpi_dev_get_resources(adev, list, fn, data) (-ENODEV)
#define acpi_dev_free_resource_list(list) do { } while (0)
#define acpi_dev_resource_memory(r, res) false
#define acpi_bus_generate_netlink_event(c, b, t, d) 0
#define acpi_reduced_hardware() false
#define ACPI_COMPANION_SET(dev, adev) do { } while (0)



#define __assign_bit(nr, addr, value) ((value) ? __set_bit(nr, addr) : __clear_bit(nr, addr))
#define time_after32(a, b) ((s32)((u32)(b) - (u32)(a)) < 0)
#define time_before32(b, a) time_after32(a, b)
#define WARN_RATELIMIT(condition, format, ...) WARN_ONCE(condition, format, ##__VA_ARGS__)
#define ktime_get_raw_fast_ns() ktime_get_mono_fast_ns()
#define seqprop_sequence(s) READ_ONCE((s)->sequence)
#define ratelimit_state_get_miss(rs) 0
#define want_init_on_free() false
#define current_is_kswapd() false
#define get_nr_swap_pages() 0L
#define force_o_largefile() true
#define pat_enabled() true
#define device_iommu_mapped(dev) false
#define fwnode_device_is_available(fw) false
#define device_get_next_child_node(dev, child) ((struct fwnode_handle *)NULL)
#define of_driver_match_device(dev, drv) 0
#define bus_find_device_by_of_node(bus, np) ((struct device *)NULL)
#define pnp_range_reserved(start, end) false
#define register_vmap_purge_notifier(nb) 0
#define unregister_vmap_purge_notifier(nb) 0
#define synchronize_srcu_expedited(ssp) synchronize_srcu(ssp)
#define queue_work_node(node, wq, work) queue_work(wq, work)
#define WQ_UNBOUND_MAX_ACTIVE 512
#define NOKPROBE_SYMBOL(fname)
extern unsigned int tsc_khz;
extern const struct sysfs_ops kobj_sysfs_ops;
#include <uapi/asm-generic/mman-common.h>
#ifndef MAP_SHARED
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#endif
#define symbol_put(x) do { } while (0)
#define single_open_size(file, show, data, size) single_open(file, show, data)
static inline void *memset_p(void **p, void *v, size_t n)
{
	for (size_t i = 0; i < n; i++) p[i] = v;
	return p;
}
static inline void seq_buf_clear(struct seq_buf *s) { s->len = 0; if (s->size) s->buffer[0] = 0; }
int hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf, size_t linebuflen,
		       bool ascii);
int _atomic_dec_and_lock_irqsave(atomic_t *atomic, spinlock_t *lock, unsigned long *flags);
#define atomic_dec_and_lock_irqsave(atomic, lock, flags) _atomic_dec_and_lock_irqsave(atomic, lock, &(flags))
int on_each_cpu(void (*func)(void *info), void *info, int wait);
int stop_machine(int (*fn)(void *), void *data, const struct cpumask *cpus);
#define stop_machine_cpuslocked stop_machine
bool synchronize_hardirq(unsigned int irq);
static inline struct pid *get_task_pid(struct task_struct *task, int type) { return task_tgid(task); }
#define PIDTYPE_PID 0
#define PIDTYPE_TGID 1


#define __copy_from_user_inatomic(to, from, n) copy_from_user(to, from, n)
#define __copy_to_user_inatomic(to, from, n) copy_to_user(to, from, n)
#define copy_from_user_inatomic_nontemporal(to, from, n) copy_from_user(to, from, n)
#define mmap_write_lock_killable(mm) down_write_killable(&(mm)->mmap_lock)


typedef struct { unsigned long pte; } pte_t;
#define __pte(x) ((pte_t){ (x) })
#define pte_val(x) ((x).pte)
#define _PAGE_SPECIAL_LKPI (1UL << 9)
#define LKPI_PTE_PFN_MASK 0x000ffffffffff000UL
static inline pte_t pfn_pte(unsigned long pfn, pgprot_t prot) { return __pte(((u64)pfn << PAGE_SHIFT) | pgprot_val(prot)); }
static inline pte_t pte_mkspecial(pte_t pte) { return __pte(pte_val(pte) | _PAGE_SPECIAL_LKPI); }
static inline unsigned long pte_pfn(pte_t pte) { return (pte_val(pte) & LKPI_PTE_PFN_MASK) >> PAGE_SHIFT; }
static inline int pte_present(pte_t pte) { return pte_val(pte) & _PAGE_PRESENT; }
static inline int pte_write(pte_t pte) { return !!(pte_val(pte) & _PAGE_RW); }
static inline void set_pte_at(struct mm_struct *mm, unsigned long addr, pte_t *ptep, pte_t pte) { *ptep = pte; }
static inline pte_t ptep_get(pte_t *ptep) { return *ptep; }
typedef int (*pte_fn_t)(pte_t *pte, unsigned long addr, void *data);
int apply_to_page_range(struct mm_struct *mm, unsigned long address, unsigned long size, pte_fn_t fn, void *data);
#define flush_cache_range(vma, start, end) do { } while (0)
#define fault_flag_allow_retry_first(flags) false
struct vma_iterator { unsigned long lkpi_index; };
#define VMA_ITERATOR(name, __mm, __addr) struct vma_iterator name = { .lkpi_index = (__addr) }
#define for_each_vma_range(__vmi, __vma, __end) for ((__vma) = NULL; (__vma); )
static inline void *kmap_local_page_prot(struct page *page, pgprot_t prot) { return kmap_local_page(page); }
#define VM_MAP_PUT_PAGES 0x00000200
#define VM_MAP 0x00000004
#define SWAP_CLUSTER_MAX 32UL
void copy_highpage(struct page *to, struct page *from);
void split_page(struct page *page, unsigned int order);
#define folio_test_writeback(folio) folio_test_Writeback(folio)
#define folio_set_reclaim(folio) set_bit(PG_reclaim, &(folio)->flags)
#define folio_clear_reclaim(folio) clear_bit(PG_reclaim, &(folio)->flags)
#define folio_test_reclaim(folio) test_bit(PG_reclaim, &(folio)->flags)
#define sg_set_folio(sg, folio, len, offset) sg_set_page(sg, &(folio)->page, len, offset)


#define MAX_LFS_FILESIZE ((loff_t)LLONG_MAX)
#define MAX_RW_COUNT (INT_MAX & PAGE_MASK)
#define ITER_SOURCE 1
#define ITER_DEST 0
#define SB_KERNMOUNT (1 << 22)
struct kiocb { struct file *ki_filp; loff_t ki_pos; int ki_flags; u16 ki_ioprio; void *private; };
struct iov_iter { u8 iter_type; bool nofault; bool data_source; size_t iov_offset; void __user *ubuf; size_t count; };
static inline void init_sync_kiocb(struct kiocb *kiocb, struct file *filp)
{
	*kiocb = (struct kiocb){ .ki_filp = filp };
}
static inline void iov_iter_ubuf(struct iov_iter *i, unsigned int direction, void __user *buf, size_t count)
{
	*i = (struct iov_iter){ .data_source = direction, .ubuf = buf, .count = count };
}
ssize_t kernel_write(struct file *file, const void *buf, size_t count, loff_t *pos);
int vfs_mmap(struct file *file, struct vm_area_struct *vma);
struct file *get_file_active(struct file **f);
enum writeback_sync_modes { WB_SYNC_NONE, WB_SYNC_ALL };
struct writeback_control {
	long nr_to_write;
	long pages_skipped;
	loff_t range_start;
	loff_t range_end;
	enum writeback_sync_modes sync_mode;
	unsigned for_kupdate:1;
	unsigned for_background:1;
	unsigned for_reclaim:1;
	unsigned range_cyclic:1;
	unsigned for_sync:1;
};
#define writeback_iter(mapping, wbc, folio, error) ((struct folio *)NULL)
#define folio_redirty_for_writepage(wbc, folio) folio_mark_dirty(folio)
#define shmem_writeout(folio, plug, folio_list) 0
struct file_system_type;
struct fs_context;
#define get_fs_type(name) ((struct file_system_type *)NULL)
#define fs_context_for_mount(type, flags) ((struct fs_context *)ERR_PTR(-ENODEV))
#define vfs_parse_fs_string(fc, key, value) (-ENODEV)
#define fc_mount_longterm(fc) ((struct vfsmount *)ERR_PTR(-ENODEV))
#define put_fs_context(fc) do { } while (0)
#define put_filesystem(fs) do { } while (0)


struct ctl_table;
typedef int proc_handler(const struct ctl_table *ctl, int write, void *buffer, size_t *lenp, loff_t *ppos);
struct ctl_table {
	const char *procname;
	void *data;
	int maxlen;
	umode_t mode;
	proc_handler *proc_handler;
	void *extra1;
	void *extra2;
};
struct ctl_table_header;
extern const int sysctl_vals[];
#define SYSCTL_ZERO ((void *)&sysctl_vals[1])
#define SYSCTL_ONE ((void *)&sysctl_vals[2])
int proc_dointvec_minmax(const struct ctl_table *table, int write, void *buffer, size_t *lenp, loff_t *ppos);
int proc_dointvec(const struct ctl_table *table, int write, void *buffer, size_t *lenp, loff_t *ppos);
#define register_sysctl(path, table) ((struct ctl_table_header *)NULL)
#define unregister_sysctl_table(h) do { } while (0)


struct irq_data { unsigned int irq; void *chip_data; };
struct irq_chip {
	const char *name;
	void (*irq_mask)(struct irq_data *data);
	void (*irq_unmask)(struct irq_data *data);
	void (*irq_enable)(struct irq_data *data);
	void (*irq_disable)(struct irq_data *data);
	unsigned long flags;
};
struct irq_desc;
typedef void (*irq_flow_handler_t)(struct irq_desc *desc);
void handle_simple_irq(struct irq_desc *desc);
#define irq_alloc_desc(node) (-ENOSPC)
#define irq_free_desc(irq) do { } while (0)
#define irq_set_chip_and_handler_name(irq, chip, handle, name) do { } while (0)
#define irq_set_chip_data(irq, data) 0
#define generic_handle_irq(irq) (-EINVAL)
#define generic_handle_irq_safe(irq) (-EINVAL)


struct platform_device_info {
	struct device *parent;
	struct fwnode_handle *fwnode;
	bool of_node_reused;
	const char *name;
	int id;
	const struct resource *res;
	unsigned int num_res;
	const void *data;
	size_t size_data;
	u64 dma_mask;
	const struct property_entry *properties;
};
struct platform_device *platform_device_register_full(const struct platform_device_info *pdevinfo);


#define BUS_NOTIFY_ADD_DEVICE 0x00000001
#define BUS_NOTIFY_DEL_DEVICE 0x00000002
#define BUS_NOTIFY_REMOVED_DEVICE 0x00000003
#define BUS_NOTIFY_BIND_DRIVER 0x00000004
#define BUS_NOTIFY_BOUND_DRIVER 0x00000005
#define BUS_NOTIFY_UNBIND_DRIVER 0x00000006
#define BUS_NOTIFY_UNBOUND_DRIVER 0x00000007
#define BUS_NOTIFY_DRIVER_NOT_BOUND 0x00000008
#define bus_register_notifier(bus, nb) 0
#define bus_unregister_notifier(bus, nb) 0
#define component_add_typed(dev, ops, subcomponent) 0


typedef int suspend_state_t;
#define PM_SUSPEND_ON 0
#define PM_SUSPEND_TO_IDLE 1
#define PM_SUSPEND_STANDBY 2
#define PM_SUSPEND_MEM 3
#define pm_suspend_via_firmware() false
#define pm_suspend_via_s2idle() false
extern const char power_group_name[];
int pm_generic_suspend(struct device *dev);
int pm_generic_resume(struct device *dev);
int pm_generic_freeze(struct device *dev);
int pm_generic_thaw(struct device *dev);
int pm_generic_poweroff(struct device *dev);
int pm_generic_restore(struct device *dev);
int pm_generic_runtime_suspend(struct device *dev);
int pm_generic_runtime_resume(struct device *dev);


#define PCIBIOS_MIN_MEM 0x10000000UL
#define PCI_STD_RESOURCES 0
#define PCI_STD_RESOURCE_END 5
resource_size_t pcibios_align_resource(void *data, const struct resource *res, resource_size_t size,
				       resource_size_t align);
int pci_bus_alloc_resource(struct pci_bus *bus, struct resource *res, resource_size_t size, resource_size_t align,
			   resource_size_t min, unsigned long type_mask,
			   resource_size_t (*alignf)(void *, const struct resource *, resource_size_t, resource_size_t),
			   void *alignf_data);
#define pci_bus_for_each_resource(bus, res, ...) for ((res) = NULL; (res); )
int pci_bus_read_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 *val);
int pci_bus_write_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 val);


enum vga_switcheroo_state { VGA_SWITCHEROO_OFF, VGA_SWITCHEROO_ON, VGA_SWITCHEROO_NOT_FOUND };
enum vga_switcheroo_client_id {
	VGA_SWITCHEROO_UNKNOWN_ID = 0x1000,
	VGA_SWITCHEROO_IGD = 0,
	VGA_SWITCHEROO_DIS,
	VGA_SWITCHEROO_MAX_CLIENTS,
};
struct vga_switcheroo_client_ops {
	void (*set_gpu_state)(struct pci_dev *dev, enum vga_switcheroo_state);
	void (*reprobe)(struct pci_dev *dev);
	bool (*can_switch)(struct pci_dev *dev);
	void (*gpu_bound)(struct pci_dev *dev, enum vga_switcheroo_client_id);
};


struct gpio_desc;
enum gpiod_flags {
	GPIOD_ASIS = 0,
	GPIOD_IN = 1,
	GPIOD_OUT_LOW = 3,
	GPIOD_OUT_HIGH = 7,
	GPIOD_OUT_LOW_OPEN_DRAIN = 11,
	GPIOD_OUT_HIGH_OPEN_DRAIN = 15,
};
#define devm_gpiod_get(dev, con_id, flags) ((struct gpio_desc *)ERR_PTR(-ENOENT))
#define devm_gpiod_get_index(dev, con_id, idx, flags) ((struct gpio_desc *)ERR_PTR(-ENOENT))
#define devm_gpiod_get_optional(dev, con_id, flags) ((struct gpio_desc *)NULL)
#define gpiod_get(dev, con_id, flags) ((struct gpio_desc *)ERR_PTR(-ENOENT))
#define gpiod_get_index(dev, con_id, idx, flags) ((struct gpio_desc *)ERR_PTR(-ENOENT))
#define gpiod_put(desc) do { } while (0)
#define gpiod_set_value(desc, value) do { } while (0)
#define gpiod_set_value_cansleep(desc, value) do { } while (0)
#define gpiod_get_value(desc) 0
#define gpiod_direction_output(desc, value) (-ENODEV)
struct pinctrl;
struct pinctrl_map { const char *dev_name; const char *name; int type; const char *ctrl_dev_name; const char *group; const char *function; };
#define PIN_MAP_MUX_GROUP(dev, state, pinctrl, grp, func) \
	{ .dev_name = dev, .name = state, .ctrl_dev_name = pinctrl, .group = grp, .function = func }
#define pinctrl_register_mappings(maps, num) (-ENODEV)
#define pinctrl_unregister_mappings(maps) do { } while (0)
#define devm_pinctrl_get_select(dev, name) ((struct pinctrl *)ERR_PTR(-ENODEV))
struct acpi_resource_i2c_serialbus { u8 type; u16 slave_address; u32 connection_speed; };
#define i2c_acpi_get_i2c_resource(ares, i2c) false
#define i2c_acpi_find_adapter_by_handle(handle) ((struct i2c_adapter *)NULL)


struct cpufreq_cpuinfo { unsigned int max_freq; unsigned int min_freq; unsigned int transition_latency; };
struct cpufreq_policy { struct cpufreq_cpuinfo cpuinfo; unsigned int min; unsigned int max; unsigned int cur; unsigned int cpu; };
#define cpufreq_cpu_get(cpu) ((struct cpufreq_policy *)NULL)
#define cpufreq_cpu_put(policy) do { } while (0)
#define cpufreq_quick_get_max(cpu) 0U
#define cpufreq_quick_get(cpu) 0U


struct cec_notifier;
struct cec_connector_info { int type; };
#define cec_notifier_conn_register(dev, port_name, info) ((struct cec_notifier *)NULL)
#define cec_notifier_conn_unregister(n) do { } while (0)
#define cec_notifier_set_phys_addr(n, pa) do { } while (0)
#define cec_notifier_set_phys_addr_from_edid(n, edid) do { } while (0)
#define cec_notifier_phys_addr_invalidate(n) do { } while (0)
#define cec_fill_conn_info_from_drm(info, connector) do { } while (0)


#include <linux/shrinker.h>


extern struct resource intel_graphics_stolen_res;
void lkpi_intel_stolen_init(void);


struct pmu { struct module *module; const char *name; int type; };
struct perf_event;


struct rchan;
struct rchan_buf;
struct rchan_callbacks;
struct rchan *relay_open(const char *base_filename, struct dentry *parent, size_t subbuf_size, size_t n_subbufs,
			 const struct rchan_callbacks *cb, void *private_data);
void relay_close(struct rchan *chan);
void relay_flush(struct rchan *chan);
size_t relay_stats(struct rchan *chan, int flags);
void relay_subbufs_consumed(struct rchan *chan, unsigned int cpu, size_t consumed);
void relay_reset(struct rchan *chan);
int relay_buf_full(struct rchan_buf *buf);
size_t relay_switch_subbuf(struct rchan_buf *buf, size_t length);
extern const struct file_operations relay_file_operations;


enum pwm_polarity { PWM_POLARITY_NORMAL, PWM_POLARITY_INVERSED };
struct pwm_state { u64 period; u64 duty_cycle; enum pwm_polarity polarity; bool enabled; bool usage_power; };
struct pwm_device;
static inline struct pwm_device *pwm_get(struct device *dev, const char *con_id) { return ERR_PTR(-ENODEV); }
#define pwm_put(pwm) do { } while (0)
#define pwm_get_state(pwm, state) memset((state), 0, sizeof(*(state)))
#define pwm_is_enabled(pwm) false
#define pwm_apply_might_sleep(pwm, state) (-ENODEV)
#define pwm_init_state(pwm, state) memset((state), 0, sizeof(*(state)))
static inline unsigned int pwm_get_relative_duty_cycle(const struct pwm_state *state, unsigned int scale)
{
	if (!state->period) return 0;
	return DIV_ROUND_CLOSEST_ULL((u64)state->duty_cycle * scale, state->period);
}
static inline int pwm_set_relative_duty_cycle(struct pwm_state *state, unsigned int duty_cycle, unsigned int scale)
{
	if (!scale || duty_cycle > scale) return -EINVAL;
	state->duty_cycle = DIV_ROUND_CLOSEST_ULL((u64)duty_cycle * state->period, scale);
	return 0;
}


#define PM_QOS_DEFAULT_VALUE (-1)
struct pm_qos_request { s32 value; bool active; };
static inline void cpu_latency_qos_add_request(struct pm_qos_request *req, s32 value) { req->value = value; req->active = true; }
static inline void cpu_latency_qos_update_request(struct pm_qos_request *req, s32 value) { req->value = value; }
static inline void cpu_latency_qos_remove_request(struct pm_qos_request *req) { req->active = false; }
static inline bool cpu_latency_qos_request_active(struct pm_qos_request *req) { return req->active; }


#define VFM_MODEL_BIT 0
#define VFM_FAMILY_BIT 8
#define VFM_VENDOR_BIT 16
#define VFM_RSVD_BIT 24
#define VFM_MODEL_MASK GENMASK(VFM_FAMILY_BIT - 1, VFM_MODEL_BIT)
#define VFM_FAMILY_MASK GENMASK(VFM_VENDOR_BIT - 1, VFM_FAMILY_BIT)
#define VFM_VENDOR_MASK GENMASK(VFM_RSVD_BIT - 1, VFM_VENDOR_BIT)
#define VFM_MODEL(vfm) (((vfm) & VFM_MODEL_MASK) >> VFM_MODEL_BIT)
#define VFM_FAMILY(vfm) (((vfm) & VFM_FAMILY_MASK) >> VFM_FAMILY_BIT)
#define VFM_VENDOR(vfm) (((vfm) & VFM_VENDOR_MASK) >> VFM_VENDOR_BIT)
#define VFM_MAKE(_vendor, _family, _model) \
	(((_model) << VFM_MODEL_BIT) | ((_family) << VFM_FAMILY_BIT) | ((_vendor) << VFM_VENDOR_BIT))
struct x86_cpu_id {
	__u16 vendor;
	__u16 family;
	__u16 model;
	__u16 steppings;
	__u16 feature;
	__u16 flags;
	__u8 type;
	kernel_ulong_t driver_data;
};
#define X86_VENDOR_ANY 0xffff
#define X86_FAMILY_ANY 0
#define X86_MODEL_ANY 0
#define X86_STEPPING_ANY 0
#define X86_STEP_MIN 0
#define X86_STEP_MAX 0xf
#define X86_FEATURE_ANY 0
#define X86_CPU_TYPE_ANY 0
#define X86_CPU_ID_FLAG_ENTRY_VALID BIT(0)
#define X86_MATCH_CPU(_vendor, _family, _model, _steppings, _feature, _type, _data) \
	{ .vendor = _vendor, .family = _family, .model = _model, .steppings = _steppings, .feature = _feature, \
	  .flags = X86_CPU_ID_FLAG_ENTRY_VALID, .type = _type, .driver_data = (unsigned long)_data }
#define X86_MATCH_VFM(vfm, data) \
	X86_MATCH_CPU(VFM_VENDOR(vfm), VFM_FAMILY(vfm), VFM_MODEL(vfm), X86_STEPPING_ANY, X86_FEATURE_ANY, \
		      X86_CPU_TYPE_ANY, data)
#define X86_MATCH_VENDOR_FAM_MODEL(vendor, family, model, data) \
	X86_MATCH_CPU(X86_VENDOR_##vendor, family, model, X86_STEPPING_ANY, X86_FEATURE_ANY, X86_CPU_TYPE_ANY, data)
#define X86_MATCH_FEATURE(feature, data) \
	X86_MATCH_CPU(X86_VENDOR_ANY, X86_FAMILY_ANY, X86_MODEL_ANY, X86_STEPPING_ANY, feature, X86_CPU_TYPE_ANY, data)
const struct x86_cpu_id *x86_match_cpu(const struct x86_cpu_id *match);

#endif
