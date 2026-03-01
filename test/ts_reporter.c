/*
 * TS File Reporter - Analyzes MPEG-TS files and generates detailed reports
 *
 * For each TS file, reports:
 * - Input file size and packet count
 * - Bad packets (bytes with bad sync alignment)
 * - Discarded partial packets at beginning of file
 * - Discarded partial packets at end of file
 * - Valid packets found
 * - PID distribution
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libgen.h>

#define TS_SYNC_BYTE 0x47
#define MAX_PACKET_SIZE 204
#define MAX_PID 8192

typedef struct {
    const char *filename;
    size_t file_size;
    int packet_size;
    int total_slots;       /* File size / packet_size */
    int first_sync_offset; /* Bytes before first valid packet */
    int valid_packets;
    int bad_sync_bytes;    /* Bytes discarded during scanning */
    int trailing_bytes;    /* Bytes after last complete packet */
    int pid_counts[MAX_PID];
    int pid_count;         /* Number of unique PIDs found */
} TSReport;

/* Detect packet size from file */
static int detect_packet_size(const uint8_t *data, size_t size) {
    int sizes[] = {188, 192, 204, 208};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < num_sizes; i++) {
        int ps = sizes[i];
        if (size < 3 * ps) continue;

        /* Check for 3 consecutive sync bytes at packet_size intervals */
        int valid_count = 0;
        for (int offset = 0; offset < 3 * ps && offset + 3 * ps <= size; offset += ps) {
            if (data[offset] == TS_SYNC_BYTE &&
                data[offset + ps] == TS_SYNC_BYTE &&
                data[offset + 2 * ps] == TS_SYNC_BYTE) {
                valid_count++;
            }
        }

        if (valid_count >= 2) {
            return ps;
        }
    }

    return 188; /* Default */
}

/* Extract PID from TS packet */
static inline uint16_t extract_pid(const uint8_t *packet) {
    return ((packet[1] & 0x1F) << 8) | packet[2];
}

/* Analyze a single TS file */
static TSReport analyze_ts_file(const char *filepath) {
    TSReport report = {0};
    report.filename = filepath;
    report.packet_size = 188; /* Default */

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", filepath);
        return report;
    }

    fseek(f, 0, SEEK_END);
    report.file_size = ftell(f);
    rewind(f);

    uint8_t *data = malloc(report.file_size);
    if (!data) {
        fclose(f);
        return report;
    }

    fread(data, 1, report.file_size, f);
    fclose(f);

    /* Detect packet size */
    if (report.file_size >= 3 * 188) {
        report.packet_size = detect_packet_size(data, report.file_size);
    }
    report.total_slots = report.file_size / report.packet_size;

    /* Scan for packets using 3-consecutive sync method */
    int offset = 0;
    size_t remaining = report.file_size;
    int first_valid_found = 0;

    while (offset < (int)report.file_size) {
        /* Need at least 3 packets to validate sync */
        if (offset + 3 * report.packet_size > report.file_size) {
            /* Trailing bytes that don't form a complete packet */
            report.trailing_bytes = report.file_size - offset;
            break;
        }

        if (data[offset] == TS_SYNC_BYTE &&
            data[offset + report.packet_size] == TS_SYNC_BYTE &&
            data[offset + 2 * report.packet_size] == TS_SYNC_BYTE) {

            /* Found valid packet boundary */
            if (!first_valid_found) {
                report.first_sync_offset = offset;
                first_valid_found = 1;
            }

            /* Extract and count PID */
            uint16_t pid = extract_pid(data + offset);
            if (pid < MAX_PID) {
                if (report.pid_counts[pid] == 0) {
                    report.pid_count++;
                }
                report.pid_counts[pid]++;
            }

            report.valid_packets++;
            offset += report.packet_size;
        } else {
            /* Bad sync - scan forward */
            int found = 0;
            for (int scan = offset + 1; scan + 3 * report.packet_size <= report.file_size; scan++) {
                if (data[scan] == TS_SYNC_BYTE &&
                    data[scan + report.packet_size] == TS_SYNC_BYTE &&
                    data[scan + 2 * report.packet_size] == TS_SYNC_BYTE) {
                    report.bad_sync_bytes += (scan - offset);
                    offset = scan;
                    found = 1;
                    break;
                }
            }

            if (!found) {
                /* No more valid packets found */
                report.bad_sync_bytes += (report.file_size - offset);
                report.trailing_bytes = report.file_size - offset;
                break;
            }
        }
    }

    /* If we never found a valid sync, all bytes are bad */
    if (!first_valid_found && report.file_size > 0) {
        report.bad_sync_bytes = report.file_size;
        report.first_sync_offset = report.file_size;
    }

    free(data);
    return report;
}

