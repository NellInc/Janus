#!/usr/bin/env python3
"""Dismiss Win98 first-boot Hardware Wizard permanently via QEMU monitor socket.
Adds screendumps before/after shutdown for visual evidence."""
import socket
import time
import sys
import os
import subprocess

MONITOR_SOCK = "/tmp/qemu-firstboot-perm.sock"
DEBUGCON_LOG = "/tmp/nt5dbg_firstboot_perm.log"
DISK = os.path.expanduser("~/Documents/VMs/win98vm/win98.img")
TEST_HDD = "/tmp/win98vm/test_hd.img"
SHOT_PRE = "/tmp/firstboot_pre_shutdown.ppm"
SHOT_PNG = "/tmp/firstboot_pre_shutdown.png"
VERIF_SHOT_PPM = "/tmp/firstboot_verif.ppm"
VERIF_SHOT_PNG = "/tmp/firstboot_verif.png"
VERIF_LOG = "/tmp/nt5dbg_verif.log"
REPO = os.path.expanduser("~/Documents/GitHub/nt5-9x-driver-backport")


def send_monitor(sock, cmd, wait=0.3):
    try:
        sock.sendall((cmd + "\n").encode())
    except (BrokenPipeError, OSError) as e:
        print(f"  WARNING: monitor send failed: {e}")
        return ""
    time.sleep(wait)
    try:
        return sock.recv(4096).decode(errors='replace')
    except Exception:
        return ""


def send_key(sock, key, count=1, delay=0.5):
    for _ in range(count):
        send_monitor(sock, f"sendkey {key}")
        time.sleep(delay)


def connect_monitor(path, retries=10):
    for i in range(retries):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            s.settimeout(2.0)
            return s
        except Exception:
            time.sleep(1)
    return None


def ppm_to_png(ppm, png):
    subprocess.run(["sips", "-s", "format", "png", ppm, "--out", png],
                   capture_output=True)


def log_size(path):
    try:
        return os.path.getsize(path)
    except FileNotFoundError:
        return 0


