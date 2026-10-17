#!/usr/bin/env bash
#
# run-rootfs-tests.sh - build rootfs_test.c statically and run it inside an
# extracted guest rootfs under the host qemu linux-user emulator.
#
# Usage:
#   run-rootfs-tests.sh --arch {aarch64|x86_64} --rootfs <path> [--qemu <path>] [--keep]
#
# Enforced invariant: the absolute host path of the rootfs must never appear in
# the emulated program's output (host-path leakage check).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_QEMU_DIR="/data/share/harmonix/build-hnp/qemu-10/build-host"

ARCH=""
ROOTFS=""
QEMU=""
KEEP=0
FORCE_EACCES=0

usage() {
    cat <<EOF
Usage: $(basename "$0") --arch {aarch64|x86_64} --rootfs <path> [--qemu <path>] [--keep] [--force-eacces]

  --arch   {aarch64|x86_64}  guest architecture
  --rootfs <path>            extracted guest rootfs directory
  --qemu   <path>            qemu user emulator (default: ${DEFAULT_QEMU_DIR}/qemu-<arch>)
  --keep                     keep the compiled test binary (do not clean up)
  --force-eacces             export QEMU_VROOT_FORCE_EACCES=1 to force the
                             linkat/renameat EACCES fallbacks
EOF
}

die() {
    echo "ERROR: $*" >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)    ARCH="${2:-}";   shift 2 ;;
        --rootfs)  ROOTFS="${2:-}"; shift 2 ;;
        --qemu)    QEMU="${2:-}";   shift 2 ;;
        --keep)    KEEP=1;          shift ;;
        --force-eacces) FORCE_EACCES=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown argument: $1" ;;
    esac
done

[[ -n "$ARCH" ]]   || { usage >&2; die "--arch is required"; }
[[ -n "$ROOTFS" ]] || { usage >&2; die "--rootfs is required"; }

case "$ARCH" in
    aarch64) CC="aarch64-linux-gnu-gcc" ;;
    x86_64)  CC="gcc" ;;
    *) die "unsupported --arch '$ARCH' (expected aarch64 or x86_64)" ;;
esac

[[ -d "$ROOTFS" ]] || die "rootfs directory not found: $ROOTFS"
ROOTFS_ABS="$(cd "$ROOTFS" && pwd)"

if [[ -z "$QEMU" ]]; then
    QEMU="${DEFAULT_QEMU_DIR}/qemu-${ARCH}"
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/rootfs-tests.XXXXXX")"
TESTBIN="${WORKDIR}/rootfs_test"
cleanup() {
    if [[ "$KEEP" -eq 0 ]]; then
        rm -rf "$WORKDIR"
    else
        echo "kept test binary: $TESTBIN" >&2
    fi
}
trap cleanup EXIT

echo "== rootfs regression harness =="
echo "arch      : $ARCH"
echo "compiler  : $CC"
echo "rootfs    : $ROOTFS_ABS"
echo "qemu      : $QEMU"
echo "force eacces: $([[ "$FORCE_EACCES" -eq 1 ]] && echo yes || echo no)"
echo

echo "[build] $CC -static -O2 -o $TESTBIN $SCRIPT_DIR/rootfs_test.c"
if ! "$CC" -static -O2 -o "$TESTBIN" "$SCRIPT_DIR/rootfs_test.c"; then
    echo "BUILD=FAIL (static compile failed)" >&2
    exit 2
fi
echo "BUILD=PASS"
echo

if [[ ! -x "$QEMU" ]]; then
    echo "SKIP: qemu not found or not executable: $QEMU"
    echo "      (build it first; this harness is safe to run in CI before a build)"
    exit 0
fi

PATHS="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
run_out=""
run_rc=0
set +e
if [[ "$FORCE_EACCES" -eq 1 ]]; then
    run_out="$(QEMU_VROOT_FORCE_EACCES=1 "$QEMU" -L "$ROOTFS_ABS" \
        -E "PATH=${PATHS}" -E HOME=/root \
        "$TESTBIN" 2>&1)"
