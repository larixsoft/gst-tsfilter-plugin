/*
 * tsfilter.c - GStreamer MPEG-TS PID filter
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

#include "tsfilter.h"
#include "tsfilter_crc.h"
#include <stdio.h>
#include <string.h>
#include <gst/base/gstflowcombiner.h>

GST_DEBUG_CATEGORY_STATIC(tsfilter_debug);
#define GST_CAT_DEFAULT tsfilter_debug

/* TS packet constants */
#define TS_PACKET_SIZE 188
#define TS_SYNC_BYTE 0x47

/* Pad templates */
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/mpegts, systemstream = (boolean) true")
);

static GstStaticPadTemplate dump_template = GST_STATIC_PAD_TEMPLATE(
    "dump",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS("video/mpegts, systemstream = (boolean) true")
);

static GstStaticPadTemplate pid_src_template = GST_STATIC_PAD_TEMPLATE(
    "src_%u",
    GST_PAD_SRC,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS("video/mpegts, systemstream = (boolean) true")
);

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS("video/mpegts, systemstream = (boolean) true")
);

/* Signals */
enum {
  SIGNAL_BAD_PACKET,
  SIGNAL_PACKET_SIZE_MISMATCH,
  SIGNAL_NEW_PID,
  SIGNAL_DUMP_PAD_ADDED,
  SIGNAL_CRC_ERROR,
  LAST_SIGNAL
};

/* Properties */
enum {
  PROP_0,
  PROP_FILTER_PIDS,
  PROP_INVERT_FILTER,
  PROP_PACKET_SIZE,
  PROP_AUTO_DETECT,
  PROP_BAD_PACKET_COUNT,
  PROP_EMIT_PID_SIGNALS,
  PROP_ENABLE_DUMP,
  PROP_ENABLE_CRC_VALIDATION,
  PROP_ENABLE_STATS,
  PROP_STREAM_STATS,
};

static guint signals[LAST_SIGNAL] = { 0 };

static void ts_filter_set_property(GObject *object, guint prop_id,
                                   const GValue *value, GParamSpec *pspec);
static void ts_filter_get_property(GObject *object, guint prop_id,
                                   GValue *value, GParamSpec *pspec);
static void ts_filter_finalize(GObject *object);
static void ts_filter_dispose(GObject *object);

static GstFlowReturn ts_filter_chain(GstPad *pad, GstObject *parent,
                                     GstBuffer *buffer);
static gboolean ts_filter_sink_event(GstPad *pad, GstObject *parent,
                                     GstEvent *event);
static gboolean ts_filter_src_query(GstPad *pad, GstObject *parent,
                                    GstQuery *query);
static void ts_filter_release_pad(GstElement *element, GstPad *pad);
static guint ts_filter_detect_packet_size(TsFilter *filter, const guint8 *data, gsize size, gboolean at_eos);

#define ts_filter_parent_class parent_class
G_DEFINE_TYPE(TsFilter, ts_filter, GST_TYPE_ELEMENT)

/* Per-PID detailed statistics */
typedef struct {
  guint packet_count;            /* Total packets for this PID */
  guint corrupted_count;         /* Corrupted packets for this PID */
  guint filtered_count;          /* Packets pushed to src pad (if PID is in filter list) */
  guint64 first_filtered_offset; /* Byte offset of first filtered packet (0 if not filtered) */
  guint cc_discontinuities;      /* CC discontinuity count for this PID */
} PidDetailStats;

/* Global stream statistics */
typedef struct {
  guint total_packets;           /* Total packets in the stream (before EOS) */
  guint corrupted_packets;       /* Total corrupted/abandoned packets in the stream */
  GArray *pids;                  /* Array of all PIDs found (sorted, guint16) */
  GHashTable *pid_details;       /* Hash table: PID (guint16) -> PidDetailStats */
} TsStreamStats;

/* Helper to create new PidDetailStats */
static PidDetailStats *
pid_detail_stats_new(void)
{
  PidDetailStats *stats = g_new0(PidDetailStats, 1);
  return stats;
}

/* Helper to free PidDetailStats */
static void
pid_detail_stats_free(gpointer data)
{
  if (data) {
    g_free(data);
  }
}

/* Helper to create new TsStreamStats */
static TsStreamStats *
ts_stream_stats_new(void)
{
  TsStreamStats *stats = g_new0(TsStreamStats, 1);
  if (!stats) {
    return NULL;
  }

  stats->pids = g_array_new(FALSE, FALSE, sizeof(guint16));
  if (!stats->pids) {
    g_free(stats);
    return NULL;
  }

  stats->pid_details = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, pid_detail_stats_free);
  if (!stats->pid_details) {
    g_array_unref(stats->pids);
    g_free(stats);
    return NULL;
  }

  return stats;
}

/* Helper to free TsStreamStats */
static void
ts_stream_stats_free(gpointer data)
{
  if (data) {
    TsStreamStats *stats = (TsStreamStats *)data;
    if (stats->pids) {
      g_array_unref(stats->pids);
    }
    if (stats->pid_details) {
      g_hash_table_unref(stats->pid_details);
    }
    g_free(stats);
  }
}

/* Helper to free a GSList of packet data (used as GDestroyNotify for pending_packets) */
static void
ts_filter_pending_list_free(gpointer data)
{
  if (data) {
    GSList *list = (GSList *)data;
    /* Free each packet data in the list */
    for (GSList *l = list; l; l = l->next) {
      g_free(l->data);  /* Free the packet data allocated with g_memdup */
    }
    /* Free the list itself */
    g_slist_free(list);
  }
}

/* Helper to get or create PidDetailStats for a PID */
static PidDetailStats *
ts_filter_get_or_create_pid_detail(TsFilter *filter, guint16 pid)
{
  if (!filter->enable_stats) {
    return NULL;  /* Statistics disabled */
  }
  PidDetailStats *stats = g_hash_table_lookup(filter->pid_stats, GUINT_TO_POINTER(pid));
  if (!stats) {
    stats = pid_detail_stats_new();
    if (stats) {
      g_hash_table_insert(filter->pid_stats, GUINT_TO_POINTER(pid), stats);
      GST_DEBUG_OBJECT(filter, "Created new detail stats for PID %u", pid);
    }
  }
  return stats;
}

/* Helper to update packet count for a PID */
static void
ts_filter_update_packet_count(TsFilter *filter, guint16 pid)
{
  if (!filter->enable_stats) {
    return;  /* Statistics disabled */
  }
  PidDetailStats *stats = ts_filter_get_or_create_pid_detail(filter, pid);
  if (stats) {
    stats->packet_count++;
    GST_TRACE_OBJECT(filter, "PID %u packet_count now %u", pid, stats->packet_count);
  }
}

/* Helper to update corrupted packet count for a PID */
static void
ts_filter_update_corrupted_count(TsFilter *filter, guint16 pid)
{
  if (!filter->enable_stats) {
    return;  /* Statistics disabled */
  }
  filter->total_corrupted++;
  PidDetailStats *stats = ts_filter_get_or_create_pid_detail(filter, pid);
  if (stats) {
    stats->corrupted_count++;
  }
}

/* Helper to update filtered count and track first offset for a PID */
static void
ts_filter_update_filtered_count(TsFilter *filter, guint16 pid, guint64 byte_offset)
{
  if (!filter->enable_stats) {
    return;  /* Statistics disabled */
  }
  PidDetailStats *stats = ts_filter_get_or_create_pid_detail(filter, pid);
  if (stats) {
    stats->filtered_count++;
    /* Track first filtered packet offset */
    if (stats->first_filtered_offset == 0) {
      stats->first_filtered_offset = byte_offset;
      GST_DEBUG_OBJECT(filter, "PID %u first filtered packet at offset %" G_GUINT64_FORMAT,
                       pid, byte_offset);
    }
  }
}

/* Helper to check and track CC discontinuity for a PID */
static void
ts_filter_check_cc_discontinuity(TsFilter *filter, guint16 pid, guint8 cc)
{
  /* Skip CC check for null packets (PID 8191) and when stats are disabled */
  if (pid == 0x1FFF || !filter->enable_stats) {
    return;
  }

  /* Get or create last CC entry */
  gpointer key = GUINT_TO_POINTER(pid);
  gpointer value = g_hash_table_lookup(filter->last_cc, key);
  guint8 last_cc = value ? GPOINTER_TO_UINT(value) : 0xFF;

  /* Check for discontinuity (last_cc != 0xFF means we've seen this PID before) */
  if (last_cc != 0xFF && ((last_cc + 1) & 0x0F) != cc) {
    /* CC discontinuity detected */
    GST_DEBUG_OBJECT(filter, "CC discontinuity: PID %u lastCC=%u currentCC=%u",
                     pid, last_cc, cc);
    PidDetailStats *stats = ts_filter_get_or_create_pid_detail(filter, pid);
    if (stats) {
      stats->cc_discontinuities++;
    }
  }

  /* Update last CC for this PID */
  g_hash_table_insert(filter->last_cc, key, GUINT_TO_POINTER(cc));
}

/* Helper function for comparing guint16 values (for sorting PID arrays) */
static gint
guint16_compare(gconstpointer a, gconstpointer b)
{
  guint16 pid_a = *(const guint16 *)a;
  guint16 pid_b = *(const guint16 *)b;
  return (pid_a > pid_b) ? 1 : ((pid_a < pid_b) ? -1 : 0);
}

/* Helper function to check if a PID should pass through the filter */
static inline gboolean
ts_filter_check_pid(TsFilter *filter, guint16 pid)
{
  guint i;

  /* If no filter is set, pass all PIDs */
  if (filter->filter_pids == NULL || filter->filter_pids->len == 0)
    return TRUE;

  /* Check if PID is in the filter list */
  for (i = 0; i < filter->filter_pids->len; i++) {
    if (g_array_index(filter->filter_pids, guint16, i) == pid) {
      /* PID found in list */
      return !filter->invert_filter;  /* pass if not inverted */
    }
  }

  /* PID not in list */
  return filter->invert_filter;  /* pass if inverted (deny list mode) */
}

/* Extract PID from TS packet */
static inline guint16
ts_filter_extract_pid(const guint8 *packet)
{
  return ((packet[1] & 0x1F) << 8) | packet[2];
}

/* Extract CC (Continuity Counter) from TS packet */
static inline guint8
ts_filter_extract_cc(const guint8 *packet)
{
  return packet[3] & 0x0F;
}

/* Validate CRC-32 in a buffer (last 4 bytes contain the CRC)
 * Returns TRUE if CRC is valid, FALSE otherwise */
static gboolean
ts_filter_validate_crc32(const guint8 *data, gsize length)
{
  guint32 calculated_crc;
  guint32 stored_crc;

  if (length < 4)
    return FALSE;  /* Too short to contain CRC */

  /* Calculate CRC over data excluding the 4-byte CRC field itself */
  calculated_crc = ts_filter_crc32(data, length - 4);
  stored_crc = (data[length - 4] << 24) | (data[length - 3] << 16) |
               (data[length - 2] << 8) | data[length - 1];

  return calculated_crc == stored_crc;
}

/* Check if a packet contains a table section with adaptation field
 * and potentially CRC-32. Returns TRUE if adaptation field is present. */
static inline gboolean
ts_filter_has_adaptation_field(const guint8 *packet)
{
  guint8 adaptation_field_control = (packet[3] >> 4) & 0x03;
  return adaptation_field_control != 0x01;  /* Not "payload only" */
}

/* Emit CRC error signal */
static void
ts_filter_emit_crc_error(TsFilter *filter, guint16 pid, guint packet_offset)
{
  filter->crc_errors++;
  guint count = 1;  /* Emit one error at a time */
  g_signal_emit(filter, signals[SIGNAL_CRC_ERROR], 0, count);
  GST_WARNING_OBJECT(filter, "CRC error detected in PID %u at packet offset %u (total errors: %u)",
                   pid, packet_offset, filter->crc_errors);
}

/* Validate CRC-32 in MPEG-TS packet containing table section data
 *
 * MPEG-TS CRC-32 Validation:
 * - PSI/SI tables (PAT, PMT, CAT, NIT, EIT, etc.) contain CRC-32 at end
 * - Only validates if section_syntax_indicator bit is set to 1
 * - Regular audio/video PES packets have no CRC and pass immediately
 *
 * Parameters:
 * @filter: the TsFilter instance
 * @packet: the packet data (must start with sync byte)
 * @pid: the packet PID (for logging)
 * @force_validation: if TRUE, reject packets with bad CRC even when enable_crc_validation=FALSE
 *                    This is used during 3-sync detection to prevent false sync patterns
 *                    if FALSE, only reject when enable_crc_validation=TRUE
 *
 * Returns: TRUE if packet has valid CRC or no CRC to check, FALSE if CRC is invalid
 *
 * Note: Audio/video/PES packets return TRUE immediately (no CRC to validate)
 *       Only PSI/SI packets with section_syntax_indicator=1 undergo CRC validation
 */