/* Print report for a single file */
static void print_report(const TSReport *report) {
    printf("\n");
    printf("========================================\n");
    printf("File: %s\n", report->filename);
    printf("========================================\n");
    printf("File Size:        %zu bytes\n", report->file_size);
    printf("Packet Size:      %d bytes\n", report->packet_size);
    printf("Total Slots:      %d packets\n", report->total_slots);
    printf("  Input Packets:  %d\n", report->total_slots);
    printf("\n");
    printf("--- Packet Analysis ---\n");
    printf("Discarded at Beginning: %d bytes (%.2f packets)\n",
           report->first_sync_offset,
           (double)report->first_sync_offset / report->packet_size);
    printf("Valid Packets Found:    %d packets\n", report->valid_packets);
    printf("Bad Sync Bytes:         %d bytes\n", report->bad_sync_bytes);
    printf("Trailing Bytes:         %d bytes (%.2f packets)\n",
           report->trailing_bytes,
           (double)report->trailing_bytes / report->packet_size);
    printf("\n");
    printf("--- Summary ---\n");
    printf("Total Bytes Accounted For:\n");
    printf("  Beginning discard:  %6d bytes\n", report->first_sync_offset);
    printf("  Valid packets:      %6d packets x %d = %zu bytes\n",
           report->valid_packets, report->packet_size,
           (size_t)report->valid_packets * report->packet_size);
    printf("  Bad sync bytes:     %6d bytes\n", report->bad_sync_bytes);
    printf("  Trailing bytes:     %6d bytes\n", report->trailing_bytes);
    printf("  Total:              %6zu bytes (expected: %zu)\n",
           report->first_sync_offset +
           (size_t)report->valid_packets * report->packet_size +
           report->bad_sync_bytes + report->trailing_bytes,
           report->file_size);
    printf("\n");
    printf("Unique PIDs Found:  %d\n", report->pid_count);
}

/* Print compact CSV-style report */
static void print_compact_report(const TSReport *report, int file_num, int total_files) {
    const char *basename = strrchr(report->filename, '/');
    basename = basename ? basename + 1 : report->filename;

    printf("[%3d/%3d] %-40s Size:%8zu  PS:%3d  In:%5d  Bad:%4d  Trail:%3d  Valid:%5d  PIDs:%3d\n",
           file_num, total_files, basename, report->file_size, report->packet_size,
           report->total_slots, report->bad_sync_bytes, report->trailing_bytes,
           report->valid_packets, report->pid_count);
}

/* Find all .ts files in directory recursively */
static char **find_ts_files(const char *dirpath, int *count) {
    char **files = NULL;
    *count = 0;
    int capacity = 0;

    DIR *dir = opendir(dirpath);
    if (!dir) {
        perror("opendir");
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dirpath, entry->d_name);

        struct stat statbuf;
        if (stat(path, &statbuf) == -1) {
            continue;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            /* Recurse into subdirectory */
            int sub_count;
            char **sub_files = find_ts_files(path, &sub_count);
            if (sub_files) {
                for (int i = 0; i < sub_count; i++) {
                    if (*count >= capacity) {
                        capacity = capacity == 0 ? 256 : capacity * 2;
                        files = realloc(files, capacity * sizeof(char *));
                    }
                    files[(*count)++] = sub_files[i];
                }
                free(sub_files);
            }
        } else if (S_ISREG(statbuf.st_mode)) {
            /* Check if file ends with .ts or .m2ts */
            size_t len = strlen(entry->d_name);
            if ((len > 3 && strcmp(entry->d_name + len - 3, ".ts") == 0) ||
                (len > 5 && strcmp(entry->d_name + len - 5, ".m2ts") == 0)) {
                if (*count >= capacity) {
                    capacity = capacity == 0 ? 256 : capacity * 2;
                    files = realloc(files, capacity * sizeof(char *));
                }
                files[*count] = strdup(path);
                (*count)++;
            }
        }
    }

    closedir(dir);
    return files;
}

int main(int argc, char **argv) {
    const char *test_dir = "/home/gwang1/develop/tsduck-test";
    int verbose = 0;
    int specific_file = 0;

    if (argc > 1) {
        if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--verbose") == 0) {
            verbose = 1;
        } else {
            test_dir = argv[1];
            /* Check if path is a directory or file */
            struct stat statbuf;
            if (stat(test_dir, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
                specific_file = 0;  /* Directory mode */
            } else {
                specific_file = 1;  /* File mode */
            }
        }
    }

    if (argc > 2 && (strcmp(argv[2], "-v") == 0 || strcmp(argv[2], "--verbose") == 0)) {
        verbose = 1;
    }

    char **files;
    int file_count;

    if (specific_file) {
        /* Single file mode */
        files = malloc(sizeof(char *));
        files[0] = strdup(test_dir);
        file_count = 1;
        test_dir = argv[1];
    } else {
        /* Directory mode */
        printf("Scanning directory: %s\n", test_dir);
        files = find_ts_files(test_dir, &file_count);
        if (!files || file_count == 0) {
            printf("No .ts files found\n");
            return 1;
        }
    }

    printf("\n");
    printf("==========================================\n");
    printf("TS File Analysis Report\n");
    printf("==========================================\n");
    printf("Files to analyze: %d\n", file_count);
    printf("==========================================\n");

    int total_valid_packets = 0;
    int total_bad_bytes = 0;
    size_t total_size = 0;

    for (int i = 0; i < file_count; i++) {
        TSReport report = analyze_ts_file(files[i]);

        if (verbose) {
            print_report(&report);
        } else {
            print_compact_report(&report, i + 1, file_count);
        }

        total_valid_packets += report.valid_packets;
        total_bad_bytes += report.bad_sync_bytes;
        total_size += report.file_size;

        free(files[i]);
    }

    free(files);

    printf("\n");
    printf("==========================================\n");
    printf("Summary Statistics\n");
    printf("==========================================\n");
    printf("Total Files Analyzed:      %d\n", file_count);
    printf("Total Data Processed:      %.2f MB\n", total_size / (1024.0 * 1024.0));
    printf("Total Valid Packets:       %d\n", total_valid_packets);
    printf("Total Bad Bytes:           %d (%.2f KB)\n", total_bad_bytes, total_bad_bytes / 1024.0);
    printf("==========================================\n");

    return 0;
}
