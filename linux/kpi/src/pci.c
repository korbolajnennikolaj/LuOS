// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"

static int pci_bus_match(struct device *dev, const struct device_driver *drv) { return 0; }
const struct bus_type pci_bus_type = { .name = "pci", .match = pci_bus_match };

static LIST_HEAD(pci_buses);
struct list_head lkpi_pci_devices = LIST_HEAD_INIT(lkpi_pci_devices);
struct list_head lkpi_pci_drivers = LIST_HEAD_INIT(lkpi_pci_drivers);
static DEFINE_MUTEX(pci_lock);
static DEFINE_MUTEX(pci_bind_lock);

bool lkpi_dev_is_pci(const struct device *dev) { return dev && dev->bus == &pci_bus_type; }


static int cfg_read(const struct pci_dev *dev, int where, int width, u32 *val)
{
        if (where < 0 || where + width > dev->cfg_size) {
                *val = ~0u;
                return PCIBIOS_DEVICE_NOT_FOUND;
        }
        *val = luos_pci_cfg_read(dev->luos_dev, where, width);
        return PCIBIOS_SUCCESSFUL;
}

static int cfg_write(const struct pci_dev *dev, int where, int width, u32 val)
{
        if (where < 0 || where + width > dev->cfg_size) return PCIBIOS_DEVICE_NOT_FOUND;
        luos_pci_cfg_write(dev->luos_dev, where, width, val);
        return PCIBIOS_SUCCESSFUL;
}

int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
        u32 v;
        int r = cfg_read(dev, where, 1, &v);
        *val = v;
        return r;
}

int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{
        u32 v;
        int r = cfg_read(dev, where, 2, &v);
        *val = v;
        return r;
}

int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val) { return cfg_read(dev, where, 4, val); }
int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val) { return cfg_write(dev, where, 1, val); }
int pci_write_config_word(const struct pci_dev *dev, int where, u16 val) { return cfg_write(dev, where, 2, val); }
int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val) { return cfg_write(dev, where, 4, val); }

static struct pci_dev *find_dev(int domain, unsigned int bus, unsigned int devfn)
{
        struct pci_dev *d;
        list_for_each_entry(d, &lkpi_pci_devices, bus_list)
                if (d->bus->domain_nr == domain && d->bus->number == bus && d->devfn == devfn) return d;
        return NULL;
}

int pci_bus_read_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 *val)
{
        struct pci_dev *d = find_dev(bus->domain_nr, bus->number, devfn);
        if (!d) {
                *val = ~0u;
                return PCIBIOS_DEVICE_NOT_FOUND;
        }
        return pci_read_config_dword(d, where, val);
}

int pci_bus_read_config_word(struct pci_bus *bus, unsigned int devfn, int where, u16 *val)
{
        struct pci_dev *d = find_dev(bus->domain_nr, bus->number, devfn);
        if (!d) {
                *val = 0xffff;
                return PCIBIOS_DEVICE_NOT_FOUND;
        }
        return pci_read_config_word(d, where, val);
}

int pci_bus_write_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 val)
{
        struct pci_dev *d = find_dev(bus->domain_nr, bus->number, devfn);
        return d ? pci_write_config_dword(d, where, val) : PCIBIOS_DEVICE_NOT_FOUND;
}

int pcibios_err_to_errno(int err)
{
        if (err <= PCIBIOS_SUCCESSFUL) return err;
        return err == PCIBIOS_DEVICE_NOT_FOUND ? -ENODEV : -EIO;
}

int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
        *val = 0;
        if (!dev->pcie_cap) return 0;
        return pci_read_config_word(dev, dev->pcie_cap + pos, val);
}

int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val)
{
        *val = 0;
        if (!dev->pcie_cap) return 0;
        return pci_read_config_dword(dev, dev->pcie_cap + pos, val);
}

int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val)
{
        if (!dev->pcie_cap) return 0;
        return pci_write_config_word(dev, dev->pcie_cap + pos, val);
}

int pcie_capability_write_dword(struct pci_dev *dev, int pos, u32 val)
{
        if (!dev->pcie_cap) return 0;
        return pci_write_config_dword(dev, dev->pcie_cap + pos, val);
}

int pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos, u16 clear, u16 set)
{
        u16 v;
        int r = pcie_capability_read_word(dev, pos, &v);
        if (r) return r;
        return pcie_capability_write_word(dev, pos, (v & ~clear) | set);
}

int pcie_capability_clear_and_set_dword(struct pci_dev *dev, int pos, u32 clear, u32 set)
{
        u32 v;
        int r = pcie_capability_read_dword(dev, pos, &v);
        if (r) return r;
        return pcie_capability_write_dword(dev, pos, (v & ~clear) | set);
}

static u8 find_cap_from(struct pci_dev *dev, u8 ptr, int cap)
{
        u8 id, next;
        int ttl = 48;
        pci_read_config_byte(dev, ptr, &next);
        while (ttl-- && next >= 0x40) {
                next &= ~3;
                pci_read_config_byte(dev, next, &id);
                if (id == 0xff) break;
                if (id == cap) return next;
                pci_read_config_byte(dev, next + PCI_CAP_LIST_NEXT, &next);
        }
        return 0;
}

u8 pci_find_next_capability(struct pci_dev *dev, u8 pos, int cap)
{
        return find_cap_from(dev, pos + PCI_CAP_LIST_NEXT, cap);
}

u8 pci_find_capability(struct pci_dev *dev, int cap)
{
        u16 status;
        pci_read_config_word(dev, PCI_STATUS, &status);
        if (!(status & PCI_STATUS_CAP_LIST)) return 0;
        return find_cap_from(dev, PCI_CAPABILITY_LIST, cap);
}

u16 pci_find_next_ext_capability(struct pci_dev *dev, u16 start, int cap)
{
        u32 header;
        int ttl = (4096 - 256) / 8;
        u16 pos = start ? start : 0x100;
        if (dev->cfg_size <= 256) return 0;
        if (pci_read_config_dword(dev, pos, &header) || !header) return 0;
        if (start) {
                pos = PCI_EXT_CAP_NEXT(header);
                if (pos < 0x100 || pci_read_config_dword(dev, pos, &header)) return 0;
        }
        while (ttl-- > 0) {
                if (PCI_EXT_CAP_ID(header) == cap && pos != start) return pos;
                pos = PCI_EXT_CAP_NEXT(header);
                if (pos < 0x100) break;
                if (pci_read_config_dword(dev, pos, &header)) break;
        }
        return 0;
}

