#define GST_USE_UNSTABLE_API
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/mpegts/mpegts.h>
#include <map>
#include <set>
#include <vector>
#include <chrono>
#include <mutex>
#include <iostream>
#include <string>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>

// -------------------- Forward Declarations --------------------
class TSAnalyzer;

// -------------------- Statistics Structures (matching tsfilter.c) --------------------
// Per-PID detailed statistics
struct PidDetailStats {
    guint packet_count;            // Total packets for this PID
    guint corrupted_count;         // Corrupted packets for this PID
    guint filtered_count;          // Packets pushed to src pad (if PID is in filter list)
    guint64 first_filtered_offset; // Byte offset of first filtered packet (0 if not filtered)
    guint cc_discontinuities;      // CC discontinuity count for this PID
};

// Global stream statistics
struct TsStreamStats {
    guint total_packets;           // Total packets in the stream (before EOS)
    guint corrupted_packets;       // Total corrupted/abandoned packets in the stream
    GArray* pids;                  // Array of all PIDs found (sorted, guint16)
    GHashTable* pid_details;       // Hash table: PID (guint16) -> PidDetailStats
};

// -------------------- PID & Program Structures --------------------
struct PIDInfo {
    uint8_t lastCC = 0xFF;
    uint64_t packetCount = 0;
    uint64_t lastPacketCount = 0;
    std::chrono::steady_clock::time_point lastSeen;
};

struct ProgramInfo {
    uint16_t pmtPID;
    std::set<uint16_t> streamPIDs;
    uint8_t pmtVersion = 0xFF;
};

// Helper struct for callback user data
struct PIDCallbackData {
    TSAnalyzer* analyzer;
    uint16_t pid;
};

struct PIDDumpBranch {
    GstElement* queue;
    GstElement* appsink;
    std::ofstream outfile;
    gulong handler_id;
    PIDCallbackData* callback_data;  // Allocated callback data
};

// -------------------- PID Description Helper --------------------
static std::string getPIDDescription(uint16_t pid) {
    switch (pid) {
        case 0:     return "PAT (Program Association Table)";
        case 1:     return "CAT (Conditional Access Table)";
        case 2:     return "TSDT (Transport Stream Description Table)";
        case 3:     return "IPMPControl";
        case 16:    return "NIT (Network Information Table)";
        case 17:    return "SDT/BAT (Service/Bouquet Association Table)";
        case 18:    return "EIT (Event Information Table)";
        case 19:    return "RST (Running Status Table)";
        case 20:    return "TDT/TOT (Time/Time Offset Table)";
        case 8191:  return "Null packets (padding)";
        default: {
            if (pid >= 0x0010 && pid <= 0x001F) return "DVB (reserved)";
            if (pid >= 0x1FFF && pid <= 0x1FFE) return "Reserved";
            return "";
        }
    }
}

// -------------------- TS Analyzer Class --------------------
class TSAnalyzer {
public:
    std::map<uint16_t, PIDInfo> pidMap;
    std::map<uint16_t, ProgramInfo> programs;
    std::mutex mtx;

    GstElement* pipeline = nullptr;
    GstElement* tsfilter = nullptr;

    // For bitrate calculation
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point endTime;

    // Manual PAT parsing buffers
    std::vector<uint8_t> patBuffer;

    // Output directory for PID dumps
    std::string outputDir = "pid_dumps";
    bool enablePidDump = false;
    bool enableDumpAll = false;  // Enable dumping all packets to a file
    bool enableCrcValidation = false;  // Enable CRC validation
    bool useGstSink = false;  // If TRUE, use filesink instead of appsink
    std::vector<uint16_t> filterPids;  // PIDs to filter (empty = pass all)
    volatile bool shuttingDown = false;  // Flag to prevent callbacks during destruction

    // Signal handler for tsfilter new-pid signal
    static void onNewPID(GstElement* tsfilter, guint pid, gpointer user_data) {
        TSAnalyzer* analyzer = static_cast<TSAnalyzer*>(user_data);

        std::cout << "[NEW-PID] Discovered PID " << pid;
        std::string desc = getPIDDescription(pid);
        if (!desc.empty()) {
            std::cout << " [" << desc << "]";
        }
        std::cout << std::endl;

        // Create dump branch if PID dumping is enabled
        if (analyzer->enablePidDump) {
            // Check if we already have a branch for this PID
            if (analyzer->pidDumpBranches.find(pid) == analyzer->pidDumpBranches.end()) {
                analyzer->createPIDDumpBranch(pid);
            }
        }
    }

    // Signal handler for tsfilter bad-packet signal
    static void onBadPacket(GstElement* tsfilter, guint count, gpointer user_data) {
        TSAnalyzer* analyzer = static_cast<TSAnalyzer*>(user_data);
        std::lock_guard<std::mutex> lock(analyzer->mtx);
        static uint64_t totalBad = 0;
        totalBad += count;
        std::cout << "[BAD-PACKET] " << count << " bad packets detected (total: " << totalBad << ")" << std::endl;
    }

    // Signal handler for tsfilter crc-error signal
    static void onCrcError(GstElement* tsfilter, guint count, gpointer user_data) {
        TSAnalyzer* analyzer = static_cast<TSAnalyzer*>(user_data);
        std::lock_guard<std::mutex> lock(analyzer->mtx);
        static uint64_t totalCrcErrors = 0;
        totalCrcErrors += count;
        std::cout << "[CRC-ERROR] " << count << " CRC validation errors detected (total: " << totalCrcErrors << ")" << std::endl;
    }

