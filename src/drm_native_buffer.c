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
  HAL_PIXEL_FORMAT_RGB_888 = 3,
  HAL_PIXEL_FORMAT_RGB_565 = 4,
  HAL_PIXEL_FORMAT_BGRA_8888 = 5,
  HAL_PIXEL_FORMAT_YCBCR_422_SP = 16,
  HAL_PIXEL_FORMAT_YCRCB_420_SP = 17,
  HAL_PIXEL_FORMAT_YCBCR_422_I = 20,
  HAL_PIXEL_FORMAT_RGBA_FP16 = 22,
  HAL_PIXEL_FORMAT_RAW16 = 32,
  HAL_PIXEL_FORMAT_BLOB = 33,
  HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED = 34,
  HAL_PIXEL_FORMAT_YCBCR_420_888 = 35,
  HAL_PIXEL_FORMAT_RAW_OPAQUE = 36,
  HAL_PIXEL_FORMAT_RAW10 = 37,
  HAL_PIXEL_FORMAT_RAW12 = 38,
  HAL_PIXEL_FORMAT_RGBA_1010102 = 43,
  HAL_PIXEL_FORMAT_Y8 = 538982489,
  HAL_PIXEL_FORMAT_Y16 = 540422489,
  HAL_PIXEL_FORMAT_YV12 = 842094169,
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

void
drm_native_buffer_cleanup(StreamState *st)
{
  if (!st)
    return;

  if (st->backend != STREAM_BACKEND_NATIVE_BUFFER)
    return;

  DrmSink *s = &st->sink;
  const int drm_fd = s->drm_fd;

  if (st->native_slots) {
    for (guint i = 0; i < st->native_n_slots; i++) {
      NativeSlotImport *imp = &st->native_slots[i];

      if (drm_fd >= 0 && imp->fb_id)
        drmModeRmFB(drm_fd, imp->fb_id);

      if (drm_fd >= 0 && imp->gem_handle)
        native_gem_close(drm_fd, imp->gem_handle);

      imp->fb_id = 0;
      imp->gem_handle = 0;
      imp->prime_fd_index = -1;
      imp->used_fmt = 0;
      imp->used_mod = 0;
      imp->pitch = 0;
    }

    g_free(st->native_slots);
    st->native_slots = NULL;
  }

  st->native_n_slots = 0;
  st->native_stride_pixels = 0;
  st->native_width = 0;
  st->native_height = 0;
  st->native_modeset_done = FALSE;
  st->native_current_slot = 0;
  st->native_current_fb_id = 0;
}

static gboolean
native_init_from_info(StreamState *st)
{
  if (!st || !st->native_info)
    return FALSE;

  guint32 w = 0;
  guint32 h = 0;
  guint32 n = 0;
  guint32 stride = 0;
  g_autofree char *type = NULL;

  if (!native_lookup_str(st->native_info, "type", &type))
    return FALSE;

  if (!type || strcmp(type, "native-buffer") != 0)
    return FALSE;

  native_lookup_u32(st->native_info, "width", &w);
  native_lookup_u32(st->native_info, "height", &h);
  native_lookup_u32(st->native_info, "buffer_count", &n);
  native_lookup_u32(st->native_info, "stride_pixels", &stride);

  if (n == 0)
    n = 1;

  st->native_width = w ? w : st->info_width;
  st->native_height = h ? h : st->info_height;
  st->native_n_slots = n;
  st->native_stride_pixels = stride;

  if (!st->native_slots) {
    st->native_slots = g_new0(NativeSlotImport, st->native_n_slots);

    for (guint i = 0; i < st->native_n_slots; i++) {
      st->native_slots[i].prime_fd_index = -1;
      st->native_slots[i].gem_handle = 0;
      st->native_slots[i].fb_id = 0;
      st->native_slots[i].used_fmt = 0;
      st->native_slots[i].used_mod = 0;
      st->native_slots[i].pitch = 0;
    }
  }

  return TRUE;
}

