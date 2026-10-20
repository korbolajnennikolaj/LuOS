// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

static int passed, failed;
#define CHECK(c, ...) do { if (c) { passed++; printf("  [ OK ] "); } else { failed++; printf("  [FAIL] "); } printf(__VA_ARGS__); printf("\n"); } while (0)

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void test_basic(void) {
    printf("basic\n");
    struct utsname u;
    CHECK(uname(&u) == 0 && strcmp(u.sysname, "Linux") == 0, "uname: %s %s %s", u.sysname, u.release, u.machine);
    CHECK(getpid() > 0 && gettid() == getpid(), "getpid %d == gettid %d", getpid(), gettid());
    char cwd[256];
    CHECK(getcwd(cwd, sizeof cwd) != NULL, "getcwd: %s", cwd);
    uint8_t r[32] = {0};
    CHECK(getrandom(r, sizeof r, 0) == 32 && (r[0] | r[1] | r[2] | r[3]), "getrandom 32 bytes");
    time_t t = time(NULL);
    CHECK(t > 1700000000, "time() = %lld (%s)", (long long)t, ctime(&t) ? "ok" : "");
}

static void test_memory(void) {
    printf("memory\n");
    void *small[1000];
    int ok = 1;
    for (int i = 0; i < 1000; i++) { small[i] = malloc(16 + i * 8); if (!small[i]) ok = 0; else memset(small[i], i, 16 + i * 8); }
    for (int i = 0; i < 1000; i++) free(small[i]);
    CHECK(ok, "1000 small malloc/free");

    size_t big = 64u << 20;
    uint8_t *b = calloc(1, big);
    int zero = b != NULL;
    for (size_t i = 0; b && i < big; i += 4096) { if (b[i]) zero = 0; b[i] = 1; }
    CHECK(zero, "calloc 64 MiB, zeroed and writable (demand paging)");
    free(b);

    uint8_t *m = mmap(NULL, 1u << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(m != MAP_FAILED, "reserve 1 GiB PROT_NONE at %p", (void *)m);
    if (m != MAP_FAILED) {
        CHECK(mprotect(m + (256u << 20), 1u << 20, PROT_READ | PROT_WRITE) == 0, "mprotect 1 MiB inside it to RW");
        memset(m + (256u << 20), 0xAB, 1u << 20);
        CHECK(m[(256u << 20) + 12345] == 0xAB, "write/read the RW window");
        CHECK(munmap(m, 1u << 30) == 0, "munmap the reservation");
    }
    void *fixed = mmap((void *)0x200000000000ULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(fixed == (void *)0x200000000000ULL, "MAP_FIXED at 0x200000000000");
    munmap(fixed, 8192);
    long cur = syscall(SYS_brk, 0);
    long grown = syscall(SYS_brk, cur + (1 << 20));
    if (grown == cur + (1 << 20)) ((volatile char *)cur)[(1 << 20) - 1] = 1;
    CHECK(grown == cur + (1 << 20) && syscall(SYS_brk, cur) == cur, "brk grows by 1 MiB, is writable, shrinks back");
}

static void test_files(void) {
    printf("files\n");
    const char *path = "/utest.txt";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    CHECK(fd >= 0, "create %s (fd %d, errno %d)", path, fd, errno);
    if (fd < 0) return;
    char buf[8192];
    for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (char)('a' + i % 26);
    CHECK(write(fd, buf, sizeof buf) == (ssize_t)sizeof buf, "write 8 KiB");
    CHECK(lseek(fd, 1000, SEEK_SET) == 1000, "lseek to 1000");
    char rb[100];
    CHECK(read(fd, rb, 100) == 100 && memcmp(rb, buf + 1000, 100) == 0, "read back 100 bytes at 1000");
    CHECK(pread(fd, rb, 10, 26) == 10 && rb[0] == 'a', "pread at 26");
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 8192 && S_ISREG(st.st_mode), "fstat size %lld", (long long)st.st_size);
    close(fd);

    FILE *f = fopen(path, "r");
    int n = 0, c;
    while (f && (c = fgetc(f)) != EOF) n++;
    if (f) fclose(f);
    CHECK(n == 8192, "stdio fgetc counted %d bytes", n);

    fd = open(path, O_RDONLY);
    char *mp = fd >= 0 ? mmap(NULL, 8192, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    CHECK(mp != MAP_FAILED && mp[0] == 'a' && mp[8191] == buf[8191], "mmap of the file reads its contents");
    if (mp != MAP_FAILED) munmap(mp, 8192);
    if (fd >= 0) close(fd);

    CHECK(stat("/", &st) == 0 && S_ISDIR(st.st_mode), "stat / is a directory");
    DIR *d = opendir("/");
    int entries = 0, saw = 0;
    struct dirent *de;
    while (d && (de = readdir(d))) { entries++; if (!strcmp(de->d_name, "utest.txt")) saw = 1; }
    if (d) closedir(d);
    CHECK(d && saw, "readdir / lists %d entries incl. utest.txt", entries);
    CHECK(unlink(path) == 0 && access(path, F_OK) != 0, "unlink");
    CHECK(open("/no/such/file", O_RDONLY) < 0 && errno == ENOENT, "missing file -> ENOENT");
    int z = open("/dev/zero", O_RDONLY);
    CHECK(z >= 0 && read(z, rb, 50) == 50 && rb[0] == 0 && rb[49] == 0, "/dev/zero");
    if (z >= 0) close(z);
}

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static long counter;
static atomic_long acounter;
static __thread int tls_value;
static int tls_ok = 1;

static void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    tls_value = id * 1000;
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&mtx);
        counter++;
        pthread_mutex_unlock(&mtx);
        atomic_fetch_add(&acounter, 1);
    }
    if (tls_value != id * 1000) tls_ok = 0;
    return (void *)(intptr_t)(id + 100);
}