u16 pci_find_ext_capability(struct pci_dev *dev, int cap) { return pci_find_next_ext_capability(dev, 0, cap); }

struct pci_dev *pci_upstream_bridge(struct pci_dev *dev) { return dev->bus ? dev->bus->self : NULL; }

struct pci_dev *pcie_find_root_port(struct pci_dev *dev)
{
        while (dev) {
                if (pci_is_pcie(dev) && pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT) return dev;
                dev = pci_upstream_bridge(dev);
        }
        return NULL;
}


const struct pci_device_id *pci_match_id(const struct pci_device_id *ids, struct pci_dev *dev)
{
        if (!ids) return NULL;
        for (; ids->vendor || ids->subvendor || ids->class_mask; ids++) {
                if (ids->override_only) continue;
                if ((ids->vendor == PCI_ANY_ID || ids->vendor == dev->vendor) &&
                    (ids->device == PCI_ANY_ID || ids->device == dev->device) &&
                    (ids->subvendor == PCI_ANY_ID || ids->subvendor == dev->subsystem_vendor) &&
                    (ids->subdevice == PCI_ANY_ID || ids->subdevice == dev->subsystem_device) &&
                    !((ids->class ^ dev->class) & ids->class_mask))
                        return ids;
        }
        return NULL;
}

int lkpi_pci_dev_present(const struct pci_device_id *ids)
{
        struct pci_dev *d;
        list_for_each_entry(d, &lkpi_pci_devices, bus_list)
                if (pci_match_id(ids, d)) return 1;
        return 0;
}

static void pci_try_probe(struct pci_dev *pdev, struct pci_driver *drv)
{
        const struct pci_device_id *id;
        bool took_sysfb = false;
        if (pdev->driver) return;
        id = pci_match_id(drv->id_table, pdev);
        if (!id) return;
        if (pci_is_display(pdev)) took_sysfb = sysfb_disable(&pdev->dev);
        pdev->driver = drv;
        pdev->dev.driver = &drv->driver;
        dev_info(&pdev->dev, "probing with driver %s\n", drv->name);
        lkpi_set_stage("probing %s with driver %s", pci_name(pdev), drv->name);
        int ret = drv->probe(pdev, id);
        if (ret) {
                pdev->driver = NULL;
                pdev->dev.driver = NULL;
                devres_release_all(&pdev->dev);
                if (ret != -ENODEV) dev_warn(&pdev->dev, "probe with driver %s failed with error %d\n", drv->name, ret);
                if (took_sysfb) lkpi_sysfb_init();
                return;
        }
        lkpi_device_generation++;
        dev_info(&pdev->dev, "bound to driver %s\n", drv->name);
}

int __pci_register_driver(struct pci_driver *drv, struct module *owner, const char *mod_name)
{
        struct pci_dev *pdev;
        drv->driver.name = drv->name;
        drv->driver.bus = &pci_bus_type;
        drv->driver.owner = owner;
        drv->driver.mod_name = mod_name;
        struct pci_dev **devs;
        int n = 0;
        mutex_lock(&pci_lock);
        list_add_tail(&drv->lkpi_node, &lkpi_pci_drivers);
        list_for_each_entry(pdev, &lkpi_pci_devices, bus_list) n++;
        devs = kcalloc(n ? n : 1, sizeof(*devs), GFP_KERNEL);
        n = 0;
        if (devs)
                list_for_each_entry(pdev, &lkpi_pci_devices, bus_list) devs[n++] = pci_dev_get(pdev);
        mutex_unlock(&pci_lock);
        if (!devs) return -ENOMEM;
        for (int i = 0; i < n; i++) {
                if (!lkpi_gpu_autoinit && (devs[i]->class >> 16) == PCI_BASE_CLASS_DISPLAY) {
                        dev_info(&devs[i]->dev, "probe deferred to gpu-init (gpu.autoinit is off)\n");
                        pci_dev_put(devs[i]);
                        continue;
                }
                mutex_lock(&pci_bind_lock);
                pci_try_probe(devs[i], drv);
                mutex_unlock(&pci_bind_lock);
                pci_dev_put(devs[i]);
        }
        kfree(devs);
        return 0;
}

void pci_unregister_driver(struct pci_driver *drv)
{
        struct pci_dev *pdev;
        mutex_lock(&pci_bind_lock);
        list_for_each_entry(pdev, &lkpi_pci_devices, bus_list) {
                if (pdev->driver != drv) continue;
                if (drv->remove) drv->remove(pdev);
                pdev->driver = NULL;
                pdev->dev.driver = NULL;
                devres_release_all(&pdev->dev);
                lkpi_device_generation++;
        }
        mutex_unlock(&pci_bind_lock);
        mutex_lock(&pci_lock);
        list_del(&drv->lkpi_node);
        mutex_unlock(&pci_lock);
}


static void pci_set_command(struct pci_dev *dev, u16 clear, u16 set)
{
        u16 cmd;
        pci_read_config_word(dev, PCI_COMMAND, &cmd);
        u16 n = (cmd & ~clear) | set;
        if (n != cmd) pci_write_config_word(dev, PCI_COMMAND, n);
}

static int do_enable(struct pci_dev *dev, bool io)
{
        u16 set = 0;
        for (int i = 0; i < PCI_STD_NUM_BARS; i++) {
                unsigned long f = dev->resource[i].flags;
                if (f & IORESOURCE_MEM) set |= PCI_COMMAND_MEMORY;
                if ((f & IORESOURCE_IO) && io) set |= PCI_COMMAND_IO;
        }
        if (dev->resource[PCI_ROM_RESOURCE].flags) set |= PCI_COMMAND_MEMORY;
        pci_set_command(dev, 0, set);
        if (!dev->lkpi_intx_irq && dev->pin) {
                int v = luos_pci_intx_vector(dev->luos_dev);
                dev->lkpi_intx_irq = v >= 32 ? v : -1;
        }
        if (!dev->irq && !dev->lkpi_irq_count && dev->lkpi_intx_irq >= 32) {
                dev->irq = dev->lkpi_intx_irq;
                pci_set_command(dev, PCI_COMMAND_INTX_DISABLE, 0);
        }
        atomic_inc(&dev->enable_cnt);
        return 0;
}

