#!/usr/bin/env python3
import sys

def extract_pids(filename):
    pids = {}
    with open(filename, 'rb') as f:
        packet_num = 0
        while True:
            # Read TS packet header (first 3 bytes for sync + PID)
            header = f.read(3)
            if len(header) < 3:
                break
            
            # Check for sync byte (0x47)
            if header[0] != 0x47:
                # Skip to next sync byte
                f.seek(-2, 1)
                continue
            
            # Extract PID (bits 11-0 of bytes 1-2)
            pid = ((header[1] & 0x1F) << 8) | header[2]
            
            if pid not in pids:
                pids[pid] = 0
            pids[pid] += 1
            packet_num += 1
            
            # Skip rest of packet
            f.seek(185, 1)
    
    return pids, packet_num

if __name__ == '__main__':
    filename = '/tmp/pid_dump.ts'
    if len(sys.argv) > 1:
        filename = sys.argv[1]

    pids, total = extract_pids(filename)
    
    print(f"=== HLS Stream PID Dump Analysis ===")
    print(f"Total packets processed: {total}")
    print(f"\nPIDs found ({len(pids)} unique PIDs):")
    print("-" * 50)
    
    for pid in sorted(pids.keys()):
        count = pids[pid]
        pct = (count / total) * 100
        print(f"  PID {pid:4d} (0x{pid:04X}): {count:6d} packets ({pct:5.2f}%)")
