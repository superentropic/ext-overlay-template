#!/usr/bin/env bash
# run-linux.sh — DAD Research Tool launcher (Linux)
#
# Protection enhancements derived from PowerVM (github.com/not1cyyy/PowerVM):
#
#   DisguiseProcess      → launch binary under a kernel-worker-style name
#   HideDriver DKOM      → trigger module self-concealment after load
#   EtwSpoof             → suppress audit trail (auditctl) before launch
#   AddProtectedPid      → driver PID registration handled by the binary itself
#   Anti-debug           → verify no tracers are attached before starting
#   resetProcessPeb64    → argv[0] rename mirrors PEB CommandLine replacement
#   DrvMain integrity    → hash the binary before execution
#
set -euo pipefail

script="$(readlink -f -- "${BASH_SOURCE[0]}")"
project="$(dirname -- "$script")"

cd -- "$project"

if [[ $EUID -ne 0 ]]; then
    exec sudo --preserve-env=DISPLAY,XAUTHORITY -- bash "$script" "$@"
fi

# ---------------------------------------------------------------------------
# PowerVM: Fake_NtQueryInformationProcess / anti-debug pre-check
# Ensure no tracer is attached to this shell before we launch.
# ---------------------------------------------------------------------------
if [[ -f /proc/$$/status ]]; then
    tracer_pid="$(awk '/^TracerPid:/{print $2}' /proc/$$/status 2>/dev/null || echo 0)"
    if [[ "${tracer_pid:-0}" -ne 0 ]]; then
        printf 'Anti-debug: shell is being traced (TracerPid=%s). Aborting.\n' "$tracer_pid" >&2
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# Locate the most recently built executable
# ---------------------------------------------------------------------------
binary=""
for candidate in "$project"/bin/ext-research-tool "$project"/build*/ext-research-tool; do
    [[ -x "$candidate" ]] || continue
    if [[ -z "$binary" || "$candidate" -nt "$binary" ]]; then
        binary="$candidate"
    fi
done
if [[ -z "$binary" ]]; then
    printf 'Executable missing. Build with: cmake --build "%s/build-fixed" --target ext-research-tool\n' "$project" >&2
    exit 1
fi
printf 'Launching %s\n' "$binary"

# ---------------------------------------------------------------------------
# PowerVM: DrvMain integrity check — hash the binary before execution.
# Mirrors PowerVM's build fingerprint check in DriverEntry.
# ---------------------------------------------------------------------------
expected_hash_file="$project/.bin_hash"
current_hash="$(sha256sum "$binary" | awk '{print $1}')"
# Never auto-generate the baseline — trusting a potentially compromised binary
# on first run would permanently bypass integrity checks (supply-chain attack).
# Seal the hash once after a trusted build:
#   sha256sum "$binary" | awk '{print $1}' > "$project/.bin_hash" && chmod 600 "$project/.bin_hash"
if [[ ! -f "$expected_hash_file" ]]; then
    printf 'Integrity: .bin_hash is missing. Seal it after a trusted build:\n' >&2
    printf '  sha256sum "%s" | awk '"'"'{print $1}'"'"' > "%s"\n' "$binary" "$expected_hash_file" >&2
    printf '  chmod 600 "%s"\n' "$expected_hash_file" >&2
    exit 1
fi
stored_hash="$(cat "$expected_hash_file")"
if [[ "$current_hash" != "$stored_hash" ]]; then
    printf 'Integrity: binary hash mismatch — possible tampering. Aborting.\n' >&2
    printf '  Stored:  %s\n' "$stored_hash" >&2
    printf '  Current: %s\n' "$current_hash" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Kernel module — load if missing, then trigger DKOM hiding
# PowerVM: HideDriver removes the driver from InLoadOrderLinks.
# We do the same: load ext_mem, register our PID, then hide the module
# from lsmod / /proc/modules via EXT_IOCTL_PROTECT.
# ---------------------------------------------------------------------------
module="$project/kernel_driver/ext_mem.ko"
hidden_dev="/tmp/opencode/.k_ioc"   # hidden device node — path not known to scanners

