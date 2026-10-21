// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef _LKPI_PCI_H
#define _LKPI_PCI_H

#include <lkpi/device.h>
#include <uapi/linux/pci_regs.h>

#define PCI_ANY_ID (~0)
#define DEVICE_COUNT_RESOURCE 17
#define PCI_STD_NUM_BARS 6
#define PCI_ROM_RESOURCE 6
#define PCI_BRIDGE_RESOURCES 7
#define PCI_NUM_RESOURCES 11
#define PCI_IOV_RESOURCES 7

#define PCI_DEVFN(slot, func) ((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_SLOT(devfn) (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn) ((devfn) & 0x07)
#define PCI_BUS_NUM(x) (((x) >> 8) & 0xff)

#include <linux/pci_ids.h>

typedef int pci_power_t;
#define PCI_D0 ((pci_power_t)0)
#define PCI_D1 ((pci_power_t)1)
#define PCI_D2 ((pci_power_t)2)
#define PCI_D3hot ((pci_power_t)3)
#define PCI_D3cold ((pci_power_t)4)
#define PCI_UNKNOWN ((pci_power_t)5)
typedef unsigned int pci_channel_state_t;
#define pci_channel_io_normal ((pci_channel_state_t)1)
#define pci_channel_io_frozen ((pci_channel_state_t)2)
#define pci_channel_io_perm_failure ((pci_channel_state_t)3)
typedef unsigned int pci_ers_result_t;
#define PCI_ERS_RESULT_NONE ((pci_ers_result_t)1)
#define PCI_ERS_RESULT_CAN_RECOVER ((pci_ers_result_t)2)
#define PCI_ERS_RESULT_NEED_RESET ((pci_ers_result_t)3)
#define PCI_ERS_RESULT_DISCONNECT ((pci_ers_result_t)4)
#define PCI_ERS_RESULT_RECOVERED ((pci_ers_result_t)5)
enum pci_bus_speed { PCI_SPEED_UNKNOWN = 0xff, PCIE_SPEED_2_5GT = 0x14, PCIE_SPEED_5_0GT = 0x15,
		     PCIE_SPEED_8_0GT = 0x16, PCIE_SPEED_16_0GT = 0x17, PCIE_SPEED_32_0GT = 0x18,
		     PCIE_SPEED_64_0GT = 0x19 };
enum pcie_link_width { PCIE_LNK_WIDTH_RESRV = 0x00, PCIE_LNK_X1 = 0x01, PCIE_LNK_X2 = 0x02, PCIE_LNK_X4 = 0x04,
		       PCIE_LNK_X8 = 0x08, PCIE_LNK_X12 = 0x0c, PCIE_LNK_X16 = 0x10, PCIE_LNK_X32 = 0x20,
		       PCIE_LNK_WIDTH_UNKNOWN = 0xff };

struct pci_bus {
	struct list_head node;
	struct pci_bus *parent;
	struct list_head devices;
	struct pci_dev *self;
	unsigned char number;
	unsigned char primary;
	int domain_nr;
	unsigned int bus_flags;
	struct device dev;
	enum pci_bus_speed max_bus_speed;
	enum pci_bus_speed cur_bus_speed;
};

struct pci_driver;
struct pci_dev {
	struct list_head bus_list;
	struct pci_bus *bus;
	struct pci_bus *subordinate;
	void *sysdata;
	unsigned int devfn;
	unsigned short vendor;
	unsigned short device;
	unsigned short subsystem_vendor;
	unsigned short subsystem_device;
	unsigned int class;
	u8 revision;
	u8 hdr_type;
	u8 pcie_cap;
	u8 msi_cap;
	u8 msix_cap;
	u8 rom_base_reg;
	u8 pin;
	u16 pcie_flags_reg;
	u8 pm_cap;
	unsigned int d3hot_delay;
	unsigned int d3cold_delay;
	struct pci_driver *driver;
	u64 dma_mask;
	struct device_dma_parameters dma_parms;
	pci_power_t current_state;
	pci_channel_state_t error_state;
	struct device dev;
	int cfg_size;
	unsigned int irq;
	struct resource resource[DEVICE_COUNT_RESOURCE];
	bool match_driver;
	unsigned int transparent:1;
	unsigned int multifunction:1;
	unsigned int is_busmaster:1;
	unsigned int no_msi:1;
	unsigned int no_64bit_msi:1;
	unsigned int msi_enabled:1;
	unsigned int msix_enabled:1;
	unsigned int is_virtfn:1;
	unsigned int is_physfn:1;
	unsigned int is_hotplug_bridge:1;
	unsigned int is_thunderbolt:1;
	unsigned int untrusted:1;
	unsigned int external_facing:1;
	unsigned int broken_intx_masking:1;
	unsigned int ats_enabled:1;
	unsigned int pasid_enabled:1;
	unsigned int pri_enabled:1;
	unsigned int no_vf_scan:1;
	unsigned int is_managed:1;
	unsigned int skip_bus_pm:1;
	unsigned int dev_flags;
	atomic_t enable_cnt;
	u32 saved_config_space[16];
	bool state_saved;
	u16 saved_pcie[4];
	phys_addr_t rom;
	size_t romlen;
	struct pci_dev *physfn;
	void *luos_dev;
	int lkpi_irq_base;
	int lkpi_irq_count;
	int *lkpi_irqs;
	int lkpi_intx_irq;
	void *lkpi_rom_copy;
};
#define to_pci_dev(n) container_of(n, struct pci_dev, dev)
#define dev_is_pci(d) lkpi_dev_is_pci(d)
bool lkpi_dev_is_pci(const struct device *dev);
extern const struct bus_type pci_bus_type;
#define for_each_pci_dev(d) while ((d = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, d)) != NULL)
#define pci_dev_lkpi_present 1