static gboolean
ts_filter_validate_packet_crc(TsFilter *filter, const guint8 *packet, guint16 pid,
                               gboolean force_validation)
{
  /* ========== STEP 1: Check transport_error_indicator ========== */
  /* Bit 7 of first byte: If set, packet is marked corrupted by transport layer
   * Such packets should not be trusted regardless of CRC state */
  if (packet[0] & 0x80) {
    GST_DEBUG_OBJECT(filter, "Transport error indicator set in PID %u, skipping CRC validation",
                     pid);
    return FALSE;  /* Reject corrupted packet immediately */
  }

  /* ========== STEP 2: Extract adaptation field to find payload ========== */
  guint8 adaptation_field_control = (packet[3] >> 4) & 0x03;
  guint payload_offset = 4;  /* Start after TS header (4 bytes) */
  gboolean has_crc = FALSE;
  gboolean crc_valid = TRUE;

  /* If there's an adaptation field, skip it */
  if (adaptation_field_control == 0x02 || adaptation_field_control == 0x03) {
    if (filter->packet_size > 5) {  /* Need at least AF length byte */
      guint8 adaptation_field_length = packet[4];
      if (adaptation_field_length > 183) {  /* Invalid AF length */
        GST_DEBUG_OBJECT(filter, "Invalid adaptation field length %u in PID %u",
                         adaptation_field_length, pid);
        return FALSE;  /* Invalid packet */
      }
      payload_offset += 1 + adaptation_field_length;
    }
  }

  /* Check payload for CRC */
  if (adaptation_field_control != 0x01 && payload_offset < filter->packet_size) {
    guint8 payload_length = filter->packet_size - payload_offset;

    if (payload_length >= 4) {  /* Need at least 4 bytes for CRC */
      /* Check for table section structure */
      guint8 table_id = packet[payload_offset];

      /* Check section_syntax_indicator (bit 0 of byte after table_id) */
      if (payload_offset + 2 <= filter->packet_size) {
        guint8 section_syntax_indicator = (packet[payload_offset + 1] >> 7) & 0x01;

        /* Check section_length (bytes 2-3, 12 bits) */
        if (payload_offset + 3 <= filter->packet_size) {
          guint16 section_length = ((packet[payload_offset + 2] & 0x0F) << 8) |
                                  packet[payload_offset + 3];
          guint16 total_section_length = section_length + 3;  /* +3 for table_id to section_length fields */

          /* If section_syntax_indicator is 1 and section fits in payload, validate CRC */
          if (section_syntax_indicator == 1 && total_section_length >= 4 &&
              total_section_length <= payload_length) {
            /* Validate CRC-32 */
            const guint8 *section_start = packet + payload_offset;
            has_crc = TRUE;
            if (!ts_filter_validate_crc32(section_start, total_section_length)) {
              GST_WARNING_OBJECT(filter, "CRC error in PID %u table_id=0x%02x section_length=%u",
                                 pid, table_id, section_length);
              crc_valid = FALSE;
            }
          }
        }
      }
    }
  }

  /* Handle CRC validation result */
  if (has_crc && !crc_valid) {
    /* CRC error detected */
    if (filter->enable_crc_validation || force_validation) {
      /* Emit error signal only if validation is enabled */
      if (filter->enable_crc_validation) {
        ts_filter_emit_crc_error(filter, pid, 0);
      }
      /* Reject packet with bad CRC if validation is forced or enabled */
      if (force_validation || filter->enable_crc_validation) {
        return FALSE;
      }
    }
  }

  return TRUE;  /* No CRC or CRC is valid */
}

/* Create or remove the dump pad based on enable_dump property
 *
 * Thread Safety: This function is only called when the element is in
 * GST_STATE_NULL (from property setter) or during NULL_TO_READY transition.
 * This ensures no concurrent access from the chain function, which only
 * runs when the element is in PAUSED or higher states. Therefore, no
 * mutex lock is needed when accessing filter->dumppad.
 */
static void
ts_filter_update_dump_pad(TsFilter *filter)
{
  /* Check if dump pad should exist based on enable_dump */
  gboolean should_have_dump = filter->enable_dump;
  gboolean has_dump = (filter->dumppad != NULL);

  if (should_have_dump && !has_dump) {
    /* Create dump pad */
    filter->dumppad = gst_pad_new_from_static_template(&dump_template, "dump");
    if (!filter->dumppad) {
      GST_ERROR_OBJECT(filter, "Failed to create dump pad");
      return;
    }
    gst_pad_set_query_function(filter->dumppad,
                               GST_DEBUG_FUNCPTR(ts_filter_src_query));
    gst_element_add_pad(GST_ELEMENT(filter), filter->dumppad);
    gst_flow_combiner_add_pad(filter->flow_combiner, filter->dumppad);
    GST_INFO_OBJECT(filter, "Created dump pad (enable-dump=TRUE)");

    /* Emit signal to notify application that dump pad is now available */
    g_signal_emit(filter, signals[SIGNAL_DUMP_PAD_ADDED], 0);
  } else if (!should_have_dump && has_dump) {
    /* Remove dump pad */
    gst_flow_combiner_remove_pad(filter->flow_combiner, filter->dumppad);
    gst_element_remove_pad(GST_ELEMENT(filter), filter->dumppad);
    filter->dumppad = NULL;
    GST_INFO_OBJECT(filter, "Removed dump pad (enable-dump=FALSE)");
  }
}

/* Emit signal for bad packet count */
static inline void
ts_filter_emit_bad_packet(TsFilter *filter, guint count)
{
  g_signal_emit(filter, signals[SIGNAL_BAD_PACKET], 0, count);
}

/*
 * Track PID and emit signal if this is the first time we've seen it
 * Returns: TRUE if this is a new PID (first time seeing it)
 */
static inline gboolean
ts_filter_track_and_emit_pid(TsFilter *filter, guint16 pid)
{
  /* Don't emit signals if disabled */
  if (!filter->emit_pid_signals) {
    return FALSE;
  }

  /* Check if we've seen this PID before */
  if (g_hash_table_contains(filter->seen_pids, GUINT_TO_POINTER(pid))) {
    return FALSE;  /* Already seen this PID */
  }

  /* New PID! Add to seen set and emit signal */
  g_hash_table_add(filter->seen_pids, GUINT_TO_POINTER(pid));

  GST_INFO_OBJECT(filter, "Discovered new PID %u in stream", pid);

  /* Emit signal with PID number */
  g_signal_emit(filter, signals[SIGNAL_NEW_PID], 0, pid);

  return TRUE;
}

/* Create or update a buffer pool */
static GstBufferPool *
ts_filter_create_pool(TsFilter *filter, gsize size, const gchar *name)
{
  GstBufferPool *pool;
  GstStructure *config;
  GstAllocationParams params = {
    .flags = 0,
    .align = 15,  /* Align to 16 bytes */
    .prefix = 0,
    .padding = 0,
  };

  pool = gst_buffer_pool_new();
  if (!pool) {
    GST_ERROR_OBJECT(filter, "Failed to create buffer pool for %s", name);
    return NULL;
  }

  config = gst_buffer_pool_get_config(pool);
  gst_buffer_pool_config_set_params(config, NULL, size, 0, 0);
  gst_buffer_pool_config_set_allocator(config, NULL, &params);

  if (!gst_buffer_pool_set_config(pool, config)) {
    GST_WARNING_OBJECT(filter, "Failed to set pool config for %s", name);
    gst_object_unref(pool);
    return NULL;
  }

  if (!gst_buffer_pool_set_active(pool, TRUE)) {
    GST_WARNING_OBJECT(filter, "Failed to activate pool for %s", name);
    gst_object_unref(pool);
    return NULL;
  }

  GST_INFO_OBJECT(filter, "Created buffer pool %s with size %" G_GSIZE_FORMAT, name, size);
  return pool;
}

/* Ensure buffer pools are created and active */
static void
ts_filter_ensure_pools(TsFilter *filter)
{
  g_mutex_lock(&filter->pool_lock);

  /* Create src pad pool if needed */
  if (filter->srcpool == NULL) {
    filter->srcpool = ts_filter_create_pool(filter, filter->packet_size * 256, "srcpool");
  }

  /* Create dump pad pool if needed */
  if (filter->dumppool == NULL) {
    filter->dumppool = ts_filter_create_pool(filter, filter->packet_size * 256, "dumppool");
  }

  /* Create PID pad pool if needed (fixed-size packets) */
  if (filter->pidpool == NULL) {
    filter->pidpool = ts_filter_create_pool(filter, filter->packet_size, "pidpool");
  }

  g_mutex_unlock(&filter->pool_lock);
}

/*
 * Scan buffer for valid TS packets and record their positions
 * Returns: number of valid packets found, updates packet_indices and out_size
 * Also counts bad sync bytes in bad_sync_count
 *
 * We track two sets of packets:
 * - valid_packets: ALL packets that pass sync validation (for PID pads)
 * - good_packets: packets that pass BOTH sync validation AND PID filter (for src pad)
 */
typedef struct {
  guint valid_packets;   /* All packets passing sync validation */
  guint good_packets;    /* Packets passing sync AND PID filter */
  guint bad_sync_count;
  gsize valid_out_size;  /* Size of all valid packets */
  gsize filtered_out_size; /* Size of filtered packets */
} PacketScanResult;

/*
 * Scan buffer for valid MPEG-TS packets and record their positions
 *
 * This is the core packet scanning function that implements a state machine for
 * MPEG-TS sync detection and packet validation. It handles two main scenarios:
 *
 * 1. FAST PATH (already synced): Process packets at fixed intervals with minimal overhead
 * 2. SYNC ESTABLISHMENT: Use 3-sync validation to find packet boundaries
 *
 * Sync Detection Algorithm:
 * - 3-sync: Requires 3 consecutive sync bytes at packet_size intervals
 * - Lenient sync: For 1-2 packet buffers, accept with reduced validation
 * - Lost sync recovery: After losing sync, re-establish with 3-sync validation
 *
 * Parameters:
 * @filter: the TsFilter instance
 * @data: buffer data to scan
 * @size: size of buffer in bytes
 * @packet_indices: output array to store valid packet positions
 *
 * Returns: PacketScanResult with statistics (good_packets, bad_sync_count, etc.)
 */