def main():
    # Kill any lingering QEMU
    subprocess.run(["pkill", "-9", "-f", "qemu-system-i386"], capture_output=True)
    time.sleep(3)
    # Verify killed
    r = subprocess.run(["pgrep", "-f", "qemu-system-i386"], capture_output=True)
    if r.returncode == 0:
        print("WARNING: QEMU still alive, waiting more...")
        time.sleep(5)

    for path in [MONITOR_SOCK, DEBUGCON_LOG, SHOT_PRE, SHOT_PNG]:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass

    # No extra HDD — stripped to avoid crash during first-boot wizard dismissal
    qemu_args = [
        "qemu-system-i386", "-m", "256", "-M", "pc", "-cpu", "pentium2",
        "-drive", f"file={DISK},format=raw",
        "-boot", "c", "-vga", "std", "-rtc", "base=localtime",
        "-display", "none",
        "-debugcon", f"file:{DEBUGCON_LOG}",
        "-monitor", f"unix:{MONITOR_SOCK},server,nowait",
    ]

    print("Starting QEMU...")
    proc = subprocess.Popen(qemu_args)
    time.sleep(5)
    if proc.poll() is not None:
        print(f"FATAL: QEMU exited immediately (code={proc.returncode})")
        sys.exit(1)
    print(f"QEMU running (PID={proc.pid})")

    sock = connect_monitor(MONITOR_SOCK)
    if sock is None:
        print("FATAL: Cannot connect to monitor socket")
        sys.exit(1)

    send_monitor(sock, "")
    print("Connected to QEMU monitor")

    # Phase 1: Wait for VxD to load (debug log grows)
    print("Phase 1: Waiting for VxD load (up to 120s)...")
    loaded_at = None
    for i in range(120):
        time.sleep(1)
        if proc.poll() is not None:
            print(f"FATAL: QEMU died during Phase 1 at T+{i+1}s (code={proc.returncode})")
            sys.exit(1)
        size = log_size(DEBUGCON_LOG)
        if size > 5000:
            print(f"  VxD loaded! Log size: {size} bytes at T+{i+1}s")
            loaded_at = i + 1
            break
        if i % 10 == 9:
            print(f"  T+{i+1}s: log={size}")
    else:
        size = log_size(DEBUGCON_LOG)
        print(f"  Phase 1 timeout. Log size: {size}")
        if size < 500:
            print("FATAL: Image not booting at all")
            send_monitor(sock, "quit")
            sys.exit(1)
        print("  Continuing anyway...")

    # Phase 2: Wait for GUI / Hardware Wizard (120s after VxD init)
    print("Phase 2: Waiting 120s for GUI/Hardware Wizard...")
    for i in range(120):
        time.sleep(1)
        if proc.poll() is not None:
            print(f"FATAL: QEMU died during Phase 2 at T+{i+1}s (code={proc.returncode})")
            sys.exit(1)
        if i % 30 == 29:
            sz = log_size(DEBUGCON_LOG)
            print(f"  T+{i+1}s: log={sz}, QEMU alive")

    # Phase 3: Dismiss wizard - ESC + Enter shotgun
    print("Phase 3: Dismissing Hardware Wizard...")
    # Round 1: ESC barrage
    send_key(sock, "esc", count=5, delay=2)
    send_key(sock, "tab", count=3, delay=0.5)
    send_key(sock, "ret", count=1, delay=1)
    time.sleep(3)
    # Round 2
    send_key(sock, "esc", count=3, delay=2)
    send_key(sock, "ret", count=2, delay=2)
    time.sleep(5)
    # Round 3
    send_key(sock, "esc", count=3, delay=2)
    send_key(sock, "ret", count=2, delay=2)
    time.sleep(5)

    # Phase 4: Wait for desktop to stabilize (90s)
    print("Phase 4: Waiting 90s for desktop stabilization...")
    prev = log_size(DEBUGCON_LOG)
    for i in range(90):
        time.sleep(1)
        if proc.poll() is not None:
            print(f"WARNING: QEMU died during Phase 4 at T+{i+1}s")
            break
        cur = log_size(DEBUGCON_LOG)
        if cur > prev + 100:
            print(f"  New debug output at T+{i+1}s: {prev} -> {cur}")
            prev = cur

    # Screenshot before shutdown
    print("Screendump (pre-shutdown)...")
    send_monitor(sock, f"screendump {SHOT_PRE}", wait=1.0)
    time.sleep(1)
    ppm_to_png(SHOT_PRE, SHOT_PNG)
    shot_exists = os.path.exists(SHOT_PNG)
    print(f"  Screenshot: {SHOT_PNG} ({'exists' if shot_exists else 'MISSING'})")

    final_size = log_size(DEBUGCON_LOG)
    print(f"\nFinal debug log size: {final_size}")

    # Phase 5: Graceful shutdown
    print("Phase 5: Shutting down...")
    send_monitor(sock, "system_powerdown", wait=1.0)
    time.sleep(20)
    send_monitor(sock, "quit")
    sock.close()
    time.sleep(3)

    # --- Re-deploy to clear FAT dirty flags ---
    print("\nRe-deploying VxD (clears FAT dirty flags)...")
    vxd = os.path.join(REPO, "binaries", "NT5FIXED.VXD")
    result = subprocess.run(
        ["python3", os.path.join(REPO, "src", "deploy_sysini.py"), vxd],
        capture_output=True, text=True
    )
    print(result.stdout[-2000:] if result.stdout else "(no stdout)")
    if result.returncode != 0:
        print(f"  ERROR: {result.stderr[-500:]}")

    # --- Verification boot (90s headless) ---
    print("\n=== VERIFICATION BOOT (90s) ===")
    subprocess.run(["pkill", "-f", "qemu-system-i386"], capture_output=True)
    time.sleep(2)

    VERIF_SOCK = "/tmp/qemu-verif.sock"
    for path in [VERIF_SOCK, VERIF_LOG, VERIF_SHOT_PPM, VERIF_SHOT_PNG]:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass

    verif_args = [
        "qemu-system-i386", "-m", "256", "-M", "pc", "-cpu", "pentium2",
        "-drive", f"file={DISK},format=raw",
        "-boot", "c", "-vga", "std", "-rtc", "base=localtime",
        "-display", "none",
        "-debugcon", f"file:{VERIF_LOG}",
        "-monitor", f"unix:{VERIF_SOCK},server,nowait",
    ]

    subprocess.Popen(verif_args)
    time.sleep(3)

    vsock = connect_monitor(VERIF_SOCK)
    if vsock is None:
        print("ERROR: Cannot connect to verification monitor")
    else:
        send_monitor(vsock, "")
        print("Waiting 90s for verification boot...")
        for i in range(90):
            time.sleep(1)
            if i % 15 == 14:
                vsize = log_size(VERIF_LOG)
                print(f"  T+{i+1}s: verif log={vsize}")

        # Screenshot
        send_monitor(vsock, f"screendump {VERIF_SHOT_PPM}", wait=1.0)
        time.sleep(1)
        ppm_to_png(VERIF_SHOT_PPM, VERIF_SHOT_PNG)
        verif_size = log_size(VERIF_LOG)
        print(f"\nVerification boot debug log: {verif_size} bytes")
        print(f"Post-first-boot (>37000): {'YES' if verif_size > 37000 else 'NO'} ({verif_size})")
        vshot = os.path.exists(VERIF_SHOT_PNG)
        print(f"Verification screenshot: {VERIF_SHOT_PNG} ({'exists' if vshot else 'MISSING'})")

        send_monitor(vsock, "quit")
        vsock.close()

    print("\n=== SUMMARY ===")
    print(f"Pre-shutdown screenshot: {SHOT_PNG}")
    print(f"Verification screenshot: {VERIF_SHOT_PNG}")
    print(f"First-boot debug log: {final_size} bytes")
    print(f"Verif log: {log_size(VERIF_LOG)} bytes")


if __name__ == "__main__":
    main()
