#include "i810_legacy.h"

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/drm_ioctl.h>
#include <drm/intel/intel-gtt.h>

#include "agp.h"

static int drm_legacy_addmap(struct drm_device *dev, struct drm_map *request);
static int drm_legacy_addbufs_agp(struct drm_device *dev, struct drm_buf_desc *request);
static int drm_legacy_rmmap(struct drm_device *dev, struct drm_map *request);

int agp_off;
EXPORT_SYMBOL(agp_off);

static struct drm_legacy_driver *drm_legacy_active_driver;
static int drm_legacy_driver_priv_size;

static int drm_legacy_addmap_ioctl(struct drm_device *dev, void *data,
                                   struct drm_file *file_priv);
static int drm_legacy_addbufs_ioctl(struct drm_device *dev, void *data,
                                    struct drm_file *file_priv);
static int drm_legacy_rmmap_ioctl(struct drm_device *dev, void *data,
                                  struct drm_file *file_priv);

static const struct agp_version agp_default_version = { 3, 5 };

struct agp_bridge_data *agp_bridge;

static spinlock_t agp_key_lock = __SPIN_LOCK_UNLOCKED(agp_key_lock);

void global_cache_flush(void)
{
        wbinvd_on_all_cpus();
}

int agp_alloc_key(void)
{
        int key, i;

        if (!agp_bridge || !agp_bridge->key_list)
                return -1;
        spin_lock(&agp_key_lock);
        for (key = 0; key < MAXKEY; key++) {
                i = key / 32;
                if (!(agp_bridge->key_list[i] & (1 << (key % 32)))) {
                        agp_bridge->key_list[i] |= 1 << (key % 32);
                        spin_unlock(&agp_key_lock);
                        return key;
                }
        }
        spin_unlock(&agp_key_lock);
        return -1;
}

void agp_free_key(int key)
{
        if (!agp_bridge || !agp_bridge->key_list || key < 0 || key >= MAXKEY)
                return;
        spin_lock(&agp_key_lock);
        agp_bridge->key_list[key / 32] &= ~(1 << (key % 32));
        spin_unlock(&agp_key_lock);
}

void agp_alloc_page_array(size_t size, struct agp_memory *mem)
{
        mem->pages = kvcalloc(size / sizeof(struct page *), sizeof(struct page *), GFP_KERNEL);
}

struct agp_memory *agp_create_memory(int scratch_pages)
{
        struct agp_memory *new;

        if (!agp_bridge)
                return NULL;
        new = kzalloc(sizeof(*new), GFP_KERNEL);
        if (!new)
                return NULL;
        atomic_inc(&agp_bridge->current_memory_agp);
        new->key = agp_alloc_key();
        new->num_scratch_pages = scratch_pages;
        INIT_LIST_HEAD(&new->mapped_list);
        return new;
}

struct page *agp_generic_alloc_page(struct agp_bridge_data *bridge)
{
        struct page *page;

        page = alloc_page(GFP_KERNEL | GFP_DMA32 | __GFP_ZERO);
        if (page == NULL)
                return NULL;
        return page;
}

int agp_generic_alloc_pages(struct agp_bridge_data *bridge,
                            struct agp_memory *memory, size_t page_count)
{
        struct page *page;
        int i;

        agp_alloc_page_array(page_count * sizeof(struct page *), memory);
        if (memory->pages == NULL)
                return -ENOMEM;
        for (i = 0; i < (int)page_count; i++) {
                page = bridge->driver->agp_alloc_page ?
                        bridge->driver->agp_alloc_page(bridge) :
                        agp_generic_alloc_page(bridge);
                if (page == NULL) {
                        while (i--)
                                memory->pages[i] = NULL;
                        agp_free_page_array(memory);
                        return -ENOMEM;
                }
                memory->pages[i] = page;
        }
        memory->page_count = page_count;
        return 0;
}

void agp_generic_destroy_page(struct page *page, int flags)
{
        if (page == NULL)
                return;
        if (flags & AGP_PAGE_DESTROY_FREE)
                __free_page(page);
}

void agp_generic_destroy_pages(struct agp_memory *mem)
{
        int i;

        if (mem == NULL || mem->pages == NULL)
                return;
        for (i = 0; i < mem->page_count; i++) {
                if (mem->bridge && mem->bridge->driver && mem->bridge->driver->agp_destroy_page)
                        mem->bridge->driver->agp_destroy_page(mem->pages[i],
                                                              AGP_PAGE_DESTROY_UNMAP);
        }
        for (i = 0; i < mem->page_count; i++) {
                if (mem->bridge && mem->bridge->driver && mem->bridge->driver->agp_destroy_page)
                        mem->bridge->driver->agp_destroy_page(mem->pages[i],
                                                              AGP_PAGE_DESTROY_FREE);
        }
        agp_free_page_array(mem);
        mem->pages = NULL;
}

