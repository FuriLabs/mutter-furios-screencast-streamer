/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "drm_native_buffer.h"
#include "drm_utils.h"
#include "stream.h"
#include "utils.h"
#include "dbus.h"

typedef enum
{
  HAL_PIXEL_FORMAT_RGBA_8888 = 1,
  HAL_PIXEL_FORMAT_RGBX_8888 = 2,
  HAL_PIXEL_FORMAT_BGRA_8888 = 5,
} android_pixel_format_t;

static const uint32_t k_try_fmts_default[] = {
  DRM_FORMAT_XRGB8888,
  DRM_FORMAT_ARGB8888,
  DRM_FORMAT_XBGR8888,
  DRM_FORMAT_ABGR8888,
};

static gboolean
native_lookup_u32(GVariant   *dict,
                  const char *key,
                  guint32    *out)
{
  GVariant *v = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_UINT32);
  if (!v)
    return FALSE;

  *out = g_variant_get_uint32(v);
  g_variant_unref(v);
  return TRUE;
}

static gboolean
native_lookup_i32(GVariant   *dict,
                  const char *key,
                  gint32     *out)
{
  GVariant *v = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_INT32);
  if (!v)
    return FALSE;

  *out = g_variant_get_int32(v);
  g_variant_unref(v);
  return TRUE;
}

static gboolean
native_lookup_str(GVariant    *dict,
                  const char  *key,
                  char       **out)
{
  GVariant *v;

  if (!dict || !key || !out)
    return FALSE;

  v = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_STRING);
  if (!v)
    return FALSE;

  *out = g_variant_dup_string(v, NULL);

  g_variant_unref(v);

  return *out != NULL;
}

static gboolean
native_get_buffers_array(GVariant  *info,
                         GVariant **out_buffers)
{
  GVariant *v = g_variant_lookup_value(info, "buffers", G_VARIANT_TYPE("aa{sv}"));
  if (!v)
    return FALSE;

  *out_buffers = v;
  return TRUE;
}

static GVariant *
native_find_slot_dict(GVariant *buffers_aa_sv,
                      guint32   want_slot)
{
  GVariantIter iter;
  GVariant *child = NULL;

  g_variant_iter_init(&iter, buffers_aa_sv);

  while ((child = g_variant_iter_next_value(&iter)) != NULL) {
    guint32 slot = 0;

    if (native_lookup_u32(child, "slot", &slot) && slot == want_slot)
      return child;

    g_variant_unref(child);
  }

  return NULL;
}

static gboolean
native_extract_fd_indices(GVariant *slot_dict,
                          int     **out_indices,
                          int      *out_n)
{
  GVariant *v = g_variant_lookup_value(slot_dict, "fds", G_VARIANT_TYPE("ah"));
  if (!v)
    return FALSE;

  gsize n = g_variant_n_children(v);
  if (n == 0) {
    g_variant_unref(v);
    return FALSE;
  }

  int *idx = g_new0(int, (int)n);
  for (gsize i = 0; i < n; i++) {
    GVariant *h = g_variant_get_child_value(v, i);

    idx[i] = g_variant_get_handle(h);
    g_variant_unref(h);
  }

  g_variant_unref(v);

  *out_indices = idx;
  *out_n = (int)n;
  return TRUE;
}

static void
native_gem_close(int      drm_fd,
                 uint32_t handle)
{
  if (drm_fd < 0 || handle == 0)
    return;

  struct drm_gem_close close_arg;

  memset(&close_arg, 0, sizeof(close_arg));
  close_arg.handle = handle;

  drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
}

static uint32_t
native_import_gem_handle(StreamState *st,
                         int         *fd_indices,
                         int          n_fd_indices)
{
  const int drm_fd = st->sink.drm_fd;

  for (int i = 0; i < n_fd_indices; i++) {
    int idx = fd_indices[i];

    if (idx < 0 || idx >= st->native.n_fds)
      continue;

    int fd = st->native.fds[idx];
    if (fd < 0)
      continue;

    uint32_t gem_handle = 0;

    if (drmPrimeFDToHandle(drm_fd, fd, &gem_handle) == 0)
      return gem_handle;
  }

  return 0;
}