static PacketScanResult
ts_filter_scan_packets(TsFilter *filter, const guint8 *data, gsize size,
                       guint *packet_indices)
{
  /* Result structure: tracks packet counts and error statistics */
  PacketScanResult result = {0, 0, 0, 0, 0};
  const guint8 *in_ptr = data;      /* Current position in buffer */
  gsize remaining = size;           /* Remaining bytes to process */

  /* Sync state tracking:
   * - first_iteration: TRUE during initial sync establishment
   * - lost_sync: TRUE after byte-by-byte search, requires 3-sync to re-establish */
  gboolean first_iteration = TRUE;
  gboolean lost_sync = FALSE;

  /* ========== FAST PATH: Already synced from previous buffers ========== */
  if (filter->is_synced && remaining >= filter->packet_size && data[0] == TS_SYNC_BYTE) {
    /* Process packets at fixed packet_size intervals with sync verification
     *
     * Strategy:
     * - Small buffers (1-2 packets): Trust all packets, no sync byte checks
     *   (Rationale: Avoid losing trailing packets at buffer boundaries)
     * - Large buffers: Verify sync byte at each packet position
     *   (Rationale: Detect sync loss early) */
    guint num_packets = remaining / filter->packet_size;
    gboolean trust_small_buffer = (num_packets <= 2);

    for (guint i = 0; i < num_packets; i++) {
      guint16 pid = ts_filter_extract_pid(in_ptr);
      guint8 cc = ts_filter_extract_cc(in_ptr);

      /* Check CC discontinuity */
      ts_filter_check_cc_discontinuity(filter, pid, cc);

      /* Track total packets for this PID */
      ts_filter_update_packet_count(filter, pid);

      /* Track total packets in stream */
      if (filter->enable_stats) {
        filter->total_packets++;
      }

      /* Before processing this packet, verify it has the sync byte at the expected position.
       * Skip this check for small buffers (1-2 packets) to avoid losing trailing packets. */
      if (!trust_small_buffer && in_ptr[0] != TS_SYNC_BYTE) {
        GST_DEBUG_OBJECT(filter, "Lost sync at packet %u: expected 0x47 at offset %ld, got 0x%02x",
                       i, in_ptr - data, in_ptr[0]);
        /* Lost sync - set remaining to continue scanning from this position */
        filter->is_synced = FALSE;
        /* Mark this packet as corrupted (sync lost) */
        ts_filter_update_corrupted_count(filter, pid);
        /* Continue to sync establishment loop below - don't break, just let the loop
         * finish naturally so remaining is correctly set */
        break;
      }

      /* Validate CRC if enabled */
      if (filter->enable_crc_validation && !ts_filter_validate_packet_crc(filter, in_ptr, pid, FALSE)) {
        GST_WARNING_OBJECT(filter, "Rejecting packet with CRC error, PID %u", pid);
        /* Track corrupted packet */
        ts_filter_update_corrupted_count(filter, pid);
        /* Skip this packet and continue */
        in_ptr += filter->packet_size;
        remaining -= filter->packet_size;
        continue;
      }

      /* Emit signal for this PID (regardless of filter) - but only AFTER verifying sync byte.
       * This ensures we don't emit signals for invalid packets. */
      ts_filter_track_and_emit_pid(filter, pid);

      if (ts_filter_check_pid(filter, pid)) {
        packet_indices[result.good_packets] = in_ptr - data;
        result.filtered_out_size += filter->packet_size;
        result.good_packets++;
        /* Track filtered packet (passed the filter, will be pushed to src) */
        guint64 packet_offset = filter->byte_offset - remaining + (in_ptr - data);
        ts_filter_update_filtered_count(filter, pid, packet_offset);
      }
      in_ptr += filter->packet_size;
      remaining -= filter->packet_size;
    }

    /* If we lost sync, continue with sync establishment for remaining data */
    if (!filter->is_synced) {
      GST_DEBUG_OBJECT(filter, "Re-establishing sync with %zu remaining bytes", remaining);
      /* Continue to sync establishment loop below */
    } else {
      /* Still synced, no more packets to process */
      return result;
    }
  }

  /* ========== SYNC ESTABLISHMENT PHASE ========== */
  while (remaining >= filter->packet_size) {
    if (in_ptr[0] == TS_SYNC_BYTE) {
      /* Found potential sync byte at current position.
       * Need to validate it's at a genuine packet boundary using 3-sync pattern. */

      /* Determine if strict 3-sync validation is required:
       *
       * Rules for requiring 3-sync:
       * 1. first_iteration = TRUE: Initial sync, don't trust single sync byte
       * 2. lost_sync = TRUE: Just recovered from sync loss, need re-validation
       * 3. remaining >= 3 * packet_size: Have enough data for 3-sync check
       *
       * If any condition is FALSE, we can skip 3-sync (lenient mode) */
      gboolean need_3sync = (first_iteration || lost_sync) && (remaining >= 3 * filter->packet_size);

      /* Lenient sync validation for small buffers (1-2 packets)
       *
       * Small buffers occur at:
       * - End of stream (final packets)
       * - Buffer boundaries in GStreamer pipeline
       * - Very small test files
       *
       * Accept with reduced validation to avoid dropping valid data */
      gboolean lenient_sync = FALSE;
      if (first_iteration && !need_3sync && remaining >= filter->packet_size) {
        if (remaining == filter->packet_size) {
          /* Single packet: accept with just sync byte check
           * (Better to process it than to drop it) */
          lenient_sync = TRUE;
        } else if (remaining >= 2 * filter->packet_size) {
          /* Two packets: check sync at both packet boundaries */
          if (in_ptr[filter->packet_size] == TS_SYNC_BYTE) {
            lenient_sync = TRUE;
          }
        }
      }

      /* Validate 3-sync pattern */
      gboolean valid_3sync = FALSE;
      if (!need_3sync || lenient_sync) {
        /* Skip 3-sync validation (lenient mode or not needed) */
        valid_3sync = TRUE;
      } else if (in_ptr[filter->packet_size] == TS_SYNC_BYTE &&
                 in_ptr[2 * filter->packet_size] == TS_SYNC_BYTE) {
        /* Found 3 consecutive sync bytes - but must verify they're at VALID packet boundaries
         *
         * Verification 1: Packet Alignment
         * - Sync bytes must be at offsets 0, packet_size, 2*packet_size from buffer start
         * - This prevents false sync from payload bytes that happen to be 188 bytes apart
         *
         * Verification 2: CRC Validation (conditional)
         * - When resyncing after loss: force CRC to avoid false sync patterns
         * - When CRC validation enabled: validate all packets
         * - When initial syncing: only force CRC if explicitly enabled
         *   (Rationale: Allow sync establishment even if first packets have CRC errors) */
        gsize current_offset = in_ptr - data;
        if ((current_offset % filter->packet_size) != 0) {
          /* FAIL: Not at packet-aligned position
           * This is a false sync pattern from payload bytes
           * (Example: Corrupted packets with sync at position 134 instead of 0) */
          GST_WARNING_OBJECT(filter, "Rejecting non-packet-aligned 3-sync pattern at offset %zu (remainder=%zu)",
                            current_offset, current_offset % filter->packet_size);
          valid_3sync = FALSE;
        } else {
          /* PASS: Packet-aligned, proceed to CRC validation */
          valid_3sync = TRUE;
        }

        /* CRC validation strategy:
         *
         * Scenarios:
         * 1. Initial sync + CRC disabled: No CRC check (allow sync with bad CRC)
         * 2. Initial sync + CRC enabled: Check CRC, reject on error
         * 3. Resync after loss + CRC disabled: Force CRC check (prevent false sync)
         * 4. Resync after loss + CRC enabled: Force CRC check (prevent false sync)
         *
         * Implementation: force_crc = TRUE for cases 3 and 4 */
        gboolean force_crc = lost_sync || filter->enable_crc_validation;

        /* Extract PIDs from 3 packets for CRC validation */
        guint16 pid1 = ts_filter_extract_pid(in_ptr);
        guint16 pid2 = ts_filter_extract_pid(in_ptr + filter->packet_size);
        guint16 pid3 = ts_filter_extract_pid(in_ptr + 2 * filter->packet_size);

        /* Perform CRC validation on all 3 packets
         * Note: Only validates CRC if packets contain PSI/SI tables with CRC-32
         * Regular audio/video packets have no CRC and will pass immediately */
        gboolean crc1_ok = ts_filter_validate_packet_crc(filter, in_ptr, pid1, force_crc);
        gboolean crc2_ok = ts_filter_validate_packet_crc(filter, in_ptr + filter->packet_size, pid2, force_crc);
        gboolean crc3_ok = ts_filter_validate_packet_crc(filter, in_ptr + 2 * filter->packet_size, pid3, force_crc);

        /* Reject 3-sync if any CRC validation fails
         * (Only applies if packets actually contain CRC, otherwise passes) */
        if (!crc1_ok || !crc2_ok || !crc3_ok) {
          if (filter->enable_crc_validation) {
            /* User enabled CRC validation - emit WARNING */
            GST_WARNING_OBJECT(filter, "3-sync pattern failed CRC validation, continuing search (PIDs: %u, %u, %u)",
                            pid1, pid2, pid3);
          } else {
            /* CRC forced during resync - emit DEBUG only */
            GST_DEBUG_OBJECT(filter, "3-sync pattern failed CRC validation (CRC check forced during sync), continuing search (PIDs: %u, %u, %u)",
                            pid1, pid2, pid3);
          }
          valid_3sync = FALSE;
        }
      }

      if (valid_3sync) {
        /* ========== 3-SYNC PATTERN ACCEPTED - PACKET BOUNDARY ESTABLISHED ========== */

        /* Update sync state:
         * - First time: Mark as synced and disable 3-sync for subsequent packets
         * - After lost_sync: Mark as re-established and clear flag */
        if (!filter->is_synced) {
          filter->is_synced = TRUE;
          GST_DEBUG_OBJECT(filter, "Established MPEG-TS sync (3-sync validated)");
          first_iteration = FALSE;  /* Subsequent packets don't need 3-sync */
        }
        if (lost_sync) {
          lost_sync = FALSE;  /* Re-established sync after loss */
          GST_DEBUG_OBJECT(filter, "Re-established MPEG-TS sync after loss");
        }

        guint16 pid = ts_filter_extract_pid(in_ptr);
        guint8 cc = ts_filter_extract_cc(in_ptr);

        /* Check CC discontinuity */
        ts_filter_check_cc_discontinuity(filter, pid, cc);

        /* Track total packets for this PID */
        ts_filter_update_packet_count(filter, pid);

        /* Track total packets in stream */
        if (filter->enable_stats) {
          filter->total_packets++;
        }

        /* Validate CRC if enabled */
        if (filter->enable_crc_validation && !ts_filter_validate_packet_crc(filter, in_ptr, pid, FALSE)) {
          GST_WARNING_OBJECT(filter, "Rejecting packet with CRC error, PID %u", pid);
          /* Track corrupted packet */
          ts_filter_update_corrupted_count(filter, pid);
          /* Skip this packet and continue scanning */
          in_ptr += filter->packet_size;
          remaining -= filter->packet_size;
          continue;
        }

        /* Emit PID signal for ALL valid packets (regardless of filter) - consistent with fast path
         * This ensures all PIDs are discovered and pads can be created for them */
        ts_filter_track_and_emit_pid(filter, pid);

        if (ts_filter_check_pid(filter, pid)) {
          /* Packet passed the filter - record it for output */
          packet_indices[result.good_packets] = in_ptr - data;
          result.filtered_out_size += filter->packet_size;
          result.good_packets++;
          /* Track filtered packet (passed the filter, will be pushed to src) */
          guint64 packet_offset = filter->byte_offset - remaining + (in_ptr - data);
          ts_filter_update_filtered_count(filter, pid, packet_offset);
        }
        in_ptr += filter->packet_size;
        remaining -= filter->packet_size;
      } else {
        /* ========== FALSE SYNC DETECTED - SCAN FOR REAL PACKET BOUNDARY ========== */

        /* We have a sync byte at current position, but 3-sync validation failed.
         * This could indicate:
         * 1. Start of corrupted packet (sync byte in payload)
         * 2. Random data that happens to be 0x47
         *
         * Action: Scan forward to find next valid packet boundary.
         *
         * Scanning strategy:
         * - Only check at packet_size intervals (not byte-by-byte)
         * - This avoids false positives from sync bytes within payloads
         * - Continue until we find a valid 3-sync pattern or run out of data */
        const guint8 *scan_ptr = in_ptr + filter->packet_size;
        gsize scan_remaining = remaining - filter->packet_size;
        gboolean found = FALSE;

        while (scan_remaining >= 3 * filter->packet_size) {
          /* Verify scan_ptr is at packet-aligned position
           * (Prevents accepting sync bytes from within corrupted packet payloads) */
          gsize scan_offset = scan_ptr - data;
          if ((scan_offset % filter->packet_size) != 0) {
            /* Not packet-aligned - skip this position immediately */
            GST_DEBUG_OBJECT(filter, "Skipping non-packet-aligned 3-sync pattern at offset %zu", scan_offset);
            scan_ptr += filter->packet_size;
            scan_remaining -= filter->packet_size;
            continue;
          }

          /* Check for 3 consecutive sync bytes at packet_size intervals */
          if (scan_ptr[0] == TS_SYNC_BYTE &&
              scan_ptr[filter->packet_size] == TS_SYNC_BYTE &&
              scan_ptr[2 * filter->packet_size] == TS_SYNC_BYTE) {
            /* Found 3-sync pattern - validate with CRC to prevent false sync
             * Force CRC validation during forward scanning for extra robustness
             * (Always force here regardless of enable_crc_validation setting) */
            guint16 pid1 = ts_filter_extract_pid(scan_ptr);
            guint16 pid2 = ts_filter_extract_pid(scan_ptr + filter->packet_size);
            guint16 pid3 = ts_filter_extract_pid(scan_ptr + 2 * filter->packet_size);
            guint8 cc1 = ts_filter_extract_cc(scan_ptr);
            guint8 cc2 = ts_filter_extract_cc(scan_ptr + filter->packet_size);
            guint8 cc3 = ts_filter_extract_cc(scan_ptr + 2 * filter->packet_size);

            /* Check CC discontinuity for these packets */
            ts_filter_check_cc_discontinuity(filter, pid1, cc1);
            ts_filter_check_cc_discontinuity(filter, pid2, cc2);
            ts_filter_check_cc_discontinuity(filter, pid3, cc3);

            /* Track total packets for these PIDs */
            ts_filter_update_packet_count(filter, pid1);
            ts_filter_update_packet_count(filter, pid2);
            ts_filter_update_packet_count(filter, pid3);

            /* Track total packets in stream */
            if (filter->enable_stats) {
              filter->total_packets += 3;
            }

            gboolean crc1_ok = ts_filter_validate_packet_crc(filter, scan_ptr, pid1, TRUE);  /* force validation */
            gboolean crc2_ok = ts_filter_validate_packet_crc(filter, scan_ptr + filter->packet_size, pid2, TRUE);  /* force validation */
            gboolean crc3_ok = ts_filter_validate_packet_crc(filter, scan_ptr + 2 * filter->packet_size, pid3, TRUE);  /* force validation */

            /* Track corrupted packets if CRC failed */
            if (!crc1_ok) ts_filter_update_corrupted_count(filter, pid1);
            if (!crc2_ok) ts_filter_update_corrupted_count(filter, pid2);
            if (!crc3_ok) ts_filter_update_corrupted_count(filter, pid3);

            gboolean accept_sync = TRUE;
            if (!crc1_ok || !crc2_ok || !crc3_ok) {
              /* CRC validation failed - false sync pattern */
              if (filter->enable_crc_validation) {
                GST_WARNING_OBJECT(filter, "Scanned 3-sync pattern failed CRC validation (PIDs: %u, %u, %u), continuing scan",
                                pid1, pid2, pid3);
              } else {
                GST_DEBUG_OBJECT(filter, "Scanned 3-sync pattern failed CRC validation (CRC check forced during sync, PIDs: %u, %u, %u), continuing scan",
                                pid1, pid2, pid3);
              }
              accept_sync = FALSE;
            }

            if (accept_sync) {
              /* Valid 3-sync found! Skip all bytes before this position and resume processing */
              result.bad_sync_count += (scan_ptr - in_ptr);
              in_ptr = scan_ptr;
              remaining = scan_remaining;
              found = TRUE;
              break;
            }
          }
          /* Move to next packet boundary for next scan iteration */
          scan_ptr += filter->packet_size;
          scan_remaining -= filter->packet_size;
        }

        if (!found) {
          /* Exhausted buffer without finding valid sync - discard remaining bytes */
          result.bad_sync_count += (remaining - (remaining % filter->packet_size));
          break;
        }
      }
    } else {
      /* No sync byte at current position - skip this byte and continue scanning
       * Mark lost_sync = TRUE so we'll require 3-sync validation when we find next sync byte */
      result.bad_sync_count++;
      in_ptr++;
      remaining--;
      lost_sync = TRUE;
    }
  }

  return result;
}

/*
 * Auto-detect packet size if enabled and update filter accordingly
 * Returns: TRUE if detection was attempted (regardless of success)
 */
static gboolean
ts_filter_detect_and_set_packet_size(TsFilter *filter, const guint8 *data, gsize size)
{
  if (!filter->auto_detect || filter->detected_packet_size != 0) {
    return FALSE;
  }

  guint detected = ts_filter_detect_packet_size(filter, data, size, FALSE);  /* Not at EOS */
  if (detected == 0) {
    return TRUE;
  }

  filter->detected_packet_size = detected;

  if (detected != filter->packet_size) {
    guint user_specified = filter->packet_size;

    /* Emit WARNING instead of ERROR - auto-detection is handling the mismatch */
    GST_WARNING_OBJECT(filter, "Packet size mismatch: user specified %u bytes but auto-detected %u bytes. Using detected size.",
                      user_specified, detected);

    g_signal_emit(filter, signals[SIGNAL_PACKET_SIZE_MISMATCH], 0,
                 user_specified, detected);

    filter->packet_size = detected;
  } else {
    GST_INFO_OBJECT(filter, "Auto-detected packet size matches user setting: %u bytes",
                    detected);
  }

  return TRUE;
}

/*
 * Allocate output buffer, trying pool first with fallback to normal allocation
 */
static GstBuffer *
ts_filter_alloc_output_buffer(TsFilter *filter, gsize size)
{
  GstBuffer *outbuf = NULL;

  ts_filter_ensure_pools(filter);

  g_mutex_lock(&filter->pool_lock);
  if (filter->srcpool &&
      size <= filter->packet_size * 256 &&
      gst_buffer_pool_is_active(filter->srcpool)) {
    if (gst_buffer_pool_acquire_buffer(filter->srcpool, &outbuf, NULL) != GST_FLOW_OK) {
      outbuf = NULL;
    }
  }
  g_mutex_unlock(&filter->pool_lock);

  /* Fallback to normal allocation if pool failed or buffer too large */
  if (outbuf == NULL) {
    outbuf = gst_buffer_new_and_alloc(size);
  } else if (gst_buffer_get_size(outbuf) != size) {
    /* Resize buffer if needed */
    GstBuffer *tmp = gst_buffer_new_and_alloc(size);
    if (tmp) {
      gst_buffer_unref(outbuf);
      outbuf = tmp;
    }
  }

  return outbuf;
}