int pci_enable_device(struct pci_dev *dev) { return do_enable(dev, true); }
int pci_enable_device_mem(struct pci_dev *dev) { return do_enable(dev, false); }

resource_size_t pci_bus_address(struct pci_dev *pdev, int bar)
{
	return pci_resource_start(pdev, bar);
}


static void pcim_disable(void *data) { pci_disable_device(data); }

int pcim_enable_device(struct pci_dev *pdev)
{
        int ret = pci_enable_device(pdev);
        if (ret) return ret;
        pdev->is_managed = 1;
        return devm_add_action_or_reset(&pdev->dev, pcim_disable, pdev);
}

void pci_disable_device(struct pci_dev *dev)
{
        if (atomic_dec_return(&dev->enable_cnt) > 0) return;
        atomic_set(&dev->enable_cnt, 0);
        pci_clear_master(dev);
}

void pci_set_master(struct pci_dev *dev)
{
        pci_set_command(dev, 0, PCI_COMMAND_MASTER);
        dev->is_busmaster = 1;
}

void pci_clear_master(struct pci_dev *dev)
{
        pci_set_command(dev, PCI_COMMAND_MASTER, 0);
        dev->is_busmaster = 0;
}

void lkpi_pci_intx(struct pci_dev *dev, int enable)
{
        if (enable) pci_set_command(dev, PCI_COMMAND_INTX_DISABLE, 0);
        else pci_set_command(dev, 0, PCI_COMMAND_INTX_DISABLE);
}

int pci_request_region(struct pci_dev *pdev, int bar, const char *name) { return 0; }
void pci_release_region(struct pci_dev *pdev, int bar) { }
int pci_request_regions(struct pci_dev *pdev, const char *name) { return 0; }
void pci_release_regions(struct pci_dev *pdev) { }
int pci_request_selected_regions(struct pci_dev *pdev, int bars, const char *name) { return 0; }
void pci_release_selected_regions(struct pci_dev *pdev, int bars) { }

bool pci_device_is_present(struct pci_dev *pdev)
{
        u32 v = 0;
        pci_read_config_dword(pdev, PCI_VENDOR_ID, &v);
        return v != 0xffffffff && (v & 0xffff) != 0xffff;
}
int pcim_request_all_regions(struct pci_dev *pdev, const char *name) { return 0; }
int pcim_request_region(struct pci_dev *pdev, int bar, const char *name) { return 0; }


void __iomem *pci_iomap_range(struct pci_dev *dev, int bar, unsigned long offset, unsigned long maxlen)
{
        resource_size_t start = pci_resource_start(dev, bar);
        resource_size_t len = pci_resource_len(dev, bar);
        if (!len || offset >= len) return NULL;
        len -= offset;
        if (maxlen && len > maxlen) len = maxlen;
        if (pci_resource_flags(dev, bar) & IORESOURCE_IO) {
                dev_warn(&dev->dev, "port I/O BAR %d cannot be mapped\n", bar);
                return NULL;
        }
        return ioremap(start + offset, len);
}

void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long max) { return pci_iomap_range(dev, bar, 0, max); }

void __iomem *pci_iomap_wc(struct pci_dev *dev, int bar, unsigned long max)
{
        resource_size_t len = pci_resource_len(dev, bar);
        if (!len || (pci_resource_flags(dev, bar) & IORESOURCE_IO)) return NULL;
        if (max && len > max) len = max;
        return ioremap_wc(pci_resource_start(dev, bar), len);
}

void pci_iounmap(struct pci_dev *dev, void __iomem *p) { iounmap(p); }

struct pcim_iomap_devres {
        void __iomem *table[PCI_STD_NUM_BARS];
};

static void pcim_iomap_release(struct device *dev, void *res)
{
        struct pcim_iomap_devres *t = res;
        for (int i = 0; i < PCI_STD_NUM_BARS; i++)
                if (t->table[i]) iounmap(t->table[i]);
}

void __iomem *const *pcim_iomap_table(struct pci_dev *pdev)
{
        struct pcim_iomap_devres *t = devres_find(&pdev->dev, pcim_iomap_release, NULL, NULL);
        if (t) return t->table;
        t = devres_alloc(pcim_iomap_release, sizeof(*t), GFP_KERNEL | __GFP_ZERO);
        if (!t) return NULL;
        devres_add(&pdev->dev, t);
        return t->table;
}

void __iomem *pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
        void __iomem **tbl = (void __iomem **)pcim_iomap_table(pdev);
        if (!tbl || bar < 0 || bar >= PCI_STD_NUM_BARS) return NULL;
        if (!tbl[bar]) tbl[bar] = pci_iomap(pdev, bar, maxlen);
        return tbl[bar];
}

void __iomem *pcim_iomap_region(struct pci_dev *pdev, int bar, const char *name)
{
        void __iomem *p = pcim_iomap(pdev, bar, 0);
        return p ? p : IOMEM_ERR_PTR(-ENOMEM);
}

int pcim_iomap_regions(struct pci_dev *pdev, int mask, const char *name)
{
        for (int i = 0; i < PCI_STD_NUM_BARS; i++)
                if ((mask & (1 << i)) && pci_resource_len(pdev, i) && !pcim_iomap(pdev, i, 0)) return -ENOMEM;
        return 0;
}

static bool rom_valid(void __iomem *rom)
{
        return readb(rom) == 0x55 && readb(rom + 1) == 0xaa;
}