    // PID sample callback - writes buffer data to file
    // user_data is a pointer to PIDCallbackData
    static GstFlowReturn on_pid_new_sample(GstAppSink* sink, gpointer user_data) {
        PIDCallbackData* data = static_cast<PIDCallbackData*>(user_data);
        TSAnalyzer* analyzer = data->analyzer;
        uint16_t pid = data->pid;

        // Check if analyzer is shutting down to prevent use-after-free
        if (analyzer->shuttingDown) {
            return GST_FLOW_OK;
        }

        // Find the branch for this PID
        auto it = analyzer->pidDumpBranches.find(pid);
        if (it == analyzer->pidDumpBranches.end()) {
            return GST_FLOW_OK;  // Branch may have been removed
        }

        std::ofstream& outfile = it->second.outfile;

        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (!sample) {
            return GST_FLOW_ERROR;
        }

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (buffer) {
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                outfile.write(reinterpret_cast<const char*>(map.data), map.size);
                // Note: Not calling flush() here - let OS buffering handle it for better performance
                gst_buffer_unmap(buffer, &map);
            }
        }

        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // Create dump branch for a PID (queue + appsink/filesink based on mode)
    void createPIDDumpBranch(uint16_t pid) {
        std::string filename = outputDir + "/pid_" + std::to_string(pid) + ".ts";
        gchar* pad_name = g_strdup_printf("src_%u", pid);

        std::cout << "[DUMP] Creating branch for PID " << pid
                  << " -> " << filename << std::endl;

        // Create queue
        GstElement* queue = gst_element_factory_make("queue", nullptr);
        if (!queue) {
            std::cerr << "[ERROR] Failed to create queue for PID " << pid << std::endl;
            g_free(pad_name);
            return;
        }

        // Create unique queue name
        gchar* queue_name = g_strdup_printf("queue_pid_%u", pid);
        gst_element_set_name(queue, queue_name);
        g_free(queue_name);

        if (useGstSink) {
            // Use GStreamer filesink
            GstElement* filesink = gst_element_factory_make("filesink", nullptr);
            if (!filesink) {
                std::cerr << "[ERROR] Failed to create filesink for PID " << pid << std::endl;
                g_free(pad_name);
                gst_object_unref(queue);
                return;
            }

            // Configure filesink
            g_object_set(filesink, "location", filename.c_str(), NULL);
            g_object_set(filesink, "sync", FALSE, NULL);

            gchar* sink_name = g_strdup_printf("sink_pid_%u", pid);
            gst_element_set_name(filesink, sink_name);
            g_free(sink_name);

            // Add to pipeline and sync state
            gst_bin_add_many(GST_BIN(pipeline), queue, filesink, NULL);
            gst_element_sync_state_with_parent(queue);
            gst_element_sync_state_with_parent(filesink);

            // Link queue and filesink
            if (!gst_element_link(queue, filesink)) {
                std::cerr << "[ERROR] Failed to link queue -> filesink for PID " << pid << std::endl;
                gst_object_unref(queue);
                gst_object_unref(filesink);
                g_free(pad_name);
                return;
            }

            // Request PID-specific pad from tsfilter
            GstPad* srcpad = gst_element_request_pad_simple(tsfilter, pad_name);
            g_free(pad_name);

            if (!srcpad) {
                std::cerr << "[ERROR] Failed to request pad src_" << pid << std::endl;
                gst_bin_remove(GST_BIN(pipeline), queue);
                gst_bin_remove(GST_BIN(pipeline), filesink);
                return;
            }

            // Link tsfilter src pad to queue sink pad
            GstPad* queue_sink = gst_element_get_static_pad(queue, "sink");
            if (gst_pad_link(srcpad, queue_sink) != GST_PAD_LINK_OK) {
                std::cerr << "[ERROR] Failed to link tsfilter -> queue for PID " << pid << std::endl;
                gst_object_unref(queue_sink);
                gst_object_unref(srcpad);
                gst_bin_remove(GST_BIN(pipeline), queue);
                gst_bin_remove(GST_BIN(pipeline), filesink);
                return;
            }
            gst_object_unref(queue_sink);
            gst_object_unref(srcpad);

            // Store branch info (no callback data needed for filesink mode)
            PIDDumpBranch branch{
                queue,
                filesink,
                {},  // empty ofstream
                0,   // no handler
                nullptr  // no callback data
            };
            pidDumpBranches.emplace(pid, std::move(branch));

        } else {
            // Use appsink + C++ file I/O
            GstElement* appsink = gst_element_factory_make("appsink", nullptr);
            if (!appsink) {
                std::cerr << "[ERROR] Failed to create appsink for PID " << pid << std::endl;
                g_free(pad_name);
                gst_object_unref(queue);
                return;
            }

            // Configure appsink
            g_object_set(appsink, "emit-signals", TRUE, NULL);
            g_object_set(appsink, "sync", FALSE, NULL);

            gchar* sink_name = g_strdup_printf("sink_pid_%u", pid);
            gst_element_set_name(appsink, sink_name);
            g_free(sink_name);

            // Add to pipeline and sync state
            gst_bin_add_many(GST_BIN(pipeline), queue, appsink, NULL);
            gst_element_sync_state_with_parent(queue);
            gst_element_sync_state_with_parent(appsink);

            // Link queue and appsink
            if (!gst_element_link(queue, appsink)) {
                std::cerr << "[ERROR] Failed to link queue -> appsink for PID " << pid << std::endl;
                gst_object_unref(queue);
                gst_object_unref(appsink);
                g_free(pad_name);
                return;
            }

            // Request PID-specific pad from tsfilter
            GstPad* srcpad = gst_element_request_pad_simple(tsfilter, pad_name);
            g_free(pad_name);

            if (!srcpad) {
                std::cerr << "[ERROR] Failed to request pad src_" << pid << std::endl;
                gst_bin_remove(GST_BIN(pipeline), queue);
                gst_bin_remove(GST_BIN(pipeline), appsink);
                return;
            }

            // Link tsfilter src pad to queue sink pad
            GstPad* queue_sink = gst_element_get_static_pad(queue, "sink");
            if (gst_pad_link(srcpad, queue_sink) != GST_PAD_LINK_OK) {
                std::cerr << "[ERROR] Failed to link tsfilter -> queue for PID " << pid << std::endl;
                gst_object_unref(queue_sink);
                gst_object_unref(srcpad);
                gst_bin_remove(GST_BIN(pipeline), queue);
                gst_bin_remove(GST_BIN(pipeline), appsink);
                return;
            }
            gst_object_unref(queue_sink);
            gst_object_unref(srcpad);

            // Open output file
            std::ofstream outfile(filename, std::ios::binary | std::ios::trunc);
            if (!outfile.is_open()) {
                std::cerr << "[ERROR] Failed to open file " << filename << std::endl;
                gst_bin_remove(GST_BIN(pipeline), queue);
                gst_bin_remove(GST_BIN(pipeline), appsink);
                return;
            }

            // Allocate callback data
            PIDCallbackData* callback_data = new PIDCallbackData{this, pid};

            // Connect appsink signal with callback data
            gulong handler_id = g_signal_connect(appsink, "new-sample",
                                                  G_CALLBACK(on_pid_new_sample),
                                                  callback_data);

            // Store branch info using move semantics
            PIDDumpBranch branch{
                queue,
                appsink,
                std::move(outfile),
                handler_id,
                callback_data
            };
            pidDumpBranches.emplace(pid, std::move(branch));
        }

        std::cout << "[DUMP] Branch created for PID " << pid << std::endl;
    }