/* Copy filtered packets to output buffer using packet indices */
static void
ts_filter_copy_packets(guint8 *out_ptr, const guint8 *in_data,
                       const guint *packet_indices, guint count,
                       guint packet_size)
{
  for (guint i = 0; i < count; i++) {
    const guint8 *packet_start = in_data + packet_indices[i];
    memcpy(out_ptr, packet_start, packet_size);
    out_ptr += packet_size;
  }
}

/* Validate and filter TS packets from buffer */
static GstBuffer *
ts_filter_process_buffer(TsFilter *filter, GstBuffer *buffer)
{
  GstBuffer *outbuf;
  GstMapInfo in_map, out_map;
  guint *packet_indices = NULL;
  PacketScanResult scan_result;

  /* Map input buffer */
  if (!gst_buffer_map(buffer, &in_map, GST_MAP_READ)) {
    return NULL;
  }

  /* Pre-allocate packet indices array (max possible packets) */
  packet_indices = g_new(guint, in_map.size / filter->packet_size + 1);

  /* Auto-detect packet size if enabled and not yet detected */
  ts_filter_detect_and_set_packet_size(filter, in_map.data, in_map.size);

  /* Scan for valid TS packets and record positions */
  scan_result = ts_filter_scan_packets(filter, in_map.data, in_map.size,
                                       packet_indices);

  /* Report bad sync bytes */
  if (scan_result.bad_sync_count > 0) {
    filter->bad_packet_count += scan_result.bad_sync_count;
    ts_filter_emit_bad_packet(filter, scan_result.bad_sync_count);
    GST_WARNING_OBJECT(filter, "Discarded %u bytes with bad sync alignment",
                      scan_result.bad_sync_count);
  }

  /* No packets passed the filter */
  if (scan_result.filtered_out_size == 0) {
    GST_DEBUG_OBJECT(filter, "No packets passed filter from %" G_GSIZE_FORMAT " input bytes", in_map.size);
    g_free(packet_indices);
    gst_buffer_unmap(buffer, &in_map);
    return NULL;
  }

  GST_DEBUG_OBJECT(filter, "Scanned %" G_GSIZE_FORMAT " bytes: %u good packets, %u bad_sync bytes, output %zu bytes",
                 in_map.size, scan_result.good_packets, scan_result.bad_sync_count, scan_result.filtered_out_size);

  /* Log packet details for debugging */
  guint expected_packets = in_map.size / filter->packet_size;
  if (scan_result.good_packets != expected_packets) {
    GST_WARNING_OBJECT(filter, "Packet count mismatch: found %u packets but expected %u (lost %u packets)",
                       scan_result.good_packets, expected_packets, expected_packets - scan_result.good_packets);
  }

  /* Allocate output buffer (tries pool first, then fallback) */
  outbuf = ts_filter_alloc_output_buffer(filter, scan_result.filtered_out_size);
  if (outbuf == NULL) {
    g_free(packet_indices);
    gst_buffer_unmap(buffer, &in_map);
    return NULL;
  }

  /* Map output buffer */
  if (!gst_buffer_map(outbuf, &out_map, GST_MAP_WRITE)) {
    gst_buffer_unref(outbuf);
    gst_buffer_unmap(buffer, &in_map);
    g_free(packet_indices);
    return NULL;
  }

  /* Copy filtered packets using stored indices */
  ts_filter_copy_packets(out_map.data, in_map.data, packet_indices,
                         scan_result.good_packets, filter->packet_size);

  g_free(packet_indices);

  /* Copy metadata using GstBufferCopyFlags */
  gst_buffer_copy_into(outbuf, buffer,
                       (GstBufferCopyFlags)(GST_BUFFER_COPY_METADATA | GST_BUFFER_COPY_TIMESTAMPS),
                       0, -1);

  gst_buffer_unmap(buffer, &in_map);
  gst_buffer_unmap(outbuf, &out_map);

  return outbuf;
}

/* MPEG-TS packet sizes (in frequency order) */
#define PACKET_SIZE_188 188  /* Standard MPEG-TS (DVB, ATSC) - 95% of streams */
#define PACKET_SIZE_192 192  /* M2TS (Blu-ray, adds 4-byte timestamp) */
#define PACKET_SIZE_204 204  /* DVB-ASI (with 16-byte FEC) */
#define PACKET_SIZE_208 208  /* ATSC (with extra padding) */

/* Detect MPEG-TS packet size by finding sync byte (0x47) patterns
 *
 * Detection methods (robustness order):
 * - 4-sync: 4 consecutive sync bytes (most reliable, requires ≥752 bytes)
 * - 3-sync: 3 consecutive sync bytes (normal, requires ≥564 bytes)
 * - 2-sync: 2 consecutive sync bytes (EOS only, requires ≥376 bytes)
 *
 * @at_eos: TRUE enables 2-sync for small files at stream end
 * Returns: packet size (188/192/204/208) or 0 on failure
 */
static guint
ts_filter_detect_packet_size(TsFilter *filter, const guint8 *data, gsize size, gboolean at_eos)
{
  /* Try packet sizes in order of frequency (188 is 95% of streams) */
  static const guint packet_sizes[] = {
    PACKET_SIZE_188,  /* Standard MPEG-TS (DVB, ATSC) - MOST COMMON */
    PACKET_SIZE_192,  /* M2TS (Blu-ray) */
    PACKET_SIZE_204,  /* DVB-ASI */
    PACKET_SIZE_208   /* ATSC */
  };
  guint i, j;

  GST_DEBUG_OBJECT(filter, "Attempting to auto-detect packet size from %" G_GSIZE_FORMAT " bytes", size);

  /* Try each packet size in frequency order */
  for (j = 0; j < G_N_ELEMENTS(packet_sizes); j++) {
    guint packet_size = packet_sizes[j];

    /* 4-sync detection (most robust) */
    if (size >= 4 * packet_size) {
      for (i = 0; i + 3 * packet_size < size; i++) {
        if (data[i] == TS_SYNC_BYTE &&
            data[i + packet_size] == TS_SYNC_BYTE &&
            data[i + 2 * packet_size] == TS_SYNC_BYTE &&
            data[i + 3 * packet_size] == TS_SYNC_BYTE) {
          GST_INFO_OBJECT(filter, "Auto-detected packet size: %u bytes (4-sync) at offset %u",
                          packet_size, i);
          return packet_size;
        }
      }
    }

    /* 3-sync detection (normal mode) */
    if (size >= 3 * packet_size) {
      for (i = 0; i + 2 * packet_size < size; i++) {
        if (data[i] == TS_SYNC_BYTE &&
            data[i + packet_size] == TS_SYNC_BYTE &&
            data[i + 2 * packet_size] == TS_SYNC_BYTE) {
          GST_INFO_OBJECT(filter, "Auto-detected packet size: %u bytes (3-sync) at offset %u",
                          packet_size, i);
          return packet_size;
        }
      }
    }

    /* 2-sync detection (EOS fallback, less reliable) */
    if (at_eos && size >= 2 * packet_size) {
      for (i = 0; i + packet_size < size; i++) {
        /* Check for 2 consecutive sync bytes at packet_size intervals */
        if (data[i] == TS_SYNC_BYTE &&
            data[i + packet_size] == TS_SYNC_BYTE) {
          GST_WARNING_OBJECT(filter, "Auto-detected packet size: %u bytes (2-sync at EOS) at offset %u - less reliable",
                           packet_size, i);
          return packet_size;
        }
      }
    }
  }

  GST_WARNING_OBJECT(filter, "Failed to auto-detect packet size");
  return 0;
}

/*
 * Validate and filter TS packets from buffer
 *
 * Returns two buffers:
 * - filtered_buf: packets passing sync validation AND PID filter (for src pad)
 * - valid_buf: ALL packets passing sync validation (for PID pads)
 *
 * Sync algorithm (3-sync):
 * - Requires 3 consecutive sync bytes (0x47) at packet_size intervals
 * - False sync probability: ≈ 1/16,777,216
 * - On sync failure: scans forward byte-by-byte to find valid boundary
 */

/* Allocate buffer for PID-specific packet output (always normal allocation) */
static GstBuffer *
ts_filter_alloc_pid_buffer(TsFilter *filter)
{
  return gst_buffer_new_and_alloc(filter->packet_size);
}

/* Push a single packet to a PID-specific pad (allocates, copies, pushes) */
static void
ts_filter_push_single_packet(TsFilter *filter, GstPad *pid_pad,
                              const guint8 *packet_data, GstBuffer *src_buffer,
                              guint pid)
{
  GstBuffer *pid_buf;

  pid_buf = ts_filter_alloc_pid_buffer(filter);
  if (pid_buf == NULL) {
    GST_WARNING_OBJECT(filter, "Failed to allocate buffer for PID %u", pid);
    return;
  }

  /* Copy packet data */
  GstMapInfo pid_map;
  if (!gst_buffer_map(pid_buf, &pid_map, GST_MAP_WRITE)) {
    gst_buffer_unref(pid_buf);
    GST_WARNING_OBJECT(filter, "Failed to map buffer for PID %u", pid);
    return;
  }

  memcpy(pid_map.data, packet_data, filter->packet_size);
  gst_buffer_unmap(pid_buf, &pid_map);

  /* Copy metadata from source buffer */
  gst_buffer_copy_into(pid_buf, src_buffer,
                      (GstBufferCopyFlags)(GST_BUFFER_COPY_METADATA | GST_BUFFER_COPY_TIMESTAMPS),
                      0, -1);

  /* Push to PID-specific pad */
  GST_DEBUG_OBJECT(filter, "BEFORE gst_pad_push to PID %u", pid);
  GstFlowReturn ret = gst_pad_push(pid_pad, pid_buf);
  GST_DEBUG_OBJECT(filter, "AFTER gst_pad_push to PID %u, ret=%s", pid, gst_flow_get_name(ret));
  ret = gst_flow_combiner_update_flow(filter->flow_combiner, ret);
  if (ret != GST_FLOW_OK && ret != GST_FLOW_NOT_LINKED) {
    GST_WARNING_OBJECT(filter, "Push to PID %u pad failed: %s", pid, gst_flow_get_name(ret));
  }
}

/* Push packets to PID-specific pads
 * Returns: number of bytes processed from buffer
 */