static int lkpi_flash_vbios_image(const u8 *base, size_t window, size_t off, u16 vendor, u16 device, size_t *img_len)
{
        const u8 *p = base + off;
        u16 pcir_off, len_units, hdr, list_off;
        int score;

        if (off + 0x200 > window) return 0;
        if (p[0] != 0x55 || p[1] != 0xaa) return 0;
        pcir_off = p[0x18] | (p[0x19] << 8);
        if (pcir_off < 0x1a || pcir_off > 0x1000 || off + pcir_off + 0x18 > window) return 0;
        if (p[pcir_off + 0] != 'P' || p[pcir_off + 1] != 'C' || p[pcir_off + 2] != 'I' || p[pcir_off + 3] != 'R')
                return 0;
        if ((p[pcir_off + 4] | (p[pcir_off + 5] << 8)) != vendor) return 0;
        if (p[pcir_off + 0x14] != 0) return 0;
        len_units = p[pcir_off + 0x10] | (p[pcir_off + 0x11] << 8);
        if (len_units < 32 || off + (size_t)len_units * 512 > window) return 0;
        hdr = p[0x48] | (p[0x49] << 8);
        if (hdr < 0x4a || hdr + 8 > (size_t)len_units * 512) return 0;
        if (memcmp(p + hdr + 4, "ATOM", 4) && memcmp(p + hdr + 4, "MOTA", 4)) return 0;
        *img_len = (size_t)len_units * 512;
        score = (p[pcir_off + 6] | (p[pcir_off + 7] << 8)) == device ? 2 : 1;
        list_off = p[pcir_off + 8] | (p[pcir_off + 9] << 8);
        if (score == 1 && p[pcir_off + 0x0c] >= 3 && list_off) {
                size_t at = (size_t)pcir_off + list_off;
                for (int i = 0; i < 64 && off + at + 2 <= window && at + 2 <= *img_len; i++, at += 2) {
                        u16 id = p[at] | (p[at + 1] << 8);
                        if (!id) break;
                        if (id == device) {
                                score = 2;
                                break;
                        }
                }
        }
        return score;
}

void lkpi_scan_flash_vbios(struct pci_dev *pdev)
{
        static const resource_size_t pass_starts[] = { 0xff000000ull, 0xfe000000ull };
        const resource_size_t top = 0x100000000ull;

        for (size_t i = 0; i < ARRAY_SIZE(pass_starts) && !pdev->rom; i++) {
                resource_size_t start = pass_starts[i], size = top - start;
                void __iomem *map = ioremap(start, size);
                if (!map) continue;
                for (size_t off = 0; off + 0x200 <= size; off += 512) {
                        size_t img_len = 0;
                        int score = lkpi_flash_vbios_image((const u8 __force *)map, size, off, pdev->vendor,
                                                           pdev->device, &img_len);
                        if (!score) continue;
                        if (score == 2 || !pdev->rom) {
                                pdev->rom = start + off;
                                pdev->romlen = img_len;
                                if (score == 2) break;
                        }
                }
                iounmap(map);
        }
        if (pdev->rom)
                dev_info(&pdev->dev, "VBIOS image (%zu KiB) found in the BIOS flash at 0x%llx\n", pdev->romlen / 1024,
                         (unsigned long long)pdev->rom);
}

static void __iomem *pci_rom_from_file(struct pci_dev *pdev, size_t *size)
{
        char names[3][48];
        snprintf(names[0], sizeof(names[0]), "vbios/%04x_%04x_%04x_%04x.rom", pdev->vendor, pdev->device,
                 pdev->subsystem_vendor, pdev->subsystem_device);
        snprintf(names[1], sizeof(names[1]), "vbios/%04x_%04x.rom", pdev->vendor, pdev->device);
        snprintf(names[2], sizeof(names[2]), "vbios/%s.rom", pci_name(pdev));
        for (int i = 0; i < 3; i++) {
                const void *data = NULL;
                unsigned long len = 0;
                void *handle = NULL;
                u8 *copy;
                if (luos_firmware_get(names[i], &data, &len, &handle)) continue;
                if (len < 0x200 || ((const u8 *)data)[0] != 0x55 || ((const u8 *)data)[1] != 0xaa) {
                        luos_firmware_put(handle);
                        dev_warn(&pdev->dev, "%s is not a PCI option ROM image\n", names[i]);
                        continue;
                }
                copy = kvmalloc(len, GFP_KERNEL);
                if (!copy) {
                        luos_firmware_put(handle);
                        return NULL;
                }
                memcpy(copy, data, len);
                luos_firmware_put(handle);
                kvfree(pdev->lkpi_rom_copy);
                pdev->lkpi_rom_copy = copy;
                *size = len;
                dev_info(&pdev->dev, "VBIOS image (%lu KiB) loaded from firmware file %s\n", len / 1024, names[i]);
                return (void __iomem __force *)copy;
        }
        return NULL;
}

void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size)
{
        struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];
        void __iomem *rom;
        if (resource_size(res) > 1 && res->start) {
                u32 bar;
                pci_read_config_dword(pdev, pdev->rom_base_reg, &bar);
                pci_write_config_dword(pdev, pdev->rom_base_reg, (u32)res->start | PCI_ROM_ADDRESS_ENABLE);
                rom = ioremap(res->start, resource_size(res));
                if (rom && rom_valid(rom)) {
                        *size = resource_size(res);
                        return rom;
                }
                if (rom) iounmap(rom);
                pci_write_config_dword(pdev, pdev->rom_base_reg, bar & ~PCI_ROM_ADDRESS_ENABLE);
        }
        if (luos_pci_is_boot_vga(pdev->luos_dev)) {
                rom = ioremap(0xc0000, 0x20000);
                if (rom && rom_valid(rom)) {
                        *size = 0x20000;
                        return rom;
                }
                if (rom) iounmap(rom);
        }
        return pci_rom_from_file(pdev, size);
}

void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom)
{
        struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];
        if (rom && (void __force *)rom == pdev->lkpi_rom_copy) {
                kvfree(pdev->lkpi_rom_copy);
                pdev->lkpi_rom_copy = NULL;
                return;
        }
        iounmap(rom);
        if (resource_size(res) > 1 && res->start) {
                u32 bar;
                pci_read_config_dword(pdev, pdev->rom_base_reg, &bar);
                pci_write_config_dword(pdev, pdev->rom_base_reg, bar & ~PCI_ROM_ADDRESS_ENABLE);
        }
}

void __iomem *pci_platform_rom(struct pci_dev *pdev, size_t *size) { return NULL; }

int pci_resize_resource(struct pci_dev *dev, int i, int size, int exclude_bars) { return -EOPNOTSUPP; }
int pci_rebar_get_possible_sizes_lkpi(struct pci_dev *pdev, int bar) { return 0; }
void pci_release_resource(struct pci_dev *dev, int resno) { }


