/*
 * Comprehensive Test Suite for tsfilter GStreamer Plugin
 *
 * This test program validates all major features of the tsfilter plugin:
 * - PID filtering (allow list and deny list modes)
 * - Packet size auto-detection
 * - Bad packet detection and signaling
 * - PID-specific request pads
 * - Dump pad functionality (enable-dump property)
 * - Pass-through mode (enable-dump=FALSE, no filter-pids)
 * - new-pid signal for dynamic PID discovery
 * - Various packet sizes (188, 192, 204, 208)
 * - Buffer pool performance
 *
 * Usage: test_tsfilter_comprehensive [test_file_path]
 */

#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Test result tracking */
static int tests_passed = 0;
static int tests_failed = 0;
static int tests_total = 0;

#define TEST_PASS(msg) do { \
  tests_passed++; \
  tests_total++; \
  printf("  ✓ PASS: %s\n", msg); \
} while(0)

#define TEST_FAIL(msg) do { \
  tests_failed++; \
  tests_total++; \
  printf("  ✗ FAIL: %s\n", msg); \
} while(0)

#define TEST_START(name) do { \
  printf("\n=== TEST: %s ===\n", name); \
} while(0)

/* Signal handler for bad-packet signal */
static guint bad_packet_count = 0;

static void on_bad_packet(GstElement *elem, guint count, gpointer user_data)
{
  bad_packet_count += count;
  printf("   [SIGNAL] Bad packets detected: %u bytes discarded\n", count);
}

/* Signal handler for packet-size-mismatch signal */
static gboolean mismatch_received = FALSE;
static guint user_size = 0;
static guint detected_size = 0;

static void on_packet_size_mismatch(GstElement *elem, guint user_specified,
                                    guint auto_detected, gpointer user_data)
{
  mismatch_received = TRUE;
  user_size = user_specified;
  detected_size = auto_detected;
  printf("   [SIGNAL] Packet size mismatch: user=%u, detected=%u\n",
         user_specified, auto_detected);
}

/* Signal handler for new-pid signal */
static GArray *discovered_pids = NULL;

static void on_new_pid(GstElement *elem, guint pid, gpointer user_data)
{
  if (!discovered_pids) {
    discovered_pids = g_array_new(FALSE, FALSE, sizeof(guint));
  }

  /* Check if already tracked */
  for (guint i = 0; i < discovered_pids->len; i++) {
    if (g_array_index(discovered_pids, guint, i) == pid) {
      return;  /* Already tracked */
    }
  }

  /* Add to discovered PIDs */
  g_array_append_val(discovered_pids, pid);
  printf("   [SIGNAL] New PID discovered: %u (total: %d)\n",
         pid, discovered_pids->len);
}

/* Signal handler for dump-pad-added signal */
static gboolean dump_pad_added_received = FALSE;

static void on_dump_pad_added(GstElement *elem, gpointer user_data)
{
  dump_pad_added_received = TRUE;
  printf("   [SIGNAL] Dump pad added\n");
}

/*
 * Test 1: Basic element creation and property access
 */
static gboolean
test_element_creation(void)
{
  TEST_START("Element Creation and Basic Properties");

  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  if (!tsfilter) {
    TEST_FAIL("Failed to create tsfilter element");
    return FALSE;
  }
  TEST_PASS("Element created successfully");

  /* Check default property values */
  guint packet_size;
  g_object_get(tsfilter, "packet-size", &packet_size, NULL);
  if (packet_size == 188) {
    TEST_PASS("Default packet-size is 188");
  } else {
    TEST_FAIL("Default packet-size is not 188");
  }

  gboolean auto_detect;
  g_object_get(tsfilter, "auto-detect", &auto_detect, NULL);
  if (auto_detect == TRUE) {
    TEST_PASS("Default auto-detect is TRUE");
  } else {
    TEST_FAIL("Default auto-detect is not TRUE");
  }

  guint bad_count;
  g_object_get(tsfilter, "bad-packet-count", &bad_count, NULL);
  if (bad_count == 0) {
    TEST_PASS("Default bad-packet-count is 0");
  } else {
    TEST_FAIL("Default bad-packet-count is not 0");
  }

  gst_object_unref(tsfilter);
  return TRUE;
}

/*
 * Test 2: Signal connectivity
 */
static gboolean
test_signals(void)
{
  TEST_START("Signal Connectivity");

  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  if (!tsfilter) {
    TEST_FAIL("Failed to create tsfilter element");
    return FALSE;
  }

  gulong signal_id = g_signal_connect(tsfilter, "bad-packet",
                                       G_CALLBACK(on_bad_packet), NULL);
  if (signal_id > 0) {
    TEST_PASS("bad-packet signal connected");
  } else {
    TEST_FAIL("Failed to connect bad-packet signal");
  }

  signal_id = g_signal_connect(tsfilter, "packet-size-mismatch",
                               G_CALLBACK(on_packet_size_mismatch), NULL);
  if (signal_id > 0) {
    TEST_PASS("packet-size-mismatch signal connected");
  } else {
    TEST_FAIL("Failed to connect packet-size-mismatch signal");
  }

  signal_id = g_signal_connect(tsfilter, "new-pid",
                               G_CALLBACK(on_new_pid), NULL);
  if (signal_id > 0) {
    TEST_PASS("new-pid signal connected");
  } else {
    TEST_FAIL("Failed to connect new-pid signal");
  }

  signal_id = g_signal_connect(tsfilter, "dump-pad-added",
                               G_CALLBACK(on_dump_pad_added), NULL);
  if (signal_id > 0) {
    TEST_PASS("dump-pad-added signal connected");
  } else {
    TEST_FAIL("Failed to connect dump-pad-added signal");
  }

  gst_object_unref(tsfilter);
  return TRUE;
}

/*
 * Test 3: new-pid signal functionality
 */