static gsize
ts_filter_push_to_pid_pads(TsFilter *filter, GstBuffer *buffer)
{
  GstMapInfo in_map;
  const guint8 *in_ptr;
  gsize remaining;

  GST_DEBUG_OBJECT(filter, "Pushing to PID pads, hash table size: %d, buffer size: %zu bytes",
                 g_hash_table_size(filter->pid_pads), gst_buffer_get_size(buffer));

  if (!gst_buffer_map(buffer, &in_map, GST_MAP_READ)) {
    return 0;
  }

  in_ptr = in_map.data;
  remaining = in_map.size;

  /* Process packets with boundary validation and 3-sync checking
   * accumulated_buffer contains raw data, so we must validate sync boundaries
   * to prevent false positives from corrupted data */
  while (remaining >= filter->packet_size) {
    if (in_ptr[0] == TS_SYNC_BYTE) {
      gsize current_offset = in_ptr - (const guint8 *)in_map.data;

      /* Boundary check: reject sync bytes at wrong offsets (prevents CC 9 issue) */
      guint64 stream_pos = GST_BUFFER_OFFSET(buffer);
      gboolean at_packet_boundary = FALSE;

      if (stream_pos != GST_BUFFER_OFFSET_NONE) {
        guint64 absolute_pos = stream_pos + current_offset;
        at_packet_boundary = (absolute_pos % filter->packet_size == 0);
      } else {
        at_packet_boundary = (current_offset % filter->packet_size == 0);
      }

      if (!at_packet_boundary) {
        guint16 check_pid = ts_filter_extract_pid(in_ptr);
        guint8 check_cc = ts_filter_extract_cc(in_ptr);
        GST_DEBUG_OBJECT(filter, "Skipping sync byte at buffer offset %td (PID %u CC %u) - NOT at packet boundary",
                       current_offset, check_pid, check_cc);
        in_ptr++;
        remaining--;
        continue;
      }

      /* Use strict 3-sync only during initial sync establishment
       * Once synced, trust sync byte to avoid packet loss at buffer boundaries */
      gboolean use_strict_3sync = !filter->is_synced;
      gboolean is_valid_packet = FALSE;

      if (!use_strict_3sync && remaining >= 3 * filter->packet_size) {
        guint16 debug_pid = ts_filter_extract_pid(in_ptr);
        guint8 debug_cc = ts_filter_extract_cc(in_ptr);
        GST_DEBUG_OBJECT(filter, "Using lenient 3-sync (is_synced=%d): PID %u CC %u",
                       filter->is_synced, debug_pid, debug_cc);
      }

      /* For small buffers (1-2 packets), also use lenient check to avoid losing packets */
      if (remaining >= 3 * filter->packet_size) {
        /* We have enough data for strict 3-sync validation */
        if (use_strict_3sync) {
          if (in_ptr[filter->packet_size] == TS_SYNC_BYTE &&
              in_ptr[2 * filter->packet_size] == TS_SYNC_BYTE) {
            is_valid_packet = TRUE;
          } else {
            /* 3-sync failed - need to find valid sync boundary
             * Strategy: Check current position first, then scan forward if needed */
            gboolean found = FALSE;

            /* First, check if current position has a sync byte AND verify we're at a valid stream boundary
             * We use the buffer's offset metadata to determine the absolute stream position,
             * which allows us to correctly validate packet boundaries even when buffers don't
             * start at packet boundaries. */
            if (in_ptr[0] == TS_SYNC_BYTE) {
              guint16 current_pid = ts_filter_extract_pid(in_ptr);
              guint8 current_cc = ts_filter_extract_cc(in_ptr);
              gsize buffer_offset = in_ptr - (const guint8 *)in_map.data;

              /* Get absolute stream position using buffer offset metadata */
              guint64 stream_pos = GST_BUFFER_OFFSET(buffer);
              gboolean at_valid_boundary = FALSE;

              if (stream_pos != GST_BUFFER_OFFSET_NONE) {
                /* Buffer has offset metadata - calculate absolute stream position */
                guint64 absolute_pos = stream_pos + buffer_offset;
                at_valid_boundary = (absolute_pos % filter->packet_size == 0);
                GST_DEBUG_OBJECT(filter, "3-sync failed at buffer offset %td (stream offset %" G_GUINT64_FORMAT ", absolute %" G_GUINT64_FORMAT "), sync byte (PID %u CC %u), boundary check: %s",
                               buffer_offset, stream_pos, absolute_pos, current_pid, current_cc,
                               at_valid_boundary ? "VALID" : "INVALID");
              } else {
                /* No offset metadata - fall back to buffer offset check (may reject valid packets at buffer boundaries) */
                at_valid_boundary = (buffer_offset % filter->packet_size == 0);
                GST_DEBUG_OBJECT(filter, "3-sync failed at buffer offset %td (no stream offset), sync byte (PID %u CC %u), using buffer alignment: %s",
                               buffer_offset, current_pid, current_cc,
                               at_valid_boundary ? "VALID" : "INVALID");
              }

              if (at_valid_boundary) {
                found = TRUE;
              } else {
                /* Fall through to scan-forward logic */
              }
            } else {
              /* Current position doesn't have sync byte - scan forward packet-by-packet
               * to find next valid sync boundary */
              const guint8 *scan_ptr = in_ptr + filter->packet_size;
              gsize scan_remaining = remaining - filter->packet_size;

              GST_DEBUG_OBJECT(filter, "3-sync failed and current position lacks sync byte, scanning forward from offset %td",
                             scan_ptr - in_map.data);

              /* Scan for valid sync pattern at packet-aligned positions only */
              guint scan_count = 0;
              while (scan_remaining >= filter->packet_size) {
                scan_count++;
                guint16 scan_pid = ts_filter_extract_pid(scan_ptr);
                guint8 scan_cc = ts_filter_extract_cc(scan_ptr);
                GST_DEBUG_OBJECT(filter, "  Scan-forward iteration %u: offset %td, PID %u CC %u, has_sync=%d",
                               scan_count, scan_ptr - in_map.data, scan_pid, scan_cc, scan_ptr[0] == TS_SYNC_BYTE);

                /* Accept first sync byte found during scan-forward */
                if (scan_ptr[0] == TS_SYNC_BYTE) {
                  GST_DEBUG_OBJECT(filter, "  Accepting sync byte at offset %td", scan_ptr - in_map.data);
                  in_ptr = scan_ptr;
                  remaining = scan_remaining;
                  found = TRUE;
                  break;
                }
                scan_ptr += filter->packet_size;
                scan_remaining -= filter->packet_size;
              }
            }

            if (!found) {
              /* No valid sync found - discard remaining bytes up to packet boundary */
              gsize discard_bytes = remaining - (remaining % filter->packet_size);
              in_ptr += discard_bytes;
              remaining -= discard_bytes;
              GST_DEBUG_OBJECT(filter, "No valid sync found, discarded %zu bytes", discard_bytes);
            } else {
              /* Scan-forward found a valid sync position - mark as valid and continue
               * Don't check 3-sync again to avoid infinite loop when scan-forward accepts a packet
               * that would fail 3-sync validation (e.g., at buffer boundaries) */
              is_valid_packet = TRUE;
            }
          }
        } else {
          /* Already synced - trust the sync byte at current position
           * Don't skip packets even if 3-sync pattern is broken
           * This prevents packet loss at buffer boundaries */
          is_valid_packet = TRUE;
        }
      } else if (filter->is_synced || remaining <= 2 * filter->packet_size) {
        /* Less than 3 packets left, but we're already synced - trust the sync byte
         * OR we have a small buffer (1-2 packets) - accept it without 3-sync
         * This is safe because:
         * 1. We've already validated earlier packets with strict 3-sync
         * 2. If these are false syncs, the next buffer will fail to establish sync
         * 3. This handles both EOS and intermediate buffer boundaries
         * 4. Small test files with 1-2 packets need to be processed */
        is_valid_packet = TRUE;
      }

      if (is_valid_packet) {
        /* This is a valid packet - process it */
        guint pid = ts_filter_extract_pid(in_ptr);
        guint8 cc = ts_filter_extract_cc(in_ptr);

        /* Enhanced logging for problematic packets */
        if ((pid == 2511 && cc == 8) || (pid == 2721 && cc == 3)) {
          GST_DEBUG_OBJECT(filter, "*** FOUND TARGET PACKET *** PID %u CC %u at buffer offset %td",
                         pid, cc, in_ptr - in_map.data);
        }

        /* Check CC discontinuity */
        ts_filter_check_cc_discontinuity(filter, pid, cc);

        /* Track total packets for this PID (statistics) */
        ts_filter_update_packet_count(filter, pid);

        /* Track total packets in stream */
        if (filter->enable_stats) {
          filter->total_packets++;
        }

        /* Track this PID for new-pid signal */
        ts_filter_track_and_emit_pid(filter, pid);

        GstPad *pid_pad = g_hash_table_lookup(filter->pid_pads, GUINT_TO_POINTER(pid));

        if (pid_pad != NULL) {
          GST_DEBUG_OBJECT(filter, "Found PID %u CC %u packet at buffer offset %td, pushing to pad",
                         pid, cc, in_ptr - in_map.data);
          ts_filter_push_single_packet(filter, pid_pad, in_ptr, buffer, pid);
        } else {
          /* Pad doesn't exist yet - buffer packet for later delivery */
          GST_DEBUG_OBJECT(filter, "PID %u pad not ready, buffering packet", pid);
          g_mutex_lock(&filter->pending_lock);

          /* Allocate copy of packet data (overflow-safe with g_memdup2) */
          guint8 *packet_data = g_memdup2(in_ptr, filter->packet_size);
          if (packet_data) {
            /* Steal list to avoid double-free, prepend packet, re-insert */
            GSList *pending_list = (GSList *)g_hash_table_lookup(filter->pending_packets, GUINT_TO_POINTER(pid));
            if (pending_list) {
              g_hash_table_steal(filter->pending_packets, GUINT_TO_POINTER(pid));
            }
            pending_list = g_slist_prepend(pending_list, packet_data);
            g_hash_table_insert(filter->pending_packets, GUINT_TO_POINTER(pid), pending_list);
            GST_INFO_OBJECT(filter, "Added pending packet for PID %u (now %d packets)",
                           pid, g_slist_length(pending_list));
          }
          g_mutex_unlock(&filter->pending_lock);
        }

        in_ptr += filter->packet_size;
        remaining -= filter->packet_size;
      } else {
        /* Sync byte found but failed 3-sync validation - corrupted data */
        GST_DEBUG_OBJECT(filter, "False sync at offset %td, skipping byte",
                         in_ptr - (guint8 *)in_map.data);
        in_ptr++;
        remaining--;
      }
    } else {
      /* No sync byte found */
      in_ptr++;
      remaining--;
    }
  }

  gsize bytes_processed = in_map.size - remaining;

  GST_DEBUG_OBJECT(filter, "Finished scanning PID pads, processed %zu packets, %zu bytes",
                 bytes_processed / filter->packet_size, bytes_processed);

  gst_buffer_unmap(buffer, &in_map);

  return bytes_processed;  /* Return actual bytes processed for accurate flushing */
}

/* Replay cached events to a newly requested pad */
static void
ts_filter_replay_cached_events(TsFilter *filter, GstPad *pad)
{
  g_mutex_lock(&filter->event_cache_lock);

  /* Replay events in the correct order: STREAM_START, CAPS, SEGMENT */
  if (filter->cached_stream_start) {
    GST_DEBUG_OBJECT(filter, "Replaying STREAM_START event to pad %s",
                     GST_PAD_NAME(pad));
    gst_pad_push_event(pad, gst_event_ref(filter->cached_stream_start));
  }

  if (filter->cached_caps) {
    GST_DEBUG_OBJECT(filter, "Replaying CAPS event to pad %s",
                     GST_PAD_NAME(pad));
    gst_pad_push_event(pad, gst_event_ref(filter->cached_caps));
  }

  if (filter->cached_segment) {
    GST_DEBUG_OBJECT(filter, "Replaying SEGMENT event to pad %s",
                     GST_PAD_NAME(pad));
    gst_pad_push_event(pad, gst_event_ref(filter->cached_segment));
  }

  g_mutex_unlock(&filter->event_cache_lock);
}

/* Request new pad for sink or PID-specific src pad */
static GstPad *
ts_filter_request_new_pad(GstElement * element,
                        G_GNUC_UNUSED GstPadTemplate * templ,
                        const gchar * name,
                        G_GNUC_UNUSED const GstCaps * caps)
{
  TsFilter *filter = TSFILTER(element);

  /* Handle sink pad request */
  if (filter->sinkpad == NULL) {
    filter->sinkpad = gst_pad_new_from_static_template(&sink_template, "sink");
    if (!filter->sinkpad) {
      GST_ERROR_OBJECT(filter, "Failed to create sink pad");
      return NULL;
    }
    gst_pad_set_chain_function(filter->sinkpad,
                               GST_DEBUG_FUNCPTR(ts_filter_chain));
    gst_pad_set_event_function(filter->sinkpad,
                               GST_DEBUG_FUNCPTR(ts_filter_sink_event));
    gst_element_add_pad(GST_ELEMENT(filter), filter->sinkpad);
    GST_DEBUG_OBJECT(filter, "Created requested sink pad");
    return filter->sinkpad;
  }

  /* Handle PID-specific src pad requests (format: src_<PID>) */
  if (name != NULL && g_str_has_prefix(name, "src_")) {
    guint pid = 0;
    if (sscanf(name, "src_%u", &pid) == 1 && pid <= 8191) {
      GstPad *pad;
      gchar *pad_name;

      /* Check if pad for this PID already exists */
      pad = g_hash_table_lookup(filter->pid_pads, GUINT_TO_POINTER(pid));
      if (pad != NULL) {
        GST_DEBUG_OBJECT(filter, "Pad for PID %u already exists", pid);
        return NULL;
      }

      /* Create new PID-specific src pad */
      pad_name = g_strdup_printf("src_%u", pid);
      if (!pad_name) {
        GST_ERROR_OBJECT(filter, "Failed to allocate pad name for PID %u", pid);
        return NULL;
      }
      pad = gst_pad_new_from_static_template(&pid_src_template, pad_name);
      g_free(pad_name);

      if (!pad) {
        GST_ERROR_OBJECT(filter, "Failed to create PID-specific src pad for PID %u", pid);
        return NULL;
      }

      /* Set query function */
      gst_pad_set_query_function(pad, GST_DEBUG_FUNCPTR(ts_filter_src_query));

      /* CRITICAL: Activate pad IMMEDIATELY (like mpegtsparse does) */
      /* This must happen before adding to element to avoid preroll deadlock */
      gst_pad_set_active(pad, TRUE);

      /* Add pad to element */
      gst_element_add_pad(GST_ELEMENT(filter), pad);

      /* Add to flow combiner */
      gst_flow_combiner_add_pad(filter->flow_combiner, pad);

      /* Add to hash table */
      g_hash_table_insert(filter->pid_pads, GUINT_TO_POINTER(pid), pad);

      /* Push any pending packets for this PID */
      g_mutex_lock(&filter->pending_lock);
      GST_DEBUG_OBJECT(filter, "Checking for pending packets for PID %u, pending_packets hash table size: %u",
                     pid, g_hash_table_size(filter->pending_packets));
      GSList *pending_list = (GSList *)g_hash_table_lookup(filter->pending_packets, GUINT_TO_POINTER(pid));
      GST_DEBUG_OBJECT(filter, "Pending list lookup result: %p", pending_list);
      if (pending_list) {
        guint num_pending = g_slist_length(pending_list);
        GST_INFO_OBJECT(filter, "Pushing %u pending packets for PID %u", num_pending, pid);
        /* Packets were prepended (reverse order), so iterate from last to first */
        GSList *reversed = g_slist_reverse(pending_list);
        guint count = 0;
        for (GSList *l = reversed; l; l = l->next) {
          guint8 *packet_data = (guint8 *)l->data;
          guint16 pending_pid = ((packet_data[1] & 0x1F) << 8) | packet_data[2];
          guint8 pending_cc = packet_data[3] & 0x0F;
          GST_DEBUG_OBJECT(filter, "  Pushing pending packet %u: PID %u CC %u", ++count, pending_pid, pending_cc);
          /* Create a temporary buffer from the packet data
           * Use a GDestroyNotify to free the packet data when GstMemory is unreferenced */
          GstMemory *mem = gst_memory_new_wrapped(GST_MEMORY_FLAG_READONLY, packet_data,
                                                    filter->packet_size, 0, filter->packet_size,
                                                    packet_data, (GDestroyNotify)g_free);
          GstBuffer *tmp_buf = gst_buffer_new();
          gst_buffer_append_memory(tmp_buf, mem);
          ts_filter_push_single_packet(filter, pad, packet_data, tmp_buf, pid);
          gst_buffer_unref(tmp_buf);
          /* Note: packet_data will be freed automatically when tmp_buf is unreferenced
           * via the GDestroyNotify we set above */
        }
        /* Free the list structure (data is owned by GstMemory now)
         * Use g_hash_table_steal to avoid double-free (destroy notify won't be called) */
        g_slist_free(reversed);
        g_hash_table_steal(filter->pending_packets, GUINT_TO_POINTER(pid));
        GST_DEBUG_OBJECT(filter, "Finished pushing pending packets for PID %u", pid);
      } else {
        GST_DEBUG_OBJECT(filter, "No pending packets for PID %u", pid);
      }
      g_mutex_unlock(&filter->pending_lock);

      GST_DEBUG_OBJECT(filter, "Created PID %u pad, total pending PIDs: %u",
                     pid, g_hash_table_size(filter->pending_packets));

      /* Replay cached events (STREAM_START, CAPS, SEGMENT) to the new pad */
      ts_filter_replay_cached_events(filter, pad);

      GST_INFO_OBJECT(filter, "Created and activated PID-specific src pad for PID %u", pid);
      return pad;
    } else {
      GST_WARNING_OBJECT(filter, "Invalid PID pad name: %s", name);
    }
  }

  return NULL;
}

