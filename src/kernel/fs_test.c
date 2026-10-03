#include "fs_test.h"

#include "components/logger.h"
#include "components/Memory/heap.h"
#include "fs/fs.h"
#include "kernel/programs/service_root_fs.h"
#include "kernel/rootfs.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef LIMINE_COLOR_WHITE
#define LIMINE_COLOR_WHITE 0xFFFFFFFF
#define LIMINE_COLOR_LIGHT_GREEN 0xFF55FF55
#define LIMINE_COLOR_LIGHT_RED 0xFFFF5555
#define LIMINE_COLOR_LIGHT_GRAY 0xFFAAAAAA
#define LIMINE_COLOR_CYAN 0xFF55FFFF
#define LIMINE_COLOR_YELLOW 0xFFFFFF55
#endif

#define FST_BIG_SIZE 200000u
#define FST_APPEND_SIZE 50000u
#define FST_OVERWRITE_AT 1000u
#define FST_OVERWRITE_SIZE 5000u
#define FST_TRUNC_SIZE 70000u
#define FST_INTERLEAVE_ROUNDS 20
#define FST_INTERLEAVE_CHUNK 3000u
#define FST_EXTEND_SIZE 10000u
#define FST_MANY_FILES 60
#define FST_KEEP_BIN_SIZE 123457u
#define FST_IO_CHUNK 16384u

static struct limine_video_driver *g_video;
static fs_t *g_fs;
static char g_base[FS_MAX_PATH];
static int g_pass;
static int g_fail;
static uint8_t *g_io;

static void fst_check(const char *name, bool ok) {
    if (ok) {
        g_pass++;
        LOG_INFO("PASS %s", name);
    } else {
        g_fail++;
        LOG_ERROR("FAIL %s", name);
    }
    g_video->printf("  ", LIMINE_COLOR_LIGHT_GRAY);
    g_video->printf(name, LIMINE_COLOR_WHITE);
    g_video->printf(ok ? ": PASS\n" : ": FAIL\n", ok ? LIMINE_COLOR_LIGHT_GREEN : LIMINE_COLOR_LIGHT_RED);
}

static void fst_section(const char *title) {
    LOG_INFO("--- %s ---", title);
    g_video->printf("\n--- ", LIMINE_COLOR_LIGHT_GRAY);
    g_video->printf(title, LIMINE_COLOR_YELLOW);
    g_video->printf(" ---\n", LIMINE_COLOR_LIGHT_GRAY);
}

static uint8_t fst_byte(uint32_t seed, uint64_t i) {
    return (uint8_t)((i * 131u + (uint64_t)seed * 7919u + (i >> 9) * 17u) & 0xFFu);
}

static void fst_path(char *out, const char *base, const char *name) {
    if (strcmp(base, "/") == 0) snprintf(out, FS_MAX_PATH, "/%s", name);
    else snprintf(out, FS_MAX_PATH, "%s/%s", base, name);
}

static bool fst_write_pattern(const char *path, bool create, bool truncate, bool append,
                              uint64_t offset, uint64_t len, uint32_t seed, uint32_t chunk) {
    fs_file_t f;
    if (fs_open(g_fs, path, create, truncate, &f) != FS_OK) return false;
    if (append) offset = f.size;
    if (fs_seek(&f, offset) != FS_OK) { fs_close(&f); return false; }

    bool ok = true;
    uint64_t done = 0;
    uint32_t step = 0;
    while (done < len) {
        uint64_t n = (step & 1u) ? (chunk / 4u + 1u) : chunk;
        if (n > len - done) n = len - done;
        for (uint64_t i = 0; i < n; i++) g_io[i] = fst_byte(seed, offset + done + i);
        int64_t w = fs_write(&f, g_io, n);
        if (w != (int64_t)n) { ok = false; break; }
        done += n;
        step++;
    }
    if (fs_close(&f) != FS_OK) ok = false;
    return ok;
}