    void parsePATSection(const uint8_t* data, size_t len) {
        if (len < 3) return;

        if (data[0] != 0x00) return;

        uint16_t section_length = ((data[1] & 0x0F) << 8) | data[2];
        if (section_length > len - 3) return;

        size_t pos = 8;
        size_t end = 3 + section_length - 4;

        while (pos + 4 <= end) {
            uint16_t program_number = (data[pos] << 8) | data[pos + 1];
            uint16_t pmt_pid = ((data[pos + 2] & 0x1F) << 8) | data[pos + 3];

            if (program_number != 0) {
                if (programs.find(program_number) == programs.end()) {
                    ProgramInfo pi;
                    pi.pmtPID = pmt_pid;
                    programs[program_number] = pi;
                    std::cout << "[PAT] Program " << program_number
                              << " -> PMT PID " << pmt_pid << std::endl;
                }
            }
            pos += 4;
        }
    }

    void processTSBuffer(const guint8* buf, size_t size) {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mtx);

        if (pidMap.empty()) {
            startTime = now;
        }
        endTime = now;

        for (size_t i = 0; i + 188 <= size; i += 188) {
            const guint8* pkt = buf + i;
            uint16_t pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
            uint8_t cc = pkt[3] & 0x0F;

            bool hasPayload = (pkt[3] & 0x10) != 0;
            bool hasAdaptation = (pkt[3] & 0x20) != 0;

            size_t payloadStart = 4;
            if (hasAdaptation) {
                uint8_t afl = pkt[4];
                if (afl > 182) {
                    payloadStart = 4 + 182;
                    hasPayload = false;
                } else {
                    payloadStart = 4 + afl;
                }
            }

            // Collect PAT packets for manual parsing
            if (pid == 0 && hasPayload) {
                if (pkt[1] & 0x40) {
                    if (!patBuffer.empty() && patBuffer.size() > 1) {
                        uint8_t pointer = patBuffer[0];
                        if (patBuffer.size() > pointer + 3) {
                            parsePATSection(patBuffer.data() + 1 + pointer,
                                          patBuffer.size() - 1 - pointer);
                        }
                    }
                    patBuffer.clear();
                    if (payloadStart < 188) {
                        patBuffer.insert(patBuffer.end(),
                                      pkt + payloadStart, pkt + 188);
                    }
                } else {
                    if (payloadStart < 188) {
                        patBuffer.insert(patBuffer.end(),
                                      pkt + payloadStart, pkt + 188);
                    }
                }
            }

            // Skip CC check for null packets
            if (pid != 8191) {
                PIDInfo& info = pidMap[pid];
                if (info.lastCC != 0xFF && ((info.lastCC + 1) & 0x0F) != cc) {
                    std::cout << "[CC DISCONTINUITY] PID " << pid
                              << " lastCC=" << (int)info.lastCC
                              << " currentCC=" << (int)cc << std::endl;
                }
                info.lastCC = cc;
                info.packetCount++;
                info.lastSeen = now;
            } else {
                PIDInfo& info = pidMap[pid];
                info.packetCount++;
                info.lastSeen = now;
            }
        }
    }

    void handleSection(GstMpegtsSection* section) {
        std::lock_guard<std::mutex> lock(mtx);
        switch (section->section_type) {
            case GST_MPEGTS_SECTION_PAT: {
                GPtrArray* pat = gst_mpegts_section_get_pat(section);
                for (guint i = 0; i < pat->len; ++i) {
                    GstMpegtsPatProgram* prog = (GstMpegtsPatProgram*)g_ptr_array_index(pat, i);
                    if (programs.find(prog->program_number) == programs.end()) {
                        ProgramInfo pi;
                        pi.pmtPID = prog->network_or_program_map_PID;
                        programs[prog->program_number] = pi;
                        std::cout << "[NEW PROGRAM] " << prog->program_number
                                  << " PMT PID: " << prog->network_or_program_map_PID << std::endl;
                    }
                }
                break;
            }
            case GST_MPEGTS_SECTION_PMT: {
                const GstMpegtsPMT* pmt = gst_mpegts_section_get_pmt(section);
                ProgramInfo& pi = programs[pmt->program_number];
                if (pi.pmtVersion != section->version_number) {
                    std::cout << "[PMT VERSION CHANGE] Program " << pmt->program_number
                              << " old=" << (int)pi.pmtVersion
                              << " new=" << (int)section->version_number << std::endl;
                    pi.pmtVersion = section->version_number;
                }
                pi.streamPIDs.clear();
                for (guint i = 0; i < pmt->streams->len; ++i) {
                    GstMpegtsPMTStream* stream = (GstMpegtsPMTStream*)g_ptr_array_index(pmt->streams, i);
                    pi.streamPIDs.insert(stream->pid);
                }
                break;
            }
            default: break;
        }
    }

    std::map<uint16_t, PIDDumpBranch> pidDumpBranches;
    std::set<uint16_t> discoveredPIDs;
    uint64_t badPacketCount = 0;

    // Destructor for proper cleanup
    ~TSAnalyzer() {
        // Set shutdown flag first to prevent new callbacks
        shuttingDown = true;

        // Clean up all PID dump branches
        for (auto& [pid, branch] : pidDumpBranches) {
            // Disconnect signal handler if connected and object is still valid
            if (branch.handler_id != 0 && branch.appsink && G_IS_OBJECT(branch.appsink)) {
                g_signal_handler_disconnect(branch.appsink, branch.handler_id);
            }
            // Close output file if open
            if (branch.outfile.is_open()) {
                branch.outfile.close();
            }
            // Free callback data
            if (branch.callback_data) {
                delete branch.callback_data;
                branch.callback_data = nullptr;
            }
        }
        pidDumpBranches.clear();
    }
};

