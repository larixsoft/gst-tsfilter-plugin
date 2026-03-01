#!/usr/bin/env python3
import subprocess
import os
import sys

# Get the project root directory (parent of test directory)
script_dir = os.path.dirname(os.path.abspath(__file__))
project_root = os.path.dirname(script_dir)

os.chdir(project_root)
env = os.environ.copy()
env['GST_PLUGIN_PATH'] = os.path.join(project_root, 'tsfilter', 'build')

# Run the test script with timeout
print(f"Starting test...", flush=True)
proc = subprocess.Popen(
    ['bash', os.path.join('test', 'test_all_files.sh')],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    env=env
)

# Look for test-142.ts in the output
found = False
try:
    for line in proc.stdout:
        if 'test-142.ts' in line:
            print(line, end='', flush=True)
            found = True
        if found and ('✗' in line or '✓' in line) and 'FAILED' in line:
            print(line, end='', flush=True)
            proc.terminate()
            break
    proc.wait(timeout=10)
except subprocess.TimeoutExpired:
    proc.kill()
    print("\nTest timed out", flush=True)
    sys.exit(1)

print(f"\nDone", flush=True)