static guint
native_get_drm_formats(gint32    hal_format,
                       uint32_t *formats,
                       guint     max_formats)
{
  if (!formats || max_formats < 4)
    return 0;

  switch ((android_pixel_format_t)hal_format) {
  case HAL_PIXEL_FORMAT_RGBA_8888:
    formats[0] = DRM_FORMAT_ABGR8888;
    formats[1] = DRM_FORMAT_XBGR8888;
    formats[2] = DRM_FORMAT_ARGB8888;
    formats[3] = DRM_FORMAT_XRGB8888;
    return 4;
  case HAL_PIXEL_FORMAT_RGBX_8888:
    formats[0] = DRM_FORMAT_XBGR8888;
    formats[1] = DRM_FORMAT_ABGR8888;
    formats[2] = DRM_FORMAT_XRGB8888;
    formats[3] = DRM_FORMAT_ARGB8888;
    return 4;
  case HAL_PIXEL_FORMAT_BGRA_8888:
    formats[0] = DRM_FORMAT_ARGB8888;
    formats[1] = DRM_FORMAT_XRGB8888;
    formats[2] = DRM_FORMAT_ABGR8888;
    formats[3] = DRM_FORMAT_XBGR8888;
    return 4;
  default:
    memcpy(formats, k_try_fmts_default, sizeof(k_try_fmts_default));
    return G_N_ELEMENTS(k_try_fmts_default);
  }
}

static gboolean
native_create_fb(StreamState   *st,
                 uint32_t       gem_handle,
                 guint32        width,
                 guint32        height,
                 guint32        pitch,
                 const uint32_t *formats,
                 guint          n_formats,
                 uint32_t      *out_fb_id)
{
  const int drm_fd = st->sink.drm_fd;

  uint32_t handles[4] = { gem_handle, 0, 0, 0 };
  uint32_t pitches[4] = { pitch, 0, 0, 0 };
  uint32_t offsets[4] = { 0, 0, 0, 0 };
  uint64_t modifiers[4] = { DRM_FORMAT_MOD_LINEAR, 0, 0, 0 };

  for (guint i = 0; i < n_formats; i++) {
    errno = 0;

    int ret = drmModeAddFB2WithModifiers(drm_fd,
                                         width,
                                         height,
                                         formats[i],
                                         handles,
                                         pitches,
                                         offsets,
                                         modifiers,
                                         out_fb_id,
                                         0);
    if (ret == 0)
      return TRUE;
  }

  for (guint i = 0; i < n_formats; i++) {
    errno = 0;

    int ret = drmModeAddFB2(drm_fd,
                            width,
                            height,
                            formats[i],
                            handles,
                            pitches,
                            offsets,
                            out_fb_id,
                            0);
    if (ret == 0)
      return TRUE;
  }

  return FALSE;
}

static gboolean
native_import_slot(StreamState *st,
                   guint32      slot)
{
  if (!st || !st->native.info || !st->native.fds || st->native.n_fds <= 0)
    return FALSE;

  if (!st->native.slots || st->native.n_slots == 0)
    return FALSE;

  if (slot >= st->native.n_slots)
    return FALSE;

  NativeSlotImport *imp = &st->native.slots[slot];

  g_autoptr(GVariant) buffers = NULL;
  if (!native_get_buffers_array(st->native.info, &buffers)) {
    g_warning("[native-buffer] info has no 'buffers' array");
    return FALSE;
  }

  g_autoptr(GVariant) slot_dict = native_find_slot_dict(buffers, slot);
  if (!slot_dict) {
    g_warning("[native-buffer] could not find slot %u in 'buffers'", slot);
    return FALSE;
  }

  int *fd_indices = NULL;
  int n_fd_indices = 0;
  if (!native_extract_fd_indices(slot_dict, &fd_indices, &n_fd_indices)) {
    g_warning("[native-buffer] slot %u has no fds[]", slot);
    return FALSE;
  }

  uint32_t gem_handle = native_import_gem_handle(st,
                                                 fd_indices,
                                                 n_fd_indices);

  g_free(fd_indices);

  if (gem_handle == 0) {
    g_warning("[native-buffer] slot %u: no dma-buf FD could be imported via drmPrimeFDToHandle", slot);
    return FALSE;
  }

  guint32 stride_pixels = st->native.stride_pixels;
  if (stride_pixels == 0)
    stride_pixels = st->native.width ? st->native.width : 1920;

  guint32 pitch = stride_pixels * 4u;

  const guint32 width = st->native.width ? st->native.width : st->info_width;
  const guint32 height = st->native.height ? st->native.height : st->info_height;

  gint32 hal_format = 0;
  native_lookup_i32(st->native.info, "hal_format", &hal_format);

  uint32_t formats[4];
  guint n_formats = native_get_drm_formats(hal_format,
                                           formats,
                                           G_N_ELEMENTS(formats));

  uint32_t fb_id = 0;

  if (!native_create_fb(st,
                        gem_handle,
                        width,
                        height,
                        pitch,
                        formats,
                        n_formats,
                        &fb_id)) {
    g_warning("[native-buffer] slot %u: could not create FB (AddFB2* failed): %s",
              slot, g_strerror(errno));
    native_gem_close(st->sink.drm_fd, gem_handle);
    return FALSE;
  }

  imp->gem_handle = gem_handle;
  imp->fb_id = fb_id;

  return TRUE;
}

