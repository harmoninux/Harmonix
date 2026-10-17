/*
 * rootfs_test.c - architecture-neutral POSIX regression checks for running an
 * extracted guest rootfs under qemu linux-user (harmonix qemu fork).
 *
 * Prints one line per check to stdout:
 *     NAME=PASS
 *     NAME=FAIL:<reason>
 * and exits nonzero if any check failed.  Build statically:
 *     aarch64-linux-gnu-gcc -static -O2 -o rt_a64 rootfs_test.c
 *     gcc -static -O2 -o rt_x64 rootfs_test.c
 *
 * Self-contained: only libc + kernel headers, no dynamic allocation.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/xattr.h>

#ifdef SYS_openat2
#include <linux/openat2.h>
#endif

static int g_fail = 0;
static int g_force_eacces = 0;

static void report(const char *name, int ok, const char *reason)
{
    if (ok) {
        printf("%s=PASS\n", name);
    } else {
        printf("%s=FAIL:%s\n", name, reason ? reason : "unknown");
        g_fail = 1;
    }
    fflush(stdout);
}

static void unlink_quiet(const char *path)
{
    (void)unlink(path);
}

/* KNOWN RED today: getcwd() after chdir() should report the guest cwd. */
static void check_getcwd_after_chdir(void)
{
    char buf[PATH_MAX];

    if (chdir("/etc") != 0) {
        report("getcwd_after_chdir", 0, "chdir(/etc) failed");
        return;
    }
    if (getcwd(buf, sizeof(buf)) == NULL) {
        report("getcwd_after_chdir", 0, "getcwd() returned NULL");
        return;
    }
    report("getcwd_after_chdir", strcmp(buf, "/etc") == 0, buf);
}

static void check_readlink_proc_cwd(void)
{
    char buf[PATH_MAX];
    ssize_t n;

    if (chdir("/etc") != 0) {
        report("readlink_proc_cwd", 0, "chdir(/etc) failed");
        return;
    }
    n = readlink("/proc/self/cwd", buf, sizeof(buf) - 1);
    if (n < 0) {
        report("readlink_proc_cwd", 0, "readlink(/proc/self/cwd) failed");
        return;
    }
    buf[n] = '\0';
    report("readlink_proc_cwd", strcmp(buf, "/etc") == 0, buf);
}

static void check_realpath_dot(void)
{
    char out[PATH_MAX];

    if (chdir("/etc") != 0) {
        report("realpath_dot", 0, "chdir(/etc) failed");
        return;
    }
    if (realpath(".", out) == NULL) {
        report("realpath_dot", 0, "realpath(\".\") returned NULL");
        return;
    }
    report("realpath_dot", strcmp(out, "/etc") == 0, out);
}

static void check_symlink_resolve(void)
{
    struct stat st;

    unlink_quiet("/tmp/rt_link");
    if (symlink("/etc", "/tmp/rt_link") != 0) {
        report("symlink_resolve", 0, "symlink failed");
        return;
    }
    if (stat("/tmp/rt_link", &st) != 0) {
        report("symlink_resolve", 0, "stat through symlink failed");
        return;
    }
    report("symlink_resolve", S_ISDIR(st.st_mode), "target is not a directory");
}

static void check_xattr_roundtrip(void)
{
    char val[8];
    ssize_t n;
    int fd;

    unlink_quiet("/tmp/rt_xattr");
    fd = open("/tmp/rt_xattr", O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        report("xattr_roundtrip", 0, "create failed");
        return;
    }
    close(fd);

    if (setxattr("/tmp/rt_xattr", "user.rt", "v", 1, 0) != 0) {
        report("xattr_roundtrip", 0, "setxattr failed");
        return;
    }
    memset(val, 0, sizeof(val));
    n = getxattr("/tmp/rt_xattr", "user.rt", val, sizeof(val));
    if (n != 1 || val[0] != 'v') {
        report("xattr_roundtrip", 0, "getxattr value mismatch");
        return;
    }
    if (removexattr("/tmp/rt_xattr", "user.rt") != 0) {
        report("xattr_roundtrip", 0, "removexattr failed");
        return;
    }
    report("xattr_roundtrip", 1, NULL);
}

static void check_utimensat_mtime(void)
{
    struct timespec ts[2];
    struct stat st;
    int fd;

    unlink_quiet("/tmp/rt_time");
    fd = open("/tmp/rt_time", O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        report("utimensat_mtime", 0, "create failed");
        return;
    }
    close(fd);

    ts[0].tv_sec = 1234567890;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = 1234567890;
    ts[1].tv_nsec = 0;
    if (utimensat(AT_FDCWD, "/tmp/rt_time", ts, 0) != 0) {
        report("utimensat_mtime", 0, "utimensat failed");
        return;
    }
    if (stat("/tmp/rt_time", &st) != 0) {
        report("utimensat_mtime", 0, "stat failed");
        return;
    }
    report("utimensat_mtime", st.st_mtim.tv_sec == 1234567890,
           "st_mtim.tv_sec != 1234567890");
}

