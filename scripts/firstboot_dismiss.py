#!/usr/bin/env python3
"""Dismiss Win98 first-boot Hardware Wizard via QEMU monitor socket."""
import socket
import time
import sys
import os
import subprocess

MONITOR_SOCK = "/tmp/qemu-firstboot-mon.sock"
DEBUGCON_LOG = "/tmp/nt5dbg_firstboot2.log"
DISK = os.path.expanduser("~/Documents/VMs/win98vm/win98.img")
TEST_HDD = "/tmp/win98vm/test_hd.img"

def send_monitor(sock, cmd):
    sock.sendall((cmd + "\n").encode())
    time.sleep(0.3)
    try:
        return sock.recv(4096).decode(errors='replace')
    except:
        return ""

def send_key(sock, key, count=1, delay=0.5):
    for _ in range(count):
        send_monitor(sock, f"sendkey {key}")
        time.sleep(delay)

def main():
    subprocess.run(["pkill", "-f", "qemu-system-i386"],
                   capture_output=True)
    time.sleep(2)

    for path in [MONITOR_SOCK, DEBUGCON_LOG]:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass

    qemu_args = [
        "qemu-system-i386", "-m", "256", "-M", "pc", "-cpu", "pentium2",
        "-drive", f"file={DISK},format=raw",
        "-boot", "c", "-vga", "std", "-rtc", "base=localtime",
        "-display", "none",
        "-debugcon", f"file:{DEBUGCON_LOG}",
        "-drive", f"file={TEST_HDD},if=ide,index=3,format=raw",
        "-monitor", f"unix:{MONITOR_SOCK},server,nowait",
    ]
    subprocess.Popen(qemu_args)
    time.sleep(3)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(MONITOR_SOCK)
        sock.settimeout(2.0)
    except Exception as e:
        print(f"FATAL: Cannot connect to monitor: {e}")
        sys.exit(1)

    send_monitor(sock, "")
    print("Connected to QEMU monitor")

    # Phase 1: Wait for VxD to load (debug log grows)
    print("Phase 1: Waiting for VxD load (up to 90s)...")
    for i in range(90):
        time.sleep(1)
        try:
            size = os.path.getsize(DEBUGCON_LOG)
        except FileNotFoundError:
            size = 0
        if size > 30000:
            print(f"  VxD loaded! Log size: {size} bytes at T+{i+1}s")
            break
    else:
        try:
            size = os.path.getsize(DEBUGCON_LOG)
        except FileNotFoundError:
            size = 0
        print(f"  Timeout. Log size: {size}")
        if size < 1000:
            print("FATAL: Image not booting")
            send_monitor(sock, "quit")
            sys.exit(1)

    # Phase 2: Wait for GUI to load (Hardware Wizard ~30-60s after VxD init)
    print("Phase 2: Waiting 90s for GUI/Hardware Wizard to appear...")
    time.sleep(90)

    # Phase 3: Dismiss the wizard with ESC + Enter patterns
    print("Phase 3: Dismissing Hardware Wizard...")
    # Round 1: ESC barrage
    send_key(sock, "esc", count=5, delay=2)
    send_key(sock, "tab", count=3, delay=0.5)
    send_key(sock, "ret", count=1, delay=1)
    time.sleep(3)
    # Round 2: more dismissal
    send_key(sock, "esc", count=3, delay=2)
    send_key(sock, "ret", count=2, delay=2)
    time.sleep(5)
    # Round 3: final cleanup
    send_key(sock, "esc", count=3, delay=2)
    send_key(sock, "ret", count=2, delay=2)

    # Phase 4: Wait for desktop + SREAD.EXE to run
    print("Phase 4: Waiting 120s for desktop + SREAD execution...")
    prev_size = 0
    try:
        prev_size = os.path.getsize(DEBUGCON_LOG)
    except FileNotFoundError:
        pass
    for i in range(120):
        time.sleep(1)
        try:
            cur_size = os.path.getsize(DEBUGCON_LOG)
        except FileNotFoundError:
            cur_size = 0
        if cur_size > prev_size + 100:
            print(f"  New debug output at T+{i+1}s! Size: {prev_size} -> {cur_size}")
            prev_size = cur_size

    try:
        final_size = os.path.getsize(DEBUGCON_LOG)
    except FileNotFoundError:
        final_size = 0
    print(f"\nFinal debug log size: {final_size}")
    print(f"Growth from VxD load: {final_size - 36293} bytes")

    # Phase 5: Graceful shutdown
    print("Phase 5: Shutting down...")
    send_monitor(sock, "system_powerdown")
    time.sleep(15)
    send_monitor(sock, "quit")
    sock.close()
    print(f"\nDone. Debug log: {DEBUGCON_LOG}")

if __name__ == "__main__":
    main()