static gboolean
native_modeset_if_needed(StreamState *st,
                         guint32      slot)
{
  DrmSink *s = &st->sink;

  if (!s || s->drm_fd < 0 || !s->have_mode)
    return FALSE;

  if (!st->native.slots || st->native.n_slots == 0)
    return FALSE;

  if (slot >= st->native.n_slots)
    return FALSE;

  NativeSlotImport *imp = &st->native.slots[slot];
  if (!imp->fb_id)
    return FALSE;

  if (st->native.modeset_done)
    return TRUE;

  int ret = drmModeSetCrtc(s->drm_fd,
                           s->crtc_id,
                           imp->fb_id,
                           0, 0,
                           &s->conn_id,
                           1,
                           &s->mode);
  if (ret != 0) {
    g_warning("[native-buffer] drmModeSetCrtc failed: %s", g_strerror(errno));
    return FALSE;
  }

  st->native.modeset_done = TRUE;

  return TRUE;
}

gboolean
drm_native_buffer_init(StreamState *st)
{
  if (!st || !st->native.info)
    return FALSE;

  if (st->native.slots)
    return TRUE;

  guint32 width = 0;
  guint32 height = 0;
  guint32 buffer_count = 0;
  guint32 stride_pixels = 0;
  g_autofree char *type = NULL;

  if (!native_lookup_str(st->native.info, "type", &type))
    return FALSE;

  if (!type || strcmp(type, "native-buffer") != 0)
    return FALSE;

  native_lookup_u32(st->native.info, "width", &width);
  native_lookup_u32(st->native.info, "height", &height);
  native_lookup_u32(st->native.info, "buffer_count", &buffer_count);
  native_lookup_u32(st->native.info, "stride_pixels", &stride_pixels);

  if (buffer_count == 0)
    buffer_count = 1;

  st->native.width = width ? width : st->info_width;
  st->native.height = height ? height : st->info_height;
  st->native.n_slots = buffer_count;
  st->native.stride_pixels = stride_pixels;

  st->native.slots = g_new0(NativeSlotImport, st->native.n_slots);

  return TRUE;
}

gboolean
drm_native_buffer_import_all(StreamState *st)
{
  if (!st)
    return FALSE;

  if (st->sink.drm_fd < 0 || !st->sink.have_mode)
    return FALSE;

  if (!st->native.slots || st->native.n_slots == 0)
    return FALSE;

  for (guint32 slot = 0; slot < st->native.n_slots; slot++) {
    if (!native_import_slot(st, slot)) {
      g_warning("[native-buffer] failed to import slot %u", slot);
      return FALSE;
    }
  }

  return TRUE;
}

void
drm_native_buffer_cleanup(StreamState *st)
{
  if (!st)
    return;

  if (st->backend != STREAM_BACKEND_NATIVE_BUFFER)
    return;

  DrmSink *s = &st->sink;
  const int drm_fd = s->drm_fd;

  if (st->native.slots) {
    for (guint i = 0; i < st->native.n_slots; i++) {
      NativeSlotImport *imp = &st->native.slots[i];

      if (drm_fd >= 0 && imp->fb_id)
        drmModeRmFB(drm_fd, imp->fb_id);

      if (drm_fd >= 0 && imp->gem_handle)
        native_gem_close(drm_fd, imp->gem_handle);

      imp->fb_id = 0;
      imp->gem_handle = 0;
    }

    g_free(st->native.slots);
    st->native.slots = NULL;
  }

  st->native.n_slots = 0;
  st->native.stride_pixels = 0;
  st->native.width = 0;
  st->native.height = 0;
  st->native.modeset_done = FALSE;
}

void
drm_native_buffer_render_frame(StreamState *st)
{
  if (!st)
    return;

  DrmSink *s = &st->sink;

  if (s->pending_flip) {
    st->need_render_after_flip = TRUE;
    return;
  }

  if (!st->native.slots || st->native.n_slots == 0)
    return;

  guint32 slot = st->pending_slot % st->native.n_slots;

  NativeSlotImport *imp = &st->native.slots[slot];
  if (!imp->fb_id)
    return;

  if (!st->native.modeset_done) {
    if (!native_modeset_if_needed(st, slot))
      return;
  }

  errno = 0;
  int ret = drmModePageFlip(s->drm_fd,
                            s->crtc_id,
                            imp->fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT,
                            st);
  if (ret != 0) {
    g_warning("[native-buffer] drmModePageFlip failed: %s", g_strerror(errno));
    return;
  }

  s->pending_flip = 1;

  st->inflight_flip_seq = st->pending_seq;

  st->force_full_damage = FALSE;

  /*
   * as DRM is waiting for the next vblank, start producing the next frame
   * if that frame becomes ready before this flip completes,
   * render_or_defer() will keep it pending until the pageflip
   */
  if (st->streaming)
    request_next_frame(st);
}
