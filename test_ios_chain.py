#!/usr/bin/env python3
"""Boot Win98 VM with fixed VxD and check if IOS chain test passes."""

import subprocess, time, os, socket, json

DISK = "/tmp/win98vm/win98.img"
ISO = "/tmp/win98vm/test_cd.iso"
QMP_SOCK = "/tmp/win98vm/qmp_test.sock"
DEBUG_LOG = "/tmp/win98vm/debug_test.log"

subprocess.run(["pkill", "-f", "qemu-system-i386.*win98"], stderr=subprocess.DEVNULL)
time.sleep(1)
for f in [QMP_SOCK, DEBUG_LOG]:
    try: os.unlink(f)
    except: pass

print("Booting VM...")
proc = subprocess.Popen([
    "qemu-system-i386", "-hda", DISK, "-m", "256",
    "-drive", f"file={ISO},media=cdrom,if=ide,index=2",
    "-debugcon", f"file:{DEBUG_LOG}",
    "-display", "none",
    "-qmp", f"unix:{QMP_SOCK},server,nowait",
], stderr=subprocess.DEVNULL)

time.sleep(2)
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(QMP_SOCK)
sock.settimeout(5)
try: sock.recv(4096)
except: pass
sock.send(json.dumps({"execute": "qmp_capabilities"}).encode() + b"\r\n")
try: sock.recv(4096)
except: pass

# Send keystrokes to get past Safe Mode prompt
for i in range(20):
    time.sleep(2)
    sock.send(json.dumps({"execute": "send-key",
        "arguments": {"keys": [{"type": "qcode", "data": "1"}]}}).encode() + b"\r\n")
    try: sock.recv(4096)
    except: pass
    time.sleep(0.1)
    sock.send(json.dumps({"execute": "send-key",
        "arguments": {"keys": [{"type": "qcode", "data": "ret"}]}}).encode() + b"\r\n")
    try: sock.recv(4096)
    except: pass

# Wait for boot
print("Waiting for boot...")
for i in range(120):
    time.sleep(1)
    try:
        size = os.path.getsize(DEBUG_LOG)
        if i % 15 == 0:
            print(f"  {i}s: debug.log = {size} bytes")
        if size > 10000:
            print(f"  Boot detected at {i}s")
            time.sleep(45)  # settle: wait for desktop + system probing our device
            break
    except: pass

# Read debug log
print("\n" + "=" * 70)
print("DEBUG LOG OUTPUT")
print("=" * 70)
try:
    with open(DEBUG_LOG, "r", errors="replace") as f:
        dbg = f.read()
    # Show Phase 4 and Phase 5 sections
    lines = dbg.split("\n")
    in_phase = False
    for line in lines:
        if "PHASE 4" in line or "PHASE 5" in line or "Direct IOR" in line:
            in_phase = True
        if in_phase:
            print(line)
        if "CHAIN TEST" in line or "Skipping chain" in line or "Chain returned" in line or "handler not reached" in line:
            in_phase = False

    print("\n" + "=" * 70)
    print("FULL LOG (last 3000 chars)")
    print("=" * 70)
    print(dbg[-3000:])

    # Check for key markers
    print("\n--- Key Markers ---")
    markers = [
        ("IOS_Register",    "IOS_Register="),
        ("GOT ILB",         "GOT ILB"),
        ("CD-ROM DCB",      "FOUND CD-ROM DCB"),
        ("Handler insert",  "HANDLER INSERTED"),
        ("D: mounted",      "D: MOUNTED"),
        ("IOR received",    "IOR[READ]"),
        ("ATAPI read OK",   "ATAPI READ fail"),  # inverse: absence = success
        ("PVD via chain",   "ISO 9660 PVD VIA IOS CHAIN"),
    ]
    for label, marker in markers:
        found = marker in dbg
        status = "YES" if found else "no"
        if label == "ATAPI read OK":
            status = "no errors" if not found else "ERRORS"
        print(f"  {label}: {status}")

    if "HANDLER INSERTED" in dbg and "D: MOUNTED" in dbg:
        print("\n*** SUCCESS: Handler in NECATAPI chain + D: mounted ***")
        if "IOR[READ]" in dbg:
            print("*** IOR READ requests are flowing through our handler ***")
    elif "FOUND CD-ROM DCB" in dbg:
        print("\n*** PROGRESS: Found NECATAPI's DCB ***")
    elif "GOT ILB" in dbg:
        print("\n*** PROGRESS: Got ILB from IOS_Register ***")
    elif "IOS_Register FAILED" in dbg:
        print("\n*** BLOCKED: IOS_Register failed ***")
    else:
        print("\n*** Check debug log for details ***")

except Exception as e:
    print(f"Error reading log: {e}")

# Shutdown
sock.send(json.dumps({"execute": "quit"}).encode() + b"\r\n")
sock.close()
proc.wait(timeout=10)
print("\nVM stopped.")
