# Rootfs regression harness

Reusable regression tests for running an **extracted guest rootfs** under the
host build of the `harmonix` QEMU fork (`qemu-aarch64` / `qemu-x86_64`,
linux-user). Everything here is test scaffolding; it touches nothing outside
`build-hnp/qemu-10/tests/`.

## Files

- `rootfs_test.c` — self-contained, arch-neutral POSIX C program. Builds
  statically. Prints one line per check (`NAME=PASS` / `NAME=FAIL:<reason>`)
  and exits nonzero if any check fails.
- `run-rootfs-tests.sh` — builds the program statically, runs it under qemu with
  `-L <rootfs>`, checks for host-path leakage and file placement, prints a
  summary table.

## Run

```sh
./run-rootfs-tests.sh --arch aarch64 --rootfs /path/to/guest-rootfs
./run-rootfs-tests.sh --arch x86_64  --rootfs /path/to/guest-rootfs
```

Options:

- `--arch {aarch64|x86_64}` — required. Selects
  `aarch64-linux-gnu-gcc` / `gcc` for the static build.
- `--rootfs <path>` — required. Extracted guest rootfs directory.
- `--qemu <path>` — override the emulator. Default:
  `/data/share/harmonix/build-hnp/qemu-10/build-host/qemu-<arch>`.
- `--keep` — keep the compiled test binary (default: cleaned up).
- `--force-eacces` — export `QEMU_VROOT_FORCE_EACCES=1` to the emulator and the
  guest, forcing the `linkat`/`renameat` EACCES fallbacks so they can be
  exercised on a host whose filesystem supports those operations natively.
  The runner is then expected to stay green with the fallback semantics
  (link becomes a data-preserving copy; rename recreates the symlink).

If the qemu binary is missing, the runner prints a `SKIP:` message and exits
`0`, so it is safe to run in CI before a build exists.

The emulator is invoked as:

```sh
"$qemu" -L "$rootfs" \
    -E PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    -E HOME=/root "$testbin"
```

## What it checks

`rootfs_test.c` runs these checks (all self-asserting; no `expect/` needed):

| check | assertion |
|-------|-----------|
| `getcwd_after_chdir` | after `chdir("/etc")`, `getcwd()` returns exactly `/etc` |
| `readlink_proc_cwd` | after `chdir("/etc")`, `readlink("/proc/self/cwd")` is `/etc` |
| `realpath_dot` | after `chdir("/etc")`, `realpath(".")` is `/etc` |
| `symlink_resolve` | `stat()` through a symlink to `/etc` succeeds |
| `xattr_roundtrip` | `setxattr`/`getxattr`/`removexattr` on `user.rt` round-trips |
| `utimensat_mtime` | `utimensat` with `{1234567890,0}` twice sets `st_mtim.tv_sec` |
| `mknodat_fifo` | `mknodat(...S_IFIFO...)` creates a FIFO |
| `hardlink` | `linkat` yields same inode and `nlink == 2` (under `--force-eacces`: copy fallback preserves contents) |
| `rename_symlink` | symlink survives `renameat` and target resolves to `/etc` |
| `symlink_abs_no_escape` | `symlink("/etc", p)` keeps host target inside the rootfs and `readlink(p)` returns exactly `/etc` |
| `openat2_open` | `openat2` opens `/etc/passwd` (or `/etc/hostname`) |
| `sigblock_works` | `SIG_BLOCK` defers `SIGUSR1`; `SIG_UNBLOCK` delivers it |
| `mq_open` | raw `mq_open` returns `-1`/`ENOSYS` (documents current behavior) |

## Enforced invariant

The **absolute host path of the rootfs must never appear** in the emulated
program's output. If it does, the runner prints `LEAK=FAIL` and exits nonzero.
This catches host-path leakage from the `-L`-based rootfs translation.

The runner additionally scans the symlinks it created (`$rootfs/tmp/rt_*`) and
prints `SYMLINK_ESCAPE=FAIL` if any absolute target escapes the rootfs without
going through the explicit `/mnt/host-root` mapping.

## Placement check

After the run the runner verifies that the files created by the test landed
inside the guest rootfs (`$rootfs/tmp/rt_xattr`, `rt_fifo`, `rt_time`) and did
**not** leak to the host `/tmp/`.

## `QEMU_VROOT_FORCE_EACCES`

Debug knob read by the host process (`getenv`). When set to a non-`0` value,
`linkat`/`renameat` skip the real syscall and take their EACCES fallbacks, so
those paths can be tested on a filesystem that would otherwise succeed. It is
normally enabled through the runner's `--force-eacces` flag.


## Expected result

All checks pass on both architectures, with no host-path leak, no symlink
escape and correct guest placement (`RESULT=PASS`).

`mq_open` passes when the raw syscall returns `ENOSYS`; if the syscall is not
defined for the target it prints `PASS` with reason `not present`.

## Exit codes

- `0` — all checks passed, no leak, placement correct; **or** qemu was missing
  (SKIP).
- `1` — a check failed, the leak invariant was violated, placement was wrong, or
  the emulator exited nonzero.
- `2` — usage/configuration error, or the static build failed.
