#!/usr/bin/env bash
set -euo pipefail
ART="$1"; NAME="$2"; EXE="$3"; OUTFILE="$4"; MAXWAIT="${5:-300}"
ROOT=/Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport
DISK=/Users/nellwatson/Documents/VMs/win98vm/win98.img
RUNDIR="$ART/$NAME"
mkdir -p "$RUNDIR/checkpoints" "$ART/guest-output"
LOG="$RUNDIR/run.log"; : > "$LOG"
pids=$(pgrep -f 'qemu-system-i386|launch_debug.sh|run_one.sh' || true); if [ -n "$pids" ]; then echo "$pids" | xargs kill -9 || true; sleep 1; fi
python3 "$ART/disk_ops.py" write windows "$EXE" "$ART/harness/$EXE" > "$RUNDIR/install.log" 2>&1
python3 "$ART/disk_ops.py" delete root "$OUTFILE" >> "$RUNDIR/install.log" 2>&1 || true
python3 "$ART/disk_ops.py" delete windows "$OUTFILE" >> "$RUNDIR/install.log" 2>&1 || true
python3 "$ART/disk_ops.py" startup_absent >> "$RUNDIR/install.log" 2>&1 || true
python3 "$ART/disk_ops.py" winini_run "C:\\WINDOWS\\$EXE" >> "$RUNDIR/install.log" 2>&1
python3 "$ART/disk_ops.py" clean_bpb >> "$RUNDIR/install.log" 2>&1 || true
python3 "$ART/disk_ops.py" read windows WIN.INI "$RUNDIR/WIN.INI.installed" >> "$RUNDIR/install.log" 2>&1 || true
python3 "$ART/disk_ops.py" read windows "$EXE" "$RUNDIR/$EXE.installed" >> "$RUNDIR/install.log" 2>&1 || true
shasum -a 256 "$ART/harness/$EXE" "$RUNDIR/$EXE.installed" "$RUNDIR/WIN.INI.installed" > "$RUNDIR/install.sha256" 2>/dev/null || true
cat > "$RUNDIR/hmp_unix.py" <<'PY'
import socket, sys, time
sock_path, cmd = sys.argv[1], sys.argv[2]
s=socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(2); s.connect(sock_path)
try:
    while True: s.recv(4096)
except Exception: pass
s.sendall((cmd+'\n').encode('ascii')); time.sleep(0.2)
out=bytearray()
try:
    while True:
        c=s.recv(4096)
        if not c: break
        out.extend(c)
except Exception: pass
s.close(); sys.stdout.buffer.write(out)
PY
MON="/tmp/nt5mon_${$}.sock"; DEBUG="/tmp/nt5dbg_${$}.log"; SERIAL="/tmp/nt5ser_${$}.log"
rm -f "$MON" "$DEBUG" "$SERIAL"
CDROM_ARGS=()
if [ -f /tmp/win98vm/test_cd.iso ]; then CDROM_ARGS=(-cdrom /tmp/win98vm/test_cd.iso); fi
HDD_ARGS=()
if [ -f /tmp/win98vm/test_hd.img ]; then
  if [ "${FORCE_IDE:-0}" = "1" ]; then
    # Force IDE secondary slave (for ATA direct I/O testing)
    HDD_ARGS=(-drive "file=/tmp/win98vm/test_hd.img,if=ide,index=3,format=raw")
  elif qemu-system-i386 -device help 2>&1 | grep -q lsi53c895a; then
    # SCSI test disk via LSI53C895A controller.
    HDD_ARGS=(
      -device lsi53c895a,id=scsi0
      -drive "id=scsihd0,file=/tmp/win98vm/test_hd.img,if=none,format=raw"
      -device "scsi-hd,drive=scsihd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0"
    )
  else
    # Fallback: IDE secondary slave
    HDD_ARGS=(-drive "file=/tmp/win98vm/test_hd.img,if=ide,index=3,format=raw")
  fi
