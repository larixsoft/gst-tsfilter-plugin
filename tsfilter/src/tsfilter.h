/*
 * tsfilter.h - GStreamer MPEG-TS PID filter
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

#ifndef GST_TS_FILTER_H
#define GST_TS_FILTER_H

#include <gst/gst.h>
#include <gst/base/gstadapter.h>

G_BEGIN_DECLS

#define GST_TYPE_TS_FILTER \
  (ts_filter_get_type())
#define GST_TS_FILTER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_TS_FILTER,TsFilter))
#define GST_TS_FILTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_TS_FILTER,TsFilterClass))
#define GST_IS_TS_FILTER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_TS_FILTER))
#define GST_IS_TS_FILTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_TS_FILTER))
#define TSFILTER(obj) \
  (GST_TS_FILTER(obj))

typedef struct _TsFilter TsFilter;
typedef struct _TsFilterClass TsFilterClass;

struct _TsFilter {
  GstElement parent;

  /* Pads */
  GstPad *sinkpad;
  GstPad *srcpad;       /* Filtered output (only passes specified PIDs) */
  GstPad *dumppad;      /* Unfiltered output (passes all PIDs) */
  GHashTable *pid_pads; /* Hash table of PID-specific src pads (guint16 -> GstPad) */

  /* Flow combiner for managing multiple pad returns */
  gpointer flow_combiner; /* GstFlowCombiner * */

  /* Filter properties */
  GArray *filter_pids;        /* Array of guint16 PIDs to filter */
  gboolean invert_filter;     /* If TRUE, filter_pids is a deny list */
  guint packet_size;          /* User-specified TS packet size (default 188) */
  gboolean auto_detect;       /* If TRUE, auto-detect packet size from stream */
  gboolean enable_dump;       /* If TRUE, dump pad is created (default: FALSE) */
  gboolean enable_crc_validation; /* If TRUE, validate CRC-32 in PSI/SI tables (default: FALSE) */
  gboolean enable_stats;      /* If TRUE, collect detailed stream statistics (default: FALSE) */

  /* Auto-detected values */
  guint detected_packet_size; /* Auto-detected packet size */

  /* Statistics */
  guint64 byte_offset;        /* Current byte offset in the stream (for tracking first filtered packet) */
  guint total_packets;        /* Total packets in the stream */
  guint total_corrupted;      /* Total corrupted/abandoned packets in the stream */
  guint bad_packet_count;     /* Total bad packets detected (legacy) */
  gboolean is_synced;         /* TRUE once we've established sync with 3 consecutive packets */
  guint crc_errors;           /* Total CRC validation errors detected */
  GHashTable *pid_stats;      /* Per-PID detail statistics (guint16 -> PidDetailStats*) */
  GHashTable *last_cc;        /* Last CC value for each PID (guint16 -> guint8) for CC discontinuity detection */

  /* PID discovery */
  GHashTable *seen_pids;      /* Hash set of PIDs we've seen (for new-pid signal) */
  gboolean emit_pid_signals;  /* If TRUE, emit signals for new PIDs (default: TRUE) */

  /* Pending packets for newly discovered PIDs (before pad is requested) */
  GMutex pending_lock;        /* Mutex for pending packets access */
  GHashTable *pending_packets; /* PID -> GSList of packet data (pending until pad requested) */

  /* Buffer pools for performance */
  GstBufferPool *srcpool;     /* Buffer pool for src pad (filtered output) */
  GstBufferPool *dumppool;    /* Buffer pool for dump pad (unfiltered output) */
  GstBufferPool *pidpool;     /* Buffer pool for PID-specific pads (fixed-size packets) */
  GMutex pool_lock;           /* Mutex for pool access */

  /* Cached events for replaying to newly requested PID pads */
  GstEvent *cached_stream_start;  /* Cached STREAM_START event */
  GstEvent *cached_segment;       /* Cached SEGMENT event */
  GstEvent *cached_caps;          /* Cached CAPS event */
  GMutex event_cache_lock;        /* Mutex for event cache access */

  /* Buffer adapter for handling partial packets at buffer boundaries */
  GstAdapter *adapter;            /* Adapter for accumulating and aligning buffers */
};

struct _TsFilterClass {
  GstElementClass parent_class;
};

GType ts_filter_get_type(void);

gboolean gst_element_register_tsfilter (GstPlugin * plugin);

G_END_DECLS

#endif /* GST_TS_FILTER_H */
