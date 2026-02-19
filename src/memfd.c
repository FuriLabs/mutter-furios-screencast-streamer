/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "memfd.h"

const char *
memfd_format_name(uint32_t fmt)
{
  switch (fmt) {
  case META_FURIOS_MEMFD_FORMAT_RGBA8888:
    return "RGBA8888";
  case META_FURIOS_MEMFD_FORMAT_BGRA8888:
    return "BGRA8888";
  case META_FURIOS_MEMFD_FORMAT_BGRX8888:
    return "BGRX8888";
  default:
    return "UNKNOWN";
  }
}

void
memfd_dump_header(const char                  *tag,
                  const MetaFuriosMemfdHeader *h)
{
  if (!h)
    return;

  g_debug("%s hdr: magic=0x%08x version=%u wxh=%ux%u stride=%u fmt=%u(%s) n_slots=%u slot_bytes=%u header_bytes=%u last_slot=%u seq=%u pts_ns=%" G_GUINT64_FORMAT,
          tag ? tag : "(null)",
          h->magic,
          h->version,
          h->width, h->height,
          h->stride,
          h->format, memfd_format_name(h->format),
          h->n_slots,
          h->slot_bytes,
          h->header_bytes,
          h->last_slot,
          h->seq,
          (guint64)h->pts_ns);
}

gboolean
memfd_header_sane(const MetaFuriosMemfdHeader *h)
{
  if (!h)
    return FALSE;
  if (h->magic != META_FURIOS_MEMFD_MAGIC)
    return FALSE;
  if (h->version != META_FURIOS_MEMFD_VERSION)
    return FALSE;
  if (h->width == 0 || h->height == 0)
    return FALSE;
  if (h->stride == 0)
    return FALSE;
  if (h->n_slots == 0)
    return FALSE;
  if (h->slot_bytes == 0)
    return FALSE;
  if (h->header_bytes < sizeof(MetaFuriosMemfdHeader))
    return FALSE;

  return TRUE;
}