else
    run_out="$("$QEMU" -L "$ROOTFS_ABS" \
        -E "PATH=${PATHS}" -E HOME=/root \
        "$TESTBIN" 2>&1)"
fi
run_rc=$?
set -e

echo "---- emulator output (rc=${run_rc}) ----"
printf '%s\n' "$run_out"
echo "---------------------------------------"
echo

pass=0
fail=0
parsed=0
while IFS= read -r line; do
    case "$line" in
        *=PASS)  pass=$((pass + 1)); parsed=$((parsed + 1)) ;;
        *=FAIL:*) fail=$((fail + 1)); parsed=$((parsed + 1)) ;;
    esac
done <<< "$run_out"

leak_fail=0
if [[ ${#ROOTFS_ABS} -gt 1 ]] && printf '%s\n' "$run_out" | grep -qF -- "$ROOTFS_ABS"; then
    leak_fail=1
fi

place_fail=0
host_leak=0
placement_report=""
host_report=""
for f in rt_xattr rt_fifo rt_time rt_own rt_mode rt_dir rt_ut; do
    if [[ -e "$ROOTFS_ABS/tmp/$f" ]]; then
        placement_report+="${f}=present "
    else
        placement_report+="${f}=MISSING "
        place_fail=1
    fi
    if [[ -e "/tmp/$f" ]]; then
        host_report+="${f}=LEAK "
        host_leak=1
    else
        host_report+="${f}=absent "
    fi
done

if [[ "$leak_fail" -eq 1 ]]; then
    echo "LEAK=FAIL (rootfs host path appeared in output: $ROOTFS_ABS)"
else
    echo "LEAK=PASS"
fi

escape_fail=0
escape_report=""
if [[ -d "$ROOTFS_ABS/tmp" ]]; then
    shopt -s nullglob
    for l in "$ROOTFS_ABS"/tmp/rt_*; do
        [[ -L "$l" ]] || continue
        tgt="$(readlink "$l")"
        case "$tgt" in
            /*)
                if [[ "$tgt" == "$ROOTFS_ABS" || "$tgt" == "$ROOTFS_ABS"/* ]]; then
                    : # relocated host path: correct
                elif [[ "$tgt" == /mnt/host-root || "$tgt" == /mnt/host-root/* ]]; then
                    : # explicit host-root mapping: intentional
                else
                    escape_fail=1
                    escape_report+="$(basename "$l")->$tgt "
                fi
                ;;
        esac
    done
    shopt -u nullglob
fi

if [[ "$escape_fail" -eq 1 ]]; then
    echo "SYMLINK_ESCAPE=FAIL ($escape_report)"
else
    echo "SYMLINK_ESCAPE=PASS"
fi

echo
echo "== summary =="
printf '  %-18s %d\n' "checks passed" "$pass"
printf '  %-18s %d\n' "checks failed" "$fail"
printf '  %-18s %d\n' "checks parsed" "$parsed"
printf '  %-18s %s\n' "host-path leak" "$([[ "$leak_fail" -eq 0 ]] && echo PASS || echo FAIL)"
printf '  %-18s %s\n' "symlink escape" "$([[ "$escape_fail" -eq 0 ]] && echo PASS || echo FAIL)"
printf '  %-18s %s\n' "guest placement" "$placement_report"
printf '  %-18s %s\n' "host /tmp leak" "$host_report"

overall=0
if [[ "$fail" -gt 0 || "$parsed" -eq 0 || "$leak_fail" -ne 0 ]]; then
    overall=1
fi
if [[ "$place_fail" -ne 0 || "$host_leak" -ne 0 || "$run_rc" -ne 0 ]]; then
    overall=1
fi
if [[ "$escape_fail" -ne 0 ]]; then
    overall=1
fi

if [[ "$overall" -eq 0 ]]; then
    echo "RESULT=PASS"
else
    echo "RESULT=FAIL"
fi
exit "$overall"
