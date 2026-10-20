// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"
#include "../../../src/kernel/lkpi/lkpi.h"
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_client.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_print.h>
#include "../../upstream/drivers/gpu/drm/drm_internal.h"

struct luos_con {
	struct drm_client_dev client;
	struct drm_client_buffer *buffer;
	struct iosys_map map;
	struct mutex lock;
	struct task_struct *flusher;
	bool attached;
};

static DEFINE_MUTEX(con_global_lock);
static struct luos_con *con_active;

/*
 * Connectors the user took over with the gpu-mon-* shell commands.
 *
 * The console client used to fight the user: drm_lastclose() runs
 * drm_client_dev_restore() every time the kernel-side DRM fd closes
 * (gpu-mon-off closes it right after disabling the CRTC), and the client
 * "restored" by committing every modeset it remembered - which re-lit the
 * monitor the user had just switched off.  Hotplug polling did the same.
 * Held connectors are skipped by every commit and every buffer sizing of
 * this client until LuOS releases them again.
 */
struct con_hold {
	unsigned int devt;
	unsigned int connector_id;
};

static struct con_hold con_holds[16];
static DEFINE_MUTEX(con_holds_lock);

static unsigned int con_devt(struct drm_device *dev)
{
	return dev->primary && dev->primary->kdev ? dev->primary->kdev->devt : 0;
}

void lkpi_console_hold_connector(unsigned int primary_devt, unsigned int connector_id, int hold)
{
	mutex_lock(&con_holds_lock);
	if (hold) {
		int slot = -1;
		bool present = false;

		for (int i = 0; i < ARRAY_SIZE(con_holds); i++) {
			if (con_holds[i].devt == primary_devt && con_holds[i].connector_id == connector_id) {
				present = true;
				break;
			}
			if (slot < 0 && !con_holds[i].devt)
				slot = i;
		}
		if (!present && slot >= 0) {
			con_holds[slot].devt = primary_devt;
			con_holds[slot].connector_id = connector_id;
		}
	} else {
		for (int i = 0; i < ARRAY_SIZE(con_holds); i++) {
			if (con_holds[i].devt == primary_devt && con_holds[i].connector_id == connector_id)
				con_holds[i].devt = 0;
		}
	}
	mutex_unlock(&con_holds_lock);
}

static bool con_connector_held(struct drm_device *dev, unsigned int connector_id)
{
	unsigned int devt = con_devt(dev);
	bool held = false;

	if (!devt)
		return false;
	mutex_lock(&con_holds_lock);
	for (int i = 0; i < ARRAY_SIZE(con_holds); i++) {
		if (con_holds[i].devt == devt && con_holds[i].connector_id == connector_id) {
			held = true;
			break;
		}
	}
	mutex_unlock(&con_holds_lock);
	return held;
}

static bool con_modeset_held(struct luos_con *con, struct drm_mode_set *ms)
{
	for (int i = 0; ms->connectors && i < ms->num_connectors; i++)
		if (con_connector_held(con->client.dev, ms->connectors[i]->base.id))
			return true;
	return false;
}

/*
 * Commit only the modesets the user has not claimed.  drm_client_modeset_commit()
 * walks client->modesets, so hand it a filtered shallow copy for the duration
 * of the call.  Every caller holds con->lock and the commit paths only read
 * the array, so the swap is private to this client.
 */
static int con_commit(struct luos_con *con, bool locked)
{
	struct drm_client_dev *client = &con->client;
	struct drm_mode_set filtered[33];
	struct drm_mode_set *orig = client->modesets;
	struct drm_mode_set *ms;
	int kept = 0, total = 0, ret;

	if (!orig)
		return 0;
	for (ms = orig; ms->crtc; ms++) {
		total++;
		if (kept < 32 && !con_modeset_held(con, ms))
			filtered[kept++] = *ms;
	}
	if (!total || !kept)
		return 0;
	memset(&filtered[kept], 0, sizeof(filtered[kept]));
	client->modesets = filtered;
	if (locked)
		ret = drm_client_modeset_commit_locked(client);
	else
		ret = drm_client_modeset_commit(client);
	client->modesets = orig;
	return ret;
}