struct pci_device_id {
	__u32 vendor, device;
	__u32 subvendor, subdevice;
	__u32 class, class_mask;
	kernel_ulong_t driver_data;
	__u32 override_only;
};
#define PCI_DEVICE(vend, dev) .vendor = (vend), .device = (dev), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_DEVICE_SUB(vend, dev, subvend, subdev) .vendor = (vend), .device = (dev), .subvendor = (subvend), .subdevice = (subdev)
#define PCI_DEVICE_CLASS(dev_class, dev_class_mask) .class = (dev_class), .class_mask = (dev_class_mask), \
	.vendor = PCI_ANY_ID, .device = PCI_ANY_ID, .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_VDEVICE(vend, dev) .vendor = PCI_VENDOR_ID_##vend, .device = (dev), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, 0, 0
#define PCI_DEVICE_DATA(vend, dev, data) .vendor = PCI_VENDOR_ID_##vend, .device = PCI_DEVICE_ID_##vend##_##dev, \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, 0, 0, .driver_data = (kernel_ulong_t)(data)

struct pci_error_handlers {
	pci_ers_result_t (*error_detected)(struct pci_dev *dev, pci_channel_state_t error);
	pci_ers_result_t (*mmio_enabled)(struct pci_dev *dev);
	pci_ers_result_t (*slot_reset)(struct pci_dev *dev);
	void (*reset_prepare)(struct pci_dev *dev);
	void (*reset_done)(struct pci_dev *dev);
	void (*resume)(struct pci_dev *dev);
	void (*cor_error_detected)(struct pci_dev *dev);
};
struct pci_driver {
	const char *name;
	const struct pci_device_id *id_table;
	int (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
	void (*remove)(struct pci_dev *dev);
	int (*suspend)(struct pci_dev *dev, pm_message_t state);
	int (*resume)(struct pci_dev *dev);
	void (*shutdown)(struct pci_dev *dev);
	int (*sriov_configure)(struct pci_dev *dev, int num_vfs);
	const struct pci_error_handlers *err_handler;
	const struct attribute_group **groups;
	const struct attribute_group **dev_groups;
	struct device_driver driver;
	bool driver_managed_dma;
	struct list_head lkpi_node;
};
#define to_pci_driver(drv) container_of_const(drv, struct pci_driver, driver)
int __pci_register_driver(struct pci_driver *drv, struct module *owner, const char *mod_name);
#define pci_register_driver(driver) __pci_register_driver(driver, THIS_MODULE, KBUILD_MODNAME)
void pci_unregister_driver(struct pci_driver *dev);
#define module_pci_driver(__pci_driver) \
	static int __init __pci_driver##_init(void) { return pci_register_driver(&(__pci_driver)); } \
	module_init(__pci_driver##_init)
static inline void *pci_get_drvdata(struct pci_dev *pdev) { return dev_get_drvdata(&pdev->dev); }
static inline void pci_set_drvdata(struct pci_dev *pdev, void *data) { dev_set_drvdata(&pdev->dev, data); }
static inline const char *pci_name(const struct pci_dev *pdev) { return dev_name(&pdev->dev); }
static inline int pci_domain_nr(struct pci_bus *bus) { return bus->domain_nr; }
#define PCI_DEVID(bus, devfn) ((((u16)(bus)) << 8) | (devfn))
static inline u16 pci_dev_id(struct pci_dev *dev) { return PCI_DEVID(dev->bus->number, dev->devfn); }

int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val);
int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val);
int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val);
int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val);
int pci_write_config_word(const struct pci_dev *dev, int where, u16 val);
int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val);
int pci_bus_read_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 *val);
int pci_bus_read_config_word(struct pci_bus *bus, unsigned int devfn, int where, u16 *val);
int pci_bus_write_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 val);
#define PCIBIOS_SUCCESSFUL 0x00
#define PCIBIOS_DEVICE_NOT_FOUND 0x86
int pcibios_err_to_errno(int err);
int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val);
int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val);
int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val);
int pcie_capability_write_dword(struct pci_dev *dev, int pos, u32 val);
int pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos, u16 clear, u16 set);
int pcie_capability_clear_and_set_dword(struct pci_dev *dev, int pos, u32 clear, u32 set);
#define pcie_capability_set_word(dev, pos, set) pcie_capability_clear_and_set_word(dev, pos, 0, set)
#define pcie_capability_clear_word(dev, pos, clear) pcie_capability_clear_and_set_word(dev, pos, clear, 0)
#define pcie_capability_set_dword(dev, pos, set) pcie_capability_clear_and_set_dword(dev, pos, 0, set)
#define pcie_capability_clear_dword(dev, pos, clear) pcie_capability_clear_and_set_dword(dev, pos, clear, 0)
u8 pci_find_capability(struct pci_dev *dev, int cap);
u8 pci_find_next_capability(struct pci_dev *dev, u8 pos, int cap);
u16 pci_find_ext_capability(struct pci_dev *dev, int cap);
u16 pci_find_next_ext_capability(struct pci_dev *dev, u16 pos, int cap);
static inline bool pci_is_pcie(struct pci_dev *dev) { return !!dev->pcie_cap; }
static inline int pci_pcie_type(const struct pci_dev *dev) { return (dev->pcie_flags_reg & PCI_EXP_FLAGS_TYPE) >> 4; }
static inline bool pci_is_root_bus(struct pci_bus *pbus) { return !(pbus->parent || pbus->self); }
struct pci_dev *pci_upstream_bridge(struct pci_dev *dev);
struct pci_dev *pcie_find_root_port(struct pci_dev *dev);
static inline bool pci_is_bridge(struct pci_dev *dev) { return dev->hdr_type == 1 || dev->hdr_type == 2; }
static inline bool pci_is_thunderbolt_attached(struct pci_dev *pdev) { return false; }
#define dev_is_removable_lkpi(d) false
#define pci_is_enabled(pdev) (atomic_read(&(pdev)->enable_cnt) > 0)
#define pci_dev_is_disconnected(pdev) false
#define pci_dev_present(ids) lkpi_pci_dev_present(ids)
int lkpi_pci_dev_present(const struct pci_device_id *ids);
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids, struct pci_dev *dev);

