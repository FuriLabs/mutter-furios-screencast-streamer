/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include "memfd.h"

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