static void check_mknodat_fifo(void)
{
    struct stat st;

    unlink_quiet("/tmp/rt_fifo");
    if (mknodat(AT_FDCWD, "/tmp/rt_fifo", S_IFIFO | 0644, 0) != 0) {
        report("mknodat_fifo", 0, "mknodat failed");
        return;
    }
    if (stat("/tmp/rt_fifo", &st) != 0) {
        report("mknodat_fifo", 0, "stat failed");
        return;
    }
    report("mknodat_fifo", S_ISFIFO(st.st_mode), "not a FIFO");
}

/* chown() must operate on the guest path; an unrelocated path yields ENOENT. */
static void check_chown_path(void)
{
    int fd, rc;

    unlink_quiet("/tmp/rt_own");
    fd = open("/tmp/rt_own", O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        report("chown_path", 0, "create failed");
        return;
    }
    close(fd);

    errno = 0;
    rc = chown("/tmp/rt_own", 1234, 5678);
    if (rc == 0) {
        report("chown_path", 1, NULL);
        return;
    }
    /* Non-root: EPERM means the real syscall was reached on the guest path. */
    if (errno == EPERM) {
        report("chown_path", 1, NULL);
        return;
    }
    report("chown_path", 0, "chown failed (path not relocated?)");
}

static void check_hardlink(void)
{
    struct stat sa, sb;
    const char *data = "harmonix-link-data\n";
    size_t dlen = strlen(data);
    char buf[64];
    int fd;
    ssize_t n;

    unlink_quiet("/tmp/rt_a");
    unlink_quiet("/tmp/rt_b");
    fd = open("/tmp/rt_a", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        report("hardlink", 0, "create failed");
        return;
    }
    if (write(fd, data, dlen) != (ssize_t)dlen) {
        close(fd);
        report("hardlink", 0, "write failed");
        return;
    }
    close(fd);

#ifdef SYS_linkat
    if (syscall(SYS_linkat, AT_FDCWD, "/tmp/rt_a",
                AT_FDCWD, "/tmp/rt_b", 0) != 0) {
        report("hardlink", 0, g_force_eacces ?
               "linkat fallback failed" : "link failed");
        return;
    }
#else
    if (link("/tmp/rt_a", "/tmp/rt_b") != 0) {
        report("hardlink", 0, "link failed");
        return;
    }
#endif

    if (g_force_eacces) {
        /* Forced EACCES exercises the copy fallback: data must survive. */
        fd = open("/tmp/rt_b", O_RDONLY);
        if (fd < 0) {
            report("hardlink", 0, "fallback target missing");
            return;
        }
        n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n != (ssize_t)dlen || memcmp(buf, data, dlen) != 0) {
            report("hardlink", 0, "fallback did not preserve data");
            return;
        }
        report("hardlink", 1, NULL);
        return;
    }

    if (stat("/tmp/rt_a", &sa) != 0 || stat("/tmp/rt_b", &sb) != 0) {
        report("hardlink", 0, "stat failed");
        return;
    }
    if (sa.st_ino != sb.st_ino || sa.st_nlink != 2) {
        report("hardlink", 0, "inode/nlink mismatch");
        return;
    }
    report("hardlink", 1, NULL);
}

static void check_symlink_abs_no_escape(void)
{
    struct stat st;
    char buf[PATH_MAX];
    char probe[PATH_MAX];
    const char *cand = NULL;
    ssize_t n;

    unlink_quiet("/tmp/rt_esc");
    if (symlink("/etc", "/tmp/rt_esc") != 0) {
        report("symlink_abs_no_escape", 0, "symlink failed");
        return;
    }
    n = readlink("/tmp/rt_esc", buf, sizeof(buf) - 1);
    if (n < 0) {
        report("symlink_abs_no_escape", 0, "readlink failed");
        return;
    }
    buf[n] = '\0';
    if (strcmp(buf, "/etc") != 0) {
        report("symlink_abs_no_escape", 0, "readlink target escaped rootfs");
        return;
    }

    if (access("/etc/alpine-release", F_OK) == 0) {
        cand = "alpine-release";
    } else if (access("/etc/os-release", F_OK) == 0) {
        cand = "os-release";
    }
    if (cand == NULL) {
        report("symlink_abs_no_escape", 1, NULL);
        return;
    }
    snprintf(probe, sizeof(probe), "/tmp/rt_esc/%s", cand);
    if (stat(probe, &st) != 0) {
        report("symlink_abs_no_escape", 0, "resolved outside rootfs");
        return;
    }
    report("symlink_abs_no_escape", 1, NULL);
}