int pci_enable_device(struct pci_dev *dev);
int pci_enable_device_mem(struct pci_dev *dev);
int pcim_enable_device(struct pci_dev *pdev);
void pci_disable_device(struct pci_dev *dev);
void pci_set_master(struct pci_dev *dev);
void pci_clear_master(struct pci_dev *dev);
int pci_request_region(struct pci_dev *pdev, int bar, const char *name);
void pci_release_region(struct pci_dev *pdev, int bar);
int pci_request_regions(struct pci_dev *pdev, const char *name);
void pci_release_regions(struct pci_dev *pdev);
int pci_request_selected_regions(struct pci_dev *pdev, int bars, const char *name);
void pci_release_selected_regions(struct pci_dev *pdev, int bars);
int pcim_request_all_regions(struct pci_dev *pdev, const char *name);
int pcim_request_region(struct pci_dev *pdev, int bar, const char *name);
void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long max);
void __iomem *pci_iomap_range(struct pci_dev *dev, int bar, unsigned long offset, unsigned long maxlen);
void __iomem *pci_iomap_wc(struct pci_dev *dev, int bar, unsigned long max);
void pci_iounmap(struct pci_dev *dev, void __iomem *p);
void __iomem *pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen);
void __iomem *pcim_iomap_region(struct pci_dev *pdev, int bar, const char *name);
void __iomem *const *pcim_iomap_table(struct pci_dev *pdev);
int pcim_iomap_regions(struct pci_dev *pdev, int mask, const char *name);
void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size);
void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom);
resource_size_t pci_bus_address(struct pci_dev *pdev, int bar);
void __iomem *pci_platform_rom(struct pci_dev *pdev, size_t *size);
#define pci_resource_n(dev, bar) (&(dev)->resource[(bar)])
#define pci_resource_start(dev, bar) (pci_resource_n(dev, bar)->start)
#define pci_resource_end(dev, bar) (pci_resource_n(dev, bar)->end)
#define pci_resource_flags(dev, bar) (pci_resource_n(dev, bar)->flags)
#define pci_resource_len(dev, bar) \
	(pci_resource_end((dev), (bar)) ? resource_size(pci_resource_n((dev), (bar))) : 0)