static int turn;
static void *pinger(void *arg) {
    (void)arg;
    for (int i = 0; i < 200; i++) {
        pthread_mutex_lock(&mtx);
        while (turn != 1) pthread_cond_wait(&cond, &mtx);
        turn = 0;
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mtx);
    }
    return NULL;
}

static void *pipe_writer(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[1000];
    for (int i = 0; i < 300; i++) { memset(buf, 'A' + i % 26, sizeof buf); write(fd, buf, sizeof buf); }
    close(fd);
    return NULL;
}

static void test_threads(void) {
    printf("threads\n");
    enum { N = 8 };
    pthread_t th[N];
    tls_value = 1;
    double t0 = now_ms();
    int created = 0;
    for (int i = 0; i < N; i++) if (pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i) == 0) created++;
    int joined = 0;
    for (int i = 0; i < created; i++) { void *rv; if (pthread_join(th[i], &rv) == 0 && (intptr_t)rv == i + 100) joined++; }
    CHECK(created == N && joined == N, "%d threads created and joined with return values", joined);
    CHECK(counter == N * 20000 && acounter == N * 20000, "mutex counter %ld, atomic %ld (want %d) in %.0f ms",
          counter, (long)acounter, N * 20000, now_ms() - t0);
    CHECK(tls_ok && tls_value == 1, "__thread variables are per thread");

    pthread_t pt;
    pthread_create(&pt, NULL, pinger, NULL);
    t0 = now_ms();
    for (int i = 0; i < 200; i++) {
        pthread_mutex_lock(&mtx);
        turn = 1;
        pthread_cond_signal(&cond);
        while (turn != 0) pthread_cond_wait(&cond, &mtx);
        pthread_mutex_unlock(&mtx);
    }
    pthread_join(pt, NULL);
    CHECK(1, "condvar ping-pong 200 round trips in %.0f ms", now_ms() - t0);

    int fds[2];
    CHECK(pipe(fds) == 0, "pipe");
    pthread_t wt;
    pthread_create(&wt, NULL, pipe_writer, (void *)(intptr_t)fds[1]);
    long total = 0;
    char buf[4096];
    ssize_t k;
    int good = 1;
    while ((k = read(fds[0], buf, sizeof buf)) > 0) total += k;
    pthread_join(wt, NULL);
    close(fds[0]);
    CHECK(good && total == 300000, "300000 bytes through a pipe between threads (%ld)", total);
}

static int avx_usable(void) {
    unsigned a, b, c, d, lo, hi;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1u << 27)) || !(c & (1u << 28))) return 0;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6;
}

static atomic_int vec_bad;