# Always reload so we never run against a stale module build.
# Conditions: device node exists (DKOM-hidden run), visible in /proc/modules
# (clean-exit run), or visible in /sys/module (older module without kobject_del).
if [[ -e "$hidden_dev" ]] || [[ -c /dev/ext_mem ]] \
   || grep -q "^ext_mem" /proc/modules 2>/dev/null \
   || [[ -d /sys/module/ext_mem ]]; then
    # Send UNHIDE_MODULE ioctl first — if DKOM is active the module is removed
    # from the kernel list, so delete_module() can't find it by name until
    # unhide restores it.
    # _IOW('D', 4, 8) = 0x40084404  magic 'D'=0x44, nr=4, size=8
    # flags = EXT_PROTECT_FLAG_UNHIDE_MODULE = bit 3 = 0x08
    dev_path="${hidden_dev}"
    [[ -c /dev/ext_mem ]] && dev_path=/dev/ext_mem   # fallback if visible
    python3 -c "
import fcntl, struct, os, sys
try:
    fd = os.open('${dev_path}', os.O_RDWR)
    fcntl.ioctl(fd, 0x40084404, struct.pack('=II', 0, 0x08))
    os.close(fd)
except Exception as e:
    sys.stderr.write('unhide ioctl: ' + str(e) + '\n')
" 2>/dev/null || true

    # Use delete_module() syscall directly — libkmod's rmmod validates the module
    # via /sys/module/ which kobject_del already removed; the raw syscall searches
    # the kernel module list directly and works after UNHIDE restores the name.
    python3 -c "
import ctypes, sys
libc = ctypes.CDLL('libc.so.6', use_errno=True)
libc.delete_module.restype  = ctypes.c_int
libc.delete_module.argtypes = [ctypes.c_char_p, ctypes.c_uint]
ret = libc.delete_module(b'ext_mem', ctypes.c_uint(0))
if ret != 0:
    sys.stderr.write('delete_module errno ' + str(ctypes.get_errno()) + '\n')
" 2>/dev/null || true
    rm -f "$hidden_dev" /dev/ext_mem 2>/dev/null || true
fi
if true; then
    if [[ ! -f "$module" ]]; then
        printf 'Module missing: %s\nBuild with: make -C "%s/kernel_driver"\n' "$module" "$project" >&2
        exit 1
    fi
    if ! insmod_err="$(insmod "$module" 2>&1)"; then
        printf 'insmod failed: %s\nRebuild with: make -C "%s/kernel_driver" clean all\n' \
            "$insmod_err" "$project" >&2
        exit 1
    fi
fi

if [[ ! -c /dev/ext_mem ]]; then
    printf 'ext_mem is loaded but /dev/ext_mem is missing. Reload the module.\n' >&2
    exit 1
fi

# Move the device node from the visible /dev/ext_mem to a hidden path.
# The binary finds it via EXT_DEV_PATH env var; /dev/ext_mem is removed.
minor=$(awk '/^[[:space:]]*[0-9]+ ext_mem/{print $1}' /proc/misc 2>/dev/null || echo "")
if [[ -n "$minor" ]]; then
    rm -f /dev/ext_mem
    mknod "$hidden_dev" c 10 "$minor"
    chmod 0600 "$hidden_dev"
    export EXT_DEV_PATH="$hidden_dev"
else
    # Fallback: keep /dev/ext_mem but lock it down
    chmod 0600 /dev/ext_mem
    export EXT_DEV_PATH=/dev/ext_mem
fi

# ---------------------------------------------------------------------------
# PowerVM: EtwSpoof — suppress audit trail for this session.
# Equivalent: disable auditd temporarily so the tool's syscalls are not logged.
# Only done if auditctl is available and audit is running.
# ---------------------------------------------------------------------------
if command -v auditctl &>/dev/null && systemctl is-active --quiet auditd 2>/dev/null; then
    auditctl -e 0 2>/dev/null || true  # disable audit logging
    trap 'auditctl -e 1 2>/dev/null || true' EXIT
fi

# ---------------------------------------------------------------------------
# Hardware fingerprint spoofing
# EAC reads /sys/class/dmi/id/{product_uuid,board_serial,product_serial} and
# /sys/class/net/*/address to build a hardware fingerprint sent to its backend.
# Strategy: bind-mount random values over the real sysfs entries.
#   - Hardware is unchanged — no network disruption, no BIOS modification
#   - EAC reads a fresh random fingerprint on every launch
#   - Values are generated from /dev/urandom — unpredictable, non-repeating
#   - Bind mounts are cleaned up at the start of each run (previous values gone)
# ---------------------------------------------------------------------------
mkdir -p /tmp/opencode/dmi /tmp/opencode/net

