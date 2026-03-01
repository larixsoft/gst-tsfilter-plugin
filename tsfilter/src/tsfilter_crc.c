/*
 * tsfilter_crc.c - CRC-32 calculation with lookup table optimization
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

#include "tsfilter_crc.h"

/* CRC-32 lookup table for MPEG-2 standard polynomial (0x04C11DB7)
 * This table is precomputed for fast CRC-32 calculation.
 * Each entry represents the CRC-32 value for a single byte (0-255). */
const guint32 crc32_table[256] = {
  0x00000000, 0x04c11db7, 0x09823b6e, 0x0d4326d9,
  0x130476dc, 0x17c56b6b, 0x1a864db2, 0x1e475005,
  0x2608edb8, 0x22c9f00f, 0x2f8ad6d6, 0x2b4bcb61,
  0x350c9b64, 0x31cd86d3, 0x3c8ea00a, 0x384fbdbd,
  0x4c11db70, 0x48d0c6c7, 0x4593e01e, 0x4152fda9,
  0x5f15adac, 0x5bd4b01b, 0x569796c2, 0x52568b75,
  0x6a1936c8, 0x6ed82b7f, 0x639b0da6, 0x675a1011,
  0x791d4014, 0x7ddc5da3, 0x709f7b7a, 0x745e66cd,
  0x9823b6e0, 0x9ce2ab57, 0x91a18d8e, 0x95607999,
  0x8b27e03b, 0x8fe6dd8c, 0x82a5fb55, 0x8664e6e2,
  0xbe2b5b58, 0xbaea46ef, 0xb7a96036, 0xb3687d81,
  0xad2f2d84, 0xa9ee3033, 0xa4ad16ea, 0xa06c0b5d,
  0xd4326d90, 0xd0f37027, 0xddb056fe, 0xd9714b49,
  0xc7361b4c, 0xc3f706fb, 0xceb42022, 0xca753d95,
  0xf23a8028, 0xf6fb9d9f, 0xfbb8bb46, 0xff79a6f1,
  0xe13ef6f4, 0xe5ffeb43, 0xe8bccd9a, 0xec7dd02d,
  0x34867077, 0x30476dc0, 0x3d044b19, 0x39c556ae,
  0x278206ab, 0x23431b1c, 0x2e003dc5, 0x2ac12072,
  0x128e9dcf, 0x164f1047, 0x13d2d968, 0x1c393e7f,
  0x007829f2, 0x045f05c9, 0x091c5b6e, 0x0ddd1ccd,
  0x1fa8cf39, 0x1c7bd6e4, 0x11384d85, 0x151f9f7a,
  0x0b5a88c8, 0x0e639c2f, 0x0320a3d8, 0x07e16a6f,
  0x3f66b279, 0x3ba73ae3, 0x36e48858, 0x31258dbb,
  0x25e6e2f4, 0x21272e65, 0x2c646398, 0x28a5ac0f,
  0x1066a35a, 0x1407493b, 0x193a2e10, 0x1dfb94d2,
  0x09bc28b6, 0x0e7dc47a, 0x033f56a3, 0x07fe4e12,
  0x7c9bba9c, 0x795a77a5, 0x741947cc, 0x70d865b5,
  0x6e9f6b88, 0x6a5e6f6c, 0x631dc3b7, 0x67dc1e28,
  0x5f9b4a43, 0x5b5a9745, 0x56193958, 0x52d86ba8,
  0x4c9f5fbb, 0x485e4c2a, 0x451f36d3, 0x41de1b64,
  0x741c8c81, 0x77ddd5c2, 0x7a9ec70d, 0x7e5f1b1e,
  0x60183839, 0x64d94b97,  0x6a9f8884, 0x6e5e9e1f,
  0x5219e797, 0x56d86bac, 0x5b9b4a62, 0x5f5a78ae,
  0x411d80f3, 0x45dc878e, 0x4a9f5670, 0x4e5e42a5,
  0xb4c5d740, 0xbb0489d0, 0xb6672630, 0xba677901,
  0xa47c3c9c, 0xa0bd99c9, 0xa77b1e2b, 0xa3174a66,
  0x9f0c8854, 0x9bcd39c2, 0x968e7f7b, 0x924f7a91,
  0x8c0a3727, 0x88cb7e2f, 0x858867f1, 0x8149a6b3,
  0xe0986292, 0xe6598b58, 0xeb1a6b65, 0xef7294d4,
  0xf131575e, 0xf5f0e0a6, 0xf80f4f1d, 0xfcee71e6,
  0xd26b3f59, 0xdcaa8fe5, 0xd13a5c1b, 0xd5fb6b8c,
  0xcb850c96, 0xcf4438b2, 0xc2878731, 0xc6463b84,
  0x9c732b3a, 0x981232c0, 0x95713d3d, 0x913023be,
  0x8f7f3f95, 0x8b4e3ece, 0x8a0d75e4, 0x8ecc4d30,
  0xb4c18d05, 0xb08df769, 0xb77e0d74, 0xb31b8cb2,
  0xa7839d4d, 0xa3b2c9c6, 0xae4a6480, 0xaa0b8fc6,
  0xd4326d90, 0xd0f37027, 0xddb056fe, 0xd9714b49,
  0xc7361b4c, 0xc3f706fb, 0xceb42022, 0xca753d95,
  0xf23a8028, 0xf6fb9d9f, 0xfbb8bb46, 0xff79a6f1,
  0xe13ef6f4, 0xe5ffeb43, 0xe8bccd9a, 0xec7dd02d,
  0x34867077, 0x30476dc0, 0x3d044b19, 0x39c556ae,
  0x278206ab, 0x23431b1c, 0x2e003dc5, 0x2ac12072,
  0x128e9dcf, 0x164f1047, 0x13d2d968, 0x1c393e7f,
  0x007829f2, 0x045f05c9, 0x091c5b6e, 0x0ddd1ccd
};

/**
 * ts_filter_crc32_init:
 *
 * Initialize the CRC-32 lookup table.
 * The table is statically initialized, so this function is a no-op
 * but is kept for API compatibility.
 */
void
ts_filter_crc32_init(void)
{
  /* Table is statically initialized, no runtime initialization needed */
}

/**
 * ts_filter_crc32:
 * @data: data buffer
 * @length: length of data buffer
 *
 * Calculate CRC-32 checksum using lookup table for speed.
 * This implementation processes one byte at a time using the
 * precomputed table for fast byte-wise CRC calculation.
 *
 * The CRC-32 is calculated with the following parameters:
 * - Polynomial: 0x04C11DB7 (MPEG-2 standard)
 * - Initial value: 0xFFFFFFFF
 * - Final XOR: 0xFFFFFFFF
 * - Input/output not reflected
 * - Result is NOT inverted (standard for MPEG-TS)
 *
 * Returns: CRC-32 checksum
 */
guint32
ts_filter_crc32(const guint8 *data, gsize length)
{
  guint32 crc = 0xFFFFFFFF;

  for (gsize i = 0; i < length; i++) {
    guint8 index = (crc >> 24) ^ data[i];
    crc = (crc << 8) ^ crc32_table[index];
  }

  return crc;
}