// -------------------- Global Analyzer --------------------
static TSAnalyzer analyzer;
static GMainLoop* main_loop = nullptr;

// -------------------- Appsink Callback --------------------
static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data) {
    TSAnalyzer* analyzer = static_cast<TSAnalyzer*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;

    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        analyzer->processTSBuffer(map.data, map.size);
        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// -------------------- Bus Callback (SI/PSI) --------------------
static gboolean bus_callback(GstBus* bus, GstMessage* msg, gpointer user_data) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ELEMENT: {
            const GstStructure* s = gst_message_get_structure(msg);
            if (gst_structure_has_name(s, "mpegts-section")) {
                GstMpegtsSection* section = gst_message_parse_mpegts_section(msg);
                if (section) {
                    analyzer.handleSection(section);
                    gst_mpegts_section_unref(section);
                }
            }
            break;
        }
        case GST_MESSAGE_EOS: {
            std::cout << "\n========================================" << std::endl;
            std::cout << "       Transport Stream Analysis Report" << std::endl;
            std::cout << "========================================\n" << std::endl;

            {
                std::lock_guard<std::mutex> lock(analyzer.mtx);

                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    analyzer.endTime - analyzer.startTime).count();
                double durationSec = duration / 1000.0;

                // Get actual packet size from tsfilter (not hardcoded 188)
                guint packet_size = 188;  // Default fallback
                if (analyzer.tsfilter) {
                    g_object_get(analyzer.tsfilter, "packet-size", &packet_size, NULL);
                }

                uint64_t totalPackets = 0;
                for (auto& [pid, info] : analyzer.pidMap) {
                    totalPackets += info.packetCount;
                }
                double totalBitrate = (totalPackets * packet_size * 8.0) / durationSec;

                std::cout << "Stream Information:" << std::endl;
                std::cout << "  Duration: " << std::fixed << std::setprecision(2) << durationSec << " seconds" << std::endl;
                std::cout << "  Packet Size: " << packet_size << " bytes" << std::endl;
                std::cout << "  Total Packets: " << totalPackets << std::endl;
                std::cout << "  Total Bitrate: " << std::fixed << std::setprecision(2)
                          << (totalBitrate / 1000000.0) << " Mbps" << std::endl;
                std::cout << "  Total PIDs: " << analyzer.pidMap.size() << "\n" << std::endl;

                std::cout << "PID Details:" << std::endl;
                for (auto& [pid, info] : analyzer.pidMap) {
                    std::string desc = getPIDDescription(pid);
                    double bitrate = (info.packetCount * packet_size * 8.0) / durationSec;

                    std::cout << "  PID " << std::setw(5) << pid << " (0x"
                              << std::hex << std::setw(4) << std::setfill('0') << pid
                              << std::dec << std::setfill(' ') << "): "
                              << std::setw(7) << info.packetCount << " pkt, "
                              << std::setw(10) << std::fixed << std::setprecision(0)
                              << bitrate << " bps";

                    if (!desc.empty()) {
                        std::cout << "  [" << desc << "]";
                    }
                    std::cout << std::endl;
                }
                std::cout << std::endl;

                // Query and display comprehensive stream statistics
                if (analyzer.tsfilter) {
                    TsStreamStats* stream_stats = nullptr;
                    g_object_get(analyzer.tsfilter, "stream-stats", &stream_stats, NULL);

                    if (stream_stats) {
                        try {
                            // Display global statistics
                            std::cout << "Stream Statistics:" << std::endl;
                            std::cout << "  Total Packets: " << stream_stats->total_packets << std::endl;
                            std::cout << "  Total Corrupted: " << stream_stats->corrupted_packets << std::endl;
                            std::cout << std::endl;

                            // Display per-PID detailed statistics
                            if (stream_stats->pid_details && g_hash_table_size(stream_stats->pid_details) > 0) {
                                std::cout << "Per-PID Detailed Statistics:" << std::endl;

                                GHashTableIter iter;
                                gpointer key, value;
                                g_hash_table_iter_init(&iter, stream_stats->pid_details);

                                // Collect and sort PIDs
                                std::map<uint16_t, PidDetailStats*> sorted_stats;
                                while (g_hash_table_iter_next(&iter, &key, &value)) {
                                    uint16_t pid = GPOINTER_TO_UINT(key);
                                    PidDetailStats* stats = static_cast<PidDetailStats*>(value);
                                    sorted_stats[pid] = stats;
                                }

                                // Display statistics for each PID
                                for (auto& [pid, stats] : sorted_stats) {
                                    std::string desc = getPIDDescription(pid);
                                    std::cout << "  PID " << std::setw(5) << pid << " (0x"
                                              << std::hex << std::setw(4) << std::setfill('0') << pid
                                              << std::dec << std::setfill(' ') << "): "
                                              << "packets=" << std::setw(7) << stats->packet_count << ", "
                                              << "corrupted=" << std::setw(4) << stats->corrupted_count << ", "
                                              << "filtered=" << std::setw(4) << stats->filtered_count << ", "
                                              << "cc_disc=" << std::setw(3) << stats->cc_discontinuities;

                                    if (stats->first_filtered_offset > 0) {
                                        std::cout << ", first_offset=" << stats->first_filtered_offset;
                                    }

                                    if (!desc.empty()) {
                                        std::cout << "  [" << desc << "]";
                                    }
                                    std::cout << std::endl;
                                }
                                std::cout << std::endl;
                            }

                            // Display list of all PIDs found
                            if (stream_stats->pids && stream_stats->pids->len > 0) {
                                std::cout << "PIDs Found (" << stream_stats->pids->len << " total):" << std::endl;

                                // Display all PIDs in decimal and hex format
                                for (guint i = 0; i < stream_stats->pids->len; i++) {
                                    guint16 pid = g_array_index(stream_stats->pids, guint16, i);
                                    std::cout << "  " << std::setw(5) << pid << " (0x" << std::hex << std::setw(4) << std::setfill('0') << pid << std::dec << std::setfill(' ') << ")";

                                    // Add description for known PIDs
                                    std::string desc = getPIDDescription(pid);
                                    if (!desc.empty()) {
                                        std::cout << " [" << desc << "]";
                                    }
                                    std::cout << std::endl;
                                }
                                std::cout << std::endl;
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "[ERROR] Exception while displaying PID details: " << e.what() << std::endl;
                        } catch (...) {
                            std::cerr << "[ERROR] Unknown exception while displaying PID details" << std::endl;
                        }

                        // Free the stream stats structure (always executed)
                        if (stream_stats->pids) {
                            g_array_unref(stream_stats->pids);
                        }
                        if (stream_stats->pid_details) {
                            g_hash_table_unref(stream_stats->pid_details);
                        }
                        g_free(stream_stats);
                    } else {
                        std::cout << "Stream Statistics: (no statistics available - enable-stats may be disabled)" << std::endl;
                        std::cout << std::endl;
                    }
                }

                std::cout << "Programs Detected: " << analyzer.programs.size() << std::endl;
                if (!analyzer.programs.empty()) {
                    for (auto& [prog_num, prog_info] : analyzer.programs) {
                        std::cout << "  Program " << prog_num
                                  << " -> PMT PID " << prog_info.pmtPID << std::endl;
                    }
                }
                std::cout << std::endl;

                std::cout << "PID Dump Files Created: " << analyzer.pidDumpBranches.size() << std::endl;
                if (!analyzer.pidDumpBranches.empty()) {
                    for (auto& [pid, branch] : analyzer.pidDumpBranches) {
                        std::string filename = analyzer.outputDir + "/pid_" + std::to_string(pid) + ".ts";
                        std::ifstream file(filename, std::ios::binary | std::ios::ate);
                        if (file) {
                            auto size = file.tellg();
                            file.close();
                            std::cout << "  PID " << pid << " -> " << size << " bytes" << std::endl;
                        }
                    }
                }
                std::cout << std::endl;

                std::cout << "Notes:" << std::endl;
                std::cout << "  - PID 0 = PAT (Program Association Table)" << std::endl;
                std::cout << "  - PID 8191 = Null packets (padding)" << std::endl;
                std::cout << "  - PID dump files saved to: " << analyzer.outputDir << "/" << std::endl;
            }

            std::cout << "========================================\n" << std::endl;
            if (main_loop) {
                g_main_loop_quit(main_loop);
            }
            break;
        }
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* debug_info = nullptr;
            gst_message_parse_error(msg, &err, &debug_info);
            std::cerr << "[ERROR] " << err->message << std::endl;
            if (debug_info) {
                std::cerr << "Debug info: " << debug_info << std::endl;
            }
            g_clear_error(&err);
            g_free(debug_info);
            if (main_loop) {
                g_main_loop_quit(main_loop);
            }
            break;
        }
        default:
            break;
    }
    return TRUE;
}

