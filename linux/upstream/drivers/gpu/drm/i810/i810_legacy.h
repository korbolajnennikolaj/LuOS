#ifndef _I810_LEGACY_H_
#define _I810_LEGACY_H_

#include <linux/agp_backend.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_print.h>

#define DRM_LEGACY_MAX_ORDER 12
#define DRM_MAX_BUFS 4096

struct drm_local_map {
	resource_size_t offset;
	unsigned long size;
	enum drm_map_type type;
	enum drm_map_flags flags;
	void *handle;
	int mtrr;
};

typedef struct drm_local_map drm_local_map_t;

struct drm_map_list {
	struct list_head head;
	struct drm_local_map *map;
	unsigned long user_token;
};

struct drm_buf {
	int idx;
	int total;
	int order;
	int used;
	unsigned long offset;
	dma_addr_t bus_address;
	struct drm_file *file_priv;
	int dma_dir;
	void *dev_private;
	void *virt;
	struct list_head head;
	int dev_priv_size;
};

struct drm_buf_entry {
	int buf_count;
	struct drm_buf **buflist;
	int seg_count;
	int page_order;
	unsigned long *seglist;
};

struct drm_device_dma {
	int buf_count;
	struct drm_buf **buflist;
	struct drm_buf_entry bufs[DRM_LEGACY_MAX_ORDER + 1];
	int seg_count;
	int page_count;
	unsigned long *pagelist;
	unsigned long byte_count;
	unsigned long flags;
};

#define _DRM_DMA_USE_AGP 0x01
#define _DRM_DMA_USE_SG 0x02
#define _DRM_DMA_USE_FB 0x04
#define _DRM_DMA_USE_PCI_RO 0x08

struct drm_agp_mem {
	struct list_head head;
	struct agp_memory *memory;
	int bound;
};

struct drm_agp_head {
	struct agp_kern_info agp_info;
	struct list_head memory;
	unsigned long mode;
	struct agp_bridge_data *bridge;
	int enabled;
	int acquired;
	int agp_mtrr;
	unsigned long base;
	int cant_use_aperture;
	int page_mask;
};

struct drm_legacy_state {
	struct list_head maplist;
	struct mutex map_mutex;
	struct drm_device_dma *dma;
	struct drm_agp_head *agp;
	struct drm_local_map *agp_buffer_map;
	unsigned long agp_buffer_token;
	struct list_head agp_memory;
	struct mutex buf_mutex;
	struct mutex agp_mutex;
	atomic_t open_count;
	pid_t lock_pid;
	struct drm_file *lock_master;
	spinlock_t lock_spin;
	int idlelock_cnt;
};

struct drm_legacy_driver {
	struct drm_driver base;
	void (*lastclose)(struct drm_device *dev);
	void (*preclose)(struct drm_device *dev, struct drm_file *file_priv);
	int (*dma_quiescent)(struct drm_device *dev);
	int dev_priv_size;
};

struct drm_legacy_state *drm_legacy_state(struct drm_device *dev);
struct drm_local_map *drm_legacy_findmap(struct drm_device *dev, unsigned long offset);
void drm_legacy_ioremap(struct drm_local_map *map, struct drm_device *dev);
void drm_legacy_ioremapfree(struct drm_local_map *map, struct drm_device *dev);
struct drm_agp_head *drm_legacy_agp_init(struct drm_device *dev);
void drm_legacy_agp_cleanup(struct drm_device *dev);
int drm_legacy_irq_uninstall(struct drm_device *dev);
void drm_legacy_idlelock_take(struct drm_device *dev, struct drm_file *file_priv);
void drm_legacy_idlelock_release(struct drm_device *dev, struct drm_file *file_priv);
int drm_legacy_lock_held(struct drm_device *dev, struct drm_file *file_priv);
int drm_legacy_pci_init(struct drm_legacy_driver *driver, struct pci_driver *pci_driver);
void drm_legacy_pci_exit(struct drm_legacy_driver *driver, struct pci_driver *pci_driver);
int drm_legacy_mmap(struct file *filp, struct vm_area_struct *vma);
int drm_legacy_open(struct inode *inode, struct file *filp);
int drm_legacy_release(struct inode *inode, struct file *filp);
long drm_legacy_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
void drm_legacy_driver_finish(struct drm_device *dev);

#define LOCK_TEST_WITH_RETURN(dev, file_priv) do {				\
	if (!drm_legacy_lock_held(dev, file_priv)) {				\
		DRM_ERROR("%s called without lock held\n", __func__);		\
		return -EINVAL;						\
	}									\
} while (0)

#endif
