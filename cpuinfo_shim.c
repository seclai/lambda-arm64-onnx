/*
 * cpuinfo_shim.c — LD_PRELOAD shim for AWS Lambda ARM64 compatibility.
 *
 * ONNX Runtime bundles the pytorch/cpuinfo library which reads CPU topology
 * from /sys/devices/system/cpu/{possible,present}.  On Lambda ARM64 (Graviton)
 * these files don't exist, causing cpuinfo to crash with an unhandled C++
 * exception before ORT's logging manager is initialized:
 *
 *   "Attempt to use DefaultLogger but none has been registered."
 *
 * This is a long-standing unfixed issue:
 *   https://github.com/microsoft/onnxruntime/issues/10038  (since Dec 2021)
 *   https://github.com/microsoft/onnxruntime/issues/15650
 *
 * This shim intercepts fopen() and open().  When the real sysfs file is
 * missing AND the path is one of the two CPU topology files, the shim
 * returns a fake file descriptor / FILE* containing "0-N\n" (where N is
 * the online CPU count minus one).  For all other paths the real syscall
 * result is returned unchanged.
 *
 * Copyright (c) 2026 Seclai, Inc.
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static const char *CPU_POSSIBLE = "/sys/devices/system/cpu/possible";
static const char *CPU_PRESENT  = "/sys/devices/system/cpu/present";

static int is_cpu_topology_path(const char *path) {
    return strcmp(path, CPU_POSSIBLE) == 0
        || strcmp(path, CPU_PRESENT)  == 0;
}

/* Build "0-N\n" where N = online CPUs - 1.  Returns string length. */
static int cpu_range_string(char *buf, size_t size) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) n = 1;
    return snprintf(buf, size, "0-%ld\n", n - 1);
}

/* ── fopen() interception ─────────────────────────────────────────── */

static FILE *(*real_fopen)(const char *, const char *) = NULL;

FILE *fopen(const char *restrict path, const char *restrict mode) {
    if (!real_fopen)
        real_fopen = dlsym(RTLD_NEXT, "fopen");

    FILE *f = real_fopen(path, mode);
    if (f || !is_cpu_topology_path(path))
        return f;

    char buf[32];
    int len = cpu_range_string(buf, sizeof(buf));
    /* strndup so fmemopen owns a heap copy that outlives buf. */
    return fmemopen(strndup(buf, len), len, "r");
}

/* ── open() interception ──────────────────────────────────────────── */

static int (*real_open)(const char *, int, ...) = NULL;

int open(const char *path, int flags, ...) {
    if (!real_open)
        real_open = dlsym(RTLD_NEXT, "open");

    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }

    int fd = (flags & (O_CREAT | O_TMPFILE))
        ? real_open(path, flags, mode)
        : real_open(path, flags);

    if (fd >= 0 || !is_cpu_topology_path(path))
        return fd;

    /* Create an anonymous in-memory file with the fake content. */
    char buf[32];
    int len = cpu_range_string(buf, sizeof(buf));

    int mfd = memfd_create("cpuinfo_shim", 0);
    if (mfd >= 0) {
        write(mfd, buf, len);
        lseek(mfd, 0, SEEK_SET);
    }
    return mfd;
}