struct agp_memory *agp_allocate_memory(struct agp_bridge_data *bridge,
                                       size_t page_count, u32 type)
{
        struct agp_memory *new;
        int i;

        if (!bridge || !bridge->driver || page_count == 0)
                return NULL;
        if (atomic_read(&bridge->current_memory_agp) + (int)page_count >
            bridge->max_memory_agp)
                return NULL;
        if (type != AGP_NORMAL_MEMORY) {
                if (bridge->driver->alloc_by_type)
                        return bridge->driver->alloc_by_type(page_count, type);
                return NULL;
        }
        new = agp_create_memory(0);
        if (!new)
                return NULL;
        if (bridge->driver->agp_alloc_pages) {
                if (bridge->driver->agp_alloc_pages(bridge, new, page_count)) {
                        agp_free_key(new->key);
                        kfree(new);
                        return NULL;
                }
        } else {
                if (agp_generic_alloc_pages(bridge, new, page_count)) {
                        agp_free_key(new->key);
                        kfree(new);
                        return NULL;
                }
        }
        new->bridge = bridge;
        new->type = type;
        new->is_flushed = true;
        for (i = 0; i < (int)page_count; i++) {
                if (new->pages[i] == NULL) {
                        agp_generic_destroy_pages(new);
                        agp_free_key(new->key);
                        kfree(new);
                        return NULL;
                }
        }
        return new;
}

void agp_free_memory(struct agp_memory *mem)
{
        if (mem == NULL)
                return;
        if (mem->is_bound)
                agp_unbind_memory(mem);
        if (mem->type == AGP_NORMAL_MEMORY) {
                if (mem->bridge && mem->bridge->driver) {
                        if (mem->bridge->driver->agp_destroy_pages)
                                mem->bridge->driver->agp_destroy_pages(mem);
                        else
                                agp_generic_destroy_pages(mem);
                }
                agp_free_key(mem->key);
                atomic_dec(&mem->bridge->current_memory_agp);
                kfree(mem);
        } else {
                if (mem->bridge && mem->bridge->driver && mem->bridge->driver->free_by_type)
                        mem->bridge->driver->free_by_type(mem);
        }
}

int agp_bind_memory(struct agp_memory *mem, off_t pg_start)
{
        int ret;

        if (mem == NULL || pg_start < 0)
                return -EINVAL;
        if (mem->is_bound)
                return -EBUSY;
        if (mem->is_flushed == false) {
                if (mem->bridge->driver->cache_flush)
                        mem->bridge->driver->cache_flush();
                mem->is_flushed = true;
        }
        ret = mem->bridge->driver->insert_memory(mem, pg_start, mem->type);
        if (ret != 0)
                return ret;
        mem->is_bound = 1;
        mem->pg_start = pg_start;
        spin_lock(&mem->bridge->mapped_lock);
        list_add(&mem->mapped_list, &mem->bridge->mapped_list);
        spin_unlock(&mem->bridge->mapped_lock);
        return 0;
}

int agp_unbind_memory(struct agp_memory *mem)
{
        int ret;

        if (mem == NULL)
                return -EINVAL;
        if (!mem->is_bound)
                return -EINVAL;
        if (mem->is_flushed == false) {
                if (mem->bridge->driver->cache_flush)
                        mem->bridge->driver->cache_flush();
                mem->is_flushed = true;
        }
        ret = mem->bridge->driver->remove_memory(mem, mem->pg_start, mem->type);
        if (ret != 0)
                return ret;
        mem->is_bound = 0;
        spin_lock(&mem->bridge->mapped_lock);
        list_del(&mem->mapped_list);
        spin_unlock(&mem->bridge->mapped_lock);
        return 0;
}

void agp_enable(struct agp_bridge_data *bridge, u32 mode)
{
        if (!bridge || !bridge->driver)
                return;
        bridge->driver->agp_enable(bridge, mode);
}

int agp_copy_info(struct agp_bridge_data *bridge, struct agp_kern_info *info)
{
        if (!bridge || !bridge->driver)
                return -EINVAL;
        memset(info, 0, sizeof(*info));
        if (bridge->version)
                info->version = *bridge->version;
        info->device = bridge->dev;
        info->chipset = SUPPORTED;
        info->mode = bridge->mode;
        switch (bridge->driver->size_type) {
        case U8_APER_SIZE:
                info->aper_size = A_SIZE_8(bridge->current_size)->size;
                break;
        case U16_APER_SIZE:
                info->aper_size = A_SIZE_16(bridge->current_size)->size;
                break;
        case U32_APER_SIZE:
                info->aper_size = A_SIZE_32(bridge->current_size)->size;
                break;
        case FIXED_APER_SIZE:
                info->aper_size = A_SIZE_FIX(bridge->current_size)->size;
                break;
        default:
                return -EINVAL;
        }
        info->aper_base = bridge->gart_bus_addr;
        info->max_memory = bridge->max_memory_agp;
        info->current_memory = atomic_read(&bridge->current_memory_agp);
        info->cant_use_aperture = bridge->driver->cant_use_aperture;
        info->page_mask = ~0UL << PAGE_SHIFT;
        return 0;
}

struct agp_bridge_data *agp_backend_acquire(struct pci_dev *pdev)
{
        if (!agp_bridge || !agp_bridge->driver)
                return ERR_PTR(-ENODEV);
        if (atomic_read(&agp_bridge->agp_in_use))
                return ERR_PTR(-EBUSY);
        atomic_set(&agp_bridge->agp_in_use, 1);
        return agp_bridge;
}

void agp_backend_release(struct agp_bridge_data *bridge)
{
        if (bridge)
                atomic_set(&bridge->agp_in_use, 0);
}