/* Release a requested pad */
static void
ts_filter_release_pad(GstElement *element, GstPad *pad)
{
  TsFilter *filter = TSFILTER(element);
  const gchar *pad_name;

  pad_name = GST_PAD_NAME(pad);

  /* Handle PID-specific src pad release */
  if (g_str_has_prefix(pad_name, "src_")) {
    guint pid = 0;
    if (sscanf(pad_name, "src_%u", &pid) == 1) {
      GST_INFO_OBJECT(filter, "Releasing PID-specific src pad for PID %u", pid);

      /* Deactivate pad before releasing */
      gst_pad_set_active(pad, FALSE);

      /* Remove from flow combiner */
      gst_flow_combiner_remove_pad(filter->flow_combiner, pad);

      /* Remove from hash table WITHOUT destroying (element still owns the pad) */
      gpointer orig_key;
      if (g_hash_table_lookup_extended(filter->pid_pads, GUINT_TO_POINTER(pid),
                                      &orig_key, NULL)) {
        g_hash_table_steal(filter->pid_pads, GUINT_TO_POINTER(pid));
      }

      /* Remove pad from element - this will unref it properly */
      gst_element_remove_pad(element, pad);
    }
  }
}

/*
 * Element state change - activate request pads at the right time
 */
static GstStateChangeReturn
ts_filter_change_state(GstElement *element, GstStateChange transition)
{
  TsFilter *filter = TSFILTER(element);
  GstStateChangeReturn ret;
  GHashTableIter iter;
  gpointer key, value;

  GST_DEBUG_OBJECT(filter, "State change: %s", gst_state_change_get_name(transition));

  /* First let parent change state (this activates the element and static pads) */
  ret = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);

  if (ret == GST_STATE_CHANGE_FAILURE) {
    GST_WARNING_OBJECT(filter, "Parent state change failed!");
    return ret;
  }

  /* Send events to request pads when going to PAUSED or higher */
  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      /* Create dump pad if enabled and not yet created */
      if (filter->enable_dump && !filter->dumppad) {
        ts_filter_update_dump_pad(filter);
      }
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      g_hash_table_iter_init(&iter, filter->pid_pads);
      while (g_hash_table_iter_next(&iter, &key, &value)) {
        GstPad *pad = GST_PAD(value);
        GST_DEBUG_OBJECT(filter, "Sending events to PID pad %s", GST_PAD_NAME(pad));

        /* Send STREAM_START event */
        gchar *stream_id = gst_pad_create_stream_id(pad, element, GST_PAD_NAME(pad));
        if (stream_id) {
          GstEvent *event = gst_event_new_stream_start(stream_id);
          if (event) {
            gboolean event_res = gst_pad_push_event(pad, event);
            GST_DEBUG_OBJECT(filter, "STREAM_START event result: %s", event_res ? "OK" : "FAILED");
          } else {
            GST_WARNING_OBJECT(filter, "Failed to create STREAM_START event");
          }
          g_free(stream_id);
        } else {
          GST_WARNING_OBJECT(filter, "Failed to create stream ID for pad %s", GST_PAD_NAME(pad));
        }

        /* Send CAPS event */
        GstCaps *caps = gst_caps_new_simple("video/mpegts",
            "systemstream", G_TYPE_BOOLEAN, TRUE,
            NULL);
        if (caps) {
          GstEvent *event = gst_event_new_caps(caps);
          if (event) {
            gboolean event_res = gst_pad_push_event(pad, event);
            GST_DEBUG_OBJECT(filter, "CAPS event result: %s", event_res ? "OK" : "FAILED");
          } else {
            GST_WARNING_OBJECT(filter, "Failed to create CAPS event");
          }
          gst_caps_unref(caps);
        } else {
          GST_WARNING_OBJECT(filter, "Failed to create CAPS");
        }

        /* Send SEGMENT event */
        GstSegment *segment = gst_segment_new();
        if (segment) {
          gst_segment_init(segment, GST_FORMAT_TIME);
          GstEvent *event = gst_event_new_segment(segment);
          if (event) {
            gboolean event_res = gst_pad_push_event(pad, event);
            GST_DEBUG_OBJECT(filter, "SEGMENT event result: %s", event_res ? "OK" : "FAILED");
          } else {
            GST_WARNING_OBJECT(filter, "Failed to create SEGMENT event");
          }
          /* segment is ref'd by event, will be unreffed when event is destroyed */
        } else {
          GST_WARNING_OBJECT(filter, "Failed to create segment");
        }
      }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      GST_DEBUG_OBJECT(filter, "Transition to PLAYING, no additional activation needed");
      break;
    default:
      break;
  }

  GST_DEBUG_OBJECT(filter, "State change complete");
  return ret;
}