static const u8 pcie_saved_regs[4] = { PCI_EXP_DEVCTL, PCI_EXP_LNKCTL, PCI_EXP_DEVCTL2, PCI_EXP_LNKCTL2 };

static bool pcie_has_ctl2(struct pci_dev *dev)
{
        u16 flags;
        pci_read_config_word(dev, dev->pcie_cap + PCI_EXP_FLAGS, &flags);
        return (flags & PCI_EXP_FLAGS_VERS) >= 2;
}

int pci_save_state(struct pci_dev *dev)
{
        for (int i = 0; i < 16; i++) pci_read_config_dword(dev, i * 4, &dev->saved_config_space[i]);
        if (dev->pcie_cap) {
                int n = pcie_has_ctl2(dev) ? 4 : 2;
                for (int i = 0; i < n; i++) pci_read_config_word(dev, dev->pcie_cap + pcie_saved_regs[i], &dev->saved_pcie[i]);
        }
        dev->state_saved = true;
        return 0;
}

void pci_restore_msi_state(struct pci_dev *dev)
{
        if (dev->msix_enabled) luos_pci_msix_enable(dev->luos_dev, dev->lkpi_irqs, dev->lkpi_irq_count);
        else if (dev->msi_enabled) luos_pci_msi_enable(dev->luos_dev, dev->lkpi_irqs[0]);
}

void pci_restore_state(struct pci_dev *dev)
{
        if (!dev->state_saved) return;
        for (int i = 15; i >= 0; i--) {
                u32 v;
                pci_read_config_dword(dev, i * 4, &v);
                if (v != dev->saved_config_space[i]) pci_write_config_dword(dev, i * 4, dev->saved_config_space[i]);
        }
        if (dev->pcie_cap) {
                int n = pcie_has_ctl2(dev) ? 4 : 2;
                for (int i = 0; i < n; i++) pci_write_config_word(dev, dev->pcie_cap + pcie_saved_regs[i], dev->saved_pcie[i]);
        }
        pci_restore_msi_state(dev);
        dev->state_saved = false;
}

struct pci_saved_state *pci_store_saved_state(struct pci_dev *dev)
{
        if (!dev->state_saved) return NULL;
        struct pci_saved_state *state = kzalloc(sizeof(*state), GFP_KERNEL);
        if (!state) return ERR_PTR(-ENOMEM);
        memcpy(state->config_space, dev->saved_config_space, sizeof(state->config_space));
        memcpy(state->pcie, dev->saved_pcie, sizeof(state->pcie));
        return state;
}

int pci_load_saved_state(struct pci_dev *dev, struct pci_saved_state *state)
{
        if (!state) return -EINVAL;
        memcpy(dev->saved_config_space, state->config_space, sizeof(state->config_space));
        memcpy(dev->saved_pcie, state->pcie, sizeof(state->pcie));
        dev->state_saved = true;
        return 0;
}

int pci_load_and_free_saved_state(struct pci_dev *dev, struct pci_saved_state **state)
{
        int ret = pci_load_saved_state(dev, *state);
        kfree(*state);
        *state = NULL;
        return ret;
}

int pci_wait_for_pending_transaction(struct pci_dev *dev)
{
        if (!pci_is_pcie(dev)) return 1;
        for (int i = 0; i < 4; i++) {
                u16 status;
                if (i) msleep((1 << (i - 1)) * 100);
                pci_read_config_word(dev, dev->pcie_cap + PCI_EXP_DEVSTA, &status);
                if (!(status & PCI_EXP_DEVSTA_TRPND)) return 1;
        }
        return 0;
}

int pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
        if (dev->pm_cap && state <= PCI_D3hot) {
                u16 pmcsr;
                pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr);
                pmcsr = (pmcsr & ~PCI_PM_CTRL_STATE_MASK) | (u16)state;
                pci_write_config_word(dev, dev->pm_cap + PCI_PM_CTRL, pmcsr);
                if (state == PCI_D0 || dev->current_state == PCI_D3hot) msleep(10);
        }
        dev->current_state = state;
        return 0;
}

int pci_reset_function(struct pci_dev *dev) { return -ENOTTY; }
int pci_reset_function_locked(struct pci_dev *dev) { return -ENOTTY; }
int pci_try_reset_function(struct pci_dev *dev) { return -ENOTTY; }
int pci_reset_bus(struct pci_dev *dev) { return -ENOTTY; }


static struct pci_dev *pci_iter(struct pci_dev *from, bool (*match)(struct pci_dev *, const void *), const void *arg)
{
        struct pci_dev *d, *found = NULL;
        bool started = from == NULL;
        mutex_lock(&pci_lock);
        list_for_each_entry(d, &lkpi_pci_devices, bus_list) {
                if (!started) {
                        if (d == from) started = true;
                        continue;
                }
                if (match(d, arg)) {
                        found = d;
                        break;
                }
        }
        mutex_unlock(&pci_lock);
        if (from) pci_dev_put(from);
        return found ? pci_dev_get(found) : NULL;
}

struct pci_subsys_match {
        unsigned int vendor, device, ss_vendor, ss_device;
};

static bool match_subsys(struct pci_dev *d, const void *arg)
{
        const struct pci_subsys_match *m = arg;
        return (m->vendor == (unsigned int)PCI_ANY_ID || m->vendor == d->vendor) &&
               (m->device == (unsigned int)PCI_ANY_ID || m->device == d->device) &&
               (m->ss_vendor == (unsigned int)PCI_ANY_ID || m->ss_vendor == d->subsystem_vendor) &&
               (m->ss_device == (unsigned int)PCI_ANY_ID || m->ss_device == d->subsystem_device);
}

struct pci_dev *pci_get_subsys(unsigned int vendor, unsigned int device, unsigned int ss_vendor, unsigned int ss_device,
                               struct pci_dev *from)
{
        struct pci_subsys_match m = { vendor, device, ss_vendor, ss_device };
        return pci_iter(from, match_subsys, &m);
}

struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from)
{
        return pci_get_subsys(vendor, device, PCI_ANY_ID, PCI_ANY_ID, from);
}

static bool match_class(struct pci_dev *d, const void *arg) { return d->class == *(const unsigned int *)arg; }
static bool match_base_class(struct pci_dev *d, const void *arg) { return (d->class >> 16) == *(const unsigned int *)arg; }

struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from) { return pci_iter(from, match_class, &class); }
struct pci_dev *pci_get_base_class(unsigned int class, struct pci_dev *from) { return pci_iter(from, match_base_class, &class); }

struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn)
{
        mutex_lock(&pci_lock);
        struct pci_dev *d = find_dev(domain, bus, devfn);
        mutex_unlock(&pci_lock);
        return d ? pci_dev_get(d) : NULL;
}

struct pci_dev *pci_get_slot(struct pci_bus *bus, unsigned int devfn)
{
        return pci_get_domain_bus_and_slot(bus->domain_nr, bus->number, devfn);
}

struct pci_dev *pci_dev_get(struct pci_dev *dev)
{
        if (dev) get_device(&dev->dev);
        return dev;
}

void pci_dev_put(struct pci_dev *dev)
{
        if (dev) put_device(&dev->dev);
}

void *lkpi_vga_default_device(void)
{
        struct pci_dev *d, *first = NULL;
        list_for_each_entry(d, &lkpi_pci_devices, bus_list) {
                if ((d->class >> 16) != PCI_BASE_CLASS_DISPLAY) continue;
                if (luos_pci_is_boot_vga(d->luos_dev)) return d;
                if (!first) first = d;
        }
        return first;
}


int pci_msi_vec_count(struct pci_dev *dev) { return dev->msi_cap ? luos_pci_msi_count(dev->luos_dev) : -EINVAL; }
int pci_msix_vec_count(struct pci_dev *dev) { return dev->msix_cap ? luos_pci_msix_count(dev->luos_dev) : -EINVAL; }

void pci_free_irq_vectors(struct pci_dev *dev)
{
        if (!dev->lkpi_irq_count) return;
        luos_pci_msi_disable(dev->luos_dev);
        for (int i = 0; i < dev->lkpi_irq_count; i++) luos_irq_free(dev->lkpi_irqs[i]);
        kfree(dev->lkpi_irqs);
        dev->lkpi_irqs = NULL;
        dev->lkpi_irq_count = 0;
        dev->msi_enabled = 0;
        dev->msix_enabled = 0;
        dev->irq = dev->lkpi_intx_irq >= 32 ? dev->lkpi_intx_irq : 0;
}

static int alloc_vectors(struct pci_dev *dev, int n)
{
        dev->lkpi_irqs = kcalloc(n, sizeof(int), GFP_KERNEL);
        if (!dev->lkpi_irqs) return -ENOMEM;
        for (int i = 0; i < n; i++) {
                int v = luos_irq_alloc();
                if (v < 0) {
                        for (int j = 0; j < i; j++) luos_irq_free(dev->lkpi_irqs[j]);
                        kfree(dev->lkpi_irqs);
                        dev->lkpi_irqs = NULL;
                        return -ENOSPC;
                }
                dev->lkpi_irqs[i] = v;
        }
        return 0;
}

int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags)
{
        if (dev->lkpi_irq_count) pci_free_irq_vectors(dev);
        if ((flags & PCI_IRQ_MSIX) && dev->msix_cap) {
                int n = min_t(int, max_vecs, luos_pci_msix_count(dev->luos_dev));
                if (n >= (int)min_vecs && n > 0 && !alloc_vectors(dev, n)) {
                        if (luos_pci_msix_enable(dev->luos_dev, dev->lkpi_irqs, n) == 0) {
                                dev->lkpi_irq_count = n;
                                dev->msix_enabled = 1;
                                dev->irq = dev->lkpi_irqs[0];
                                return n;
                        }
                        for (int i = 0; i < n; i++) luos_irq_free(dev->lkpi_irqs[i]);
                        kfree(dev->lkpi_irqs);
                        dev->lkpi_irqs = NULL;
                }
        }
        if ((flags & PCI_IRQ_MSI) && dev->msi_cap && min_vecs <= 1 && !alloc_vectors(dev, 1)) {
                if (luos_pci_msi_enable(dev->luos_dev, dev->lkpi_irqs[0]) == 0) {
                        dev->lkpi_irq_count = 1;
                        dev->msi_enabled = 1;
                        dev->irq = dev->lkpi_irqs[0];
                        return 1;
                }
                luos_irq_free(dev->lkpi_irqs[0]);
                kfree(dev->lkpi_irqs);
                dev->lkpi_irqs = NULL;
        }
        if ((flags & PCI_IRQ_INTX) && dev->irq >= 32 && min_vecs <= 1) return 1;
        return -ENOSPC;
}

int pci_irq_vector(struct pci_dev *dev, unsigned int nr)
{
        if (dev->lkpi_irq_count) return nr < (unsigned int)dev->lkpi_irq_count ? dev->lkpi_irqs[nr] : -EINVAL;
        return nr == 0 && dev->irq >= 32 ? (int)dev->irq : -EINVAL;
}

int pci_enable_msi(struct pci_dev *dev)
{
        int r = pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_MSI);
        return r < 0 ? r : 0;
}

void pci_disable_msi(struct pci_dev *dev) { pci_free_irq_vectors(dev); }


static struct pci_bus *get_bus(int domain, unsigned int number)
{
        struct pci_bus *b;
        list_for_each_entry(b, &pci_buses, node)
                if (b->domain_nr == domain && b->number == number) return b;
        b = kzalloc(sizeof(*b), GFP_KERNEL);
        if (!b) return NULL;
        INIT_LIST_HEAD(&b->devices);
        b->domain_nr = domain;
        b->number = number;
        b->max_bus_speed = PCI_SPEED_UNKNOWN;
        b->cur_bus_speed = PCI_SPEED_UNKNOWN;
        device_initialize(&b->dev);
        dev_set_name(&b->dev, "pci%04x:%02x", domain, number);
        list_add_tail(&b->node, &pci_buses);
        return b;
}

static void pci_dev_release(struct device *dev) { }