static int agp_backend_initialize(struct agp_bridge_data *bridge)
{
        int size_val;

        if (!bridge->driver)
                return -EINVAL;
        if (!bridge->key_list) {
                bridge->key_list = kcalloc(MAXKEY / 32, sizeof(unsigned long), GFP_KERNEL);
                if (!bridge->key_list)
                        return -ENOMEM;
        }
        size_val = bridge->driver->fetch_size();
        if (size_val == 0)
                return -ENODEV;
        if (bridge->driver->create_gatt_table(bridge))
                return -EINVAL;
        if (bridge->driver->configure())
                return -EINVAL;
        INIT_LIST_HEAD(&bridge->mapped_list);
        spin_lock_init(&bridge->mapped_lock);
        if (bridge->max_memory_agp == 0)
                bridge->max_memory_agp =
                        (int)(A_SIZE_FIX(bridge->current_size)->num_entries);
        return 0;
}

static DEFINE_MUTEX(legacy_dev_lock);
static LIST_HEAD(legacy_dev_list);

struct drm_legacy_dev {
        struct list_head head;
        struct drm_device *dev;
        struct drm_legacy_state state;
};

struct drm_legacy_state *drm_legacy_state(struct drm_device *dev)
{
        struct drm_legacy_dev *ld;

        mutex_lock(&legacy_dev_lock);
        list_for_each_entry(ld, &legacy_dev_list, head) {
                if (ld->dev == dev) {
                        mutex_unlock(&legacy_dev_lock);
                        return &ld->state;
                }
        }
        mutex_unlock(&legacy_dev_lock);
        return NULL;
}

static struct drm_legacy_state *drm_legacy_state_create(struct drm_device *dev)
{
        struct drm_legacy_dev *ld;

        ld = kzalloc(sizeof(*ld), GFP_KERNEL);
        if (!ld)
                return NULL;
        ld->dev = dev;
        INIT_LIST_HEAD(&ld->state.maplist);
        mutex_init(&ld->state.map_mutex);
        mutex_init(&ld->state.buf_mutex);
        mutex_init(&ld->state.agp_mutex);
        INIT_LIST_HEAD(&ld->state.agp_memory);
        atomic_set(&ld->state.open_count, 0);
        spin_lock_init(&ld->state.lock_spin);
        mutex_lock(&legacy_dev_lock);
        list_add_tail(&ld->head, &legacy_dev_list);
        mutex_unlock(&legacy_dev_lock);
        return &ld->state;
}

static void drm_legacy_state_destroy(struct drm_device *dev)
{
        struct drm_legacy_dev *ld, *tmp;

        mutex_lock(&legacy_dev_lock);
        list_for_each_entry_safe(ld, tmp, &legacy_dev_list, head) {
                if (ld->dev == dev) {
                        list_del(&ld->head);
                        kfree(ld);
                }
        }
        mutex_unlock(&legacy_dev_lock);
}

struct drm_local_map *drm_legacy_findmap(struct drm_device *dev,
                                         unsigned long offset)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_map_list *r_list;

        if (!st)
                return NULL;
        list_for_each_entry(r_list, &st->maplist, head) {
                if (r_list->map && r_list->map->offset == offset)
                        return r_list->map;
        }
        return NULL;
}

static struct drm_map_list *drm_legacy_findmap_token(struct drm_device *dev,
                                                     unsigned long token)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_map_list *r_list;

        if (!st)
                return NULL;
        list_for_each_entry(r_list, &st->maplist, head) {
                if (r_list->user_token == token)
                        return r_list;
        }
        return NULL;
}

void drm_legacy_ioremap(struct drm_local_map *map, struct drm_device *dev)
{
        if (!map)
                return;
        if (map->type == _DRM_REGISTERS) {
                map->handle = ioremap(map->offset, map->size);
        } else if (map->type == _DRM_AGP) {
                if (map->flags & _DRM_WRITE_COMBINING)
                        map->handle = ioremap_wc(map->offset, map->size);
                else
                        map->handle = ioremap(map->offset, map->size);
        }
}

void drm_legacy_ioremapfree(struct drm_local_map *map, struct drm_device *dev)
{
        if (!map || !map->handle)
                return;
        if (map->type == _DRM_SHM)
                free_pages((unsigned long)map->handle, get_order(map->size));
        else
                iounmap(map->handle);
        map->handle = NULL;
}

static int drm_legacy_addmap(struct drm_device *dev, struct drm_map *request)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_map_list *entry;
        struct drm_local_map *map;

        if (!st)
                return -EINVAL;
        if (request->type < _DRM_REGISTERS || request->type > _DRM_AGP)
                return -EINVAL;
        if (request->type == _DRM_AGP && !st->agp)
                return -EINVAL;

        map = kzalloc(sizeof(*map), GFP_KERNEL);
        if (!map)
                return -ENOMEM;
        map->offset = request->offset;
        map->size = request->size;
        map->flags = request->flags;
        map->type = request->type;

        switch (map->type) {
        case _DRM_REGISTERS:
                break;
        case _DRM_SHM:
                map->handle = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
                                                       get_order(map->size));
                if (!map->handle) {
                        kfree(map);
                        return -ENOMEM;
                }
                break;
        case _DRM_AGP:
                if (st->agp && (map->offset + map->size >
                                st->agp->base + st->agp->agp_info.aper_size * 1024 * 1024)) {
                        kfree(map);
                        return -EINVAL;
                }
                map->handle = NULL;
                break;
        default:
                kfree(map);
                return -EINVAL;
        }

        entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) {
                if (map->type == _DRM_SHM)
                        free_pages((unsigned long)map->handle, get_order(map->size));
                kfree(map);
                return -ENOMEM;
        }
        entry->map = map;
        entry->user_token = map->offset;

        mutex_lock(&st->map_mutex);
        list_add_tail(&entry->head, &st->maplist);
        mutex_unlock(&st->map_mutex);

        request->handle = (void __user *)entry->user_token;
        return 0;
}

