// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "shell_proc.h"

#include "components/logger.h"
#include "components/Memory/heap.h"
#include "drivers/Video/limine_video_driver.h"
#include "kernel/proc/initfs.h"
#include "kernel/proc/linux_abi.h"
#include "kernel/proc/process.h"
#include "kernel/proc/syscall.h"
#include "kernel/proc/vfs.h"
#include "kernel/lkpi/lkpi.h"
#include "kernel/rootfs.h"

#include <stdio.h>
#include <string.h>

#define SP_MAX_ARGS 32
#define SP_LINE 512

static const char *const default_env[] = {
    "PATH=/bin:/usr/bin:/sbin:/usr/sbin",
    "HOME=/",
    "TERM=linux",
    "USER=root",
    "LANG=C.UTF-8",
};

#define SP_MAX_ENV 48
#define SP_ENV_LEN 256

static char saved_env[SP_MAX_ENV][SP_ENV_LEN];
static int saved_env_count;

static size_t env_key_len(const char *kv) {
    const char *eq = strchr(kv, '=');
    return eq ? (size_t)(eq - kv) : strlen(kv);
}

static bool env_same_key(const char *a, const char *b) {
    size_t n = env_key_len(a);
    return n == env_key_len(b) && strncmp(a, b, n) == 0;
}

static int env_add(const char **env, int n, const char *kv) {
    for (int i = 0; i < n; i++) {
        if (env_same_key(env[i], kv)) {
            env[i] = kv;
            return n;
        }
    }
    if (n < SP_MAX_ENV) env[n++] = kv;
    return n;
}

static bool is_assignment(const char *s) {
    const char *eq = strchr(s, '=');
    if (!eq || eq == s) return false;
    for (const char *c = s; c < eq; c++)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
            return false;
    return true;
}

static void resolve_program(const char *name, char *abs, size_t cap) {
    static const char *const dirs[] = { "/usr/bin/", "/bin/", "/usr/sbin/", "/sbin/" };
    if (strchr(name, '/')) {
        rootfs_normalize_path(name, abs, cap);
        return;
    }
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char path[PROC_PATH_MAX];
        struct lx_stat st;
        snprintf(path, sizeof(path), "%s%s", dirs[i], name);
        if (vfs_stat(path, &st) == 0) {
            strncpy(abs, path, cap - 1);
            abs[cap - 1] = '\0';
            return;
        }
    }
    rootfs_normalize_path(name, abs, cap);
}

static void cmd_setenv(const char *kv) {
    while (*kv == ' ') kv++;
    if (!is_assignment(kv) || strlen(kv) >= SP_ENV_LEN) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "usage: setenv NAME=VALUE\n");
        return;
    }
    for (int i = 0; i < saved_env_count; i++) {
        if (env_same_key(saved_env[i], kv)) {
            strcpy(saved_env[i], kv);
            return;
        }
    }
    if (saved_env_count >= SP_MAX_ENV) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "setenv: too many variables\n");
        return;
    }
    strcpy(saved_env[saved_env_count++], kv);
}

static void cmd_unsetenv(const char *name) {
    while (*name == ' ') name++;
    for (int i = 0; i < saved_env_count; i++) {
        if (env_key_len(saved_env[i]) == strlen(name) && strncmp(saved_env[i], name, strlen(name)) == 0) {
            memmove(saved_env[i], saved_env[saved_env_count - 1], SP_ENV_LEN);
            saved_env_count--;
            return;
        }
    }
}

static void cmd_env(void) {
    for (size_t i = 0; i < sizeof(default_env) / sizeof(default_env[0]); i++)
        printf_color(LIMINE_COLOR_DARK_GRAY, "  %s\n", default_env[i]);
    for (int i = 0; i < saved_env_count; i++) printf_color(LIMINE_COLOR_LIGHT_GRAY, "  %s\n", saved_env[i]);
}

static const char *errno_name(int e) {
    switch (e) {
        case LX_ENOENT: return "no such file";
        case LX_ENOEXEC: return "not an x86-64 ELF executable";
        case LX_EACCES: return "permission denied / not a regular file";
        case LX_ENOMEM: return "out of memory";
        case LX_E2BIG: return "argument list too long";
        case LX_EISDIR: return "is a directory";
        default: return "error";
    }
}

static int split_args(char *line, char **argv) {
    int argc = 0;
    char *p = line;
    while (*p && argc < SP_MAX_ARGS) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *out = p;
        char *start = p;
        char quote = 0;
        while (*p && (quote || *p != ' ')) {
            if (!quote && (*p == '"' || *p == '\'')) { quote = *p++; continue; }
            if (quote && *p == quote) { quote = 0; p++; continue; }
            *out++ = *p++;
        }
        if (*p) p++;
        *out = '\0';
        argv[argc++] = start;
    }
    return argc;
}