# Unmount any previous spoofs from last run before applying new ones
for _f in product_uuid board_serial product_serial product_name; do
    _real=$(readlink -f "/sys/class/dmi/id/$_f" 2>/dev/null)
    [[ -n "$_real" ]] && umount "$_real" 2>/dev/null || true
done
for _iface in $(ls /sys/class/net/ 2>/dev/null | grep -v '^lo$'); do
    _real=$(readlink -f "/sys/class/net/$_iface/address" 2>/dev/null)
    [[ -n "$_real" ]] && umount "$_real" 2>/dev/null || true
done

# Generate and apply random DMI identifiers
python3 -c "
import os, uuid

# Random UUID for product_uuid
product_uuid = str(uuid.uuid4()).upper()
# Random 16-char hex serial numbers
board_serial = os.urandom(8).hex().upper()
product_serial = os.urandom(8).hex().upper()

for name, val in [
    ('product_uuid',   product_uuid),
    ('board_serial',   board_serial),
    ('product_serial', product_serial),
    ('product_name',   'System Product Name'),
]:
    with open(f'/tmp/opencode/dmi/{name}', 'w') as f:
        f.write(val + '\n')
    import subprocess, os as _os
    real = subprocess.check_output(['readlink', '-f', f'/sys/class/dmi/id/{name}'],
                                   stderr=subprocess.DEVNULL, text=True).strip()
    if real and _os.path.exists(real):
        subprocess.run(['mount', '--bind', f'/tmp/opencode/dmi/{name}', real],
                       stderr=subprocess.DEVNULL)
" 2>/dev/null || true

# Generate and apply random MAC addresses for each non-loopback interface.
# Bit rules: byte[0] bit0=0 (unicast), bit1=1 (locally administered).
# The real hardware MAC is unchanged — sysfs returns the fake value to readers.
for _iface in $(ls /sys/class/net/ 2>/dev/null | grep -v '^lo$'); do
    _real=$(readlink -f "/sys/class/net/$_iface/address" 2>/dev/null)
    [[ -f "$_real" ]] || continue
    mkdir -p "/tmp/opencode/net/$_iface"
    _fake_mac_path="/tmp/opencode/net/${_iface}/address"
    python3 -c "
import os, sys
b = bytearray(os.urandom(6))
b[0] = (b[0] & 0xFE) | 0x02
mac = ':'.join(f'{x:02x}' for x in b)
with open(sys.argv[1], 'w') as f:
    f.write(mac + '\n')
" "$_fake_mac_path" 2>/dev/null || true
    mount --bind "$_fake_mac_path" "$_real" 2>/dev/null || true
done

# ---------------------------------------------------------------------------
# PowerVM: DisguiseProcess / resetProcessPeb64Param (CommandLine replacement)
# Launch the binary with argv[0] set to a kernel worker thread name so that
# ps/top/htop show "kworker/u16:3" rather than the real binary path.
# The binary's Protection::Init() further overwrites its own argv[0] in-place.
# ---------------------------------------------------------------------------
fake_name="kworker/u16:3"

# Copy binary to tmpfs so /proc/[pid]/exe shows an innocuous path rather
# than ~/Downloads/ext-research-tool-main/... which EAC fingerprints.
# gsd-xrandr is a legitimate GNOME settings daemon name — plausible on any desktop.
hidden_bin="/tmp/opencode/gsd-xrandr"
cp "$binary" "$hidden_bin" && chmod 0700 "$hidden_bin"

# Copy config to tmpfs and pass via env var — this keeps /proc/[pid]/cmdline
# clean (no --config /home/.../ext-research-tool/... visible to process scanners).
# Real kworker threads have empty cmdlines; any visible arg is a detection signal.
cp -f "$project/config/settings.json" /tmp/opencode/s.json 2>/dev/null
export EXT_CONFIG=/tmp/opencode/s.json

exec -a "$fake_name" "$hidden_bin" "$@"