static int drm_legacy_rmmap(struct drm_device *dev, struct drm_map *request)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_map_list *r_list;

        if (!st)
                return -EINVAL;
        mutex_lock(&st->map_mutex);
        r_list = drm_legacy_findmap_token(dev, (unsigned long)request->handle);
        if (!r_list || !r_list->map) {
                mutex_unlock(&st->map_mutex);
                return -EINVAL;
        }
        if (r_list->map == st->agp_buffer_map)
                st->agp_buffer_map = NULL;
        list_del(&r_list->head);
        mutex_unlock(&st->map_mutex);
        kfree(r_list->map);
        kfree(r_list);
        return 0;
}

int drm_legacy_mmap(struct file *filp, struct vm_area_struct *vma)
{
        struct drm_file *priv = filp->private_data;
        struct drm_device *dev;
        struct drm_map_list *r_list;
        struct drm_local_map *map;
        unsigned long token = vma->vm_pgoff << PAGE_SHIFT;
        unsigned long size = vma->vm_end - vma->vm_start;
        int ret;

        if (!priv || !priv->minor)
                return -EINVAL;
        dev = priv->minor->dev;
        r_list = drm_legacy_findmap_token(dev, token);
        if (!r_list || !r_list->map)
                return -EINVAL;
        map = r_list->map;
        if (size > map->size)
                return -EINVAL;

        switch (map->type) {
        case _DRM_REGISTERS:
        case _DRM_AGP:
                vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
                ret = io_remap_pfn_range(vma, vma->vm_start,
                                         (unsigned long)map->offset >> PAGE_SHIFT,
                                         size, vma->vm_page_prot);
                break;
        case _DRM_SHM:
                vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
                ret = remap_pfn_range(vma, vma->vm_start,
                                      (unsigned long)virt_to_phys(map->handle) >> PAGE_SHIFT,
                                      size, vma->vm_page_prot);
                break;
        default:
                return -EINVAL;
        }
        return ret;
}

struct drm_agp_mem *drm_legacy_agp_lookup(struct drm_device *dev, int handle)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_agp_mem *m;

        if (!st)
                return NULL;
        list_for_each_entry(m, &st->agp_memory, head) {
                if (m->memory && m->memory->key == handle)
                        return m;
        }
        return NULL;
}

static int drm_legacy_addbufs_agp(struct drm_device *dev,
                                  struct drm_buf_desc *request)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_device_dma *dma;
        struct drm_buf_entry *entry;
        struct drm_buf *buf;
        unsigned long offset;
        unsigned long agp_offset;
        int count, order, size, alignment, i;
        struct drm_buf **temp_buflist;

        if (!st || !st->agp)
                return -EINVAL;
        count = request->count;
        order = get_order(request->size);
        if (order < 0 || order > DRM_LEGACY_MAX_ORDER)
                return -EINVAL;
        size = 1 << order;
        alignment = (request->flags & _DRM_PAGE_ALIGN) ?
                    PAGE_ALIGN(size) : size;
        if (count <= 0 || count > DRM_MAX_BUFS)
                return -EINVAL;
        if (!st->dma) {
                st->dma = kzalloc(sizeof(struct drm_device_dma), GFP_KERNEL);
                if (!st->dma)
                        return -ENOMEM;
        }

        dma = st->dma;
        entry = &dma->bufs[order];
        if (count + dma->buf_count > DRM_MAX_BUFS)
                return -ENOMEM;

        agp_offset = st->agp->base + request->agp_start;

        temp_buflist = krealloc(dma->buflist,
                                (dma->buf_count + count) * sizeof(struct drm_buf *),
                                GFP_KERNEL);
        if (!temp_buflist)
                return -ENOMEM;
        dma->buflist = temp_buflist;

        entry->buflist = krealloc(entry->buflist,
                                  (entry->buf_count + count) * sizeof(struct drm_buf *),
                                  GFP_KERNEL);
        if (!entry->buflist)
                return -ENOMEM;

        offset = 0;
        for (i = 0; i < count; i++) {
                buf = kzalloc(sizeof(*buf), GFP_KERNEL);
                if (!buf)
                        return -ENOMEM;
                buf->idx = dma->buf_count + i;
                buf->total = alignment;
                buf->order = order;
                buf->used = 0;
                buf->offset = offset;
                buf->bus_address = agp_offset + offset;
                buf->file_priv = NULL;
                if (drm_legacy_driver_priv_size > 0) {
                        buf->dev_private = kzalloc(drm_legacy_driver_priv_size, GFP_KERNEL);
                        if (!buf->dev_private) {
                                kfree(buf);
                                return -ENOMEM;
                        }
                        buf->dev_priv_size = drm_legacy_driver_priv_size;
                }
                buf->dma_dir = 0;
                buf->virt = NULL;

                offset += alignment;
                dma->buflist[dma->buf_count + i] = buf;
                entry->buflist[entry->buf_count + i] = buf;
        }
        dma->buf_count += count;
        entry->buf_count += count;
        dma->byte_count += PAGE_ALIGN(alignment) * count;
        dma->flags |= _DRM_DMA_USE_AGP;

        request->count = entry->buf_count;
        request->size = size;
        return 0;
}