fi
{
  echo "run_start=$(date -Iseconds)"; echo "artifact=$ART name=$NAME exe=$EXE outfile=$OUTFILE maxwait=$MAXWAIT"; echo "MON=$MON"; echo "DEBUG=$DEBUG"; echo "cdrom=${CDROM_ARGS[*]:-none}"; shasum -a 256 "$ROOT/binaries/NT5FIXED.VXD";
} >> "$LOG"
qemu-system-i386 -m 256 -M pc -cpu pentium2 -drive file="$DISK",format=raw -boot c -vga std -rtc base=localtime -display none -monitor unix:"$MON",server,nowait -debugcon file:"$DEBUG" -serial file:"$SERIAL" ${CDROM_ARGS[@]:+"${CDROM_ARGS[@]}"} ${HDD_ARGS[@]:+"${HDD_ARGS[@]}"} > "$RUNDIR/qemu_stdout.log" 2>&1 &
QPID=$!; echo "$QPID" > "$RUNDIR/qemu.pid"; echo "qemu_pid=$QPID" >> "$LOG"
# Dismiss "Add New Hardware Wizard" dialogs in the background.
# The wizard appears ~100-150s into boot (after the Win98 GUI loads).
# We cover T+60s to T+240s, sending Escape (dismiss dialog) and Alt+F4
# (close window) every 10 seconds. Runs in background so checkpoint
# collection is not blocked.
(
  sleep 60
  for i in $(seq 1 18); do
    python3 "$RUNDIR/hmp_unix.py" "$MON" 'sendkey esc' >> "$RUNDIR/hmp_force.log" 2>&1 || true
    sleep 10
  done
) &
DISMISSPID=$!
start=$(date +%s)
for t in 60 120 180 240 300 360; do
  [ "$t" -gt "$MAXWAIT" ] && continue
  now=$(date +%s); target=$((start+t)); [ "$now" -lt "$target" ] && sleep $((target-now))
  echo "checkpoint_${t}s=$(date -Iseconds)" >> "$LOG"
  cp "$DEBUG" "$RUNDIR/checkpoints/vxd_${t}.log" 2>/dev/null || true
  cp "$SERIAL" "$RUNDIR/checkpoints/serial_${t}.log" 2>/dev/null || true
  python3 "$RUNDIR/hmp_unix.py" "$MON" "screendump $RUNDIR/checkpoints/screen_${t}.ppm" >> "$RUNDIR/hmp_screen.log" 2>&1 || true
  python3 "$RUNDIR/hmp_unix.py" "$MON" 'info status' > "$RUNDIR/hmp_status_${t}.log" 2>&1 || true
  python3 - <<'PY' "$RUNDIR" "$t"
import sys,pathlib
r=pathlib.Path(sys.argv[1]); t=sys.argv[2]; p=r/'checkpoints'/f'vxd_{t}.log'
if p.exists():
    s=p.read_text(errors='replace'); flat=''.join(s.splitlines())
    print(t, 'size', p.stat().st_size, 'SmokeFail', 'Smoke test failed' in s, 'SmokePass', 'Smoke test PASSED' in s, 'U<', flat.count('U<'), 'A<', flat.count('A<'), 'Z[', flat.count('Z['), 'tail', flat[-160:])
else:
    print(t, 'no vxd')
PY
done
# Clean up the background dismiss loop before QEMU teardown
kill "$DISMISSPID" 2>/dev/null || true; wait "$DISMISSPID" 2>/dev/null || true
cp "$DEBUG" "$RUNDIR/vxd_debug.final.log" 2>/dev/null || true
cp "$SERIAL" "$RUNDIR/serial.final.log" 2>/dev/null || true
python3 "$RUNDIR/hmp_unix.py" "$MON" quit > "$RUNDIR/hmp_quit.log" 2>&1 || true
sleep 2; kill "$QPID" 2>/dev/null || true
python3 "$ART/disk_ops.py" read root "$OUTFILE" "$ART/guest-output/$OUTFILE" >> "$RUNDIR/retrieve.log" 2>&1 || python3 "$ART/disk_ops.py" read windows "$OUTFILE" "$ART/guest-output/$OUTFILE" >> "$RUNDIR/retrieve.log" 2>&1 || true
python3 "$ART/disk_ops.py" winini_blank >> "$RUNDIR/retrieve.log" 2>&1 || true
python3 "$ART/disk_ops.py" startup_absent >> "$RUNDIR/retrieve.log" 2>&1 || true
python3 "$ART/disk_ops.py" read windows WIN.INI "$RUNDIR/WIN.INI.restored" >> "$RUNDIR/retrieve.log" 2>&1 || true
shasum -a 256 "$ART/guest-output/$OUTFILE" "$RUNDIR/WIN.INI.restored" > "$RUNDIR/retrieve.sha256" 2>/dev/null || true
rm -f "$MON" "$DEBUG" "$SERIAL"
echo "run_end=$(date -Iseconds)" >> "$LOG"