static void *vec_worker(void *arg) {
    uint32_t in[8], out[16];
    for (int k = 0; k < 8; k++) in[k] = (uint32_t)(intptr_t)arg * 0x01010101u + (uint32_t)k * 0x11111111u + 0x10203040u;
    for (int round = 0; round < 100; round++) {
        __asm__ volatile(
            "vmovdqu (%[in]), %%ymm7\n\t"
            "vmovdqu (%[in]), %%ymm15\n\t"
            "mov $300000, %%ecx\n"
            "1:\n\t"
            "dec %%ecx\n\t"
            "jnz 1b\n\t"
            "mov $24, %%eax\n\t"
            "syscall\n\t"
            "mov $300000, %%ecx\n"
            "2:\n\t"
            "dec %%ecx\n\t"
            "jnz 2b\n\t"
            "vmovdqu %%ymm7, (%[out])\n\t"
            "vmovdqu %%ymm15, 32(%[out])\n\t"
            "vzeroupper\n\t"
            :: [in] "r"(in), [out] "r"(out) : "rax", "rcx", "r11", "memory", "xmm7", "xmm15");
        if (memcmp(in, out, 32) || memcmp(in, out + 8, 32)) {
            atomic_fetch_add(&vec_bad, 1);
            break;
        }
    }
    return NULL;
}

static void test_vector_state(void) {
    printf("vector state\n");
    if (!avx_usable()) {
        printf("  [SKIP] AVX not enabled by the CPU/kernel\n");
        return;
    }
    enum { N = 6 };
    pthread_t th[N];
    double t0 = now_ms();
    for (int i = 0; i < N; i++) pthread_create(&th[i], NULL, vec_worker, (void *)(intptr_t)(i + 1));
    for (int i = 0; i < N; i++) pthread_join(th[i], NULL);
    CHECK(vec_bad == 0, "YMM registers of %d threads survive syscalls and preemption (%.0f ms)", N, now_ms() - t0);
}

static void test_shared(void) {
    printf("shared memory and pseudo files\n");
    int fd = memfd_create("utest", MFD_CLOEXEC);
    CHECK(fd >= 0 && ftruncate(fd, 65536) == 0, "memfd_create + ftruncate");
    char *a = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    char *b = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    char rb[16] = { 0 };
    int ok = a != MAP_FAILED && b != MAP_FAILED;
    if (ok) {
        strcpy(a + 40000, "shared page");
        ok = pread(fd, rb, 11, 40000) == 11 && strcmp(b + 40000, "shared page") == 0 && memcmp(rb, "shared page", 11) == 0;
    }
    CHECK(ok, "two MAP_SHARED mappings and read() see the same memfd pages");
    ok = ok && pwrite(fd, "via write", 9, 100) == 9 && memcmp(a + 100, "via write", 9) == 0 && memcmp(b + 100, "via write", 9) == 0;
    CHECK(ok, "write() to the memfd shows up in both mappings");
    if (a != MAP_FAILED) munmap(a, 65536);
    if (b != MAP_FAILED) munmap(b, 65536);
    if (fd >= 0) close(fd);

    char link[256];
    ssize_t n = readlink("/proc/self/exe", link, sizeof link - 1);
    CHECK(n > 0, "/proc/self/exe -> %.*s", (int)(n > 0 ? n : 0), link);
    n = readlink("/proc/self/fd/1", link, sizeof link - 1);
    CHECK(n > 0, "/proc/self/fd/1 -> %.*s", (int)(n > 0 ? n : 0), link);
    FILE *f = fopen("/sys/devices/system/cpu/online", "r");
    char line[64] = "";
    if (f) {
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        fclose(f);
        line[strcspn(line, "\n")] = 0;
    }
    CHECK(line[0] == '0', "/sys/devices/system/cpu/online = %s, sysconf says %ld CPUs", line, sysconf(_SC_NPROCESSORS_ONLN));
}

static void test_time(void) {
    printf("time\n");
    double a = now_ms();
    struct timespec ts = { 0, 50 * 1000000 };
    nanosleep(&ts, NULL);
    double b = now_ms();
    CHECK(b - a >= 49 && b - a < 200, "nanosleep 50 ms took %.1f ms", b - a);
    usleep(10000);
    CHECK(now_ms() - b >= 9.5, "usleep 10 ms");
}

int main(int argc, char **argv) {
    printf("LuOS userspace test (%s)\n", argc > 0 ? argv[0] : "?");
    test_basic();
    test_memory();
    test_files();
    test_threads();
    test_vector_state();
    test_shared();
    test_time();
    printf("== %d passed, %d failed ==\n", passed, failed);
    return failed ? 1 : 0;
}