int drm_legacy_lock_held(struct drm_device *dev, struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        int held;

        if (!st)
                return 0;
        spin_lock(&st->lock_spin);
        held = (st->lock_pid == task_pid_nr(current)) || (st->idlelock_cnt > 0);
        spin_unlock(&st->lock_spin);
        return held;
}

void drm_legacy_idlelock_take(struct drm_device *dev, struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);

        if (!st)
                return;
        spin_lock(&st->lock_spin);
        st->idlelock_cnt++;
        if (st->lock_pid == 0)
                st->lock_pid = task_pid_nr(current);
        spin_unlock(&st->lock_spin);
}

void drm_legacy_idlelock_release(struct drm_device *dev, struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);

        if (!st)
                return;
        spin_lock(&st->lock_spin);
        if (st->idlelock_cnt > 0)
                st->idlelock_cnt--;
        if (st->idlelock_cnt == 0 && st->lock_pid == task_pid_nr(current))
                st->lock_pid = 0;
        spin_unlock(&st->lock_spin);
}

static int drm_legacy_lock_ioctl(struct drm_device *dev, void *data,
                                 struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_lock *lock = data;
        unsigned long deadline;

        if (!st)
                return -EINVAL;
        deadline = jiffies + 3 * HZ;
        for (;;) {
                spin_lock(&st->lock_spin);
                if (st->lock_pid == 0 || st->lock_pid == task_pid_nr(current)) {
                        st->lock_pid = task_pid_nr(current);
                        st->lock_master = file_priv;
                        spin_unlock(&st->lock_spin);
                        return 0;
                }
                spin_unlock(&st->lock_spin);
                if (signal_pending(current))
                        return -EINTR;
                if (time_after(jiffies, deadline))
                        return -EINTR;
                schedule_timeout_interruptible(1);
        }
}

static int drm_legacy_unlock_ioctl(struct drm_device *dev, void *data,
                                   struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_lock *lock = data;

        if (!st)
                return -EINVAL;
        spin_lock(&st->lock_spin);
        if (st->lock_pid == task_pid_nr(current)) {
                st->lock_pid = 0;
                st->lock_master = NULL;
        }
        spin_unlock(&st->lock_spin);
        return 0;
}

static int drm_legacy_agp_acquire_ioctl(struct drm_device *dev, void *data,
                                        struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct agp_bridge_data *bridge;

        if (!st || !st->agp)
                return -EINVAL;
        if (st->agp->acquired)
                return -EBUSY;
        bridge = agp_backend_acquire(to_pci_dev(dev->dev));
        if (IS_ERR(bridge))
                return PTR_ERR(bridge);
        st->agp->bridge = bridge;
        st->agp->acquired = 1;
        return 0;
}

static int drm_legacy_agp_release_ioctl(struct drm_device *dev, void *data,
                                        struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);

        if (!st || !st->agp)
                return -EINVAL;
        if (!st->agp->acquired)
                return -EINVAL;
        if (st->agp->bridge)
                agp_backend_release(st->agp->bridge);
        st->agp->acquired = 0;
        return 0;
}

static int drm_legacy_agp_enable_ioctl(struct drm_device *dev, void *data,
                                       struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_agp_mode *mode = data;

        if (!st || !st->agp || !st->agp->acquired)
                return -EINVAL;
        agp_enable(st->agp->bridge, mode->mode);
        st->agp->mode = mode->mode;
        st->agp->enabled = 1;
        return 0;
}

static int drm_legacy_agp_info_ioctl(struct drm_device *dev, void *data,
                                     struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_agp_info *info = data;
        struct agp_kern_info *kern;

        if (!st || !st->agp)
                return -EINVAL;
        kern = &st->agp->agp_info;
        info->agp_version_major = kern->version.major;
        info->agp_version_minor = kern->version.minor;
        info->mode = kern->mode;
        info->aperture_base = kern->aper_base;
        info->aperture_size = kern->aper_size;
        info->memory_allowed = kern->max_memory;
        info->memory_used = kern->current_memory;
        info->id_vendor = st->agp->bridge && st->agp->bridge->dev ?
                          st->agp->bridge->dev->vendor : 0x8086;
        info->id_device = st->agp->bridge && st->agp->bridge->dev ?
                          st->agp->bridge->dev->device : 0;
        return 0;
}

static int drm_legacy_agp_alloc_ioctl(struct drm_device *dev, void *data,
                                      struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_agp_buffer *request = data;
        struct drm_agp_mem *mem;
        struct agp_memory *memory;

        if (!st || !st->agp || !st->agp->acquired)
                return -EINVAL;
        memory = agp_allocate_memory(st->agp->bridge,
                                     request->size,
                                     request->type);
        if (!memory)
                return -ENOMEM;
        mem = kzalloc(sizeof(*mem), GFP_KERNEL);
        if (!mem) {
                agp_free_memory(memory);
                return -ENOMEM;
        }
        mem->memory = memory;
        mem->bound = 0;
        list_add(&mem->head, &st->agp_memory);
        request->handle = memory->key;
        request->physical = memory->physical;
        return 0;
}

static int drm_legacy_agp_free_ioctl(struct drm_device *dev, void *data,
                                     struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_agp_buffer *request = data;
        struct drm_agp_mem *mem;

        if (!st || !st->agp)
                return -EINVAL;
        mem = drm_legacy_agp_lookup(dev, request->handle);
        if (!mem)
                return -EINVAL;
        list_del(&mem->head);
        agp_free_memory(mem->memory);
        kfree(mem);
        return 0;
}