static void check_rename_symlink(void)
{
    struct stat st;
    char buf[PATH_MAX];
    ssize_t n;

    unlink_quiet("/tmp/rt_sl");
    unlink_quiet("/tmp/rt_sl2");
    if (symlink("/etc", "/tmp/rt_sl") != 0) {
        report("rename_symlink", 0, "symlink failed");
        return;
    }
#ifdef SYS_renameat
    if (syscall(SYS_renameat, AT_FDCWD, "/tmp/rt_sl",
                AT_FDCWD, "/tmp/rt_sl2") != 0) {
        report("rename_symlink", 0, "rename failed");
        return;
    }
#else
    if (rename("/tmp/rt_sl", "/tmp/rt_sl2") != 0) {
        report("rename_symlink", 0, "rename failed");
        return;
    }
#endif
    if (lstat("/tmp/rt_sl2", &st) != 0) {
        report("rename_symlink", 0, "lstat failed");
        return;
    }
    if (!S_ISLNK(st.st_mode)) {
        report("rename_symlink", 0, "not a symlink");
        return;
    }
    n = readlink("/tmp/rt_sl2", buf, sizeof(buf) - 1);
    if (n < 0) {
        report("rename_symlink", 0, "readlink failed");
        return;
    }
    buf[n] = '\0';
    if (strcmp(buf, "/etc") != 0) {
        report("rename_symlink", 0, "wrong symlink target");
        return;
    }
    if (stat("/tmp/rt_sl2", &st) != 0) {
        report("rename_symlink", 0, "target does not resolve");
        return;
    }
    report("rename_symlink", 1, NULL);
}

static void check_openat2_open(void)
{
#ifdef SYS_openat2
    struct open_how how;
    const char *path = NULL;
    long r;

    if (access("/etc/passwd", R_OK) == 0) {
        path = "/etc/passwd";
    } else if (access("/etc/hostname", R_OK) == 0) {
        path = "/etc/hostname";
    } else {
        report("openat2_open", 0, "no probe file found in /etc");
        return;
    }

    memset(&how, 0, sizeof(how));
    how.flags = O_RDONLY;
    r = syscall(SYS_openat2, AT_FDCWD, path, &how, sizeof(how));
    if (r < 0) {
        report("openat2_open", 0, "openat2 failed");
        return;
    }
    close((int)r);
    report("openat2_open", 1, NULL);
#else
    report("openat2_open", 1, "not present");
#endif
}

static volatile sig_atomic_t got_sig = 0;

static void sighandler(int sig)
{
    (void)sig;
    got_sig = 1;
}

static void check_sigblock_works(void)
{
    struct sigaction sa;
    sigset_t set;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sighandler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        report("sigblock_works", 0, "sigaction failed");
        return;
    }

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0) {
        report("sigblock_works", 0, "sigprocmask(SIG_BLOCK) failed");
        return;
    }

    got_sig = 0;
    raise(SIGUSR1);
    if (got_sig != 0) {
        report("sigblock_works", 0, "signal delivered while blocked");
        return;
    }

    if (sigprocmask(SIG_UNBLOCK, &set, NULL) != 0) {
        report("sigblock_works", 0, "sigprocmask(SIG_UNBLOCK) failed");
        return;
    }
    if (got_sig != 1) {
        report("sigblock_works", 0, "signal not delivered after unblock");
        return;
    }
    report("sigblock_works", 1, NULL);
}

static void check_mq_open(void)
{
#ifdef SYS_mq_open
    long r;

    errno = 0;
    r = syscall(SYS_mq_open, "/rt_q", O_CREAT | O_RDWR, 0600, NULL);
    if (r == -1 && errno == ENOSYS) {
        report("mq_open", 1, NULL);
        return;
    }
    if (r >= 0) {
        close((int)r);
        report("mq_open", 0, "unexpected success (expected ENOSYS)");
        return;
    }
    report("mq_open", 0, "failed but errno != ENOSYS");
#else
    report("mq_open", 1, "not present");
#endif
}

static void check_long_path_bounded(void)
{
    char path[PATH_MAX + 64];
    size_t i;

    /* An absolute path strictly longer than PATH_MAX. */
    path[0] = '/';
    for (i = 1; i < sizeof(path) - 1; i++) {
        path[i] = 'a';
    }
    path[sizeof(path) - 1] = '\0';

    errno = 0;
    if (chdir(path) != -1) {
        report("long_path_bounded", 0, "chdir(long path) unexpectedly succeeded");
        return;
    }
    if (errno != ENAMETOOLONG) {
        report("long_path_bounded", 0, "chdir(long path) errno != ENAMETOOLONG");
        return;
    }
    report("long_path_bounded", 1, NULL);
}

int main(void)
{
    const char *force = getenv("QEMU_VROOT_FORCE_EACCES");
    g_force_eacces = (force != NULL && force[0] != '\0' && strcmp(force, "0") != 0);

    check_getcwd_after_chdir();
    check_readlink_proc_cwd();
    check_realpath_dot();
    check_symlink_resolve();
    check_xattr_roundtrip();
    check_utimensat_mtime();
    check_mknodat_fifo();
    check_chown_path();
    check_hardlink();
    check_rename_symlink();
    check_symlink_abs_no_escape();
    check_openat2_open();
    check_sigblock_works();
    check_mq_open();
    check_long_path_bounded();

    printf("TOTAL_FAILURES=%d\n", g_fail);
    return g_fail ? 1 : 0;
}