static gboolean
native_import_slot_if_needed(StreamState *st,
                             guint32      slot)
{
  if (!st || !st->native_info || !st->native_fds || st->native_n_fds <= 0)
    return FALSE;

  if (!native_init_from_info(st))
    return FALSE;

  if (slot >= st->native_n_slots)
    slot = slot % st->native_n_slots;

  NativeSlotImport *imp = &st->native_slots[slot];
  if (imp->fb_id != 0 && imp->gem_handle != 0)
    return TRUE;

  g_autoptr(GVariant) buffers = NULL;
  if (!native_get_buffers_array(st->native_info, &buffers)) {
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

  DrmSink *s = &st->sink;
  const int drm_fd = s->drm_fd;

  int chosen_fd_index = -1;
  uint32_t gem_handle = 0;

  for (int i = 0; i < n_fd_indices; i++) {
    int idx = fd_indices[i];

    if (idx < 0 || idx >= st->native_n_fds)
      continue;

    int fd = st->native_fds[idx];
    if (fd < 0)
      continue;

    uint32_t hnd = 0;
    if (drmPrimeFDToHandle(drm_fd, fd, &hnd) == 0) {
      chosen_fd_index = idx;
      gem_handle = hnd;
      break;
    }
  }

  g_free(fd_indices);

  if (chosen_fd_index < 0 || gem_handle == 0) {
    g_warning("[native-buffer] slot %u: no dma-buf FD could be imported via drmPrimeFDToHandle", slot);
    return FALSE;
  }

  guint32 stride_pixels = st->native_stride_pixels;
  if (stride_pixels == 0)
    native_lookup_u32(st->native_info, "stride_pixels", &stride_pixels);
  if (stride_pixels == 0)
    stride_pixels = st->native_width ? st->native_width : 1920;

  guint32 pitch = stride_pixels * 4u;

  uint32_t fb_id = 0;
  gboolean fb_ok = FALSE;
  uint32_t used_fmt = 0;
  uint64_t used_mod = DRM_FORMAT_MOD_LINEAR;

  uint32_t handles[4] = { gem_handle, 0, 0, 0 };
  uint32_t pitches[4] = { pitch, 0, 0, 0 };
  uint32_t offsets[4] = { 0, 0, 0, 0 };
  uint64_t modifiers[4] = { DRM_FORMAT_MOD_LINEAR, 0, 0, 0 };

  const guint32 W = st->native_width ? st->native_width : st->info_width;
  const guint32 H = st->native_height ? st->native_height : st->info_height;

  gint32 hal_format = 0;
  native_lookup_i32(st->native_info, "hal_format", &hal_format);

  uint32_t try_fmts[4];
  guint n_try = 0;

  switch ((android_pixel_format_t)hal_format) {
  case HAL_PIXEL_FORMAT_RGBA_8888:
    try_fmts[n_try++] = DRM_FORMAT_ABGR8888;
    try_fmts[n_try++] = DRM_FORMAT_XBGR8888;
    try_fmts[n_try++] = DRM_FORMAT_ARGB8888;
    try_fmts[n_try++] = DRM_FORMAT_XRGB8888;
    break;
  case HAL_PIXEL_FORMAT_RGBX_8888:
    try_fmts[n_try++] = DRM_FORMAT_XBGR8888;
    try_fmts[n_try++] = DRM_FORMAT_ABGR8888;
    try_fmts[n_try++] = DRM_FORMAT_XRGB8888;
    try_fmts[n_try++] = DRM_FORMAT_ARGB8888;
    break;
  case HAL_PIXEL_FORMAT_BGRA_8888:
    try_fmts[n_try++] = DRM_FORMAT_ARGB8888;
    try_fmts[n_try++] = DRM_FORMAT_XRGB8888;
    try_fmts[n_try++] = DRM_FORMAT_ABGR8888;
    try_fmts[n_try++] = DRM_FORMAT_XBGR8888;
    break;
  default:
    for (guint i = 0; i < (guint)(sizeof(k_try_fmts_default) / sizeof(k_try_fmts_default[0])); i++)
      try_fmts[n_try++] = k_try_fmts_default[i];
    break;
  }

  for (guint fi = 0; fi < n_try && !fb_ok; fi++) {
    errno = 0;
    int ret = drmModeAddFB2WithModifiers(drm_fd,
                                         W,
                                         H,
                                         try_fmts[fi],
                                         handles,
                                         pitches,
                                         offsets,
                                         modifiers,
                                         &fb_id,
                                         0);
    if (ret == 0) {
      fb_ok = TRUE;
      used_fmt = try_fmts[fi];
      used_mod = DRM_FORMAT_MOD_LINEAR;
    }
  }

  if (!fb_ok) {
    for (guint fi = 0; fi < n_try && !fb_ok; fi++) {
      errno = 0;
      int ret = drmModeAddFB2(drm_fd,
                              W,
                              H,
                              try_fmts[fi],
                              handles,
                              pitches,
                              offsets,
                              &fb_id,
                              0);
      if (ret == 0) {
        fb_ok = TRUE;
        used_fmt = try_fmts[fi];
        used_mod = 0;
      }
    }
  }

  if (!fb_ok || fb_id == 0) {
    g_warning("[native-buffer] slot %u: could not create FB (AddFB2* failed): %s",
              slot, g_strerror(errno));
    native_gem_close(drm_fd, gem_handle);
    return FALSE;
  }

  imp->prime_fd_index = chosen_fd_index;
  imp->gem_handle = gem_handle;
  imp->fb_id = fb_id;
  imp->used_fmt = used_fmt;
  imp->used_mod = used_mod;
  imp->pitch = pitch;

  return TRUE;
}

static gboolean
native_modeset_if_needed(StreamState *st,
                         guint32      slot)
{
  DrmSink *s = &st->sink;

  if (!s || s->drm_fd < 0 || !s->have_mode)
    return FALSE;

  if (!native_import_slot_if_needed(st, slot))
    return FALSE;

  if (slot >= st->native_n_slots)
    slot = slot % st->native_n_slots;

  NativeSlotImport *imp = &st->native_slots[slot];
  if (!imp->fb_id)
    return FALSE;

  if (st->native_modeset_done)
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

  st->native_modeset_done = TRUE;
  st->native_current_slot = slot;
  st->native_current_fb_id = imp->fb_id;

  return TRUE;
}

void
render_frame_drm_native_buffer(StreamState *st)
{
  if (!st)
    return;

  ensure_drm_ready(st);

  DrmSink *s = &st->sink;
  if (s->drm_fd < 0)
    return;

  if (!st->native_info) {
    g_warning("[native-buffer] selected but native_info is NULL");
    return;
  }

  if (!native_init_from_info(st)) {
    g_warning("[native-buffer] invalid native_info (missing type/fields)");
    return;
  }

  if (s->pending_flip) {
    st->need_render_after_flip = TRUE;
    return;
  }

  guint32 slot = st->pending_slot;
  if (st->native_n_slots > 0)
    slot = slot % st->native_n_slots;

  if (!native_modeset_if_needed(st, slot))
    return;

  if (!native_import_slot_if_needed(st, slot))
    return;

  NativeSlotImport *imp = &st->native_slots[slot];
  if (!imp->fb_id)
    return;

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
  s->pending_flip_next_front = s->front_idx;

  st->inflight_flip_seq = st->pending_seq;

  st->native_current_slot = slot;
  st->native_current_fb_id = imp->fb_id;

  st->force_full_damage = FALSE;

  /*
   * as DRM is waiting for the next vblank, start producing the next frame
   * if that frame becomes ready before this flip completes,
   * render_or_defer() will keep it pending until the pageflip
   */
  if (st->streaming)
    request_next_frame(st);
}