static int drm_legacy_agp_bind_ioctl(struct drm_device *dev, void *data,
                                     struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_agp_binding *request = data;
        struct drm_agp_mem *mem;
        int ret;

        if (!st || !st->agp)
                return -EINVAL;
        mem = drm_legacy_agp_lookup(dev, request->handle);
        if (!mem || mem->bound)
                return -EINVAL;
        ret = agp_bind_memory(mem->memory, request->offset / PAGE_SIZE);
        if (ret)
                return ret;
        mem->bound = 1;
        return 0;
}

static int drm_legacy_agp_unbind_ioctl(struct drm_device *dev, void *data,
                                       struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_agp_binding *request = data;
        struct drm_agp_mem *mem;
        int ret;

        if (!st || !st->agp)
                return -EINVAL;
        mem = drm_legacy_agp_lookup(dev, request->handle);
        if (!mem || !mem->bound)
                return -EINVAL;
        ret = agp_unbind_memory(mem->memory);
        if (ret)
                return ret;
        mem->bound = 0;
        return 0;
}

static int drm_legacy_infobufs_ioctl(struct drm_device *dev, void *data,
                                     struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_buf_info *request = data;
        int count = 0, i;

        if (!st || !st->dma)
                return -EINVAL;
        for (i = 0; i <= DRM_LEGACY_MAX_ORDER; i++) {
                if (st->dma->bufs[i].buf_count)
                        count++;
        }
        if (request->count >= count) {
                count = 0;
                for (i = 0; i <= DRM_LEGACY_MAX_ORDER; i++) {
                        if (!st->dma->bufs[i].buf_count)
                                continue;
                        if (count >= request->count)
                                break;
                        struct drm_buf_desc desc = {
                                .count = st->dma->bufs[i].buf_count,
                                .size = 1 << i,
                                .flags = _DRM_AGP_BUFFER,
                        };
                        if (copy_to_user(request->list + count, &desc, sizeof(desc)))
                                return -EFAULT;
                        count++;
                }
        }
        request->count = count;
        return 0;
}

static int drm_legacy_mapbufs_ioctl(struct drm_device *dev, void *data,
                                    struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_buf_map *request = data;
        int i;

        if (!st || !st->dma || !st->dma->buf_count)
                return -EINVAL;
        if (request->count < st->dma->buf_count)
                return -ENOMEM;
        for (i = 0; i < st->dma->buf_count; i++) {
                struct drm_buf *buf = st->dma->buflist[i];
                struct drm_buf_pub pub;
                struct drm_local_map map = {
                        .offset = buf->bus_address,
                        .size = buf->total,
                        .type = _DRM_AGP,
                        .flags = _DRM_WRITE_COMBINING,
                };
                if (!buf->virt) {
                        drm_legacy_ioremap(&map, dev);
                        buf->virt = map.handle;
                }
                if (!buf->virt)
                        return -ENOMEM;
                pub.idx = buf->idx;
                pub.total = buf->total;
                pub.used = buf->used;
                pub.address = buf->virt;
                if (copy_to_user(request->list + i, &pub, sizeof(pub)))
                        return -EFAULT;
        }
        request->count = st->dma->buf_count;
        return 0;
}

static int drm_legacy_unmapbufs_ioctl(struct drm_device *dev, void *data,
                                      struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_buf_map *request = data;
        int i;

        if (!st || !st->dma)
                return -EINVAL;
        for (i = 0; i < st->dma->buf_count; i++) {
                struct drm_buf *buf = st->dma->buflist[i];
                if (buf->virt) {
                        iounmap(buf->virt);
                        buf->virt = NULL;
                }
        }
        return 0;
}

static int drm_legacy_freebufs_ioctl(struct drm_device *dev, void *data,
                                     struct drm_file *file_priv)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_buf_desc *request = data;
        struct drm_buf *buf;
        int i, order;

        if (!st || !st->dma)
                return -EINVAL;
        order = get_order(request->size);
        if (order < 0 || order > DRM_LEGACY_MAX_ORDER)
                return -EINVAL;
        for (i = request->low_mark; i <= request->high_mark; i++) {
                if (i < 0 || i >= st->dma->buf_count)
                        continue;
                buf = st->dma->buflist[i];
                if (buf && buf->order == order)
                        buf->file_priv = NULL;
        }
        return 0;
}

int drm_legacy_irq_uninstall(struct drm_device *dev)
{
        return 0;
}

struct drm_agp_head *drm_legacy_agp_init(struct drm_device *dev)
{
        struct pci_dev *pdev = to_pci_dev(dev->dev);
        struct pci_dev *bridge_pdev = NULL;
        struct agp_bridge_data *bridge;
        struct drm_agp_head *head;
        static const unsigned short i810_bridge_ids[] = {
                0x7120, 0x7124, 0x1130, 0
        };
        int i;

        for (i = 0; i810_bridge_ids[i]; i++) {
                bridge_pdev = pci_get_device(PCI_VENDOR_ID_INTEL,
                                             i810_bridge_ids[i], NULL);
                if (bridge_pdev)
                        break;
        }
        if (!bridge_pdev)
                return NULL;