static bool fst_verify_pattern(const char *path, uint64_t offset, uint64_t len, uint32_t seed) {
    fs_file_t f;
    if (fs_open(g_fs, path, false, false, &f) != FS_OK) return false;
    if (fs_seek(&f, offset) != FS_OK) { fs_close(&f); return false; }

    bool ok = true;
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > FST_IO_CHUNK) n = FST_IO_CHUNK;
        int64_t r = fs_read(&f, g_io, n);
        if (r != (int64_t)n) { ok = false; break; }
        for (uint64_t i = 0; i < n; i++) {
            if (g_io[i] != fst_byte(seed, offset + done + i)) { ok = false; break; }
        }
        if (!ok) break;
        done += n;
    }
    fs_close(&f);
    return ok;
}

static bool fst_verify_zero(const char *path, uint64_t offset, uint64_t len) {
    fs_file_t f;
    if (fs_open(g_fs, path, false, false, &f) != FS_OK) return false;
    if (fs_seek(&f, offset) != FS_OK) { fs_close(&f); return false; }
    bool ok = true;
    uint64_t done = 0;
    while (done < len && ok) {
        uint64_t n = len - done;
        if (n > FST_IO_CHUNK) n = FST_IO_CHUNK;
        int64_t r = fs_read(&f, g_io, n);
        if (r != (int64_t)n) { ok = false; break; }
        for (uint64_t i = 0; i < n; i++) if (g_io[i] != 0) { ok = false; break; }
        done += n;
    }
    fs_close(&f);
    return ok;
}

static bool fst_write_text(const char *path, const char *text, bool truncate) {
    fs_file_t f;
    if (fs_open(g_fs, path, true, truncate, &f) != FS_OK) return false;
    uint64_t len = strlen(text);
    int64_t w = fs_write(&f, text, len);
    bool ok = (w == (int64_t)len);
    if (fs_close(&f) != FS_OK) ok = false;
    return ok;
}

static bool fst_read_text(const char *path, const char *expect) {
    fs_file_t f;
    if (fs_open(g_fs, path, false, false, &f) != FS_OK) return false;
    uint64_t len = strlen(expect);
    bool ok = (f.size == len);
    if (ok && len) {
        int64_t r = fs_read(&f, g_io, len);
        ok = (r == (int64_t)len) && memcmp(g_io, expect, (size_t)len) == 0;
    }
    fs_close(&f);
    return ok;
}

static int64_t fst_size(const char *path) {
    fs_dirent_t st;
    if (fs_stat(g_fs, path, &st) != FS_OK) return -1;
    return (int64_t)st.size;
}

static int fst_count_dir(const char *path) {
    fs_dir_t d;
    if (fs_opendir(g_fs, path, &d) != FS_OK) return -1;
    fs_dirent_t e;
    int count = 0;
    while (fs_readdir(&d, &e) == FS_OK) {
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
        count++;
    }
    fs_closedir(&d);
    return count;
}

static int fst_rmtree(const char *path) {
    fs_dirent_t st;
    int r = fs_stat(g_fs, path, &st);
    if (r != FS_OK) return r;
    if (st.type == FS_ENTRY_FILE) return fs_unlink(g_fs, path);

    for (;;) {
        fs_dir_t d;
        if (fs_opendir(g_fs, path, &d) != FS_OK) return FS_ERR_IO;
        fs_dirent_t e;
        char child[FS_MAX_PATH];
        bool found = false;
        while (fs_readdir(&d, &e) == FS_OK) {
            if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
            fst_path(child, path, e.name);
            found = true;
            break;
        }
        fs_closedir(&d);
        if (!found) break;
        r = fst_rmtree(child);
        if (r != FS_OK) return r;
    }
    return fs_rmdir(g_fs, path);
}

static fs_t *fst_resolve(const char *arg, char *abs, char *rel) {
    if (!rootfs_is_mounted()) return NULL;
    rootfs_normalize_path((arg && arg[0]) ? arg : ".", abs, FS_MAX_PATH);
    return rootfs_resolve(abs, rel, FS_MAX_PATH);
}