static void
ts_filter_class_init(TsFilterClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

  gobject_class->set_property = ts_filter_set_property;
  gobject_class->get_property = ts_filter_get_property;
  gobject_class->dispose = ts_filter_dispose;
  gobject_class->finalize = ts_filter_finalize;

  /* Override state change to activate request pads */
  element_class->change_state = ts_filter_change_state;

  /* Signal: emitted when bad TS packets are detected.
   * Parameter: count of bad sync bytes since last emission */
  signals[SIGNAL_BAD_PACKET] =
      g_signal_new("bad-packet",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0,
                   NULL, NULL,
                   NULL,
                   G_TYPE_NONE,
                   1,
                   G_TYPE_UINT);  /* guint: bad packet count */

  /* Signal: emitted when auto-detected packet size differs from user-specified */
  signals[SIGNAL_PACKET_SIZE_MISMATCH] =
      g_signal_new("packet-size-mismatch",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0,
                   NULL, NULL,
                   NULL,
                   G_TYPE_NONE,
                   2,
                   G_TYPE_UINT,  /* user-specified size */
                   G_TYPE_UINT); /* auto-detected size */

  /* Signal: emitted when a new PID is discovered in the stream.
   * Parameter: PID number (guint16)
   * This signal is emitted once for each unique PID encountered in the stream.
   * Applications can connect to this signal to dynamically discover PIDs. */
  signals[SIGNAL_NEW_PID] =
      g_signal_new("new-pid",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0,
                   NULL, NULL,
                   NULL,
                   G_TYPE_NONE,
                   1,
                   G_TYPE_UINT);  /* guint: PID number */

  /* Signal: emitted when the dump pad is added.
   * No parameters.
   * This signal is emitted when enable-dump is set to TRUE and the dump pad
   * is created. Applications can connect to this signal to dynamically link
   * to the dump pad when it becomes available. */
  signals[SIGNAL_DUMP_PAD_ADDED] =
      g_signal_new("dump-pad-added",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0,
                   NULL, NULL,
                   NULL,
                   G_TYPE_NONE,
                   0);  /* No parameters */

  /* Signal: emitted when a CRC error is detected.
   * Parameter: count of CRC errors (only emitted if enable-crc-validation is TRUE)
   * This signal is emitted when a packet containing a table section with CRC-32
   * has an invalid CRC value. */
  signals[SIGNAL_CRC_ERROR] =
      g_signal_new("crc-error",
                   G_TYPE_FROM_CLASS(klass),
                   G_SIGNAL_RUN_LAST,
                   0,
                   NULL, NULL,
                   NULL,
                   G_TYPE_NONE,
                   1,  /* Number of parameters */
                   G_TYPE_UINT);  /* guint: count of CRC errors in this batch */

  /* Properties */
  g_object_class_install_property(
      gobject_class, PROP_FILTER_PIDS,
      g_param_spec_pointer("filter-pids", "Filter PIDs",
                           "Array of PIDs to filter (NULL = pass all). "
                           "Use GArray of guint16.",
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_INVERT_FILTER,
      g_param_spec_boolean("invert-filter", "Invert Filter",
                           "If TRUE, filter-pids becomes a deny list. "
                           "If FALSE, filter-pids is an allow list.",
                           FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_PACKET_SIZE,
      g_param_spec_uint("packet-size", "Packet Size",
                        "MPEG-TS packet size in bytes (standard is 188, "
                        "some systems use 192 or 204)",
                        TS_PACKET_SIZE, 2048, TS_PACKET_SIZE,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_AUTO_DETECT,
      g_param_spec_boolean("auto-detect", "Auto-Detect Packet Size",
                           "If TRUE, automatically detect packet size from stream. "
                           "If detected size differs from packet-size property, "
                           "an error will be posted and detected size will be used.",
                           TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_BAD_PACKET_COUNT,
      g_param_spec_uint("bad-packet-count", "Bad Packet Count",
                        "Total number of packets discarded due to bad sync byte",
                        0, G_MAXUINT, 0,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_EMIT_PID_SIGNALS,
      g_param_spec_boolean("emit-pid-signals", "Emit PID Signals",
                           "If TRUE, emit 'new-pid' signals when new PIDs are discovered. "
                           "Applications can use this to dynamically discover PIDs in the stream.",
                           TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ENABLE_DUMP,
      g_param_spec_boolean("enable-dump", "Enable Dump Pad",
                           "Enable the 'dump' pad (unfiltered output). "
                           "If FALSE (default), the dump pad is not created, saving resources. "
                           "When dump pad is disabled and filter-pids is empty, "
                           "the 'src' pad outputs all packets (pass-through mode).",
                           FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ENABLE_CRC_VALIDATION,
      g_param_spec_boolean("enable-crc-validation", "Enable CRC Validation",
                           "Enable CRC-32 validation in PSI/SI table packets. "
                           "If TRUE, validates CRC-32 in packets containing table sections "
                           "and rejects packets with invalid CRC. "
                           "FALSE (default) - CRC validation disabled for performance.",
                           FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_ENABLE_STATS,
      g_param_spec_boolean("enable-stats", "Enable Statistics Collection",
                           "Enable detailed stream statistics collection. "
                           "If TRUE, collects comprehensive statistics including total packets, corrupted packets, "
                           "per-PID packet counts, filtered packet counts, and first filtered packet offsets. "
                           "FALSE (default) - Statistics disabled for performance.",
                           FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(
      gobject_class, PROP_STREAM_STATS,
      g_param_spec_pointer("stream-stats", "Stream Statistics",
                           "Get comprehensive statistics for the entire stream. "
                           "Returns a TsStreamStats structure containing: total_packets, total_corrupted, "
                           "pids (sorted array), pid_details (hash table of per-PID statistics). "
                           "Each PidDetailStats contains: packet_count, corrupted_count, filtered_count, "
                           "first_filtered_offset. Caller is responsible for unreferencing using ts_stream_stats_free(). "
                           "READ-ONLY property.",
                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /* Element class */
  gst_element_class_add_static_pad_template(element_class, &src_template);
  gst_element_class_add_static_pad_template(element_class, &dump_template);
  gst_element_class_add_static_pad_template(element_class, &pid_src_template);
  gst_element_class_add_static_pad_template(element_class, &sink_template);

  gst_element_class_set_static_metadata(
      element_class, "MPEG-TS PID Filter", "Filter/Video",
      "Filters MPEG-TS packets by PID. 'src' outputs filtered PIDs, "
      "'dump' outputs all PIDs. Request 'src_<PID>' pads for PID-specific outputs. "
      "NOTE: When connecting to sink elements (filesink, fakesink, etc.), "
      "use queue elements to avoid blocking: tsfilter.src_PID ! queue ! sink",
      "TSFilter Plugin");

  element_class->request_new_pad = ts_filter_request_new_pad;
  element_class->release_pad = ts_filter_release_pad;
}

static void
ts_filter_init(TsFilter *filter)
{
  /* Create source pad (filtered output) */
  filter->srcpad = gst_pad_new_from_static_template(&src_template, "src");
  if (!filter->srcpad) {
    GST_ERROR_OBJECT(filter, "Failed to create src pad");
    return;
  }
  gst_pad_set_query_function(filter->srcpad,
                             GST_DEBUG_FUNCPTR(ts_filter_src_query));
  gst_element_add_pad(GST_ELEMENT(filter), filter->srcpad);

  /* Create dump pad (unfiltered output) - only if enabled */
  filter->dumppad = NULL;  /* Will be created conditionally or remain NULL */

  filter->filter_pids = NULL;
  filter->invert_filter = FALSE;
  filter->packet_size = TS_PACKET_SIZE;  /* Standard MPEG-TS packet size */
  filter->auto_detect = TRUE;  /* Auto-detect by default */
  filter->enable_dump = FALSE;  /* Dump pad disabled by default */
  filter->enable_crc_validation = FALSE;  /* CRC validation disabled by default */
  filter->detected_packet_size = 0;
  filter->bad_packet_count = 0;
  filter->is_synced = FALSE;  /* Not synced yet */
  filter->crc_errors = 0;  /* No CRC errors yet */
  filter->emit_pid_signals = TRUE;  /* Emit new-pid signals by default */
  filter->enable_stats = FALSE;  /* Statistics disabled by default */
  filter->byte_offset = 0;  /* Start at byte offset 0 */
  filter->total_packets = 0;  /* No packets yet */
  filter->total_corrupted = 0;  /* No corrupted packets yet */
  filter->sinkpad = NULL;
  filter->pid_pads = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, (GDestroyNotify) gst_object_unref);
  if (!filter->pid_pads) {
    GST_ERROR_OBJECT(filter, "Failed to create pid_pads hash table");
    return;
  }
  filter->seen_pids = g_hash_table_new(g_direct_hash, g_direct_equal);
  if (!filter->seen_pids) {
    GST_ERROR_OBJECT(filter, "Failed to create seen_pids hash table");
    g_hash_table_unref(filter->pid_pads);
    filter->pid_pads = NULL;
    return;
  }

  /* Initialize per-PID statistics tracking */
  filter->pid_stats = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, (GDestroyNotify) pid_detail_stats_free);
  if (!filter->pid_stats) {
    GST_ERROR_OBJECT(filter, "Failed to create pid_stats hash table");
    g_hash_table_unref(filter->seen_pids);
    filter->seen_pids = NULL;
    g_hash_table_unref(filter->pid_pads);
    filter->pid_pads = NULL;
    return;
  }

  /* Initialize CC tracking for discontinuity detection */
  filter->last_cc = g_hash_table_new(g_direct_hash, g_direct_equal);
  if (!filter->last_cc) {
    GST_ERROR_OBJECT(filter, "Failed to create last_cc hash table");
    g_hash_table_unref(filter->pid_stats);
    filter->pid_stats = NULL;
    g_hash_table_unref(filter->seen_pids);
    filter->seen_pids = NULL;
    g_hash_table_unref(filter->pid_pads);
    filter->pid_pads = NULL;
    return;
  }

  /* Initialize pending packets for newly discovered PIDs */
  filter->pending_packets = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                     NULL, (GDestroyNotify) ts_filter_pending_list_free);
  if (!filter->pending_packets) {
    GST_ERROR_OBJECT(filter, "Failed to create pending_packets hash table");
    g_hash_table_unref(filter->last_cc);
    filter->last_cc = NULL;
    g_hash_table_unref(filter->pid_stats);
    filter->pid_stats = NULL;
    g_hash_table_unref(filter->seen_pids);
    filter->seen_pids = NULL;
    g_hash_table_unref(filter->pid_pads);
    filter->pid_pads = NULL;
    return;
  }
  g_mutex_init(&filter->pending_lock);

  /* Initialize flow combiner */
  filter->flow_combiner = gst_flow_combiner_new();
  if (!filter->flow_combiner) {
    GST_ERROR_OBJECT(filter, "Failed to create flow combiner");
    g_hash_table_unref(filter->pending_packets);
    filter->pending_packets = NULL;
    g_hash_table_unref(filter->last_cc);
    filter->last_cc = NULL;
    g_hash_table_unref(filter->pid_stats);
    filter->pid_stats = NULL;
    g_hash_table_unref(filter->seen_pids);
    filter->seen_pids = NULL;
    g_hash_table_unref(filter->pid_pads);
    filter->pid_pads = NULL;
    return;
  }
  gst_flow_combiner_add_pad(filter->flow_combiner, filter->srcpad);
  /* Dump pad will be added to flow combiner when/if it's created */

  /* Initialize buffer pools */
  filter->srcpool = NULL;
  filter->dumppool = NULL;
  filter->pidpool = NULL;
  g_mutex_init(&filter->pool_lock);

  /* Initialize cached events */
  filter->cached_stream_start = NULL;
  filter->cached_segment = NULL;
  filter->cached_caps = NULL;
  g_mutex_init(&filter->event_cache_lock);

  /* Initialize adapter for buffer accumulation */
  filter->adapter = gst_adapter_new();
  if (!filter->adapter) {
    GST_ERROR_OBJECT(filter, "Failed to create adapter");
  }
}

static void
ts_filter_dispose(GObject *object)
{
  TsFilter *filter = TSFILTER(object);

  /* Deactivate and release buffer pools */
  g_mutex_lock(&filter->pool_lock);

  if (filter->srcpool) {
    gst_buffer_pool_set_active(filter->srcpool, FALSE);
    gst_object_unref(filter->srcpool);
    filter->srcpool = NULL;
  }

  if (filter->dumppool) {
    gst_buffer_pool_set_active(filter->dumppool, FALSE);
    gst_object_unref(filter->dumppool);
    filter->dumppool = NULL;
  }

  if (filter->pidpool) {
    gst_buffer_pool_set_active(filter->pidpool, FALSE);
    gst_object_unref(filter->pidpool);
    filter->pidpool = NULL;
  }

  g_mutex_unlock(&filter->pool_lock);

  /* Clean up cached events */
  g_mutex_lock(&filter->event_cache_lock);
  if (filter->cached_stream_start) {
    gst_event_unref(filter->cached_stream_start);
    filter->cached_stream_start = NULL;
  }
  if (filter->cached_segment) {
    gst_event_unref(filter->cached_segment);
    filter->cached_segment = NULL;
  }
  if (filter->cached_caps) {
    gst_event_unref(filter->cached_caps);
    filter->cached_caps = NULL;
  }
  g_mutex_unlock(&filter->event_cache_lock);

  /* Clean up dump pad if it exists
   * Note: This is only needed if element is disposed while enable-dump=TRUE
   * Normally the dump pad is removed when enable-dump is set to FALSE
   * or during state changes. We explicitly remove it from the flow combiner
   * before the parent dispose to avoid any potential issues. */
  if (filter->dumppad) {
    gst_flow_combiner_remove_pad(filter->flow_combiner, filter->dumppad);
    filter->dumppad = NULL;  /* Element will remove the actual pad in parent dispose */
  }

  /* Clean up adapter */
  if (filter->adapter) {
    g_object_unref(filter->adapter);
    filter->adapter = NULL;
  }

  G_OBJECT_CLASS(parent_class)->dispose(object);
}

static void
ts_filter_finalize(GObject *object)
{
  TsFilter *filter = TSFILTER(object);

  if (filter->flow_combiner) {
    gst_flow_combiner_free(filter->flow_combiner);
    filter->flow_combiner = NULL;
  }

  if (filter->filter_pids) {
    g_array_unref(filter->filter_pids);
    filter->filter_pids = NULL;
  }

  if (filter->pid_pads) {
    g_hash_table_unref(filter->pid_pads);
    filter->pid_pads = NULL;
  }

  if (filter->seen_pids) {
    g_hash_table_unref(filter->seen_pids);
    filter->seen_pids = NULL;
  }

  if (filter->pid_stats) {
    g_hash_table_unref(filter->pid_stats);
    filter->pid_stats = NULL;
  }

  if (filter->pending_packets) {
    g_hash_table_unref(filter->pending_packets);
    filter->pending_packets = NULL;
  }

  if (filter->last_cc) {
    g_hash_table_unref(filter->last_cc);
    filter->last_cc = NULL;
  }

  g_mutex_clear(&filter->pool_lock);
  g_mutex_clear(&filter->pending_lock);
  g_mutex_clear(&filter->event_cache_lock);

  G_OBJECT_CLASS(parent_class)->finalize(object);
}

static void
ts_filter_set_property(GObject *object, guint prop_id,
                       const GValue *value, GParamSpec *pspec)
{
  TsFilter *filter = TSFILTER(object);

  switch (prop_id) {
    case PROP_FILTER_PIDS: {
      GArray *new_pids = g_value_get_pointer(value);
      if (filter->filter_pids)
        g_array_unref(filter->filter_pids);
      filter->filter_pids = new_pids ? g_array_ref(new_pids) : NULL;
      /* Reset statistics when filter changes */
      filter->bad_packet_count = 0;
      GST_DEBUG_OBJECT(filter, "Set filter PIDs (count: %d)",
                       filter->filter_pids ? filter->filter_pids->len : 0);
      break;
    }
    case PROP_INVERT_FILTER:
      filter->invert_filter = g_value_get_boolean(value);
      GST_DEBUG_OBJECT(filter, "Set invert filter: %s",
                       filter->invert_filter ? "TRUE" : "FALSE");
      break;
    case PROP_PACKET_SIZE: {
      guint old_size = filter->packet_size;
      filter->packet_size = g_value_get_uint(value);

      /* Recreate pools if packet size changes */
      if (old_size != filter->packet_size) {
        g_mutex_lock(&filter->pool_lock);

        if (filter->srcpool) {
          gst_buffer_pool_set_active(filter->srcpool, FALSE);
          gst_object_unref(filter->srcpool);
          filter->srcpool = NULL;
        }
        if (filter->dumppool) {
          gst_buffer_pool_set_active(filter->dumppool, FALSE);
          gst_object_unref(filter->dumppool);
          filter->dumppool = NULL;
        }
        if (filter->pidpool) {
          gst_buffer_pool_set_active(filter->pidpool, FALSE);
          gst_object_unref(filter->pidpool);
          filter->pidpool = NULL;
        }

        g_mutex_unlock(&filter->pool_lock);
        GST_INFO_OBJECT(filter, "Reset buffer pools due to packet size change: %u -> %u",
                       old_size, filter->packet_size);
      }

      GST_DEBUG_OBJECT(filter, "Set packet size: %u bytes", filter->packet_size);
      break;
    }
    case PROP_AUTO_DETECT:
      filter->auto_detect = g_value_get_boolean(value);
      if (filter->auto_detect) {
        filter->detected_packet_size = 0;  /* Reset detection state */
      }
      GST_DEBUG_OBJECT(filter, "Set auto-detect: %s",
                       filter->auto_detect ? "TRUE" : "FALSE");
      break;
    case PROP_EMIT_PID_SIGNALS:
      filter->emit_pid_signals = g_value_get_boolean(value);
      GST_DEBUG_OBJECT(filter, "Set emit-pid-signals: %s",
                       filter->emit_pid_signals ? "TRUE" : "FALSE");
      break;
    case PROP_ENABLE_DUMP:
      /* Update dump pad existence - only if element is in NULL state
       * If element is running, reject the property change to avoid inconsistent state */
      if (GST_STATE(filter) != GST_STATE_NULL) {
        GST_WARNING_OBJECT(filter, "Cannot change dump pad while element is running. "
                                "Set enable-dump before starting the pipeline.");
        /* Don't modify filter->enable_dump - reject the change */
        return;
      }
      filter->enable_dump = g_value_get_boolean(value);
      GST_DEBUG_OBJECT(filter, "Set enable-dump: %s",
                       filter->enable_dump ? "TRUE" : "FALSE");
      ts_filter_update_dump_pad(filter);
      break;
    case PROP_ENABLE_CRC_VALIDATION:
      filter->enable_crc_validation = g_value_get_boolean(value);
      filter->crc_errors = 0;  /* Reset error counter when enabling */
      GST_DEBUG_OBJECT(filter, "Set enable-crc-validation: %s",
                       filter->enable_crc_validation ? "TRUE" : "FALSE");
      break;
    case PROP_ENABLE_STATS:
      filter->enable_stats = g_value_get_boolean(value);
      GST_DEBUG_OBJECT(filter, "Set enable-stats: %s",
                       filter->enable_stats ? "TRUE" : "FALSE");
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void
ts_filter_get_property(GObject *object, guint prop_id,
                       GValue *value, GParamSpec *pspec)
{
  TsFilter *filter = TSFILTER(object);

  switch (prop_id) {
    case PROP_FILTER_PIDS:
      g_value_set_pointer(value, filter->filter_pids);
      break;
    case PROP_INVERT_FILTER:
      g_value_set_boolean(value, filter->invert_filter);
      break;
    case PROP_PACKET_SIZE:
      g_value_set_uint(value, filter->packet_size);
      break;
    case PROP_AUTO_DETECT:
      g_value_set_boolean(value, filter->auto_detect);
      break;
    case PROP_BAD_PACKET_COUNT:
      g_value_set_uint(value, filter->bad_packet_count);
      break;
    case PROP_EMIT_PID_SIGNALS:
      g_value_set_boolean(value, filter->emit_pid_signals);
      break;
    case PROP_ENABLE_DUMP:
      g_value_set_boolean(value, filter->enable_dump);
      break;
    case PROP_ENABLE_CRC_VALIDATION:
      g_value_set_boolean(value, filter->enable_crc_validation);
      break;
    case PROP_ENABLE_STATS:
      g_value_set_boolean(value, filter->enable_stats);
      break;
    case PROP_STREAM_STATS: {
      /* Return a copy of the stream statistics
       * The caller should free using ts_stream_stats_free() when done */
      TsStreamStats *stats = ts_stream_stats_new();
      if (stats) {
        /* Copy global statistics */
        stats->total_packets = filter->total_packets;
        stats->corrupted_packets = filter->total_corrupted;

        /* Copy all PIDs and per-PID details */
        if (filter->pid_stats) {
          GHashTableIter iter;
          gpointer key, value;

          /* Collect and sort PIDs */
          g_hash_table_iter_init(&iter, filter->pid_stats);
          while (g_hash_table_iter_next(&iter, &key, &value)) {
            guint16 pid = GPOINTER_TO_UINT(key);
            g_array_append_val(stats->pids, pid);

            /* Copy per-PID detail stats */
            PidDetailStats *orig = (PidDetailStats *)value;
            PidDetailStats *detail = pid_detail_stats_new();
            if (detail) {
              detail->packet_count = orig->packet_count;
              detail->corrupted_count = orig->corrupted_count;
              detail->filtered_count = orig->filtered_count;
              detail->first_filtered_offset = orig->first_filtered_offset;
              detail->cc_discontinuities = orig->cc_discontinuities;
              g_hash_table_insert(stats->pid_details, GUINT_TO_POINTER(pid), detail);
            }
          }

          /* Sort PIDs array for consistent output */
          g_array_sort(stats->pids, (GCompareFunc)guint16_compare);
        }
      }

      g_value_set_pointer(value, stats);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static GstFlowReturn
ts_filter_chain(G_GNUC_UNUSED GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
  TsFilter *filter = TSFILTER(parent);
  GstBuffer *filtered_buf;
  GstBuffer *dump_buf;
  GstFlowReturn ret = GST_FLOW_OK;
  gsize combined_size;
  gsize total_packets;
  GstBuffer *accumulated_buffer;

  GST_LOG_OBJECT(filter, "Processing buffer of size %" G_GSIZE_FORMAT,
                gst_buffer_get_size(buffer));

  /* Thread Safety Note: filter->dumppad is only modified when the element
   * is in GST_STATE_NULL (via property setter) or during NULL_TO_READY
   * transition. Since chain() only runs in PAUSED/PLAYING states, the
   * dumppad pointer cannot change concurrently and no mutex is needed. */

  /* Update byte offset for tracking first filtered packet position */
  gsize buffer_size = gst_buffer_get_size(buffer);
  if (filter->enable_stats) {
    filter->byte_offset += buffer_size;
  }

  /* Push buffer into adapter for accumulation and alignment */
  gst_adapter_push(filter->adapter, buffer);

  /* Calculate how many complete packets we have available */
  gsize available = gst_adapter_available(filter->adapter);

  /* Auto-detect packet size BEFORE calculating packet count
   * This ensures we use the correct packet size for non-standard streams */
  if (filter->auto_detect && filter->detected_packet_size == 0 && available >= 3 * PACKET_SIZE_188) {
    /* Try to detect packet size from accumulated data */
    const guint8 *data = gst_adapter_map(filter->adapter, available);
    if (data) {
      guint detected = ts_filter_detect_packet_size(filter, data, available, FALSE);  /* Not at EOS */
      gst_adapter_unmap(filter->adapter);
      if (detected != 0) {
        GST_INFO_OBJECT(filter, "Auto-detected packet size: %u bytes", detected);
        filter->packet_size = detected;
        filter->detected_packet_size = detected;
      }
    }
  }

  total_packets = available / filter->packet_size;

  /* Wait until we have at least 3 packets to establish sync, unless already synced.
   * Exception: If we have 1-2 packets and the first byte is a sync byte, process them anyway.
   * This handles small test files gracefully. */
  if (!filter->is_synced && total_packets > 0 && total_packets < 3) {
    /* Check if we have a valid sync byte at the start */
    const guint8 *data = gst_adapter_map(filter->adapter, 1);
    gboolean has_sync = data && data[0] == TS_SYNC_BYTE;
    if (data) gst_adapter_unmap(filter->adapter);

    if (!has_sync) {
      GST_DEBUG_OBJECT(filter, "No sync byte found, waiting for more data");
      return GST_FLOW_OK;
    }
    /* Has sync byte but < 3 packets - will be processed with lenient validation */
    GST_DEBUG_OBJECT(filter, "Processing small buffer (%zu packets) with lenient sync",
                   total_packets);
  }

  if (total_packets == 0) {
    /* Not enough data for even one packet, keep accumulating */
    GST_DEBUG_OBJECT(filter, "Not enough data (only %zu bytes), accumulating", available);
    return GST_FLOW_OK;
  }

  /* Calculate size to process (only complete packets) */
  combined_size = total_packets * filter->packet_size;

  GST_DEBUG_OBJECT(filter, "Processing %zu complete packets (%zu bytes), %zu bytes remaining",
                 total_packets, combined_size, available - combined_size);

  /* Get buffer from adapter for processing (does not copy data) */
  accumulated_buffer = gst_adapter_get_buffer(filter->adapter, combined_size);
  if (!accumulated_buffer) {
    GST_ERROR_OBJECT(filter, "Failed to get buffer from adapter");
    return GST_FLOW_ERROR;
  }

  /* Process the accumulated buffer */

  /* Check if we should pass all packets through src pad (no filtering, no dump) */
  gboolean pass_all = (!filter->dumppad && (!filter->filter_pids || filter->filter_pids->len == 0));

  if (pass_all) {
    /* No filtering, no dump pad - pass everything through src pad */
    GST_DEBUG_OBJECT(filter, "Pass-through mode: src gets all packets");
    GstFlowReturn src_ret = gst_pad_push(filter->srcpad, gst_buffer_ref(accumulated_buffer));
    if (src_ret != GST_FLOW_OK && src_ret != GST_FLOW_NOT_LINKED) {
      ret = src_ret;
    }
  } else {
    /* Normal filtering mode */
    /* Process buffer for filtered output */
    filtered_buf = ts_filter_process_buffer(filter, accumulated_buffer);

    /* Only create dump buffer if dump pad exists AND is linked (has peer) */
    dump_buf = NULL;
    if (filter->dumppad) {
      GstPad *peer = gst_pad_get_peer(filter->dumppad);
      if (peer) {
        /* Dump pad is linked, create a deep copy for unfiltered output
         * We use gst_buffer_copy_deep() to ensure we have a completely independent buffer
         * This is necessary because the adapter's buffer may be reused or freed */
        dump_buf = gst_buffer_copy_deep(accumulated_buffer);
        gst_object_unref(peer);
        if (dump_buf == NULL) {
          GST_WARNING_OBJECT(filter, "Failed to deep-copy buffer for dump pad");
        }
      }
    }

    /* Push to dump pad first (unfiltered) - if it exists and we have a buffer */
    if (dump_buf && filter->dumppad) {
      GST_DEBUG_OBJECT(filter, "BEFORE dump pad push");
      GstFlowReturn dump_ret = gst_pad_push(filter->dumppad, dump_buf);
      dump_ret = gst_flow_combiner_update_flow(filter->flow_combiner, dump_ret);
      GST_DEBUG_OBJECT(filter, "AFTER dump pad push, ret=%s", gst_flow_get_name(dump_ret));
      if (dump_ret != GST_FLOW_OK && dump_ret != GST_FLOW_NOT_LINKED) {
        ret = dump_ret;
      }
    }

    /* Push filtered output to src pad */
    if (filtered_buf) {
      GST_DEBUG_OBJECT(filter, "BEFORE src pad push (buffer size %" G_GSIZE_FORMAT ")",
                       gst_buffer_get_size(filtered_buf));
      GstFlowReturn src_ret = gst_pad_push(filter->srcpad, filtered_buf);
      src_ret = gst_flow_combiner_update_flow(filter->flow_combiner, src_ret);
      GST_DEBUG_OBJECT(filter, "AFTER src pad push, ret=%s", gst_flow_get_name(src_ret));
      if (src_ret != GST_FLOW_OK && src_ret != GST_FLOW_NOT_LINKED) {
        ret = src_ret;
      }
      /* Note: gst_pad_push consumes the reference to filtered_buf, so we don't unref it here */
    } else {
      GST_LOG_OBJECT(filter, "All packets filtered out on src pad");
    }
  }

  /* Push packets to PID-specific pads if any exist or buffer for future pads */
  /* Always call this even if pid_pads is empty, as new PIDs might be discovered */
  /* Use accumulated_buffer - this allows ALL packets (including those not passing PID filter)
   * to be scanned, ensuring all PIDs are discovered and pads can be created */
  gsize pid_pad_bytes_processed = ts_filter_push_to_pid_pads(filter, accumulated_buffer);

  /* Unref the accumulated buffer (adapter keeps its own reference) */
  gst_buffer_unref(accumulated_buffer);

  /* Unref filtered_buf if it exists (already consumed by gst_pad_push) */
  /* Note: gst_pad_push consumes the reference, so we don't unref here */

  /* Flush ONLY the bytes that were actually processed by ts_filter_push_to_pid_pads
   * NOT combined_size, because ts_filter_push_to_pid_pads might skip packets due to
   * failed 3-sync validation during initial sync establishment.
   * Using combined_size would flush bytes that weren't actually processed, causing packet loss. */
  gst_adapter_flush(filter->adapter, pid_pad_bytes_processed);

  /* Get final combined flow return by updating with current ret */
  ret = gst_flow_combiner_update_flow(filter->flow_combiner, ret);

  return ret;
}

static gboolean
ts_filter_sink_event(G_GNUC_UNUSED GstPad *pad, GstObject *parent, GstEvent *event)
{
  TsFilter *filter = TSFILTER(parent);

  GST_LOG_OBJECT(filter, "Handling event: %s",
                gst_event_type_get_name(GST_EVENT_TYPE(event)));

  /* Cache important events for replay to newly requested PID pads */
  GstEventType type = GST_EVENT_TYPE(event);
  if (type == GST_EVENT_STREAM_START || type == GST_EVENT_SEGMENT || type == GST_EVENT_CAPS) {
    g_mutex_lock(&filter->event_cache_lock);
    /* Replace old cached event with new one */
    if (type == GST_EVENT_STREAM_START) {
      if (filter->cached_stream_start)
        gst_event_unref(filter->cached_stream_start);
      filter->cached_stream_start = gst_event_ref(event);
    } else if (type == GST_EVENT_SEGMENT) {
      if (filter->cached_segment)
        gst_event_unref(filter->cached_segment);
      filter->cached_segment = gst_event_ref(event);
      /* Reset packet size detection on new segment */
      filter->bad_packet_count = 0;
      filter->is_synced = FALSE;  /* Reset sync state */
      if (filter->auto_detect) {
        filter->detected_packet_size = 0;
      }
      /* Reset seen PIDs to re-emit new-pid signals for new streams */
      if (filter->seen_pids) {
        g_hash_table_remove_all(filter->seen_pids);
        GST_DEBUG_OBJECT(filter, "Reset seen PIDs for new segment");
      }
      /* Reset adapter on new segment */
      if (filter->adapter) {
        gst_adapter_clear(filter->adapter);
        GST_DEBUG_OBJECT(filter, "Reset adapter for new segment");
      }
    } else if (type == GST_EVENT_CAPS) {
      if (filter->cached_caps)
        gst_event_unref(filter->cached_caps);
      filter->cached_caps = gst_event_ref(event);
    }
    g_mutex_unlock(&filter->event_cache_lock);
  }

  /* Handle EOS - flush any remaining accumulated data */
  if (type == GST_EVENT_EOS) {
    gsize available = gst_adapter_available(filter->adapter);

    /* Last chance to detect packet size at EOS using 2-sync fallback
     * This handles files with only ~2 packets that couldn't be detected earlier */
    if (filter->auto_detect && filter->detected_packet_size == 0 && available >= 2 * PACKET_SIZE_188) {
      const guint8 *data = gst_adapter_map(filter->adapter, available);
      if (data) {
        guint detected = ts_filter_detect_packet_size(filter, data, available, TRUE);  /* at_eos=TRUE */
        gst_adapter_unmap(filter->adapter);
        if (detected != 0) {
          GST_INFO_OBJECT(filter, "EOS: Auto-detected packet size: %u bytes", detected);
          filter->packet_size = detected;
          filter->detected_packet_size = detected;
        }
      }
    }

    if (available >= filter->packet_size) {
      guint num_packets = available / filter->packet_size;
      guint process_size = num_packets * filter->packet_size;

      GST_DEBUG_OBJECT(filter, "EOS: Flushing %u packets (%u bytes) of accumulated data",
                     num_packets, process_size);

      /* Get buffer from adapter for remaining data */
      GstBuffer *eos_buffer = gst_adapter_get_buffer(filter->adapter, process_size);
      if (!eos_buffer) {
        GST_ERROR_OBJECT(filter, "Failed to get EOS buffer from adapter");
      } else {
        /* Process the remaining buffer */
        GstBuffer *filtered_buf = ts_filter_process_buffer(filter, eos_buffer);

        /* Get a copy for PID pad pushing (deep copy since adapter will flush) */
        GstBuffer *pid_buffer = gst_buffer_copy_deep(eos_buffer);

        gst_buffer_unref(eos_buffer);

        /* Push filtered output */
        if (filtered_buf) {
          gst_pad_push(filter->srcpad, filtered_buf);
        }

        /* Push to PID-specific pads if any exist */
        if (pid_buffer && g_hash_table_size(filter->pid_pads) > 0) {
          GST_DEBUG_OBJECT(filter, "EOS: Pushing %u packets to PID pads", num_packets);
          ts_filter_push_to_pid_pads(filter, pid_buffer);
        }

        /* Push to dump pad if exists */
        if (pid_buffer && filter->dumppad) {
          gst_pad_push(filter->dumppad, pid_buffer);
        }

        if (pid_buffer) {
          gst_buffer_unref(pid_buffer);
        }

        /* Flush processed data from adapter */
        gst_adapter_flush(filter->adapter, process_size);
      }
    }
  }

  /* Send event to src and dump pads */
  /* Each push needs its own ref to avoid use-after-free */
  gboolean ret1 = gst_pad_push_event(filter->srcpad, gst_event_ref(event));
  gboolean ret2 = TRUE;  /* Assume TRUE if dump pad doesn't exist */

  /* Only forward to dump pad if it exists */
  if (filter->dumppad) {
    ret2 = gst_pad_push_event(filter->dumppad, gst_event_ref(event));
  }

  /* Forward event to all PID-specific pads */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, filter->pid_pads);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GstPad *pid_pad = (GstPad *)value;
    gst_pad_push_event(pid_pad, gst_event_ref(event));
  }

  /* Unref our original reference */
  gst_event_unref(event);

  return ret1 && ret2;
}

static gboolean
ts_filter_src_query(GstPad *pad, GstObject *parent, GstQuery *query)
{
  TsFilter *filter = TSFILTER(parent);

  switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_ALLOCATION: {
      GstBufferPool *pool = NULL;
      GstCaps *caps;
      gboolean need_pool;

      gst_query_parse_allocation(query, &caps, &need_pool);

      /* Determine which pool to use based on pad */
      if (pad == filter->srcpad || pad == filter->dumppad) {
        ts_filter_ensure_pools(filter);

        g_mutex_lock(&filter->pool_lock);
        if (pad == filter->srcpad && filter->srcpool) {
          pool = gst_object_ref(filter->srcpool);
        } else if (pad == filter->dumppad && filter->dumppool) {
          pool = gst_object_ref(filter->dumppool);
        }
        g_mutex_unlock(&filter->pool_lock);

        if (pool) {
          /* Add pool to allocation query */
          gst_query_add_allocation_pool(query, pool, filter->packet_size * 256, 0, 0);
          gst_object_unref(pool);

          GST_LOG_OBJECT(filter, "Answered ALLOCATION query for %s",
                        GST_PAD_NAME(pad));
        }
      }
      return TRUE;
    }
    case GST_QUERY_LATENCY:
      /* Forward latency query to upstream */
      if (filter->sinkpad) {
        return gst_pad_peer_query(filter->sinkpad, query);
      }
      return FALSE;
    default:
      return gst_pad_query_default(pad, parent, query);
  }
}
/* Element registration function */
gboolean
gst_element_register_tsfilter (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT(tsfilter_debug, "tsfilter", 0,
                          "MPEG-TS PID filter");

  return gst_element_register(plugin, "tsfilter",
                              GST_RANK_NONE,
                              GST_TYPE_TS_FILTER);
}