        bridge = kzalloc(sizeof(*bridge), GFP_KERNEL);
        if (!bridge)
                return NULL;
        bridge->version = &agp_default_version;
        bridge->dev = bridge_pdev;
        atomic_set(&bridge->agp_in_use, 0);
        atomic_set(&bridge->current_memory_agp, 0);
        bridge->mode = 0x1f000000;

        if (!intel_gmch_probe(bridge_pdev, pdev, bridge)) {
                kfree(bridge);
                return NULL;
        }
        agp_bridge = bridge;

        if (agp_backend_initialize(bridge)) {
                agp_bridge = NULL;
                intel_gmch_remove();
                kfree(bridge);
                return NULL;
        }
        intel_gmch_enable_gtt();

        head = kzalloc(sizeof(*head), GFP_KERNEL);
        if (!head) {
                agp_backend_release(bridge);
                agp_bridge = NULL;
                intel_gmch_remove();
                kfree(bridge);
                return NULL;
        }
        head->bridge = bridge;
        agp_copy_info(bridge, &head->agp_info);
        head->base = head->agp_info.aper_base;
        head->mode = head->agp_info.mode;
        head->page_mask = head->agp_info.page_mask;
        INIT_LIST_HEAD(&head->memory);
        head->agp_mtrr = arch_phys_wc_add(head->agp_info.aper_base,
                                          head->agp_info.aper_size * 1024 * 1024);
        return head;
}

void drm_legacy_agp_cleanup(struct drm_device *dev)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_agp_mem *m, *mtmp;

        if (!st || !st->agp)
                return;
        list_for_each_entry_safe(m, mtmp, &st->agp_memory, head) {
                list_del(&m->head);
                agp_free_memory(m->memory);
                kfree(m);
        }
        if (st->agp->agp_mtrr >= 0)
                arch_phys_wc_del(st->agp->agp_mtrr);
        if (st->agp->bridge)
                agp_backend_release(st->agp->bridge);
        if (agp_bridge) {
                agp_bridge = NULL;
                intel_gmch_remove();
        }
        kfree(st->agp);
        st->agp = NULL;
}


int drm_legacy_open(struct inode *inode, struct file *filp)
{
        int ret = drm_open(inode, filp);
        struct drm_file *priv;
        struct drm_legacy_state *st;

        if (ret)
                return ret;
        priv = filp->private_data;
        if (!priv || !priv->minor)
                return ret;
        st = drm_legacy_state(priv->minor->dev);
        if (st)
                atomic_inc(&st->open_count);
        return ret;
}

int drm_legacy_release(struct inode *inode, struct file *filp)
{
        struct drm_file *priv = filp->private_data;
        struct drm_device *dev;
        struct drm_legacy_state *st;

        if (priv && priv->minor) {
                dev = priv->minor->dev;
                st = drm_legacy_state(dev);
                if (st) {
                        if (drm_legacy_active_driver && drm_legacy_active_driver->preclose)
                                drm_legacy_active_driver->preclose(dev, priv);
                        if (atomic_dec_and_test(&st->open_count) &&
                            drm_legacy_active_driver && drm_legacy_active_driver->lastclose)
                                drm_legacy_active_driver->lastclose(dev);
                }
        }
        return drm_release(inode, filp);
}

void drm_legacy_driver_finish(struct drm_device *dev)
{
        struct drm_legacy_state *st = drm_legacy_state(dev);
        struct drm_map_list *r_list, *rtmp;
        int i;

        if (!st)
                return;
        if (st->dma) {
                for (i = 0; i < st->dma->buf_count; i++) {
                        struct drm_buf *buf = st->dma->buflist[i];
                        if (!buf)
                                continue;
                        if (buf->virt)
                                iounmap(buf->virt);
                        kfree(buf->dev_private);
                        kfree(buf);
                }
                for (i = 0; i <= DRM_LEGACY_MAX_ORDER; i++)
                        kfree(st->dma->bufs[i].buflist);
                kfree(st->dma->buflist);
                kfree(st->dma);
                st->dma = NULL;
        }
        list_for_each_entry_safe(r_list, rtmp, &st->maplist, head) {
                list_del(&r_list->head);
                if (r_list->map) {
                        drm_legacy_ioremapfree(r_list->map, dev);
                        kfree(r_list->map);
                }
                kfree(r_list);
        }
        drm_legacy_agp_cleanup(dev);
        drm_legacy_state_destroy(dev);
}

static int drm_legacy_pci_probe(struct pci_dev *pdev,
                                const struct pci_device_id *ent)
{
        struct drm_legacy_driver *ldrv = drm_legacy_active_driver;
        struct drm_device *dev;
        struct drm_legacy_state *st;
        int ret;

        if (!ldrv)
                return -ENODEV;
        dev = drm_dev_alloc(&ldrv->base, &pdev->dev);
        if (IS_ERR(dev))
                return PTR_ERR(dev);
        st = drm_legacy_state_create(dev);
        if (!st) {
                drm_dev_put(dev);
                return -ENOMEM;
        }
        ret = drm_dev_register(dev, ent->driver_data);
        if (ret) {
                drm_legacy_state_destroy(dev);
                drm_dev_put(dev);
                return ret;
        }
        pci_set_drvdata(pdev, dev);
        return 0;
}

static void drm_legacy_pci_remove(struct pci_dev *pdev)
{
        struct drm_device *dev = pci_get_drvdata(pdev);

        if (!dev)
                return;
        pci_set_drvdata(pdev, NULL);
        drm_legacy_driver_finish(dev);
        drm_dev_unregister(dev);
        drm_dev_put(dev);
}