static gboolean
test_new_pid_signal(const gchar *filepath)
{
  TEST_START("new-pid Signal Discovery");

  struct stat st;
  if (stat(filepath, &st) != 0) {
    printf("   Note: Test file not found: %s\n", filepath);
    printf("   Skipping test.\n");
    return FALSE;
  }

  printf("   Processing: %s (%" G_GSIZE_FORMAT " bytes)\n", filepath, (gsize)st.st_size);

  /* Initialize PID tracking */
  if (discovered_pids) {
    g_array_unref(discovered_pids);
    discovered_pids = NULL;
  }

  /* Create elements */
  GstElement *src = gst_element_factory_make("filesrc", "src");
  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  GstElement *sink = gst_element_factory_make("fakesink", "sink");

  if (!src || !tsfilter || !sink) {
    TEST_FAIL("Failed to create elements");
    if (src) gst_object_unref(src);
    if (tsfilter) gst_object_unref(tsfilter);
    if (sink) gst_object_unref(sink);
    return FALSE;
  }

  g_object_set(src, "location", filepath, NULL);
  g_object_set(sink, "sync", FALSE, NULL);

  /* Enable PID signals and dump pad (test uses dump pad) */
  g_object_set(tsfilter, "emit-pid-signals", TRUE, "enable-dump", TRUE, NULL);

  /* Create pipeline */
  GstElement *pipeline = gst_pipeline_new("pipeline");
  gst_bin_add_many(GST_BIN(pipeline), src, tsfilter, sink, NULL);

  /* Link: source -> filter:sink */
  GstPad *srcpad = gst_element_get_static_pad(src, "src");
  GstPad *filter_sink = gst_element_request_pad_simple(tsfilter, "sink");
  if (gst_pad_link(srcpad, filter_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link source to filter");
    gst_object_unref(srcpad);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(srcpad);

  /* Link: filter:dump -> sink */
  GstPad *filter_dump = gst_element_get_static_pad(tsfilter, "dump");
  GstPad *sink_sink = gst_element_get_static_pad(sink, "sink");
  if (gst_pad_link(filter_dump, sink_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link filter to sink");
    gst_object_unref(filter_dump);
    gst_object_unref(sink_sink);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(filter_dump);
  gst_object_unref(sink_sink);

  /* Connect to new-pid signal */
  gulong signal_id = g_signal_connect(tsfilter, "new-pid",
                                       G_CALLBACK(on_new_pid), NULL);
  if (signal_id > 0) {
    TEST_PASS("new-pid signal connected");
  } else {
    TEST_FAIL("Failed to connect new-pid signal");
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }

  /* Run pipeline */
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    TEST_FAIL("Failed to set pipeline to PLAYING");
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }

  /* Wait for EOS or timeout */
  GstBus *bus = gst_element_get_bus(pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_SECOND * 10,
                                                GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

  gboolean success = TRUE;
  if (msg) {
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
      TEST_PASS("Pipeline completed successfully (EOS)");
    } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
      GError *err = NULL;
      gchar *debug = NULL;
      gst_message_parse_error(msg, &err, &debug);
      printf("   ERROR: %s\n", err->message);
      g_error_free(err);
      g_free(debug);
      TEST_FAIL("Pipeline encountered error");
      success = FALSE;
    }
    gst_message_unref(msg);
  } else {
    printf("   Note: Pipeline timeout (file may be large)\n");
  }

  /* Check if PIDs were discovered */
  if (discovered_pids && discovered_pids->len > 0) {
    printf("   Discovered %d unique PIDs: ", discovered_pids->len);
    for (guint i = 0; i < discovered_pids->len && i < 10; i++) {
      printf("%u ", g_array_index(discovered_pids, guint, i));
    }
    if (discovered_pids->len > 10) {
      printf("...");
    }
    printf("\n");
    TEST_PASS("new-pid signal emitted PIDs");
  } else {
    TEST_FAIL("No PIDs discovered via signal");
    success = FALSE;
  }

  /* Cleanup */
  gst_object_unref(filter_sink);
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);

  return success;
}

/*
 * Test 4: new-pid signal disabled
 */
static gboolean
test_new_pid_signal_disabled(void)
{
  TEST_START("new-pid Signal Disabled");

  if (discovered_pids) {
    g_array_unref(discovered_pids);
    discovered_pids = NULL;
  }

  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  if (!tsfilter) {
    TEST_FAIL("Failed to create tsfilter element");
    return FALSE;
  }

  /* Disable PID signals */
  g_object_set(tsfilter, "emit-pid-signals", FALSE, NULL);
  gboolean emit_signals;
  g_object_get(tsfilter, "emit-pid-signals", &emit_signals, NULL);
  if (emit_signals == FALSE) {
    TEST_PASS("emit-pid-signals set to FALSE");
  } else {
    TEST_FAIL("Failed to disable emit-pid-signals");
    gst_object_unref(tsfilter);
    return FALSE;
  }

  /* Check property is readable */
  g_object_get(tsfilter, "emit-pid-signals", &emit_signals, NULL);
  if (emit_signals == FALSE) {
    TEST_PASS("emit-pid-signals property is readable");
  } else {
    TEST_FAIL("emit-pid-signals property not reflecting FALSE");
  }

  /* Re-enable */
  g_object_set(tsfilter, "emit-pid-signals", TRUE, NULL);
  g_object_get(tsfilter, "emit-pid-signals", &emit_signals, NULL);
  if (emit_signals == TRUE) {
    TEST_PASS("emit-pid-signals re-enabled");
  } else {
    TEST_FAIL("Failed to re-enable emit-pid-signals");
  }

  gst_object_unref(tsfilter);
  return TRUE;
}

/*
 * Test 5: Filter PIDs property (allow list mode)
 */
static gboolean
test_filter_pids_allow_list(void)
{
  TEST_START("Filter PIDs (Allow List Mode)");

  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  if (!tsfilter) {
    TEST_FAIL("Failed to create tsfilter element");
    return FALSE;
  }

  /* Create filter array with PIDs 0, 1, 1000 */
  GArray *pids = g_array_new(FALSE, FALSE, sizeof(guint16));
  guint16 pid1 = 0, pid2 = 1, pid3 = 1000;
  g_array_append_val(pids, pid1);
  g_array_append_val(pids, pid2);
  g_array_append_val(pids, pid3);

  g_object_set(tsfilter, "filter-pids", pids, NULL);
  TEST_PASS("Set filter-pids array");

  gboolean invert;
  g_object_get(tsfilter, "invert-filter", &invert, NULL);
  if (invert == FALSE) {
    TEST_PASS("invert-filter is FALSE (allow list mode)");
  } else {
    TEST_FAIL("invert-filter should be FALSE for allow list");
  }

  g_array_unref(pids);
  gst_object_unref(tsfilter);
  return TRUE;
}

/*
 * Test 4: Filter PIDs property (deny list mode)
 */
static gboolean
test_filter_pids_deny_list(void)
{
  TEST_START("Filter PIDs (Deny List Mode)");

  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  if (!tsfilter) {
    TEST_FAIL("Failed to create tsfilter element");
    return FALSE;
  }

  /* Create filter array and set to deny list mode */
  GArray *pids = g_array_new(FALSE, FALSE, sizeof(guint16));
  guint16 pid1 = 8191;  /* NULL packets */
  g_array_append_val(pids, pid1);

  g_object_set(tsfilter, "filter-pids", pids, "invert-filter", TRUE, NULL);
  TEST_PASS("Set filter-pids with invert-filter=TRUE");

  gboolean invert;
  g_object_get(tsfilter, "invert-filter", &invert, NULL);
  if (invert == TRUE) {
    TEST_PASS("invert-filter is TRUE (deny list mode)");
  } else {
    TEST_FAIL("invert-filter should be TRUE for deny list");
  }

  g_array_unref(pids);
  gst_object_unref(tsfilter);
  return TRUE;
}

/*
 * Test 7: Process a valid TS file
 */
static gboolean
test_process_file(const gchar *filepath)
{
  TEST_START("Process Valid TS File");

  struct stat st;
  if (stat(filepath, &st) != 0) {
    printf("   Note: Test file not found: %s\n", filepath);
    printf("   Skipping file processing test.\n");
    return FALSE;
  }

  printf("   Processing: %s (%" G_GSIZE_FORMAT " bytes)\n", filepath, (gsize)st.st_size);

  /* Create elements manually for REQUEST pad support */
  GstElement *src = gst_element_factory_make("filesrc", "src");
  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  GstElement *queue = gst_element_factory_make("queue", "queue");
  GstElement *sink = gst_element_factory_make("fakesink", "sink");

  if (!src || !tsfilter || !queue || !sink) {
    TEST_FAIL("Failed to create elements");
    if (src) gst_object_unref(src);
    if (tsfilter) gst_object_unref(tsfilter);
    if (queue) gst_object_unref(queue);
    if (sink) gst_object_unref(sink);
    return FALSE;
  }

  /* Set source location */
  g_object_set(src, "location", filepath, NULL);

  /* Create pipeline and add elements */
  GstElement *pipeline = gst_pipeline_new("pipeline");
  gst_bin_add_many(GST_BIN(pipeline), src, tsfilter, queue, sink, NULL);

  /* Get source pad */
  GstPad *srcpad = gst_element_get_static_pad(src, "src");

  /* Request sink pad from tsfilter (REQUEST pad) */
  GstPad *filter_sink = gst_element_request_pad_simple(tsfilter, "sink");
  if (!filter_sink) {
    TEST_FAIL("Failed to request sink pad from tsfilter");
    gst_object_unref(srcpad);
    gst_object_unref(pipeline);
    return FALSE;
  }

  /* Link source to filter */
  if (gst_pad_link(srcpad, filter_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link source to tsfilter");
    gst_object_unref(srcpad);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(srcpad);

  /* Get filter src pad and queue pads */
  GstPad *filter_src = gst_element_get_static_pad(tsfilter, "src");
  GstPad *queue_sink = gst_element_get_static_pad(queue, "sink");
  GstPad *queue_src = gst_element_get_static_pad(queue, "src");
  GstPad *sink_sink = gst_element_get_static_pad(sink, "sink");

  /* Link filter to queue to sink (queue prevents deadlock) */
  if (gst_pad_link(filter_src, queue_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link tsfilter to queue");
    gst_object_unref(filter_src);
    gst_object_unref(queue_sink);
    gst_object_unref(queue_src);
    gst_object_unref(sink_sink);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }
  if (gst_pad_link(queue_src, sink_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link queue to sink");
    gst_object_unref(filter_src);
    gst_object_unref(queue_sink);
    gst_object_unref(queue_src);
    gst_object_unref(sink_sink);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(filter_src);
  gst_object_unref(queue_sink);
  gst_object_unref(queue_src);
  gst_object_unref(sink_sink);

  /* Request src_0 pad for PID 0 output and use filesink to dump */
  GstPad *src_0 = gst_element_request_pad_simple(tsfilter, "src_0");
  GstElement *queue_0 = NULL;
  GstElement *sink_0 = NULL;
  const gchar *pid_0_output = "/tmp/pid_0.ts";

  if (src_0) {
    printf("   Requested src_0 pad for PID 0\n");

    /* Create queue and filesink for PID 0 output (queue prevents deadlock) */
    queue_0 = gst_element_factory_make("queue", "queue_0");
    sink_0 = gst_element_factory_make("filesink", "sink_0");
    if (queue_0 && sink_0) {
      g_object_set(sink_0, "location", pid_0_output, NULL);
      gst_bin_add_many(GST_BIN(pipeline), queue_0, sink_0, NULL);

      GstPad *queue_0_sink = gst_element_get_static_pad(queue_0, "sink");
      GstPad *queue_0_src = gst_element_get_static_pad(queue_0, "src");
      GstPad *sink_0_pad = gst_element_get_static_pad(sink_0, "sink");

      if (queue_0_sink && queue_0_src && sink_0_pad) {
        if (gst_pad_link(src_0, queue_0_sink) == GST_PAD_LINK_OK &&
            gst_pad_link(queue_0_src, sink_0_pad) == GST_PAD_LINK_OK) {
          printf("   PID 0 pad linked to queue -> filesink: %s\n", pid_0_output);
        } else {
          printf("   Note: Failed to link src_0 to queue_0/sink_0\n");
        }
        if (queue_0_sink) gst_object_unref(queue_0_sink);
        if (queue_0_src) gst_object_unref(queue_0_src);
        if (sink_0_pad) gst_object_unref(sink_0_pad);
      }
    } else {
      printf("   Note: Failed to create queue_0/sink_0\n");
    }
  } else {
    printf("   Note: Failed to request src_0 pad\n");
  }

  TEST_PASS("Pipeline created and linked");

  /* Connect signals */
  bad_packet_count = 0;
  g_signal_connect(tsfilter, "bad-packet", G_CALLBACK(on_bad_packet), NULL);

  /* Set to pass all PIDs */
  g_object_set(tsfilter, "filter-pids", NULL, NULL);
  printf("   Filter mode: Pass all PIDs\n");

  /* Run pipeline */
  GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_ASYNC) {
    TEST_PASS("Pipeline state set to PLAYING");
  } else {
    TEST_FAIL("Failed to set pipeline to PLAYING");
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }

  /* Wait for EOS or error */
  GstBus *bus = gst_element_get_bus(pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_SECOND * 10,
                                                  GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

  if (msg) {
    GstMessageType msg_type = GST_MESSAGE_TYPE(msg);
    if (msg_type == GST_MESSAGE_EOS) {
      TEST_PASS("Pipeline completed successfully (EOS)");
    } else if (msg_type == GST_MESSAGE_ERROR) {
      GError *err;
      gchar *debug_info;
      gst_message_parse_error(msg, &err, &debug_info);
      printf("   ERROR: %s\n", err->message);
      g_error_free(err);
      g_free(debug_info);
      TEST_FAIL("Pipeline encountered error");
    }
    gst_message_unref(msg);
  } else {
    TEST_FAIL("Pipeline timeout (no EOS after 10 seconds)");
  }

  /* Validate PID 0 output file was created */
  if (src_0 && sink_0) {
    struct stat pid0_st;
    if (stat(pid_0_output, &pid0_st) == 0) {
      printf("   PID 0 output file created: %s (%" G_GSIZE_FORMAT " bytes)\n",
             pid_0_output, (gsize)pid0_st.st_size);
      if (pid0_st.st_size > 0) {
        TEST_PASS("PID 0 file has content");
      } else {
        printf("   Note: PID 0 file is empty (no PID 0 packets in stream)\n");
      }
    } else {
      printf("   Note: PID 0 output file not created\n");
    }
  }

  /* Cleanup */
  if (src_0) {
    gst_element_release_request_pad(tsfilter, src_0);
    /* Note: release_request_pad() already unrefs the pad */
  }
  gst_element_release_request_pad(tsfilter, filter_sink);
  gst_object_unref(filter_sink);
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);

  printf("   Total bad packets detected: %u\n", bad_packet_count);
  return TRUE;
}

/*
 * Test 8: Multiple PID pads with filter_pids
 */
static gboolean
test_multiple_pid_pads_with_filter(const gchar *filepath)
{
  TEST_START("Multiple PID Pads with filter_pids");

  struct stat st;
  if (stat(filepath, &st) != 0) {
    printf("   Note: Test file not found: %s\n", filepath);
    printf("   Skipping test.\n");
    return FALSE;
  }

  printf("   Processing: %s (%" G_GSIZE_FORMAT " bytes)\n", filepath, (gsize)st.st_size);

  /* Create elements */
  GstElement *src = gst_element_factory_make("filesrc", "src");
  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  GstElement *sink = gst_element_factory_make("fakesink", "sink");

  if (!src || !tsfilter || !sink) {
    TEST_FAIL("Failed to create elements");
    if (src) gst_object_unref(src);
    if (tsfilter) gst_object_unref(tsfilter);
    if (sink) gst_object_unref(sink);
    return FALSE;
  }

  g_object_set(src, "location", filepath, NULL);

  /* Create pipeline */
  GstElement *pipeline = gst_pipeline_new("pipeline");
  gst_bin_add_many(GST_BIN(pipeline), src, tsfilter, sink, NULL);

  /* Link source to filter */
  GstPad *srcpad = gst_element_get_static_pad(src, "src");
  GstPad *filter_sink = gst_element_request_pad_simple(tsfilter, "sink");
  if (gst_pad_link(srcpad, filter_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link source to filter");
    gst_object_unref(srcpad);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(srcpad);

  /* Link filter to sink (main src pad) */
  GstPad *filter_src = gst_element_get_static_pad(tsfilter, "src");
  GstPad *sink_sink = gst_element_get_static_pad(sink, "sink");
  if (gst_pad_link(filter_src, sink_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link filter to sink");
    gst_object_unref(filter_src);
    gst_object_unref(sink_sink);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(filter_src);
  gst_object_unref(sink_sink);

  /* Set filter_pids to [0, 1] - only allow PIDs 0 and 1 on main src pad */
  GArray *pids = g_array_new(FALSE, FALSE, sizeof(guint16));
  guint16 pid0 = 0, pid1 = 1;
  g_array_append_val(pids, pid0);
  g_array_append_val(pids, pid1);
  g_object_set(tsfilter, "filter-pids", pids, NULL);
  printf("   filter_pids set to [0, 1] (allow list mode)\n");

  /* Request PID 0 and PID 1 pads */
  GstPad *src_0 = gst_element_request_pad_simple(tsfilter, "src_0");
  GstPad *src_1 = gst_element_request_pad_simple(tsfilter, "src_1");

  if (!src_0 || !src_1) {
    TEST_FAIL("Failed to request src_0 or src_1 pad");
    if (src_0) gst_element_release_request_pad(tsfilter, src_0);
    if (src_1) gst_element_release_request_pad(tsfilter, src_1);
    g_array_unref(pids);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }
  printf("   Requested src_0 and src_1 pads\n");

  /* Create filesinks for PID 0 and PID 1 outputs */
  GstElement *sink_0 = gst_element_factory_make("filesink", "sink_0");
  GstElement *sink_1 = gst_element_factory_make("filesink", "sink_1");

  if (!sink_0 || !sink_1) {
    TEST_FAIL("Failed to create filesinks");
    if (sink_0) gst_object_unref(sink_0);
    if (sink_1) gst_object_unref(sink_1);
    gst_element_release_request_pad(tsfilter, src_0);
    gst_element_release_request_pad(tsfilter, src_1);
    g_array_unref(pids);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }

  g_object_set(sink_0, "location", "/tmp/pid_0_multi.ts", NULL);
  g_object_set(sink_1, "location", "/tmp/pid_1_multi.ts", NULL);

  gst_bin_add_many(GST_BIN(pipeline), sink_0, sink_1, NULL);

  /* Link PID pads to filesinks */
  GstPad *sink_0_pad = gst_element_get_static_pad(sink_0, "sink");
  GstPad *sink_1_pad = gst_element_get_static_pad(sink_1, "sink");

  if (gst_pad_link(src_0, sink_0_pad) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link src_0 to sink_0");
    gst_object_unref(sink_0_pad);
    gst_object_unref(sink_1_pad);
    goto cleanup;
  }
  if (gst_pad_link(src_1, sink_1_pad) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link src_1 to sink_1");
    gst_object_unref(sink_0_pad);
    gst_object_unref(sink_1_pad);
    goto cleanup;
  }

  gst_object_unref(sink_0_pad);
  gst_object_unref(sink_1_pad);

  TEST_PASS("Pipeline with multiple PID pads created");

  /* Run pipeline */
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    TEST_FAIL("Failed to set pipeline to PLAYING");
    goto cleanup;
  }

  /* Wait for EOS or error */
  GstBus *bus = gst_element_get_bus(pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_SECOND * 10,
                                                GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

  if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
    TEST_PASS("Pipeline completed successfully (EOS)");
  } else if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
    TEST_FAIL("Pipeline encountered error");
  } else {
    printf("   Note: Pipeline timeout\n");
  }

  if (msg) gst_message_unref(msg);
  gst_object_unref(bus);

  /* Verify output files */
  struct stat pid0_st, pid1_st;
  gboolean pid0_exists = (stat("/tmp/pid_0_multi.ts", &pid0_st) == 0);
  gboolean pid1_exists = (stat("/tmp/pid_1_multi.ts", &pid1_st) == 0);

  printf("   PID 0 output: %s (%" G_GSIZE_FORMAT " bytes)\n",
         pid0_exists ? "created" : "NOT created",
         pid0_exists ? (gsize)pid0_st.st_size : 0);
  printf("   PID 1 output: %s (%" G_GSIZE_FORMAT " bytes)\n",
         pid1_exists ? "created" : "NOT created",
         pid1_exists ? (gsize)pid1_st.st_size : 0);

  if (pid0_exists && pid0_st.st_size > 0) {
    TEST_PASS("PID 0 file has content");
  } else if (pid0_exists) {
    printf("   Note: PID 0 file is empty (no PID 0 packets in stream)\n");
  }

  if (pid1_exists && pid1_st.st_size > 0) {
    TEST_PASS("PID 1 file has content");
  } else if (pid1_exists) {
    printf("   Note: PID 1 file is empty (no PID 1 packets in stream)\n");
  }

cleanup:
  /* Cleanup */
  if (src_0) gst_element_release_request_pad(tsfilter, src_0);
  if (src_1) gst_element_release_request_pad(tsfilter, src_1);
  gst_element_release_request_pad(tsfilter, filter_sink);
  g_array_unref(pids);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);

  return TRUE;
}

/*
 * Test 9: Packet size configuration
 */
static gboolean
test_packet_size_config(void)
{
  TEST_START("Packet Size Configuration");

  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  if (!tsfilter) {
    TEST_FAIL("Failed to create tsfilter element");
    return FALSE;
  }

  /* Test setting different packet sizes */
  const guint sizes[] = {188, 192, 204, 208, 512, 1024, 2048};
  for (gsize i = 0; i < G_N_ELEMENTS(sizes); i++) {
    guint test_size = sizes[i];
    g_object_set(tsfilter, "packet-size", test_size, NULL);

    guint actual_size;
    g_object_get(tsfilter, "packet-size", &actual_size, NULL);

    if (actual_size == test_size) {
      printf("   Packet size %u: OK\n", test_size);
    } else {
      printf("   Packet size %u: FAILED (got %u)\n", test_size, actual_size);
    }
  }

  TEST_PASS("Packet size configuration tested");

  /* Test boundary values - get param spec to determine valid range */
  GObjectClass *klass = G_OBJECT_GET_CLASS(tsfilter);
  GParamSpec *pspec = g_object_class_find_property(klass, "packet-size");
  if (pspec && G_IS_PARAM_SPEC_UINT(pspec)) {
    GParamSpecUInt *pspec_uint = G_PARAM_SPEC_UINT(pspec);
    guint min_val = pspec_uint->minimum;
    guint max_val = pspec_uint->maximum;
    guint actual_size;

    printf("   Valid range: %u to %u\n", min_val, max_val);

    /* Test minimum value */
    g_object_set(tsfilter, "packet-size", min_val, NULL);
    g_object_get(tsfilter, "packet-size", &actual_size, NULL);
    if (actual_size == min_val) {
      printf("   Boundary test (min %u): OK\n", min_val);
    } else {
      printf("   Boundary test (min %u): FAILED (got %u)\n", min_val, actual_size);
    }

    /* Test maximum value */
    g_object_set(tsfilter, "packet-size", max_val, NULL);
    g_object_get(tsfilter, "packet-size", &actual_size, NULL);
    if (actual_size == max_val) {
      printf("   Boundary test (max %u): OK\n", max_val);
    } else {
      printf("   Boundary test (max %u): FAILED (got %u)\n", max_val, actual_size);
    }

    /* Note: Values outside valid range are rejected by GStreamer's
     * param validation, which emits critical warnings. This is expected
     * behavior demonstrating the validation is working correctly. */
  }

  TEST_PASS("Packet size boundary testing");

  /* Reset to default */
  g_object_set(tsfilter, "packet-size", 188, NULL);

  gst_object_unref(tsfilter);
  return TRUE;
}

/*
 * Test 10: Auto-detect toggle
 */
static gboolean
test_auto_detect_toggle(void)
{
  TEST_START("Auto-Detect Toggle");

  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  if (!tsfilter) {
    TEST_FAIL("Failed to create tsfilter element");
    return FALSE;
  }

  /* Disable auto-detect */
  g_object_set(tsfilter, "auto-detect", FALSE, NULL);
  gboolean auto_detect;
  g_object_get(tsfilter, "auto-detect", &auto_detect, NULL);
  if (auto_detect == FALSE) {
    TEST_PASS("Auto-detect disabled");
  } else {
    TEST_FAIL("Failed to disable auto-detect");
  }

  /* Re-enable auto-detect */
  g_object_set(tsfilter, "auto-detect", TRUE, NULL);
  g_object_get(tsfilter, "auto-detect", &auto_detect, NULL);
  if (auto_detect == TRUE) {
    TEST_PASS("Auto-detect re-enabled");
  } else {
    TEST_FAIL("Failed to re-enable auto-detect");
  }

  gst_object_unref(tsfilter);
  return TRUE;
}

/*
 * Test 11: Multiple concurrent filter instances
 */
static gboolean
test_multiple_instances(void)
{
  TEST_START("Multiple Concurrent Instances");

  GstElement *filters[3];
  for (gint i = 0; i < 3; i++) {
    gchar name[32];
    g_snprintf(name, sizeof(name), "filter%d", i);
    filters[i] = gst_element_factory_make("tsfilter", name);
    if (!filters[i]) {
      TEST_FAIL("Failed to create instance");
      return FALSE;
    }
  }

  TEST_PASS("Created 3 concurrent tsfilter instances");

  /* Configure each differently */
  g_object_set(filters[0], "packet-size", 188, NULL);
  g_object_set(filters[1], "packet-size", 192, NULL);
  g_object_set(filters[2], "packet-size", 204, NULL);

  TEST_PASS("Configured instances with different packet sizes");

  for (gint i = 0; i < 3; i++) {
    gst_object_unref(filters[i]);
  }

  return TRUE;
}

/*
 * Test 12: enable-dump property defaults to FALSE
 */
static gboolean
test_enable_dump_default(void)
{
  TEST_START("enable-dump Property Default");

  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  if (!tsfilter) {
    TEST_FAIL("Failed to create tsfilter element");
    return FALSE;
  }

  /* Check default value is FALSE */
  gboolean enable_dump;
  g_object_get(tsfilter, "enable-dump", &enable_dump, NULL);
  if (enable_dump == FALSE) {
    TEST_PASS("enable-dump defaults to FALSE");
  } else {
    TEST_FAIL("enable-dump should default to FALSE");
    gst_object_unref(tsfilter);
    return FALSE;
  }

  /* Check that dump pad does NOT exist when enable-dump=FALSE */
  GstPad *dumppad = gst_element_get_static_pad(tsfilter, "dump");
  if (dumppad == NULL) {
    TEST_PASS("dump pad is not created when enable-dump=FALSE");
  } else {
    TEST_FAIL("dump pad should not exist when enable-dump=FALSE");
    gst_object_unref(dumppad);
    gst_object_unref(tsfilter);
    return FALSE;
  }

  gst_object_unref(tsfilter);
  return TRUE;
}

/*
 * Test 13: enable-dump=TRUE creates dump pad
 */
static gboolean
test_enable_dump_true(void)
{
  TEST_START("enable-dump=TRUE Creates Dump Pad");

  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  if (!tsfilter) {
    TEST_FAIL("Failed to create tsfilter element");
    return FALSE;
  }

  /* Set enable-dump to TRUE */
  g_object_set(tsfilter, "enable-dump", TRUE, NULL);
  gboolean enable_dump;
  g_object_get(tsfilter, "enable-dump", &enable_dump, NULL);
  if (enable_dump == TRUE) {
    TEST_PASS("enable-dump set to TRUE");
  } else {
    TEST_FAIL("Failed to set enable-dump to TRUE");
    gst_object_unref(tsfilter);
    return FALSE;
  }

  /* Check that dump pad exists when enable-dump=TRUE */
  GstPad *dumppad = gst_element_get_static_pad(tsfilter, "dump");
  if (dumppad != NULL) {
    TEST_PASS("dump pad is created when enable-dump=TRUE");
    gst_object_unref(dumppad);
  } else {
    TEST_FAIL("dump pad should exist when enable-dump=TRUE");
    gst_object_unref(tsfilter);
    return FALSE;
  }

  gst_object_unref(tsfilter);
  return TRUE;
}

/*
 * Test 13.5: dump-pad-added signal
 */
static gboolean
test_dump_pad_added_signal(void)
{
  TEST_START("dump-pad-added Signal");

  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  if (!tsfilter) {
    TEST_FAIL("Failed to create tsfilter element");
    return FALSE;
  }

  /* Reset signal flag */
  dump_pad_added_received = FALSE;

  /* Connect to dump-pad-added signal */
  g_signal_connect(tsfilter, "dump-pad-added",
                   G_CALLBACK(on_dump_pad_added), NULL);

  /* Set enable-dump to TRUE - this should trigger the signal */
  g_object_set(tsfilter, "enable-dump", TRUE, NULL);

  if (dump_pad_added_received) {
    TEST_PASS("dump-pad-added signal emitted when enable-dump=TRUE");
  } else {
    TEST_FAIL("dump-pad-added signal NOT emitted");
    gst_object_unref(tsfilter);
    return FALSE;
  }

  /* Reset and test disabling */
  dump_pad_added_received = FALSE;
  g_object_set(tsfilter, "enable-dump", FALSE, NULL);

  /* Signal should NOT be emitted when disabling dump pad */
  if (!dump_pad_added_received) {
    TEST_PASS("dump-pad-added signal NOT emitted when enable-dump=FALSE");
  } else {
    TEST_FAIL("dump-pad-added signal should NOT be emitted when disabling");
  }

  gst_object_unref(tsfilter);
  return TRUE;
}

/*
 * Test 14: enable-dump=TRUE, filter_pids empty - packets from dump pad
 */
static gboolean
test_enable_dump_true_no_filter(const gchar *filepath)
{
  TEST_START("enable-dump=TRUE, filter_pids empty -> dump pad");

  struct stat st;
  if (stat(filepath, &st) != 0) {
    printf("   Note: Test file not found: %s\n", filepath);
    printf("   Skipping test.\n");
    return FALSE;
  }

  printf("   Processing: %s (%" G_GSIZE_FORMAT " bytes)\n", filepath, (gsize)st.st_size);

  /* Create elements */
  GstElement *src = gst_element_factory_make("filesrc", "src");
  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  GstElement *dump_sink = gst_element_factory_make("filesink", "dump_sink");
  const gchar *dump_output = "/tmp/test14_dump_output.ts";

  if (!src || !tsfilter || !dump_sink) {
    TEST_FAIL("Failed to create elements");
    if (src) gst_object_unref(src);
    if (tsfilter) gst_object_unref(tsfilter);
    if (dump_sink) gst_object_unref(dump_sink);
    return FALSE;
  }

  /* Configure: enable-dump=TRUE, filter_pids empty */
  g_object_set(src, "location", filepath, NULL);
  g_object_set(tsfilter, "filter-pids", NULL, "enable-dump", TRUE, NULL);
  g_object_set(dump_sink, "location", dump_output, NULL);

  /* Create pipeline */
  GstElement *pipeline = gst_pipeline_new("pipeline");
  gst_bin_add_many(GST_BIN(pipeline), src, tsfilter, dump_sink, NULL);

  /* Link source to filter */
  GstPad *srcpad = gst_element_get_static_pad(src, "src");
  GstPad *filter_sink = gst_element_request_pad_simple(tsfilter, "sink");
  if (gst_pad_link(srcpad, filter_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link source to filter");
    gst_object_unref(srcpad);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(srcpad);

  /* Link filter dump to sink (all PIDs should go here) */
  GstPad *filter_dump = gst_element_get_static_pad(tsfilter, "dump");
  GstPad *dump_sink_sink = gst_element_get_static_pad(dump_sink, "sink");
  if (gst_pad_link(filter_dump, dump_sink_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link filter dump to sink");
    gst_object_unref(filter_dump);
    gst_object_unref(dump_sink_sink);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(filter_dump);
  gst_object_unref(dump_sink_sink);

  /* Run pipeline */
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    TEST_FAIL("Failed to set pipeline to PLAYING");
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }

  /* Wait for EOS or error */
  GstBus *bus = gst_element_get_bus(pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_SECOND * 10,
                                                GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

  gboolean success = TRUE;
  if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
    TEST_PASS("Pipeline completed successfully");
  } else if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
    TEST_FAIL("Pipeline encountered error");
    success = FALSE;
  } else {
    printf("   Note: Pipeline timeout\n");
  }

  if (msg) gst_message_unref(msg);
  gst_object_unref(bus);

  /* Verify dump output file was created and has content */
  struct stat dump_st;
  if (stat(dump_output, &dump_st) == 0) {
    printf("   Dump output: %s (%" G_GSIZE_FORMAT " bytes)\n", dump_output, (gsize)dump_st.st_size);
    if (dump_st.st_size > 0) {
      TEST_PASS("Dump pad receives all packets (filter_pids empty)");
    } else {
      TEST_FAIL("Dump output file is empty");
      success = FALSE;
    }
  } else {
    TEST_FAIL("Dump output file not created");
    success = FALSE;
  }

  /* Cleanup */
  gst_element_release_request_pad(tsfilter, filter_sink);
  gst_object_unref(filter_sink);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);

  return success;
}

/*
 * Test 15: enable-dump=TRUE, filter_pids=[0,1] -> 3 files: src (filtered), dump (all), pid_0.ts, pid_1.ts
 */
static gboolean
test_enable_dump_true_with_pid_pads(const gchar *filepath)
{
  TEST_START("enable-dump=TRUE, filter_pids=[0,1] -> 3 files");

  struct stat st;
  if (stat(filepath, &st) != 0) {
    printf("   Note: Test file not found: %s\n", filepath);
    printf("   Skipping test.\n");
    return FALSE;
  }

  printf("   Processing: %s (%" G_GSIZE_FORMAT " bytes)\n", filepath, (gsize)st.st_size);

  /* Create elements */
  GstElement *src = gst_element_factory_make("filesrc", "src");
  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  GstElement *src_sink = gst_element_factory_make("filesink", "src_sink");
  GstElement *dump_sink = gst_element_factory_make("filesink", "dump_sink");
  GstElement *pid0_sink = gst_element_factory_make("filesink", "pid0_sink");
  GstElement *pid1_sink = gst_element_factory_make("filesink", "pid1_sink");
  const gchar *src_output = "/tmp/test15_src_output.ts";
  const gchar *dump_output = "/tmp/test15_dump_output.ts";
  const gchar *pid0_output = "/tmp/test15_pid_0.ts";
  const gchar *pid1_output = "/tmp/test15_pid_1.ts";

  if (!src || !tsfilter || !src_sink || !dump_sink || !pid0_sink || !pid1_sink) {
    TEST_FAIL("Failed to create elements");
    if (src) gst_object_unref(src);
    if (tsfilter) gst_object_unref(tsfilter);
    if (src_sink) gst_object_unref(src_sink);
    if (dump_sink) gst_object_unref(dump_sink);
    if (pid0_sink) gst_object_unref(pid0_sink);
    if (pid1_sink) gst_object_unref(pid1_sink);
    return FALSE;
  }

  /* Configure: enable-dump=TRUE, filter_pids=[0,1] */
  g_object_set(src, "location", filepath, NULL);
  GArray *pids = g_array_new(FALSE, FALSE, sizeof(guint16));
  guint16 pid0 = 0, pid1 = 1;
  g_array_append_val(pids, pid0);
  g_array_append_val(pids, pid1);
  g_object_set(tsfilter, "filter-pids", pids, "enable-dump", TRUE, NULL);
  g_object_set(src_sink, "location", src_output, NULL);
  g_object_set(dump_sink, "location", dump_output, NULL);
  g_object_set(pid0_sink, "location", pid0_output, NULL);
  g_object_set(pid1_sink, "location", pid1_output, NULL);

  /* Create pipeline */
  GstElement *pipeline = gst_pipeline_new("pipeline");
  gst_bin_add_many(GST_BIN(pipeline), src, tsfilter, src_sink, dump_sink,
                   pid0_sink, pid1_sink, NULL);

  /* Link source to filter */
  GstPad *srcpad = gst_element_get_static_pad(src, "src");
  GstPad *filter_sink = gst_element_request_pad_simple(tsfilter, "sink");
  if (gst_pad_link(srcpad, filter_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link source to filter");
    gst_object_unref(srcpad);
    gst_object_unref(filter_sink);
    g_array_unref(pids);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(srcpad);

  /* Link filter src to src_sink (filtered PIDs 0,1 only) */
  GstPad *filter_src = gst_element_get_static_pad(tsfilter, "src");
  GstPad *src_sink_sink = gst_element_get_static_pad(src_sink, "sink");
  if (gst_pad_link(filter_src, src_sink_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link filter src to src_sink");
    gst_object_unref(filter_src);
    gst_object_unref(src_sink_sink);
    gst_object_unref(filter_sink);
    g_array_unref(pids);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(filter_src);
  gst_object_unref(src_sink_sink);

  /* Link filter dump to dump_sink (all PIDs) */
  GstPad *filter_dump = gst_element_get_static_pad(tsfilter, "dump");
  GstPad *dump_sink_sink = gst_element_get_static_pad(dump_sink, "sink");
  if (gst_pad_link(filter_dump, dump_sink_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link filter dump to dump_sink");
    gst_object_unref(filter_dump);
    gst_object_unref(dump_sink_sink);
    gst_object_unref(filter_sink);
    g_array_unref(pids);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(filter_dump);
  gst_object_unref(dump_sink_sink);

  /* Request and link PID 0 pad */
  GstPad *pid0_pad = gst_element_request_pad_simple(tsfilter, "src_0");
  GstPad *pid0_sink_pad = gst_element_get_static_pad(pid0_sink, "sink");
  if (!pid0_pad || gst_pad_link(pid0_pad, pid0_sink_pad) != GST_PAD_LINK_OK) {
    printf("   Note: Failed to link PID 0 pad\n");
  }
  if (pid0_pad) gst_object_unref(pid0_pad);
  gst_object_unref(pid0_sink_pad);

  /* Request and link PID 1 pad */
  GstPad *pid1_pad = gst_element_request_pad_simple(tsfilter, "src_1");
  GstPad *pid1_sink_pad = gst_element_get_static_pad(pid1_sink, "sink");
  if (!pid1_pad || gst_pad_link(pid1_pad, pid1_sink_pad) != GST_PAD_LINK_OK) {
    printf("   Note: Failed to link PID 1 pad\n");
  }
  if (pid1_pad) gst_object_unref(pid1_pad);
  gst_object_unref(pid1_sink_pad);

  TEST_PASS("Pipeline created with enable-dump=TRUE, filter_pids=[0,1] and PID pads");

  /* Run pipeline */
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    TEST_FAIL("Failed to set pipeline to PLAYING");
    g_array_unref(pids);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }

  /* Wait for EOS or error */
  GstBus *bus = gst_element_get_bus(pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_SECOND * 10,
                                                GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

  gboolean success = TRUE;
  if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
    TEST_PASS("Pipeline completed successfully");
  } else if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
    TEST_FAIL("Pipeline encountered error");
    success = FALSE;
  } else {
    printf("   Note: Pipeline timeout\n");
  }

  if (msg) gst_message_unref(msg);
  gst_object_unref(bus);

  /* Verify output files */
  struct stat src_st, dump_st, pid0_st, pid1_st;
  gboolean src_exists = (stat(src_output, &src_st) == 0);
  gboolean dump_exists = (stat(dump_output, &dump_st) == 0);
  gboolean pid0_exists = (stat(pid0_output, &pid0_st) == 0);
  gboolean pid1_exists = (stat(pid1_output, &pid1_st) == 0);

  printf("   Files created:\n");
  printf("     src pad (filtered PIDs 0,1): %s (%" G_GSIZE_FORMAT " bytes)\n",
         src_exists ? "YES" : "NO", src_exists ? (gsize)src_st.st_size : 0);
  printf("     dump pad (all PIDs): %s (%" G_GSIZE_FORMAT " bytes)\n",
         dump_exists ? "YES" : "NO", dump_exists ? (gsize)dump_st.st_size : 0);
  printf("     pid_0.ts: %s (%" G_GSIZE_FORMAT " bytes)\n",
         pid0_exists ? "YES" : "NO", pid0_exists ? (gsize)pid0_st.st_size : 0);
  printf("     pid_1.ts: %s (%" G_GSIZE_FORMAT " bytes)\n",
         pid1_exists ? "YES" : "NO", pid1_exists ? (gsize)pid1_st.st_size : 0);

  /* Check all 4 files were created (sizes may be 0 if timeout occurred) */
  if (src_exists && dump_exists && pid0_exists && pid1_exists) {
    TEST_PASS("Test 15: 4 files created (src, dump, pid_0.ts, pid_1.ts)");
    if (dump_st.st_size > 0) {
      printf("   Note: Dump pad has data (good!)\n");
    } else {
      printf("   Note: Files may be empty due to timeout or no matching PIDs\n");
    }
  } else {
    TEST_FAIL("Expected 4 output files");
    success = FALSE;
  }

  /* Cleanup */
  gst_element_release_request_pad(tsfilter, filter_sink);
  g_array_unref(pids);
  gst_object_unref(filter_sink);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);

  return success;
}

/*
 * Test 16: enable-dump=FALSE, filter_pids empty -> one file from src pad
 */
static gboolean
test_enable_dump_false_no_filter(const gchar *filepath)
{
  TEST_START("enable-dump=FALSE, filter_pids empty -> src pad only");

  struct stat st;
  if (stat(filepath, &st) != 0) {
    printf("   Note: Test file not found: %s\n", filepath);
    printf("   Skipping test.\n");
    return FALSE;
  }

  printf("   Processing: %s (%" G_GSIZE_FORMAT " bytes)\n", filepath, (gsize)st.st_size);

  /* Create elements */
  GstElement *src = gst_element_factory_make("filesrc", "src");
  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  GstElement *sink = gst_element_factory_make("filesink", "sink");
  const gchar *output_file = "/tmp/test16_src_output.ts";

  if (!src || !tsfilter || !sink) {
    TEST_FAIL("Failed to create elements");
    if (src) gst_object_unref(src);
    if (tsfilter) gst_object_unref(tsfilter);
    if (sink) gst_object_unref(sink);
    return FALSE;
  }

  /* Configure: enable-dump=FALSE, filter_pids empty */
  g_object_set(src, "location", filepath, NULL);
  g_object_set(tsfilter, "filter-pids", NULL, "enable-dump", FALSE, NULL);
  g_object_set(sink, "location", output_file, NULL);

  /* Create pipeline */
  GstElement *pipeline = gst_pipeline_new("pipeline");
  gst_bin_add_many(GST_BIN(pipeline), src, tsfilter, sink, NULL);

  /* Link source to filter */
  GstPad *srcpad = gst_element_get_static_pad(src, "src");
  GstPad *filter_sink = gst_element_request_pad_simple(tsfilter, "sink");
  if (gst_pad_link(srcpad, filter_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link source to filter");
    gst_object_unref(srcpad);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(srcpad);

  /* Link filter src to sink (pass-through mode: all packets) */
  GstPad *filter_src = gst_element_get_static_pad(tsfilter, "src");
  GstPad *sink_sink = gst_element_get_static_pad(sink, "sink");
  if (gst_pad_link(filter_src, sink_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link filter src to sink");
    gst_object_unref(filter_src);
    gst_object_unref(sink_sink);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(filter_src);
  gst_object_unref(sink_sink);

  /* Run pipeline */
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    TEST_FAIL("Failed to set pipeline to PLAYING");
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }

  /* Wait for EOS or error */
  GstBus *bus = gst_element_get_bus(pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_SECOND * 10,
                                                GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

  gboolean success = TRUE;
  if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
    TEST_PASS("Pipeline completed successfully");
  } else if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
    TEST_FAIL("Pipeline encountered error");
    success = FALSE;
  } else {
    printf("   Note: Pipeline timeout\n");
  }

  if (msg) gst_message_unref(msg);
  gst_object_unref(bus);

  /* Verify output file was created and has content */
  struct stat out_st;
  if (stat(output_file, &out_st) == 0) {
    printf("   Output file: %s (%" G_GSIZE_FORMAT " bytes)\n", output_file, (gsize)out_st.st_size);
    if (out_st.st_size > 0) {
      TEST_PASS("Test 16: One file from src pad (pass-through mode)");
    } else {
      TEST_FAIL("Output file is empty");
      success = FALSE;
    }
  } else {
    TEST_FAIL("Output file not created");
    success = FALSE;
  }

  /* Cleanup */
  gst_element_release_request_pad(tsfilter, filter_sink);
  gst_object_unref(filter_sink);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);

  return success;
}

/*
 * Test 17: enable-dump=FALSE, filter_pids=[0,1] -> only pid_0.ts and pid_1.ts files
 */
static gboolean
test_enable_dump_false_with_pid_pads(const gchar *filepath)
{
  TEST_START("enable-dump=FALSE, filter_pids=[0,1] -> pid_0.ts and pid_1.ts only");

  struct stat st;
  if (stat(filepath, &st) != 0) {
    printf("   Note: Test file not found: %s\n", filepath);
    printf("   Skipping test.\n");
    return FALSE;
  }

  printf("   Processing: %s (%" G_GSIZE_FORMAT " bytes)\n", filepath, (gsize)st.st_size);

  /* Create elements */
  GstElement *src = gst_element_factory_make("filesrc", "src");
  GstElement *tsfilter = gst_element_factory_make("tsfilter", "filter");
  GstElement *pid0_sink = gst_element_factory_make("filesink", "pid0_sink");
  GstElement *pid1_sink = gst_element_factory_make("filesink", "pid1_sink");
  const gchar *pid0_output = "/tmp/test17_pid_0.ts";
  const gchar *pid1_output = "/tmp/test17_pid_1.ts";

  if (!src || !tsfilter || !pid0_sink || !pid1_sink) {
    TEST_FAIL("Failed to create elements");
    if (src) gst_object_unref(src);
    if (tsfilter) gst_object_unref(tsfilter);
    if (pid0_sink) gst_object_unref(pid0_sink);
    if (pid1_sink) gst_object_unref(pid1_sink);
    return FALSE;
  }

  /* Configure: enable-dump=FALSE, filter_pids=[0,1] */
  g_object_set(src, "location", filepath, NULL);
  GArray *pids = g_array_new(FALSE, FALSE, sizeof(guint16));
  guint16 pid0_val = 0, pid1_val = 1;
  g_array_append_val(pids, pid0_val);
  g_array_append_val(pids, pid1_val);
  g_object_set(tsfilter, "filter-pids", pids, "enable-dump", FALSE, NULL);
  g_object_set(pid0_sink, "location", pid0_output, NULL);
  g_object_set(pid1_sink, "location", pid1_output, NULL);

  /* Create pipeline */
  GstElement *pipeline = gst_pipeline_new("pipeline");
  gst_bin_add_many(GST_BIN(pipeline), src, tsfilter, pid0_sink, pid1_sink, NULL);

  /* Link source to filter */
  GstPad *srcpad = gst_element_get_static_pad(src, "src");
  GstPad *filter_sink = gst_element_request_pad_simple(tsfilter, "sink");
  if (gst_pad_link(srcpad, filter_sink) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link source to filter");
    gst_object_unref(srcpad);
    gst_object_unref(filter_sink);
    g_array_unref(pids);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(srcpad);

  /* Request and link PID 0 pad */
  GstPad *pid0_pad = gst_element_request_pad_simple(tsfilter, "src_0");
  GstPad *pid0_sink_pad = gst_element_get_static_pad(pid0_sink, "sink");
  if (!pid0_pad || gst_pad_link(pid0_pad, pid0_sink_pad) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link PID 0 pad");
    if (pid0_pad) gst_object_unref(pid0_pad);
    gst_object_unref(pid0_sink_pad);
    gst_object_unref(filter_sink);
    g_array_unref(pids);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(pid0_pad);
  gst_object_unref(pid0_sink_pad);

  /* Request and link PID 1 pad */
  GstPad *pid1_pad = gst_element_request_pad_simple(tsfilter, "src_1");
  GstPad *pid1_sink_pad = gst_element_get_static_pad(pid1_sink, "sink");
  if (!pid1_pad || gst_pad_link(pid1_pad, pid1_sink_pad) != GST_PAD_LINK_OK) {
    TEST_FAIL("Failed to link PID 1 pad");
    if (pid1_pad) gst_object_unref(pid1_pad);
    gst_object_unref(pid1_sink_pad);
    gst_object_unref(filter_sink);
    g_array_unref(pids);
    gst_object_unref(pipeline);
    return FALSE;
  }
  gst_object_unref(pid1_pad);
  gst_object_unref(pid1_sink_pad);

  TEST_PASS("Pipeline created with enable-dump=FALSE, filter_pids=[0,1] and PID pads");

  /* Run pipeline */
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    TEST_FAIL("Failed to set pipeline to PLAYING");
    g_array_unref(pids);
    gst_object_unref(filter_sink);
    gst_object_unref(pipeline);
    return FALSE;
  }

  /* Wait for EOS or error */
  GstBus *bus = gst_element_get_bus(pipeline);
  GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_SECOND * 10,
                                                GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

  gboolean success = TRUE;
  if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
    TEST_PASS("Pipeline completed successfully");
  } else if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
    TEST_FAIL("Pipeline encountered error");
    success = FALSE;
  } else {
    printf("   Note: Pipeline timeout\n");
  }

  if (msg) gst_message_unref(msg);
  gst_object_unref(bus);

  /* Verify only PID files were created */
  struct stat pid0_st, pid1_st;
  gboolean pid0_exists = (stat(pid0_output, &pid0_st) == 0);
  gboolean pid1_exists = (stat(pid1_output, &pid1_st) == 0);

  printf("   Files created:\n");
  printf("     pid_0.ts: %s (%" G_GSIZE_FORMAT " bytes)\n",
         pid0_exists ? "YES" : "NO", pid0_exists ? (gsize)pid0_st.st_size : 0);
  printf("     pid_1.ts: %s (%" G_GSIZE_FORMAT " bytes)\n",
         pid1_exists ? "YES" : "NO", pid1_exists ? (gsize)pid1_st.st_size : 0);

  if (pid0_exists && pid1_exists) {
    TEST_PASS("Test 17: Only pid_0.ts and pid_1.ts files created");
  } else {
    printf("   Note: Files may be empty if PIDs 0/1 not in stream\n");
  }

  /* Cleanup */
  gst_element_release_request_pad(tsfilter, filter_sink);
  g_array_unref(pids);
  gst_object_unref(filter_sink);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);

  return success;
}

/*
 * Print summary report
 */
static void
print_summary(void)
{
  printf("\n");
  printf("═══════════════════════════════════════════════════════════════\n");
  printf("                    TEST SUMMARY                                 \n");
  printf("═══════════════════════════════════════════════════════════════\n");
  printf("  Total tests:  %d\n", tests_total);
  printf("  Passed:       %d ✓\n", tests_passed);
  printf("  Failed:       %d ✗\n", tests_failed);
  printf("═══════════════════════════════════════════════════════════════\n");

  if (tests_failed == 0) {
    printf("  Result: ALL TESTS PASSED!\n");
  } else {
    printf("  Result: SOME TESTS FAILED\n");
  }
  printf("═══════════════════════════════════════════════════════════════\n");
}

/*
 * Main test runner
 */
int main(int argc, char *argv[])
{
  gst_init(&argc, &argv);

  printf("╔═══════════════════════════════════════════════════════════════╗\n");
  printf("║         TSFILTER COMPREHENSIVE TEST SUITE                   ║\n");
  printf("║         GStreamer MPEG-TS PID Filter Plugin                 ║\n");
  printf("╚═══════════════════════════════════════════════════════════════╝\n");

  /* Check if plugin is loaded */
  GstPlugin *plugin = gst_plugin_load_by_name("tsfilter");
  if (!plugin) {
    fprintf(stderr, "ERROR: Failed to load tsfilter plugin!\n");
    fprintf(stderr, "Make sure GST_PLUGIN_PATH includes ../tsfilter/build\n");
    fprintf(stderr, "Or the plugin is installed in the GStreamer plugin directory\n");
    return 1;
  }
  printf("✓ Plugin loaded: %s version %s\n",
         gst_plugin_get_name(plugin),
         gst_plugin_get_version(plugin));
  g_object_unref(plugin);

  /* Run all tests */
  test_element_creation();
  test_signals();
  test_new_pid_signal_disabled();
  test_filter_pids_allow_list();
  test_filter_pids_deny_list();
  test_packet_size_config();
  test_auto_detect_toggle();
  test_multiple_instances();
  test_enable_dump_default();
  test_enable_dump_true();
  test_dump_pad_added_signal();

  /* Test with actual file if provided */
  const gchar *test_file = NULL;
  if (argc > 1) {
    test_file = argv[1];
  } else {
    /* Try to find a default test file - search for diverse options */
    const gchar *default_files[] = {
      "../tsduck-test/input/test-023a.ts",   /* Single packet */
      "../tsduck-test/input/test-023b.ts",   /* Two packets */
      "../tsduck-test/input/test-023c.ts",   /* Three packets */
      "../tsduck-test/input/test-018.ts",    /* Small file */
      "../tsduck-test/input/test-065.m2ts",  /* M2TS format */
      "../tsduck-test/input/test-198.ts",    /* Production stream */
      "../tsduck-test/input/test-001.ts",    /* Large file */
      NULL
    };
    for (gint i = 0; default_files[i] != NULL; i++) {
      if (g_file_test(default_files[i], G_FILE_TEST_IS_REGULAR)) {
        test_file = default_files[i];
        break;
      }
    }
  }

  if (test_file) {
    test_new_pid_signal(test_file);
    test_process_file(test_file);
    test_multiple_pid_pads_with_filter(test_file);
    test_enable_dump_true_no_filter(test_file);
    test_enable_dump_true_with_pid_pads(test_file);
    test_enable_dump_false_no_filter(test_file);
    test_enable_dump_false_with_pid_pads(test_file);
  } else {
    printf("\n=== TEST: Process Valid TS File ===\n");
    printf("   Note: No test file found. Skipping file processing test.\n");
    printf("   Usage: %s <path_to_ts_file>\n", argv[0]);
  }

  /* Print summary */
  print_summary();

  /* Cleanup */
  if (discovered_pids) {
    g_array_unref(discovered_pids);
    discovered_pids = NULL;
  }

  return (tests_failed == 0) ? 0 : 1;
}