void lkpi_pci_init(void)
{
        int n = luos_pci_count();
        struct pci_dev **byindex = kcalloc(n ? n : 1, sizeof(*byindex), GFP_KERNEL);
        int *parents = kcalloc(n ? n : 1, sizeof(int), GFP_KERNEL);
        if (!byindex || !parents) return;

        for (int i = 0; i < n; i++) {
                struct luos_pci_info info;
                if (luos_pci_info(i, &info)) continue;
                struct pci_dev *d = kzalloc(sizeof(*d), GFP_KERNEL);
                if (!d) break;
                d->bus = get_bus(info.segment, info.bus);
                if (!d->bus) {
                        kfree(d);
                        break;
                }
                d->devfn = PCI_DEVFN(info.slot, info.func);
                d->vendor = info.vendor;
                d->device = info.device;
                d->subsystem_vendor = info.subsys_vendor;
                d->subsystem_device = info.subsys_device;
                d->class = info.class_code;
                d->revision = info.revision;
                d->hdr_type = info.header_type;
                d->pin = info.irq_pin;
                d->msi_cap = info.msi_cap;
                d->msix_cap = info.msix_cap;
                d->pcie_cap = info.pcie_cap;
                d->pm_cap = info.pm_cap;
                d->cfg_size = info.cfg_size ? info.cfg_size : 256;
                d->luos_dev = info.handle;
                d->rom_base_reg = d->hdr_type == 1 ? PCI_ROM_ADDRESS1 : PCI_ROM_ADDRESS;
                d->current_state = PCI_D0;
                d->error_state = pci_channel_io_normal;
                d->d3hot_delay = 10;
                d->d3cold_delay = 100;
                d->match_driver = true;
                d->dma_mask = DMA_BIT_MASK(32);
                if (d->pcie_cap) pci_read_config_word(d, d->pcie_cap + PCI_EXP_FLAGS, &d->pcie_flags_reg);

                for (int b = 0; b < 6; b++) {
                        unsigned int f = info.bar_flags[b];
                        struct resource *r = &d->resource[b];
                        if (!(f & LUOS_BAR_PRESENT) || (f & LUOS_BAR_UPPER) || !info.bar_size[b]) continue;
                        r->start = info.bar_base[b];
                        r->end = info.bar_base[b] + info.bar_size[b] - 1;
                        r->flags = (f & LUOS_BAR_IO) ? IORESOURCE_IO : IORESOURCE_MEM;
                        if (f & LUOS_BAR_PREFETCH) r->flags |= IORESOURCE_PREFETCH;
                        if (f & LUOS_BAR_MEM64) r->flags |= IORESOURCE_MEM_64;
                }
                if (info.rom_size) {
                        struct resource *r = &d->resource[PCI_ROM_RESOURCE];
                        r->start = info.rom_base;
                        r->end = info.rom_base + info.rom_size - 1;
                        r->flags = IORESOURCE_MEM | IORESOURCE_READONLY;
                }
                if (pci_is_display(d) && d->vendor == PCI_VENDOR_ID_ATI)
                        lkpi_scan_flash_vbios(d);

                device_initialize(&d->dev);
                d->dev.bus = &pci_bus_type;
                d->dev.release = pci_dev_release;
                d->dev.dma_mask = &d->dma_mask;
                d->dev.coherent_dma_mask = DMA_BIT_MASK(32);
                d->dev.dma_parms = &d->dma_parms;
                d->dma_parms.max_segment_size = UINT_MAX;
                dev_set_name(&d->dev, "%04x:%02x:%02x.%d", info.segment, info.bus, info.slot, info.func);
                for (int b = 0; b < 6; b++) d->resource[b].name = dev_name(&d->dev);
                d->dev.parent = &d->bus->dev;
                device_add(&d->dev);

                list_add_tail(&d->bus_list, &lkpi_pci_devices);
                byindex[i] = d;
                parents[i] = info.parent;
        }
        for (int i = 0; i < n; i++) {
                struct pci_dev *d = byindex[i];
                if (!d) continue;
                if (parents[i] >= 0 && parents[i] < n && byindex[parents[i]]) {
                        struct pci_dev *bridge = byindex[parents[i]];
                        d->bus->self = bridge;
                        d->bus->parent = bridge->bus;
                        bridge->subordinate = d->bus;
                }
        }
        kfree(byindex);
        kfree(parents);
        pr_info("LinuxKPI: %d PCI functions available to Linux drivers\n", n);
}

void lkpi_pci_list_lock(void) { mutex_lock(&pci_lock); }
void lkpi_pci_list_unlock(void) { mutex_unlock(&pci_lock); }

void lkpi_status(void (*emit)(const char *line))
{
        struct pci_dev *d;
        char line[160], stage[96];
        if (lkpi_startup_stage(stage, sizeof(stage))) {
                snprintf(line, sizeof(line), "Linux drivers are still starting: %s", stage);
                emit(line);
        }
        if (!mutex_trylock(&pci_lock)) {
                emit("PCI device list is busy");
                return;
        }
        int displays = 0, driven = 0;
        list_for_each_entry(d, &lkpi_pci_devices, bus_list) {
                bool display = (d->class >> 16) == PCI_BASE_CLASS_DISPLAY;
                if (display) {
                        displays++;
                        if (d->driver) driven++;
                }
                if (!d->driver && !display) continue;
                snprintf(line, sizeof(line), "%s %04x:%04x class %06x  %s%s", pci_name(d), d->vendor, d->device, d->class,
                         d->driver ? d->driver->name : "no Linux driver",
                         luos_pci_is_boot_vga(d->luos_dev) ? "  (boot display)" : "");
                emit(line);
        }
        mutex_unlock(&pci_lock);
        if (displays && !driven && !lkpi_gpu_autoinit)
                emit("GPU stack not started: run gpu-init (with a firmware PATH if needed); gpu.autoinit=1 probes at boot");
}