int drm_legacy_pci_init(struct drm_legacy_driver *driver,
                        struct pci_driver *pci_driver)
{
        drm_legacy_active_driver = driver;
        drm_legacy_driver_priv_size = driver->dev_priv_size;
        pci_driver->probe = drm_legacy_pci_probe;
        pci_driver->remove = drm_legacy_pci_remove;
        return pci_register_driver(pci_driver);
}

void drm_legacy_pci_exit(struct drm_legacy_driver *driver,
                         struct pci_driver *pci_driver)
{
        pci_unregister_driver(pci_driver);
        drm_legacy_active_driver = NULL;
}



struct legacy_ioctl_def {
        unsigned int nr;
        size_t size;
        unsigned int dir;
        int (*fn)(struct drm_device *dev, void *data, struct drm_file *file_priv);
        bool need_auth;
};

static const struct legacy_ioctl_def legacy_ioctl_table[] = {
        { 0x15, sizeof(struct drm_map), _IOC_WRITE | _IOC_READ, drm_legacy_addmap_ioctl, true },
        { 0x16, sizeof(struct drm_buf_desc), _IOC_WRITE | _IOC_READ, drm_legacy_addbufs_ioctl, true },
        { 0x18, sizeof(struct drm_buf_info), _IOC_WRITE | _IOC_READ, drm_legacy_infobufs_ioctl, true },
        { 0x19, sizeof(struct drm_buf_map), _IOC_WRITE | _IOC_READ, drm_legacy_mapbufs_ioctl, true },
        { 0x1a, sizeof(struct drm_buf_desc), _IOC_WRITE, drm_legacy_freebufs_ioctl, true },
        { 0x1b, sizeof(struct drm_map), _IOC_WRITE, drm_legacy_rmmap_ioctl, true },
        { 0x2a, sizeof(struct drm_lock), _IOC_WRITE, drm_legacy_lock_ioctl, true },
        { 0x2b, sizeof(struct drm_lock), _IOC_WRITE, drm_legacy_unlock_ioctl, true },
        { 0x30, 0, _IOC_NONE, drm_legacy_agp_acquire_ioctl, true },
        { 0x31, 0, _IOC_NONE, drm_legacy_agp_release_ioctl, true },
        { 0x32, sizeof(struct drm_agp_mode), _IOC_WRITE, drm_legacy_agp_enable_ioctl, true },
        { 0x33, sizeof(struct drm_agp_info), _IOC_READ, drm_legacy_agp_info_ioctl, true },
        { 0x34, sizeof(struct drm_agp_buffer), _IOC_WRITE | _IOC_READ, drm_legacy_agp_alloc_ioctl, true },
        { 0x35, sizeof(struct drm_agp_buffer), _IOC_WRITE, drm_legacy_agp_free_ioctl, true },
        { 0x36, sizeof(struct drm_agp_binding), _IOC_WRITE, drm_legacy_agp_bind_ioctl, true },
        { 0x37, sizeof(struct drm_agp_binding), _IOC_WRITE, drm_legacy_agp_unbind_ioctl, true },
};

long drm_legacy_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
        struct drm_file *priv;
        struct drm_device *dev;
        unsigned int nr = DRM_IOCTL_NR(cmd);
        char kdata[128];
        size_t insize, outsize;
        int i, ret;

        if (!filp || !filp->private_data)
                return -ENOENT;
        if (DRM_IOCTL_TYPE(cmd) != DRM_IOCTL_BASE)
                return -ENOENT;
        priv = filp->private_data;
        if (!priv || !priv->minor)
                return -ENOENT;
        dev = priv->minor->dev;
        if (!dev || !dev->driver)
                return -ENOENT;
        if (!drm_legacy_active_driver || dev->driver != &drm_legacy_active_driver->base)
                return -ENOENT;

        for (i = 0; i < (int)ARRAY_SIZE(legacy_ioctl_table); i++) {
                const struct legacy_ioctl_def *def = &legacy_ioctl_table[i];
                if (def->nr != nr)
                        continue;
                if (def->need_auth && !priv->authenticated)
                        return -EACCES;
                if (def->size) {
                        insize = (cmd & _IOC_WRITE) ? _IOC_SIZE(cmd) : 0;
                        outsize = (cmd & _IOC_READ) ? _IOC_SIZE(cmd) : 0;
                        if (insize > sizeof(kdata) || outsize > sizeof(kdata))
                                return -EINVAL;
                        memset(kdata, 0, sizeof(kdata));
                        if (insize && copy_from_user(kdata, (void __user *)arg, insize))
                                return -EFAULT;
                        ret = def->fn(dev, kdata, priv);
                        if (!ret && outsize && copy_to_user((void __user *)arg, kdata, outsize))
                                return -EFAULT;
                        return ret;
                }
                return def->fn(dev, kdata, priv);
        }
        return -ENOENT;
}

static int drm_legacy_addmap_ioctl(struct drm_device *dev, void *data,
                                   struct drm_file *file_priv)
{
        return drm_legacy_addmap(dev, data);
}

static int drm_legacy_addbufs_ioctl(struct drm_device *dev, void *data,
                                    struct drm_file *file_priv)
{
        return drm_legacy_addbufs_agp(dev, data);
}

static int drm_legacy_rmmap_ioctl(struct drm_device *dev, void *data,
                                  struct drm_file *file_priv)
{
        return drm_legacy_rmmap(dev, data);
}
