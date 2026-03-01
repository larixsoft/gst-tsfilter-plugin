/*
 * tsfilter_crc.h - CRC-32 calculation with lookup table optimization
 * Copyright (C) 2025 LarixSoft <https://github.com/larixsoft>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 */

#ifndef GST_TS_FILTER_CRC_H
#define GST_TS_FILTER_CRC_H

#include <glib.h>
#include <gst/gst.h>

G_BEGIN_DECLS

/* CRC-32 lookup table (initialized on first use) */
extern const guint32 crc32_table[256];

/**
 * ts_filter_crc32_init:
 *
 * Initialize the CRC-32 lookup table.
 * This function is thread-safe and idempotent.
 */
void ts_filter_crc32_init(void);

/**
 * ts_filter_crc32:
 * @data: data buffer
 * @length: length of data buffer
 *
 * Calculate CRC-32 checksum using MPEG-2 standard polynomial (0x04C11DB7).
 * This implementation uses a precomputed lookup table for speed.
 *
 * Returns: CRC-32 checksum
 */
guint32 ts_filter_crc32(const guint8 *data, gsize length);

G_END_DECLS

#endif /* GST_TS_FILTER_CRC_H */