int lkpi_gpu_reinit(void)
{
        struct pci_dev *d;
        struct pci_driver *drv;
        struct pci_dev **devs;
        struct pci_driver *drvs[32];
        int n = 0, nd = 0, bound = 0;

        mutex_lock(&pci_lock);
        list_for_each_entry(d, &lkpi_pci_devices, bus_list)
                if ((d->class >> 16) == PCI_BASE_CLASS_DISPLAY) n++;
        list_for_each_entry(drv, &lkpi_pci_drivers, lkpi_node)
                if (nd < (int)ARRAY_SIZE(drvs)) drvs[nd++] = drv;
        devs = kmalloc((n ? n : 1) * sizeof(*devs), GFP_KERNEL);
        n = 0;
        if (devs)
                list_for_each_entry(d, &lkpi_pci_devices, bus_list)
                        if ((d->class >> 16) == PCI_BASE_CLASS_DISPLAY) devs[n++] = pci_dev_get(d);
        mutex_unlock(&pci_lock);
        if (!devs) return -ENOMEM;
        if (!nd) {
                kfree(devs);
                return 0;
        }

        for (int k = 0; k < nd; k++) {
                for (int i = 0; i < n; i++) {
                        if (devs[i]->driver) continue;
                        mutex_lock(&pci_bind_lock);
                        pci_try_probe(devs[i], drvs[k]);
                        mutex_unlock(&pci_bind_lock);
                }
        }
        for (int i = 0; i < n; i++) {
                if (devs[i]->driver) bound++;
                pci_dev_put(devs[i]);
        }
        kfree(devs);

        if (bound) lkpi_console_setup_unclaimed();
        return bound;
}

int lkpi_gpu_restart(int *unbound, int *bound)
{
        struct pci_dev *d;
        struct pci_dev **devs;
        int n = 0, detached = 0;

        mutex_lock(&pci_lock);
        list_for_each_entry(d, &lkpi_pci_devices, bus_list)
                if (d->driver && (d->class >> 16) == PCI_BASE_CLASS_DISPLAY) n++;
        devs = kmalloc((n ? n : 1) * sizeof(*devs), GFP_KERNEL);
        n = 0;
        if (devs)
                list_for_each_entry(d, &lkpi_pci_devices, bus_list)
                        if (d->driver && (d->class >> 16) == PCI_BASE_CLASS_DISPLAY) devs[n++] = pci_dev_get(d);
        mutex_unlock(&pci_lock);
        if (!devs) return -ENOMEM;

        mutex_lock(&pci_bind_lock);
        for (int i = 0; i < n; i++) {
                struct pci_driver *drv = devs[i]->driver;
                if (!drv) continue;
                if (!drv->remove) {
                        dev_info(&devs[i]->dev, "driver %s has no remove(), left bound\n", drv->name);
                        continue;
                }
                dev_info(&devs[i]->dev, "unbinding driver %s for the restart\n", drv->name);
                drv->remove(devs[i]);
                devs[i]->driver = NULL;
                devs[i]->dev.driver = NULL;
                devres_release_all(&devs[i]->dev);
                lkpi_device_generation++;
                detached++;
        }
        mutex_unlock(&pci_bind_lock);
        for (int i = 0; i < n; i++) pci_dev_put(devs[i]);
        kfree(devs);

        int r = lkpi_gpu_reinit();
        if (unbound) *unbound = detached;
        if (bound) *bound = r < 0 ? 0 : r;
        return r < 0 ? r : 0;
}

int lkpi_gpu_bind_one(const char *pci_name_)
{
        struct pci_dev *d;
        struct pci_driver *drvs[32];
        struct pci_driver *drv;
        struct pci_dev *target = NULL;
        int nd = 0;

        mutex_lock(&pci_lock);
        list_for_each_entry(d, &lkpi_pci_devices, bus_list) {
                if ((d->class >> 16) != PCI_BASE_CLASS_DISPLAY) continue;
                if (strcmp(pci_name(d), pci_name_) == 0) { target = pci_dev_get(d); break; }
        }
        list_for_each_entry(drv, &lkpi_pci_drivers, lkpi_node)
                if (nd < (int)ARRAY_SIZE(drvs)) drvs[nd++] = drv;
        mutex_unlock(&pci_lock);
        if (!target) return -ENXIO;
        if (!nd) { pci_dev_put(target); return -ENOENT; }

        for (int k = 0; k < nd && !target->driver; k++) {
                mutex_lock(&pci_bind_lock);
                pci_try_probe(target, drvs[k]);
                mutex_unlock(&pci_bind_lock);
        }
        int bound = target->driver ? 0 : -ENODEV;
        if (target->driver) lkpi_console_setup_unclaimed();
        pci_dev_put(target);
        return bound;
}

int lkpi_gpu_unbind_one(const char *pci_name_)
{
        struct pci_dev *d;
        struct pci_dev *target = NULL;

        mutex_lock(&pci_lock);
        list_for_each_entry(d, &lkpi_pci_devices, bus_list) {
                if ((d->class >> 16) != PCI_BASE_CLASS_DISPLAY) continue;
                if (strcmp(pci_name(d), pci_name_) == 0) { target = pci_dev_get(d); break; }
        }
        mutex_unlock(&pci_lock);
        if (!target) return -ENXIO;

        int r = 0;
        mutex_lock(&pci_bind_lock);
        struct pci_driver *drv = target->driver;
        if (!drv) {
                r = -ENOENT;
        } else if (!drv->remove) {
                dev_info(&target->dev, "driver %s has no remove(), left bound\n", drv->name);
                r = -EPERM;
        } else {
                dev_info(&target->dev, "unbinding driver %s (gpu-core)\n", drv->name);
                drv->remove(target);
                target->driver = NULL;
                target->dev.driver = NULL;
                devres_release_all(&target->dev);
                lkpi_device_generation++;
        }
        mutex_unlock(&pci_bind_lock);
        pci_dev_put(target);
        return r;
}

int pci_enable_rom(struct pci_dev *pdev)
{
        struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];
        u32 rom_addr;

        if (!res->flags) return -1;
        if (res->flags & IORESOURCE_ROM_SHADOW) return 0;
        pci_read_config_dword(pdev, pdev->rom_base_reg, &rom_addr);
        rom_addr &= ~PCI_ROM_ADDRESS_MASK;
        rom_addr |= (u32)res->start | PCI_ROM_ADDRESS_ENABLE;
        pci_write_config_dword(pdev, pdev->rom_base_reg, rom_addr);
        return 0;
}

void pci_disable_rom(struct pci_dev *pdev)
{
        struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];
        u32 rom_addr;

        if (res->flags & IORESOURCE_ROM_SHADOW) return;
        pci_read_config_dword(pdev, pdev->rom_base_reg, &rom_addr);
        rom_addr &= ~PCI_ROM_ADDRESS_ENABLE;
        pci_write_config_dword(pdev, pdev->rom_base_reg, rom_addr);
}