static struct luos_con *to_con(struct drm_client_dev *client) { return container_of(client, struct luos_con, client); }

static void con_flush(struct luos_con *con, const unsigned int *r)
{
	struct drm_rect rect;
	if (r) rect = (struct drm_rect){ r[0], r[1], r[2], r[3] };
	drm_client_framebuffer_flush(con->buffer, r ? &rect : NULL);
}

static int con_flusher_fn(void *data)
{
	struct luos_con *con = data;
	while (!kthread_should_stop()) {
		schedule_timeout_interruptible(msecs_to_jiffies(16));
		mutex_lock(&con->lock);
		if (con->attached && drm_master_internal_acquire(con->client.dev)) {
			unsigned int r[4];
			if (luos_console_take_damage(r)) con_flush(con, r);
			drm_master_internal_release(con->client.dev);
		}
		mutex_unlock(&con->lock);
	}
	return 0;
}

static int con_bind_buffer(struct luos_con *con)
{
	struct drm_client_dev *client = &con->client;
	struct drm_mode_set *ms;
	u32 w = 0, h = 0;
	int ret = drm_client_modeset_probe(client, 0, 0);
	if (ret) return ret;

	mutex_lock(&client->modeset_mutex);
	drm_client_for_each_modeset(ms, client) {
		if (!ms->mode || !ms->num_connectors) continue;
		if (con_modeset_held(con, ms)) continue;
		w = max_t(u32, w, ms->x + ms->mode->hdisplay);
		h = max_t(u32, h, ms->y + ms->mode->vdisplay);
	}
	mutex_unlock(&client->modeset_mutex);
	if (!w || !h) return -ENODEV;

	if (!con->buffer) {
		struct drm_client_buffer *buffer = drm_client_framebuffer_create(client, w, h, DRM_FORMAT_XRGB8888);
		if (IS_ERR(buffer)) return PTR_ERR(buffer);
		ret = drm_client_buffer_vmap(buffer, &con->map);
		if (ret) {
			drm_client_buffer_delete(buffer);
			return ret;
		}
		con->buffer = buffer;
	} else if (w > con->buffer->fb->width || h > con->buffer->fb->height) {
		return -ENOSPC;
	}

	mutex_lock(&client->modeset_mutex);
	drm_client_for_each_modeset(ms, client)
		if (ms->mode && ms->num_connectors && !con_modeset_held(con, ms))
			ms->fb = con->buffer->fb;
	mutex_unlock(&client->modeset_mutex);
	return 0;
}

static void con_attach(struct luos_con *con)
{
	struct drm_framebuffer *fb = con->buffer->fb;
	void *vaddr = con->map.is_iomem ? (void __force *)con->map.vaddr_iomem : con->map.vaddr;
	if (con->attached) return;
	if (luos_console_attach(vaddr, fb->width, fb->height, fb->pitches[0]) == 0) {
		con->attached = true;
		drm_info(con->client.dev, "LuOS console on %ux%u framebuffer %u\n", fb->width, fb->height, fb->base.id);
	}
}

static int con_hotplug(struct drm_client_dev *client)
{
	struct luos_con *con = to_con(client);
	int ret;
	mutex_lock(&con->lock);
	ret = con_bind_buffer(con);
	if (!ret) ret = con_commit(con, false);
	if (!ret) {
		con_attach(con);
		con_flush(con, NULL);
	}
	mutex_unlock(&con->lock);
	return ret == -EBUSY ? 0 : ret;
}

static int con_restore(struct drm_client_dev *client, bool force)
{
	struct luos_con *con = to_con(client);
	int ret;
	mutex_lock(&con->lock);
	if (!con->buffer) {
		mutex_unlock(&con->lock);
		return -ENODEV;
	}
	ret = con_commit(con, force);
	if (!ret) con_flush(con, NULL);
	mutex_unlock(&con->lock);
	return ret;
}