int pci_resize_resource(struct pci_dev *dev, int i, int size, int exclude_bars);
int pci_rebar_get_possible_sizes_lkpi(struct pci_dev *pdev, int bar);
#define pci_rebar_get_possible_sizes(pdev, bar) pci_rebar_get_possible_sizes_lkpi(pdev, bar)
#define pci_rebar_bytes_to_size(bytes) (max(ilog2(bytes), 20) - 20)
#define pci_rebar_size_to_bytes(size) (1ULL << ((size) + 20))
void pci_release_resource(struct pci_dev *dev, int resno);
int pci_assign_unassigned_bus_resources_lkpi(struct pci_bus *bus);
#define pci_assign_unassigned_bus_resources(bus) do { } while (0)
int pci_save_state(struct pci_dev *dev);
void pci_restore_state(struct pci_dev *dev);
struct pci_saved_state { u32 config_space[16]; u16 pcie[4]; };
struct pci_saved_state *pci_store_saved_state(struct pci_dev *dev);
int pci_load_saved_state(struct pci_dev *dev, struct pci_saved_state *state);
int pci_load_and_free_saved_state(struct pci_dev *dev, struct pci_saved_state **state);
int pci_wait_for_pending_transaction(struct pci_dev *dev);
void pci_restore_msi_state(struct pci_dev *dev);
int pci_enable_rom(struct pci_dev *pdev);
void pci_disable_rom(struct pci_dev *pdev);
#define PCI_ERROR_RESPONSE (~0ULL)
#define PCI_SET_ERROR_RESPONSE(val) (*(val) = ((typeof(*(val))) PCI_ERROR_RESPONSE))
#define PCI_POSSIBLE_ERROR(val) ((val) == ((typeof(val)) PCI_ERROR_RESPONSE))
int pci_set_power_state(struct pci_dev *dev, pci_power_t state);
#define pci_wake_from_d3(dev, enable) 0
#define pci_prepare_to_sleep(dev) 0
#define pci_back_from_sleep(dev) 0
#define pci_choose_state(dev, state) PCI_D3hot
#define pci_ignore_hotplug(dev) do { } while (0)
#define pci_d3cold_disable(dev) do { } while (0)
#define pci_d3cold_enable(dev) do { } while (0)
int pci_reset_function(struct pci_dev *dev);
int pci_reset_function_locked(struct pci_dev *dev);
int pci_try_reset_function(struct pci_dev *dev);
int pci_reset_bus(struct pci_dev *dev);
#define pci_dev_lock(dev) device_lock(&(dev)->dev)
#define pci_dev_unlock(dev) device_unlock(&(dev)->dev)
#define pci_cfg_access_lock(dev) do { } while (0)
#define pci_cfg_access_unlock(dev) do { } while (0)
#define pci_intx(dev, enable) lkpi_pci_intx(dev, enable)
void lkpi_pci_intx(struct pci_dev *dev, int enable);
#define pci_enable_pcie_error_reporting(d) 0
#define pci_disable_pcie_error_reporting(d) 0
#define pci_enable_atomic_ops_to_root(dev, cap) (-EINVAL)
#define pci_pr3_present(pdev) false
#define pci_bus_max_busnr(bus) 255
#define pci_platform_pm_ops_lkpi 0
#define pci_is_dev_assigned(d) false
#define pci_num_vf(d) 0
#define pci_sriov_get_totalvfs(d) 0
#define pci_enable_sriov(d, n) (-ENODEV)
#define pci_disable_sriov(d) do { } while (0)
#define pci_vfs_assigned(d) 0
#define pci_iov_vf_id(d) 0
#define pci_physfn(d) (d)
#define pcie_aspm_enabled(d) false
#define pci_disable_link_state(d, s) 0
#define pcie_get_speed_cap(d) PCIE_SPEED_8_0GT
#define pcie_get_width_cap(d) PCIE_LNK_X16
#define pcie_bandwidth_available(d, sp, w, bw) 0
#define pcie_print_link_status(d) do { } while (0)
#define pcie_get_readrq(d) 512
#define pcie_set_readrq(d, r) 0
#define pcie_get_mps(d) 256
#define pci_get_dsn(d) 0ULL
#define pcie_link_speed_lkpi 0
#define PCIE_SPEED2STR(s) ""

struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from);
struct pci_dev *pci_get_subsys(unsigned int vendor, unsigned int device, unsigned int ss_vendor, unsigned int ss_device,
			       struct pci_dev *from);
struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from);
struct pci_dev *pci_get_base_class(unsigned int class, struct pci_dev *from);
struct pci_dev *pci_get_slot(struct pci_bus *bus, unsigned int devfn);
struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn);
struct pci_dev *pci_dev_get(struct pci_dev *dev);
void pci_dev_put(struct pci_dev *dev);
DEFINE_FREE(pci_dev_put, struct pci_dev *, if (_T) pci_dev_put(_T))

#define PCI_IRQ_INTX (1 << 0)
#define PCI_IRQ_LEGACY PCI_IRQ_INTX
#define PCI_IRQ_MSI (1 << 1)
#define PCI_IRQ_MSIX (1 << 2)
#define PCI_IRQ_AFFINITY (1 << 3)
#define PCI_IRQ_ALL_TYPES (PCI_IRQ_INTX | PCI_IRQ_MSI | PCI_IRQ_MSIX)
int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags);
void pci_free_irq_vectors(struct pci_dev *dev);
int pci_irq_vector(struct pci_dev *dev, unsigned int nr);
static inline int pci_alloc_irq_vectors_affinity(struct pci_dev *dev, unsigned int min_vecs, unsigned int max_vecs,
						 unsigned int flags, struct irq_affinity *affd)
{
	return pci_alloc_irq_vectors(dev, min_vecs, max_vecs, flags & ~PCI_IRQ_AFFINITY);
}
#define pci_irq_get_affinity(dev, nr) ((const struct cpumask *)NULL)
bool pci_device_is_present(struct pci_dev *pdev);
static inline bool pci_is_vga(struct pci_dev *pdev)
{
	return (pdev->class >> 8) == PCI_CLASS_DISPLAY_VGA || (pdev->class >> 8) == 0x0001;
}
static inline bool pci_is_display(struct pci_dev *pdev)
{
	return (pdev->class >> 16) == PCI_BASE_CLASS_DISPLAY;
}
#define pci_iov_get_pf_drvdata(dev, pf_driver) ERR_PTR(-EOPNOTSUPP)
int pci_enable_msi(struct pci_dev *dev);
void pci_disable_msi(struct pci_dev *dev);
int pci_msi_vec_count(struct pci_dev *dev);
int pci_msix_vec_count(struct pci_dev *dev);
#define pci_dev_msi_enabled(pdev) ((pdev)->msi_enabled || (pdev)->msix_enabled)

#define pci_acpi_lkpi 0
#define PCI_DEVICE_ID_VGA_PCI_LKPI 0

#define VGA_RSRC_NONE 0x00
#define VGA_RSRC_LEGACY_IO 0x01
#define VGA_RSRC_LEGACY_MEM 0x02
#define VGA_RSRC_LEGACY_MASK (VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM)
#define VGA_RSRC_NORMAL_IO 0x04
#define VGA_RSRC_NORMAL_MEM 0x08
#define vga_get_interruptible(pdev, rsrc) 0
#define vga_get_uninterruptible(pdev, rsrc) 0
#define vga_put(pdev, rsrc) do { } while (0)
#define vga_set_legacy_decoding(pdev, decodes) do { } while (0)
#define vga_client_register(pdev, set_decode) 0
#define vga_client_unregister(pdev) do { } while (0)
#define vga_default_device() ((struct pci_dev *)lkpi_vga_default_device())
void *lkpi_vga_default_device(void);
#define vga_set_default_device(pdev) do { } while (0)
#define vga_remove_vgacon(pdev) 0
#define vga_tryget(pdev, rsrc) 0
#define vga_switcheroo_register_client(pdev, ops, d) 0
#define vga_switcheroo_unregister_client(pdev) do { } while (0)
#define vga_switcheroo_client_fb_set(pdev, info) do { } while (0)
#define vga_switcheroo_handler_flags() 0
#define vga_switcheroo_process_delayed_switch() 0
#define vga_switcheroo_init_domain_pm_ops(dev, dom) (-EINVAL)
#define vga_switcheroo_fini_domain_pm_ops(dev) do { } while (0)
#define vga_switcheroo_lock_ddc(pdev) do { } while (0)
#define vga_switcheroo_unlock_ddc(pdev) 0
#define vga_switcheroo_client_probe_defer(pdev) false
#define VGA_SWITCHEROO_CAN_SWITCH_DDC 1
#define VGA_SWITCHEROO_NEEDS_EDP_CONFIG 2
struct vga_switcheroo_client_ops;

#endif