static uint32_t fst_crc32_update(uint32_t crc, const uint8_t *data, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static void fst_sum_walk(const char *path, const char *shown, int depth, int *files, int *dirs) {
    if (depth > 16) return;
    fs_dir_t d;
    if (fs_opendir(g_fs, path, &d) != FS_OK) {
        LOG_ERROR("SUM cannot open %s", shown);
        return;
    }
    fs_dirent_t e;
    while (fs_readdir(&d, &e) == FS_OK) {
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
        char child[FS_MAX_PATH], child_shown[FS_MAX_PATH];
        fst_path(child, path, e.name);
        fst_path(child_shown, shown, e.name);

        if (e.type == FS_ENTRY_DIR) {
            (*dirs)++;
            LOG_INFO("SUM dir %s", child_shown);
            g_video->printf("  <DIR>     ", LIMINE_COLOR_CYAN);
            g_video->printf(child_shown, LIMINE_COLOR_CYAN);
            g_video->printf("\n", LIMINE_COLOR_CYAN);
            fst_sum_walk(child, child_shown, depth + 1, files, dirs);
            continue;
        }

        fs_file_t f;
        if (fs_open(g_fs, child, false, false, &f) != FS_OK) {
            LOG_ERROR("SUM cannot read %s", child_shown);
            continue;
        }
        uint32_t crc = 0xFFFFFFFFu;
        uint64_t total = 0;
        int64_t n;
        while ((n = fs_read(&f, g_io, FST_IO_CHUNK)) > 0) {
            crc = fst_crc32_update(crc, g_io, (uint64_t)n);
            total += (uint64_t)n;
        }
        uint64_t expect = f.size;
        fs_close(&f);
        crc ^= 0xFFFFFFFFu;
        (*files)++;

        char line[FS_MAX_PATH + 48];
        snprintf(line, sizeof(line), "  %08x %10llu  %s%s\n", (unsigned)crc,
                 (unsigned long long)total, child_shown, total == expect ? "" : "  (short read)");
        g_video->printf(line, total == expect ? LIMINE_COLOR_WHITE : LIMINE_COLOR_LIGHT_RED);
        LOG_INFO("SUM %08x %llu %s%s", (unsigned)crc, (unsigned long long)total, child_shown,
                 total == expect ? "" : " SHORT");
    }
    fs_closedir(&d);
}

void cmd_fs_sum(struct limine_video_driver* video, const char *arg) {
    g_video = video;
    char abs[FS_MAX_PATH], rel[FS_MAX_PATH];
    g_fs = fst_resolve(arg, abs, rel);
    if (!g_fs) {
        video->printf("fs-sum: nothing mounted for that path\n", LIMINE_COLOR_LIGHT_RED);
        return;
    }
    g_io = kmalloc(FST_IO_CHUNK);
    if (!g_io) return;

    int files = 0, dirs = 0;
    LOG_INFO("SUM begin %s (%s)", abs, rootfs_type_name(g_fs->type));
    fst_sum_walk(rel, "", 0, &files, &dirs);
    LOG_INFO("SUM end files %d dirs %d", files, dirs);

    char line[96];
    snprintf(line, sizeof(line), "fs-sum: %d file(s), %d dir(s)\n", files, dirs);
    video->printf(line, LIMINE_COLOR_LIGHT_GREEN);
    kfree(g_io);
    g_io = NULL;
}

static void fst_run_writable(void) {
    char work[FS_MAX_PATH], keep[FS_MAX_PATH], p[FS_MAX_PATH], q[FS_MAX_PATH], name[96];

    fst_path(work, g_base, "fstest");
    fst_path(keep, g_base, "fskeep");

    fst_section("small files");
    fst_path(p, work, "hello.txt");
    fst_check("create hello.txt", fst_write_text(p, "Hello, LuOS!\n", false));
    fst_check("read hello.txt", fst_read_text(p, "Hello, LuOS!\n"));
    fst_check("stat hello.txt size", fst_size(p) == 13);
    fst_check("reopen with truncate", fst_write_text(p, "x", true));
    fst_check("truncated content", fst_read_text(p, "x"));
    fst_path(q, work, "empty.dat");
    {
        fs_file_t f;
        bool ok = fs_open(g_fs, q, true, false, &f) == FS_OK;
        if (ok) ok = fs_close(&f) == FS_OK;
        fst_check("create empty file", ok && fst_size(q) == 0);
    }

    fst_section("large file");
    fst_path(p, work, "big.bin");
    fst_check("write 200000 bytes", fst_write_pattern(p, true, false, false, 0, FST_BIG_SIZE, 1, 4096));
    fst_check("size after write", fst_size(p) == (int64_t)FST_BIG_SIZE);
    fst_check("verify content", fst_verify_pattern(p, 0, FST_BIG_SIZE, 1));
    fst_check("append 50000 bytes", fst_write_pattern(p, false, false, true, 0, FST_APPEND_SIZE, 2, 7000));
    fst_check("size after append", fst_size(p) == (int64_t)(FST_BIG_SIZE + FST_APPEND_SIZE));
    fst_check("verify original part", fst_verify_pattern(p, 0, FST_BIG_SIZE, 1));
    fst_check("verify appended part", fst_verify_pattern(p, FST_BIG_SIZE, FST_APPEND_SIZE, 2));
    fst_check("overwrite middle", fst_write_pattern(p, false, false, false, FST_OVERWRITE_AT, FST_OVERWRITE_SIZE, 3, 1500));
    fst_check("size unchanged by overwrite", fst_size(p) == (int64_t)(FST_BIG_SIZE + FST_APPEND_SIZE));
    fst_check("verify head", fst_verify_pattern(p, 0, FST_OVERWRITE_AT, 1));
    fst_check("verify overwritten", fst_verify_pattern(p, FST_OVERWRITE_AT, FST_OVERWRITE_SIZE, 3));
    fst_check("verify after overwrite", fst_verify_pattern(p, FST_OVERWRITE_AT + FST_OVERWRITE_SIZE,
                                                           FST_BIG_SIZE - FST_OVERWRITE_AT - FST_OVERWRITE_SIZE, 1));

    fst_section("truncate");
    {
        fs_file_t f;
        bool ok = fs_open(g_fs, p, false, false, &f) == FS_OK;
        if (ok) { ok = fs_truncate(&f, FST_TRUNC_SIZE) == FS_OK; fs_close(&f); }
        fst_check("shrink to 70000", ok);
    }
    fst_check("size after shrink", fst_size(p) == (int64_t)FST_TRUNC_SIZE);
    fst_check("content after shrink", fst_verify_pattern(p, 0, FST_OVERWRITE_AT, 1) &&
                                      fst_verify_pattern(p, FST_OVERWRITE_AT, FST_OVERWRITE_SIZE, 3) &&
                                      fst_verify_pattern(p, FST_OVERWRITE_AT + FST_OVERWRITE_SIZE,
                                                         FST_TRUNC_SIZE - FST_OVERWRITE_AT - FST_OVERWRITE_SIZE, 1));
    {
        fs_file_t f;
        bool ok = fs_open(g_fs, p, false, false, &f) == FS_OK;
        if (ok) { ok = fs_truncate(&f, FST_TRUNC_SIZE + FST_EXTEND_SIZE) == FS_OK; fs_close(&f); }
        fst_check("extend by 10000", ok);
    }
    fst_check("size after extend", fst_size(p) == (int64_t)(FST_TRUNC_SIZE + FST_EXTEND_SIZE));
    fst_check("extended area is zero", fst_verify_zero(p, FST_TRUNC_SIZE, FST_EXTEND_SIZE));
    fst_path(q, work, "gap.bin");
    fst_check("write past end", fst_write_pattern(q, true, false, false, 9000, 3000, 6, 1024));
    fst_check("gap size", fst_size(q) == 12000);
    fst_check("gap is zero", fst_verify_zero(q, 0, 9000));
    fst_check("data after gap", fst_verify_pattern(q, 9000, 3000, 6));
    {
        fs_file_t f;
        bool ok = fs_open(g_fs, q, false, false, &f) == FS_OK;
        if (ok) { ok = fs_truncate(&f, 0) == FS_OK; fs_close(&f); }
        fst_check("truncate to zero", ok && fst_size(q) == 0);
    }
    fst_check("write after zero truncate", fst_write_pattern(q, false, false, false, 0, 5000, 7, 2048));
    fst_check("verify after zero truncate", fst_size(q) == 5000 && fst_verify_pattern(q, 0, 5000, 7));

    fst_section("interleaved growth");
    {
        char a[FS_MAX_PATH], b[FS_MAX_PATH];
        fst_path(a, work, "a.bin");
        fst_path(b, work, "b.bin");
        fs_file_t fa, fb;
        bool ok = fs_open(g_fs, a, true, false, &fa) == FS_OK;
        if (ok) ok = fs_open(g_fs, b, true, false, &fb) == FS_OK;
        for (int round = 0; ok && round < FST_INTERLEAVE_ROUNDS; round++) {
            uint64_t off = (uint64_t)round * FST_INTERLEAVE_CHUNK;
            for (uint32_t i = 0; i < FST_INTERLEAVE_CHUNK; i++) g_io[i] = fst_byte(4, off + i);
            if (fs_write(&fa, g_io, FST_INTERLEAVE_CHUNK) != (int64_t)FST_INTERLEAVE_CHUNK) ok = false;
            for (uint32_t i = 0; i < FST_INTERLEAVE_CHUNK; i++) g_io[i] = fst_byte(5, off + i);
            if (ok && fs_write(&fb, g_io, FST_INTERLEAVE_CHUNK) != (int64_t)FST_INTERLEAVE_CHUNK) ok = false;
        }
        fs_close(&fa);
        fs_close(&fb);
        uint64_t total = (uint64_t)FST_INTERLEAVE_ROUNDS * FST_INTERLEAVE_CHUNK;
        fst_check("write two files in turns", ok);
        fst_check("verify first", fst_size(a) == (int64_t)total && fst_verify_pattern(a, 0, total, 4));
        fst_check("verify second", fst_size(b) == (int64_t)total && fst_verify_pattern(b, 0, total, 5));
    }

    fst_section("directories");
    fst_path(p, work, "sub");
    fst_check("mkdir sub", fs_mkdir(g_fs, p) == FS_OK);
    fst_path(q, p, "deeper");
    fst_check("mkdir sub/deeper", fs_mkdir(g_fs, q) == FS_OK);
    {
        char r[FS_MAX_PATH];
        fst_path(r, q, "nested.txt");
        fst_check("file in nested dir", fst_write_text(r, "nested\n", false) && fst_read_text(r, "nested\n"));
    }
    fst_check("mkdir existing fails", fs_mkdir(g_fs, p) == FS_ERR_EXIST);
    fst_check("rmdir non-empty fails", fs_rmdir(g_fs, p) == FS_ERR_NOTEMPTY);
    {
        fs_file_t f;
        fst_check("open dir as file fails", fs_open(g_fs, p, false, false, &f) == FS_ERR_ISDIR);
        fst_path(q, work, "missing.txt");
        fst_check("open missing fails", fs_open(g_fs, q, false, false, &f) == FS_ERR_NOENT);
    }
    fst_check("unlink missing fails", fs_unlink(g_fs, q) == FS_ERR_NOENT);
    {
        fs_dirent_t st;
        fst_check("stat dir", fs_stat(g_fs, p, &st) == FS_OK && st.type == FS_ENTRY_DIR);
    }

    fst_section("many files");
    fst_path(p, work, "many");
    fst_check("mkdir many", fs_mkdir(g_fs, p) == FS_OK);
    {
        bool ok = true;
        for (int i = 0; i < FST_MANY_FILES && ok; i++) {
            snprintf(name, sizeof(name), "file_with_a_rather_long_name_%02d.txt", i);
            fst_path(q, p, name);
            ok = fst_write_pattern(q, true, false, false, 0, 100 + (uint64_t)i, (uint32_t)(10 + i), 64);
        }
        fst_check("create 60 files", ok);
    }
    fst_check("count 60", fst_count_dir(p) == FST_MANY_FILES);
    {
        bool ok = true;
        for (int i = 0; i < FST_MANY_FILES && ok; i += 7) {
            snprintf(name, sizeof(name), "file_with_a_rather_long_name_%02d.txt", i);
            fst_path(q, p, name);
            ok = fst_size(q) == (int64_t)(100 + i) && fst_verify_pattern(q, 0, 100 + (uint64_t)i, (uint32_t)(10 + i));
        }
        fst_check("verify sample of files", ok);
    }
    {
        bool ok = true;
        for (int i = 0; i < FST_MANY_FILES && ok; i += 2) {
            snprintf(name, sizeof(name), "file_with_a_rather_long_name_%02d.txt", i);
            fst_path(q, p, name);
            ok = fs_unlink(g_fs, q) == FS_OK;
        }
        fst_check("unlink every second", ok);
    }
    fst_check("count 30", fst_count_dir(p) == FST_MANY_FILES / 2);
    {
        bool ok = true;
        for (int i = 0; i < 5 && ok; i++) {
            snprintf(name, sizeof(name), "again_%d.dat", i);
            fst_path(q, p, name);
            ok = fst_write_pattern(q, true, false, false, 0, 2000, (uint32_t)(80 + i), 512);
        }
        fst_check("reuse freed entries", ok);
    }
    fst_check("count 35", fst_count_dir(p) == FST_MANY_FILES / 2 + 5);
    {
        snprintf(name, sizeof(name), "file_with_a_rather_long_name_%02d.txt", 31);
        fst_path(q, p, name);
        bool ok = fst_verify_pattern(q, 0, 131, 41);
        fst_path(q, p, "again_3.dat");
        ok = ok && fst_verify_pattern(q, 0, 2000, 83);
        fst_check("old and new files intact", ok);
    }
    {
        snprintf(name, sizeof(name), "FILE_WITH_A_RATHER_LONG_NAME_%02d.TXT", 31);
        fst_path(q, p, name);
        bool folds = g_fs->type == FS_TYPE_FAT12 || g_fs->type == FS_TYPE_FAT16 ||
                     g_fs->type == FS_TYPE_FAT32 || g_fs->type == FS_TYPE_EXFAT;
        if (folds) fst_check("case-insensitive lookup", fst_size(q) == 131);
        else fst_check("case-sensitive lookup", fst_size(q) < 0);
    }

    fst_section("keep set");
    fst_check("mkdir fskeep", fs_mkdir(g_fs, keep) == FS_OK);
    fst_path(p, keep, "keep.txt");
    fst_check("write keep.txt", fst_write_text(p, "LuOS fs-test keep file\n", false));
    fst_path(p, keep, "keep.bin");
    fst_check("write keep.bin", fst_write_pattern(p, true, false, false, 0, FST_KEEP_BIN_SIZE, 42, 7000));
    fst_path(p, keep, "sub");
    fst_check("mkdir fskeep/sub", fs_mkdir(g_fs, p) == FS_OK);
    fst_path(q, p, "inner.txt");
    fst_check("write inner.txt", fst_write_text(q, "inner\n", false));
    {
        char a[FS_MAX_PATH], b[FS_MAX_PATH];
        fst_path(a, keep, "frag_a.bin");
        fst_path(b, keep, "frag_b.bin");
        fs_file_t fa, fb;
        bool ok = fs_open(g_fs, a, true, false, &fa) == FS_OK;
        if (ok) ok = fs_open(g_fs, b, true, false, &fb) == FS_OK;
        for (int round = 0; ok && round < 30; round++) {
            uint64_t off = (uint64_t)round * 5000u;
            for (uint32_t i = 0; i < 5000u; i++) g_io[i] = fst_byte(50, off + i);
            if (fs_write(&fa, g_io, 5000u) != 5000) ok = false;
            for (uint32_t i = 0; i < 5000u; i++) g_io[i] = fst_byte(51, off + i);
            if (ok && fs_write(&fb, g_io, 5000u) != 5000) ok = false;
        }
        fs_close(&fa);
        fs_close(&fb);
        fst_check("write fragmented pair", ok && fst_verify_pattern(a, 0, 150000, 50) && fst_verify_pattern(b, 0, 150000, 51));
    }
    fst_path(p, keep, "dir40");
    fst_check("mkdir fskeep/dir40", fs_mkdir(g_fs, p) == FS_OK);
    {
        bool ok = true;
        for (int i = 0; i < 40 && ok; i++) {
            snprintf(name, sizeof(name), "keep_file_with_long_name_%02d.dat", i);
            fst_path(q, p, name);
            ok = fst_write_pattern(q, true, false, false, 0, 300 + (uint64_t)i, (uint32_t)(100 + i), 128);
        }
        fst_check("write 40 kept files", ok && fst_count_dir(p) == 40);
    }
    fst_path(p, keep, "\xD1\x8E\xD0\xBD\xD0\xB8\xD0\xBA\xD0\xBE\xD0\xB4_\xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB.txt");
    fst_check("unicode file name", fst_write_text(p, "unicode\n", false) && fst_read_text(p, "unicode\n"));

    fst_section("cleanup");
    fst_check("remove fstest tree", fst_rmtree(work) == FS_OK);
    {
        fs_dirent_t st;
        fst_check("fstest is gone", fs_stat(g_fs, work, &st) == FS_ERR_NOENT);
    }
    fst_path(p, keep, "keep.bin");
    fst_check("keep.bin still intact", fst_verify_pattern(p, 0, FST_KEEP_BIN_SIZE, 42));
}

void cmd_fs_test(struct limine_video_driver* video, const char *arg) {
    g_video = video;
    g_pass = 0;
    g_fail = 0;

    char abs[FS_MAX_PATH];
    g_fs = fst_resolve(arg, abs, g_base);
    if (!g_fs) {
        video->printf("fs-test: nothing mounted for that path\n", LIMINE_COLOR_LIGHT_RED);
        return;
    }

    g_io = kmalloc(FST_IO_CHUNK);
    if (!g_io) return;

    LOG_INFO("fs-test begin %s (%s)", abs, rootfs_type_name(g_fs->type));
    video->printf("fs-test on ", LIMINE_COLOR_CYAN);
    video->printf(abs, LIMINE_COLOR_CYAN);
    video->printf(" (", LIMINE_COLOR_CYAN);
    video->printf(rootfs_type_name(g_fs->type), LIMINE_COLOR_CYAN);
    video->printf(")\n", LIMINE_COLOR_CYAN);

    char work[FS_MAX_PATH], keep[FS_MAX_PATH];
    fst_path(work, g_base, "fstest");
    fst_path(keep, g_base, "fskeep");
    fst_rmtree(work);
    fst_rmtree(keep);

    int r = fs_mkdir(g_fs, work);
    if (r == FS_ERR_NOSUPP) {
        LOG_INFO("fs-test: read-only filesystem, write tests skipped");
        video->printf("read-only filesystem, write tests skipped\n", LIMINE_COLOR_YELLOW);
    } else {
        fst_check("mkdir fstest", r == FS_OK);
        if (r == FS_OK) fst_run_writable();
    }

    LOG_INFO("fs-test end %s: %d passed, %d failed", abs, g_pass, g_fail);
    char line[96];
    snprintf(line, sizeof(line), "\nfs-test: %d passed, %d failed\n", g_pass, g_fail);
    video->printf(line, g_fail ? LIMINE_COLOR_LIGHT_RED : LIMINE_COLOR_LIGHT_GREEN);

    kfree(g_io);
    g_io = NULL;
}
