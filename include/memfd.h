/**
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#ifndef MEMFD_H
#define MEMFD_H

#include <stdint.h>

#define META_FURIOS_MEMFD_MAGIC 0x46555249u
#define META_FURIOS_MEMFD_VERSION 1u

typedef enum
{
  META_FURIOS_MEMFD_FORMAT_RGBA8888 = 1,
  META_FURIOS_MEMFD_FORMAT_BGRA8888 = 2,
  META_FURIOS_MEMFD_FORMAT_BGRX8888 = 3,
} MetaFuriosMemfdFormat;

typedef struct __attribute__((packed)) MetaFuriosMemfdHeader
{
  uint32_t magic;        /* META_FURIOS_MEMFD_MAGIC */
  uint32_t version;	 /* META_FURIOS_MEMFD_VERSION */

  uint32_t width;
  uint32_t height;
  uint32_t stride;	 /* bytes per row */
  uint32_t format;	 /* MetaFuriosMemfdFormat */

  uint32_t n_slots;	 /* ring size */
  uint32_t slot_bytes;   /* stride * height */
  uint32_t header_bytes; /* sizeof(MetaFuriosMemfdHeader) */
  uint32_t reserved0;

  /* updated per frame */
  uint32_t last_slot;    /* slot index last written */
  uint32_t seq;          /* increments each frame */
  uint64_t pts_ns;	 /* monotonic pts */
  uint64_t reserved1;
} MetaFuriosMemfdHeader;

/**
 * Convert MetaFuriosMemfdFormat value to a readable string.
 *
 * @param fmt  Format value from MetaFuriosMemfdHeader::format
 * @return     Static string describing the format
 */
const char *
memfd_format_name(uint32_t fmt);

/**
 * Print the memfd header fields for debugging.
 *
 * @param tag  Prefix tag printed before the header content
 * @param h    Pointer to the header
 */
void
memfd_dump_header(const char *tag,
                  const MetaFuriosMemfdHeader *h);

/**
 * Validate basic header sanity.
 *
 * @param h  Pointer to the header
 * @return   TRUE if header looks valid, FALSE otherwise
 */
gboolean
memfd_header_sane(const MetaFuriosMemfdHeader *h);

#endif // MEMFD_H