static void cmd_exec(const char *args) {
    char line[SP_LINE];
    strncpy(line, args, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    char *words[SP_MAX_ARGS];
    int nwords = split_args(line, words);
    const char *env[SP_MAX_ENV];
    int envc = 0;
    for (size_t i = 0; i < sizeof(default_env) / sizeof(default_env[0]); i++) envc = env_add(env, envc, default_env[i]);
    for (int i = 0; i < saved_env_count; i++) envc = env_add(env, envc, saved_env[i]);
    int first = 0;
    while (first < nwords && is_assignment(words[first])) envc = env_add(env, envc, words[first++]);
    char **argv = words + first;
    int argc = nwords - first;
    if (argc == 0) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "usage: exec [NAME=VALUE...] PATH [ARGS...]\n");
        return;
    }

    char abs[PROC_PATH_MAX];
    resolve_program(argv[0], abs, sizeof(abs));

    struct process *p = NULL;
    uint64_t t0 = sys_monotonic_ns();
    int pid = proc_spawn(abs, argc, (const char *const *)argv, envc, env, &p);
    if (pid < 0) {
        printf_color(LIMINE_COLOR_LIGHT_RED, "exec %s: %s (errno %d)\n", abs, errno_name(-pid), -pid);
        return;
    }
    int status = 0;
    proc_wait(p, &status);
    uint64_t ms = (sys_monotonic_ns() - t0) / 1000000ULL;
    proc_put(p);

    if (status & PROC_EXIT_SIGNAL)
        printf_color(LIMINE_COLOR_LIGHT_RED, "[pid %d killed by signal %d after %llu ms]\n", pid, status & 0x7f,
                     (unsigned long long)ms);
    else if (status)
        printf_color(LIMINE_COLOR_AMBER, "[pid %d exited with %d after %llu ms]\n", pid, status, (unsigned long long)ms);
    else
        printf_color(LIMINE_COLOR_DARK_GRAY, "[pid %d exited with 0 after %llu ms]\n", pid, (unsigned long long)ms);
}

static void cmd_procs(void) {
    int n = proc_count();
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "%d process(es), %llu syscalls so far\n", n,
                 (unsigned long long)syscall_count());
    for (int i = 0; i < n; i++) {
        struct process *p = proc_at(i);
        if (!p) continue;
        printf_color(LIMINE_COLOR_LIGHT_GRAY, "  pid %-5d threads %-3d resident %6llu KiB  %s%s\n", p->pid,
                     p->threads, (unsigned long long)(p->mm.resident_pages * 4), p->exe,
                     p->done ? " (exited)" : "");
    }
}

static void emit_linux_line(const char *line) {
    printf_color(LIMINE_COLOR_LIGHT_GRAY, "  %s\n", line);
}

static void cmd_initfs(void) {
    uint32_t entries;
    uint64_t image, files, cached;
    initfs_stats(&entries, &image, &files, &cached);
    if (!entries) {
        printf_color(LIMINE_COLOR_AMBER, "no built-in filesystem in this kernel\n");
        return;
    }
    printf_color(LIMINE_COLOR_AMBER, "built-in filesystem: %u entries, %llu MiB of files in a %llu MiB image, %llu MiB unpacked in RAM\n",
                 entries, (unsigned long long)(files >> 20), (unsigned long long)(image >> 20),
                 (unsigned long long)(cached >> 20));
}

static void cmd_drm_debug(const char *arg) {
    unsigned long mask = 0;
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) arg += 2;
    for (; *arg; arg++) {
        char c = *arg;
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) break;
        mask = mask * 16 + (unsigned long)v;
    }
    lkpi_set_drm_debug(mask);
    printf_color(LIMINE_COLOR_AMBER, "drm.debug = 0x%lx (to the kernel log)\n", mask);
}

static void cmd_linux_devs(void) {
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "Linux drivers (LinuxKPI):\n");
    lkpi_status(emit_linux_line);
    int n = vfs_devnode_count();
    printf_color(LIMINE_COLOR_LIGHT_BLUE, "%d device node(s):\n", n);
    for (int i = 0; i < n; i++) {
        const char *path = vfs_devnode_path(i);
        if (path) printf_color(LIMINE_COLOR_LIGHT_GRAY, "  %s\n", path);
    }
}

bool shell_proc_command(const char *cmd) {
    if (strncmp(cmd, "exec ", 5) == 0) { cmd_exec(cmd + 5); return true; }
    if (strcmp(cmd, "procs") == 0) { cmd_procs(); return true; }
    if (strcmp(cmd, "linux-devs") == 0) { cmd_linux_devs(); return true; }
    if (strncmp(cmd, "drm-debug ", 10) == 0) { cmd_drm_debug(cmd + 10); return true; }
    if (strcmp(cmd, "initfs") == 0) { cmd_initfs(); return true; }
    if (strncmp(cmd, "setenv ", 7) == 0) { cmd_setenv(cmd + 7); return true; }
    if (strncmp(cmd, "unsetenv ", 9) == 0) { cmd_unsetenv(cmd + 9); return true; }
    if (strcmp(cmd, "env") == 0) { cmd_env(); return true; }
    if (strcmp(cmd, "strace on") == 0 || strcmp(cmd, "strace off") == 0) {
        syscall_set_trace(cmd[8] == 'n');
        printf_color(LIMINE_COLOR_AMBER, "syscall tracing %s (to the kernel log)\n", syscall_get_trace() ? "on" : "off");
        return true;
    }
    return false;
}

void help_row(uint32_t color, const char *left, const char *right);
void help_section(const char *title);

void shell_proc_help(void) {
    help_section("User programs (Linux x86-64 ELF, built-in Mesa)");
    help_row(LIMINE_COLOR_AMBER, "exec [VAR=V] PROG [ARGS] - run, wait", "procs - running processes");
    help_row(LIMINE_COLOR_AMBER, "setenv VAR=V / unsetenv VAR - env", "env - environment for exec");
    help_row(LIMINE_COLOR_AMBER, "strace on|off - log every syscall", "linux-devs - Linux drivers, /dev");
    help_row(LIMINE_COLOR_AMBER, "drm-debug MASK - Linux drm.debug (hex)", "initfs - built-in Mesa filesystem");
}