// -------------------- Periodic Status Update --------------------
static gboolean periodic_status_update(gpointer user_data) {
    TSAnalyzer* a = static_cast<TSAnalyzer*>(user_data);
    auto now = std::chrono::steady_clock::now();

    if (!a->mtx.try_lock()) {
        return TRUE;
    }
    std::lock_guard<std::mutex> lock(a->mtx, std::adopt_lock);

    std::cout << "--- Status Update ---" << std::endl;
    std::cout << "PIDs: " << a->pidMap.size() << " | Dump branches: " << a->pidDumpBranches.size() << std::endl;

    // Get actual packet size from tsfilter (not hardcoded 188)
    guint packet_size = 188;  // Default fallback
    if (a->tsfilter) {
        g_object_get(a->tsfilter, "packet-size", &packet_size, NULL);
    }

    for (auto& [pid, info] : a->pidMap) {
        uint64_t pktDelta = info.packetCount - info.lastPacketCount;
        double bitrate = pktDelta * packet_size * 8;
        info.lastPacketCount = info.packetCount;

        std::cout << "PID " << pid << " (0x" << std::hex << std::setw(4) << std::setfill('0') << pid << std::dec << std::setfill(' ') << "): "
                  << info.packetCount << " packets, "
                  << bitrate << " bps" << std::endl;
    }

    // Query and display tsfilter PID statistics
    if (a->tsfilter) {
        TsStreamStats* stream_stats = nullptr;
        g_object_get(a->tsfilter, "stream-stats", &stream_stats, NULL);

        if (stream_stats) {
            try {
                std::cout << "tsfilter stats: ";
                if (stream_stats->pid_details) {
                    GHashTableIter iter;
                    gpointer key, value;
                    guint total_corrupted = 0;
                    guint total_filtered = 0;
                    guint total_cc_disc = 0;
                    int pid_count = 0;

                    g_hash_table_iter_init(&iter, stream_stats->pid_details);
                    while (g_hash_table_iter_next(&iter, &key, &value)) {
                        PidDetailStats* stats = static_cast<PidDetailStats*>(value);
                        if (stats->corrupted_count > 0 || stats->filtered_count > 0 || stats->cc_discontinuities > 0) {
                            uint16_t pid = GPOINTER_TO_UINT(key);
                            std::cout << "PID" << pid << "(0x" << std::hex << std::setw(4) << std::setfill('0') << pid << std::dec << std::setfill(' ')
                                      << ",c:" << stats->corrupted_count
                                      << ",f:" << stats->filtered_count
                                      << ",cc:" << stats->cc_discontinuities << ") ";
                            total_corrupted += stats->corrupted_count;
                            total_filtered += stats->filtered_count;
                            total_cc_disc += stats->cc_discontinuities;
                            pid_count++;
                        }
                    }
                    if (pid_count == 0) {
                        std::cout << "no errors";
                    }
                    std::cout << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] Exception while displaying stream statistics: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[ERROR] Unknown exception while displaying stream statistics" << std::endl;
            }

            // Free the stream stats structure (always executed)
            if (stream_stats->pids) {
                g_array_unref(stream_stats->pids);
            }
            if (stream_stats->pid_details) {
                g_hash_table_unref(stream_stats->pid_details);
            }
            g_free(stream_stats);
        }
    }

    std::cout << "--------------------" << std::endl;
    return TRUE;
}

