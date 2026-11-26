#define _POSIX_C_SOURCE 200809L

#include "secret.h"
#include "str.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char *get_key_file_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    DkStr p;
    dk_str_init_cap(&p, 128);
    if (xdg && *xdg) {
        dk_str_appendf(&p, "%s/dkchatebras/api_key", xdg);
    } else {
        const char *home = getenv("HOME");
        dk_str_appendf(&p, "%s/.config/dkchatebras/api_key", home ? home : ".");
    }
    return dk_str_steal(&p);
}

static bool ensure_parent_dir(const char *file_path) {
    char *copy = dk_strdup(file_path);
    if (!copy) return false;
    char *slash = strrchr(copy, '/');
    if (!slash) {
        free(copy);
        return true;
    }
    *slash = '\0';
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(copy, 0700);
            *p = '/';
        }
    }
    bool ok = (mkdir(copy, 0700) == 0 || errno == EEXIST);
    free(copy);
    return ok;
}

static int run_secret_tool(char *const argv[], const char *stdin_data, char *stdout_buf, size_t stdout_cap) {
    int in_pipe[2] = { -1, -1 };
    int out_pipe[2] = { -1, -1 };

    if (stdin_data && pipe(in_pipe) != 0) return -1;
    if (stdout_buf && pipe(out_pipe) != 0) {
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        if (out_pipe[0] >= 0) { close(out_pipe[0]); close(out_pipe[1]); }
        return -1;
    }

    if (pid == 0) {
        if (stdin_data) {
            dup2(in_pipe[0], STDIN_FILENO);
            close(in_pipe[0]);
            close(in_pipe[1]);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        }
        if (stdout_buf) {
            dup2(out_pipe[1], STDOUT_FILENO);
            close(out_pipe[0]);
            close(out_pipe[1]);
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
        }
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp("secret-tool", argv);
        _exit(127);
    }

    if (stdin_data) {
        close(in_pipe[0]);
        size_t len = strlen(stdin_data);
        size_t off = 0;
        while (off < len) {
            ssize_t n = write(in_pipe[1], stdin_data + off, len - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
        close(in_pipe[1]);
    }

    if (stdout_buf && stdout_cap > 0) {
        close(out_pipe[1]);
        size_t total = 0;
        while (total + 1 < stdout_cap) {
            ssize_t n = read(out_pipe[0], stdout_buf + total, stdout_cap - 1 - total);
            if (n <= 0) break;
            total += (size_t)n;
        }
        stdout_buf[total] = '\0';
        close(out_pipe[0]);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

char *dk_secret_lookup(void) {
    const char *env = getenv("CEREBRAS_API_KEY");
    if (env && *env)
        return dk_strdup(env);

    /* 1. Try Secret Service (GNOME Keyring / KWallet via secret-tool) */
    char buf[512];
    char *const argv[] = { "secret-tool", "lookup", "service", "cerebras-api", NULL };
    if (run_secret_tool(argv, NULL, buf, sizeof(buf)) == 0) {
        trim_crlf(buf);
        if (buf[0] != '\0') {
            char *res = dk_strdup(buf);
            memset(buf, 0, sizeof(buf));
            return res;
        }
    }

    /* 2. Fallback: 0600 local config file */
    char *path = get_key_file_path();
    int fd = open(path, O_RDONLY);
    free(path);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            trim_crlf(buf);
            if (buf[0] != '\0') {
                char *res = dk_strdup(buf);
                memset(buf, 0, sizeof(buf));
                return res;
            }
        }
    }

    memset(buf, 0, sizeof(buf));
    return NULL;
}

bool dk_secret_store(const char *api_key, char *err, size_t err_cap, bool *used_keyring) {
    if (used_keyring) *used_keyring = false;
    if (!api_key || !*api_key) {
        if (err && err_cap) snprintf(err, err_cap, "API key is empty");
        return false;
    }

    /* Try Secret Service first (passes secret via stdin pipe, never argv) */
    char *const argv[] = {
        "secret-tool", "store",
        "--label=DKChatebras Cerebras API key",
        "service", "cerebras-api", NULL
    };
    if (run_secret_tool(argv, api_key, NULL, 0) == 0) {
        if (used_keyring) *used_keyring = true;
        /* Remove stale fallback file if present */
        char *path = get_key_file_path();
        unlink(path);
        free(path);
        return true;
    }

    /* Fallback to 0600 file in XDG_CONFIG_HOME/dkchatebras/api_key */
    char *path = get_key_file_path();
    if (!ensure_parent_dir(path)) {
        if (err && err_cap) snprintf(err, err_cap, "Could not create config directory");
        free(path);
        return false;
    }

    DkStr tmp;
    dk_str_init_cap(&tmp, strlen(path) + 24);
    dk_str_appendf(&tmp, "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp.data, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        if (err && err_cap) snprintf(err, err_cap, "Could not save API key: %s", strerror(errno));
        dk_str_free(&tmp);
        free(path);
        return false;
    }
    size_t len = strlen(api_key);
    ssize_t w = write(fd, api_key, len);
    fchmod(fd, 0600);
    fsync(fd);
    close(fd);
    if (w != (ssize_t)len || rename(tmp.data, path) != 0) {
        unlink(tmp.data);
        if (err && err_cap) snprintf(err, err_cap, "Could not save API key");
        dk_str_free(&tmp);
        free(path);
        return false;
    }
    dk_str_free(&tmp);
    free(path);
    return true;
}

bool dk_secret_clear(void) {
    char *const argv[] = { "secret-tool", "clear", "service", "cerebras-api", NULL };
    run_secret_tool(argv, NULL, NULL, 0);
    char *path = get_key_file_path();
    unlink(path);
    free(path);
    return true;
}

void dk_secret_free(char *key) {
    if (!key) return;
    volatile char *p = key;
    while (*p) *p++ = 0;
    free(key);
}