/*
 * drm_lastclose() restores the console only when the device's LAST file
 * closes.  After gpu-init the Mesa host keeps the render node open, so a
 * demo closing its primary fd never reached open_count == 0: its RMFB had
 * already switched the CRTC off and nothing re-lit the console - the shell
 * stopped drawing after every render program.  LuOS restores explicitly.
 */
void lkpi_console_restore(unsigned int primary_devt)
{
	struct luos_con *con;

	mutex_lock(&con_global_lock);
	con = con_active;
	if (con && (!primary_devt || con_devt(con->client.dev) == primary_devt))
		con_restore(&con->client, true);
	mutex_unlock(&con_global_lock);
}

static void con_unregister(struct drm_client_dev *client)
{
	struct luos_con *con = to_con(client);
	if (con->flusher) kthread_stop(con->flusher);
	mutex_lock(&con->lock);
	if (con->attached) luos_console_detach();
	con->attached = false;
	if (con->buffer) {
		drm_client_buffer_vunmap(con->buffer);
		drm_client_buffer_delete(con->buffer);
		con->buffer = NULL;
	}
	mutex_unlock(&con->lock);
	drm_client_release(client);
	mutex_lock(&con_global_lock);
	if (con_active == con) con_active = NULL;
	mutex_unlock(&con_global_lock);
	kfree(con);
}

static const struct drm_client_funcs con_funcs = {
	.owner = THIS_MODULE,
	.unregister = con_unregister,
	.restore = con_restore,
	.hotplug = con_hotplug,
};

void drm_client_setup(struct drm_device *dev, const struct drm_format_info *format)
{
	struct luos_con *con;
	int ret;

	if (dev->dev && dev_is_pci(dev->dev) && !video_is_primary_device(dev->dev)) {
		drm_dbg(dev, "not the boot display, LuOS console stays where it is\n");
		return;
	}
	mutex_lock(&con_global_lock);
	if (con_active) {
		mutex_unlock(&con_global_lock);
		return;
	}
	con = kzalloc(sizeof(*con), GFP_KERNEL);
	if (!con) {
		mutex_unlock(&con_global_lock);
		return;
	}
	mutex_init(&con->lock);
	ret = drm_client_init(dev, &con->client, "luoscon", &con_funcs);
	if (ret) {
		drm_err(dev, "LuOS console client failed: %d\n", ret);
		kfree(con);
		mutex_unlock(&con_global_lock);
		return;
	}
	con_active = con;
	mutex_unlock(&con_global_lock);

	con->flusher = kthread_run(con_flusher_fn, con, "drm-console");
	if (IS_ERR(con->flusher)) con->flusher = NULL;
	drm_client_register(&con->client);
}

void drm_client_setup_with_fourcc(struct drm_device *dev, u32 fourcc) { drm_client_setup(dev, NULL); }
void drm_client_setup_with_color_mode(struct drm_device *dev, unsigned int color_mode) { drm_client_setup(dev, NULL); }

struct unclaimed_scan {
	struct drm_device *devs[8];
	int count;
};

static void collect_card(struct device *kdev, void *arg)
{
	struct unclaimed_scan *scan = arg;
	struct drm_minor *minor;
	if (!kdev->type || !kdev->type->name || strcmp(kdev->type->name, "drm_minor")) return;
	minor = dev_get_drvdata(kdev);
	if (!minor || minor->type != DRM_MINOR_PRIMARY || scan->count >= ARRAY_SIZE(scan->devs)) return;
	drm_dev_get(minor->dev);
	scan->devs[scan->count++] = minor->dev;
}

void lkpi_console_setup_unclaimed(void)
{
	struct unclaimed_scan scan = { .count = 0 };
	lkpi_for_each_device(collect_card, &scan);
	for (int i = 0; i < scan.count; i++) {
		bool claimed;
		mutex_lock(&con_global_lock);
		claimed = con_active != NULL;
		mutex_unlock(&con_global_lock);
		if (!claimed && drm_core_check_feature(scan.devs[i], DRIVER_MODESET)) drm_client_setup(scan.devs[i], NULL);
		drm_dev_put(scan.devs[i]);
	}
}