// -------------------- Main --------------------
int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <input.ts> [options]" << std::endl;
        std::cerr << "Options:" << std::endl;
        std::cerr << "  --filter-pids <pids> Filter only specified PIDs (comma-separated, e.g., 0,100,256)" << std::endl;
        std::cerr << "  --dump-pids, -d    Enable PID dumping (saves to pid_*.ts files)" << std::endl;
        std::cerr << "  --dump-all         Enable dumping all packets to dump_all.ts file" << std::endl;
        std::cerr << "  --gs               Use GStreamer filesink (default: C++ file I/O)" << std::endl;
        std::cerr << "  --output-dir, -o   Output directory for PID dumps (default: pid_dumps)" << std::endl;
        std::cerr << "  --enable-crc-validation  Enable CRC-32 validation for PSI/SI tables (default: off)" << std::endl;
        std::cerr << "  --help, -h         Show this help message" << std::endl;
        return -1;
    }

    std::string inputFile;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--filter-pids") {
            if (i + 1 < argc) {
                // Parse comma-separated PID list
                std::string pidList = argv[++i];
                std::stringstream ss(pidList);
                std::string pidStr;
                bool parse_error = false;

                while (std::getline(ss, pidStr, ',')) {
                    // Trim whitespace
                    pidStr.erase(0, pidStr.find_first_not_of(" \t"));
                    pidStr.erase(pidStr.find_last_not_of(" \t") + 1);

                    // Skip empty strings
                    if (pidStr.empty()) {
                        continue;
                    }

                    // Parse PID (supports decimal and hex with 0x prefix)
                    try {
                        uint16_t pid;
                        if (pidStr.substr(0, 2) == "0x" || pidStr.substr(0, 2) == "0X") {
                            pid = static_cast<uint16_t>(std::stoi(pidStr, nullptr, 16));
                        } else {
                            pid = static_cast<uint16_t>(std::stoi(pidStr));
                        }

                        // Validate PID range (0-8191 for MPEG-TS)
                        if (pid > 8191) {
                            std::cerr << "Error: PID " << pid << " is out of range (valid range: 0-8191)" << std::endl;
                            parse_error = true;
                            break;
                        }

                        analyzer.filterPids.push_back(pid);
                    } catch (const std::invalid_argument& e) {
                        std::cerr << "Error: Invalid PID value '" << pidStr << "' (not a valid number)" << std::endl;
                        parse_error = true;
                        break;
                    } catch (const std::out_of_range& e) {
                        std::cerr << "Error: PID value '" << pidStr << "' is out of range" << std::endl;
                        parse_error = true;
                        break;
                    }
                }

                if (parse_error) {
                    return -1;
                }

                std::cout << "[INFO] Filtering PIDs: ";
                for (size_t j = 0; j < analyzer.filterPids.size(); j++) {
                    if (j > 0) std::cout << ", ";
                    std::cout << analyzer.filterPids[j];
                }
                std::cout << std::endl;
            } else {
                std::cerr << "Error: --filter-pids requires a PID list argument" << std::endl;
                return -1;
            }
        } else if (arg == "--dump-pids" || arg == "-d") {
            analyzer.enablePidDump = true;
        } else if (arg == "--dump-all") {
            analyzer.enableDumpAll = true;
        } else if (arg == "--gs") {
            analyzer.useGstSink = true;
        } else if (arg == "--enable-crc-validation") {
            analyzer.enableCrcValidation = true;
        } else if (arg == "--output-dir" || arg == "-o") {
            if (i + 1 < argc) {
                analyzer.outputDir = argv[++i];
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " <input.ts> [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --filter-pids <pids> Filter only specified PIDs (comma-separated, e.g., 0,100,256)" << std::endl;
            std::cout << "  --dump-pids, -d    Enable PID dumping (saves to pid_*.ts files)" << std::endl;
            std::cout << "  --dump-all         Enable dumping all packets to dump_all.ts file" << std::endl;
            std::cout << "  --gs               Use GStreamer filesink (default: C++ file I/O)" << std::endl;
            std::cout << "  --output-dir, -o   Output directory for PID dumps (default: pid_dumps)" << std::endl;
            std::cout << "  --enable-crc-validation  Enable CRC-32 validation for PSI/SI tables (default: off)" << std::endl;
            std::cout << "  --help, -h         Show this help message" << std::endl;
            std::cout << std::endl;
            std::cout << "Notes:" << std::endl;
            std::cout << "  - --filter-pids: Only output packets for specified PIDs (comma-separated)" << std::endl;
            std::cout << "                    PIDs can be in decimal or hex format (0x prefix for hex)" << std::endl;
            std::cout << "  - --dump-pids: Creates separate files for each PID (pid_*.ts)" << std::endl;
            std::cout << "  - --dump-all: Creates a single file with all packets (dump_all.ts)" << std::endl;
            std::cout << "  - Default: --dump-pids uses appsink + C++ file I/O" << std::endl;
            std::cout << "  - With --gs: --dump-pids uses GStreamer filesink instead" << std::endl;
            std::cout << "  - --enable-crc-validation: Validates CRC-32 in PSI/SI tables during 3-sync and packet processing" << std::endl;
            return 0;
        } else if (arg.substr(0, 2) == "--") {
            std::cerr << "Unknown option: " << arg << std::endl;
            return -1;
        } else {
            if (!inputFile.empty()) {
                std::cerr << "Error: Multiple input files specified" << std::endl;
                return -1;
            }
            inputFile = arg;
        }
    }

    if (inputFile.empty()) {
        std::cerr << "Error: No input file specified" << std::endl;
        return -1;
    }

    // Check if tsfilter is available
    GstElement* test = gst_element_factory_make("tsfilter", nullptr);
    if (!test) {
        std::cerr << "Error: tsfilter element not available!" << std::endl;
        std::cerr << "Please install the tsfilter plugin." << std::endl;
        return -1;
    }
    gst_object_unref(test);

    std::cout << "[INFO] Using tsfilter with new-pid signal" << std::endl;
    if (analyzer.enablePidDump) {
        std::cout << "[INFO] PID dumping enabled - output: " << analyzer.outputDir << "/pid_*.ts" << std::endl;
        std::cout << "[INFO] Using " << (analyzer.useGstSink ? "GStreamer filesink" : "C++ file I/O") << " for output" << std::endl;
    }
    if (analyzer.enableDumpAll) {
        std::cout << "[INFO] Dump all packets enabled - output: " << analyzer.outputDir << "/dump_all.ts" << std::endl;
    }

    // Build pipeline using tsfilter
    // Include dump pad link if --dump-all is specified
    std::string pipelineStr = "filesrc location=" + inputFile + " ! tsfilter name=filter";
    if (analyzer.enableCrcValidation) {
        pipelineStr += " enable-crc-validation=true";
    }
    if (analyzer.enableDumpAll) {
        // Enable dump pad and link it to a filesink
        std::string dumpFile = analyzer.outputDir + "/dump_all.ts";
        pipelineStr += " enable-dump=true";
        pipelineStr += " filter.src ! queue ! appsink name=rawsink emit-signals=true sync=false";
        pipelineStr += " filter.dump ! queue ! filesink location=" + dumpFile + " sync=false";
    } else {
        pipelineStr += " filter.src ! queue ! appsink name=rawsink emit-signals=true sync=false";
    }

    analyzer.pipeline = gst_parse_launch(pipelineStr.c_str(), nullptr);
    if (!analyzer.pipeline) {
        std::cerr << "Failed to create GStreamer pipeline" << std::endl;
        return -1;
    }

    // Get tsfilter element
    analyzer.tsfilter = gst_bin_get_by_name(GST_BIN(analyzer.pipeline), "filter");
    if (!analyzer.tsfilter) {
        std::cerr << "Failed to get 'tsfilter' element from pipeline" << std::endl;
        gst_object_unref(analyzer.pipeline);
        return -1;
    }

    // Set filter-pids property if specified
    if (!analyzer.filterPids.empty()) {
        GArray* pids = g_array_new(FALSE, FALSE, sizeof(guint16));
        for (uint16_t pid : analyzer.filterPids) {
            g_array_append_val(pids, pid);
        }
        g_object_set(analyzer.tsfilter, "filter-pids", pids, NULL);
        g_array_unref(pids);
    }

    // Enable statistics collection
    g_object_set(analyzer.tsfilter, "enable-stats", TRUE, NULL);
    std::cout << "[INFO] Statistics collection enabled" << std::endl;

    // Connect to signals
    g_signal_connect(analyzer.tsfilter, "new-pid", G_CALLBACK(TSAnalyzer::onNewPID), &analyzer);
    g_signal_connect(analyzer.tsfilter, "bad-packet", G_CALLBACK(TSAnalyzer::onBadPacket), &analyzer);
    if (analyzer.enableCrcValidation) {
        g_signal_connect(analyzer.tsfilter, "crc-error", G_CALLBACK(TSAnalyzer::onCrcError), &analyzer);
    }

    // Get appsink for filtered output
    GstElement* rawsink = gst_bin_get_by_name(GST_BIN(analyzer.pipeline), "rawsink");
    if (!rawsink) {
        std::cerr << "Failed to get 'rawsink' element from pipeline" << std::endl;
        gst_object_unref(analyzer.tsfilter);
        gst_object_unref(analyzer.pipeline);
        return -1;
    }
    g_signal_connect(rawsink, "new-sample", G_CALLBACK(on_new_sample), &analyzer);

    // Create output directory if PID dumping or dump-all is enabled
    if (analyzer.enablePidDump || analyzer.enableDumpAll) {
        std::error_code ec;
        std::filesystem::create_directories(analyzer.outputDir, ec);
        if (ec) {
            std::cerr << "[ERROR] Failed to create output directory: " << analyzer.outputDir << std::endl;
            gst_object_unref(analyzer.pipeline);
            return -1;
        }
        std::cout << "[INFO] Created output directory: " << analyzer.outputDir << std::endl;
    }

    GstBus* bus = gst_element_get_bus(analyzer.pipeline);
    gst_bus_add_watch(bus, bus_callback, nullptr);

    std::cout << "[INFO] Starting pipeline..." << std::endl;
    gst_element_set_state(analyzer.pipeline, GST_STATE_PLAYING);

    // Give the pipeline a moment to start processing
    g_usleep(100000);  // 100ms

    // Now create dump branches for already-discovered PIDs
    // (Wait a bit more for new-pid signals to be processed)
    g_usleep(200000);  // 200ms

    // If PID dumping is enabled, we'll create dump branches as PIDs are discovered
    // For now, the new-pid signal will trigger creation

    g_timeout_add_seconds(1, periodic_status_update, &analyzer);

    main_loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(main_loop);

    // Cleanup
    g_main_loop_unref(main_loop);
    main_loop = nullptr;

    gst_element_set_state(analyzer.pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(rawsink);
    gst_object_unref(analyzer.tsfilter);
    gst_object_unref(analyzer.pipeline);

    return 0;
}
