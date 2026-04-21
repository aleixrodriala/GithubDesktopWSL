/*
 * wsl-git-daemon — Persistent WSL daemon for GitHub Desktop WSL fork.
 *
 * Runs inside WSL, listens on TCP localhost. GitHub Desktop connects from
 * Windows to execute git commands and file operations without 9P overhead.
 *
 * Wire protocol: binary length-prefixed frames over TCP.
 *   [1 byte type][4 bytes payload length (big-endian)][payload]
 *
 * Frame types:
 *   0x01 INIT       — client→daemon: JSON { "token": "...", "cmd": "git|readfile|writefile|stat|pathexists", "args": [...], "cwd": "...", "env": {...} }
 *   0x02 STDIN      — client→daemon: raw bytes for git stdin
 *   0x03 STDOUT     — daemon→client: raw bytes from git stdout / file content
 *   0x04 STDERR     — daemon→client: raw bytes from git stderr
 *   0x05 EXIT       — daemon→client: 4-byte exit code (big-endian)
 *   0x06 ERROR      — daemon→client: UTF-8 error message
 *   0x07 STAT_RESULT — daemon→client: JSON { "exists": bool, "size": number, "isDir": bool }
 *
 * Auth: on startup writes JSON to /tmp/wsl-git-daemon.info:
 *   { "port": <port>, "token": "<random hex>" }
 * Client must send matching token in INIT frame.
 *
 * Build: gcc -O2 -o wsl-git-daemon daemon.c -lpthread
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

/* Frame types */
#define FRAME_INIT        0x01
#define FRAME_STDIN       0x02
#define FRAME_STDOUT      0x03
#define FRAME_STDERR      0x04
#define FRAME_EXIT        0x05
#define FRAME_ERROR       0x06
#define FRAME_STAT_RESULT 0x07

#define MAX_FRAME_SIZE (64 * 1024 * 1024)  /* 64 MB max frame */
#define TOKEN_LEN 32
#define INFO_PATH "/tmp/wsl-git-daemon.info"

static char g_token[TOKEN_LEN * 2 + 1];
static volatile int g_running = 1;

/* --- Utility --- */

static void generate_token(char *buf, size_t hex_len) {
    FILE *f = fopen("/dev/urandom", "r");
    if (!f) { perror("urandom"); exit(1); }
    for (size_t i = 0; i < hex_len / 2; i++) {
        int c = fgetc(f);
        sprintf(buf + i * 2, "%02x", (unsigned char)c);
    }
    buf[hex_len] = '\0';
    fclose(f);
}

static void write_info_file(int port) {
    FILE *f = fopen(INFO_PATH, "w");
    if (!f) { perror("write info"); exit(1); }
    fprintf(f, "{\"port\":%d,\"token\":\"%s\"}\n", port, g_token);
    fclose(f);
    chmod(INFO_PATH, 0600);
}

static void sighandler(int sig) {
    (void)sig;
    g_running = 0;
}

/* --- Frame I/O --- */

static int read_exact(int fd, void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0) return -1;
        done += r;
    }
    return 0;
}

static int write_exact(int fd, const void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, (const char *)buf + done, n - done);
        if (w <= 0) return -1;
        done += w;
    }
    return 0;
}

static int read_frame(int fd, uint8_t *type, uint8_t **payload, uint32_t *len) {
    uint8_t hdr[5];
    if (read_exact(fd, hdr, 5) < 0) return -1;
    *type = hdr[0];
    *len = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16) |
           ((uint32_t)hdr[3] << 8) | (uint32_t)hdr[4];
    if (*len > MAX_FRAME_SIZE) return -1;
    if (*len == 0) { *payload = NULL; return 0; }
    *payload = malloc(*len);
    if (!*payload) return -1;
    if (read_exact(fd, *payload, *len) < 0) { free(*payload); return -1; }
    return 0;
}

static int send_frame(int fd, uint8_t type, const void *data, uint32_t len) {
    uint8_t hdr[5];
    hdr[0] = type;
    hdr[1] = (len >> 24) & 0xFF;
    hdr[2] = (len >> 16) & 0xFF;
    hdr[3] = (len >> 8) & 0xFF;
    hdr[4] = len & 0xFF;
    if (write_exact(fd, hdr, 5) < 0) return -1;
    if (len > 0 && write_exact(fd, data, len) < 0) return -1;
    return 0;
}

static int send_error(int fd, const char *msg) {
    return send_frame(fd, FRAME_ERROR, msg, strlen(msg));
}

static int send_exit(int fd, int code) {
    uint8_t buf[4];
    buf[0] = (code >> 24) & 0xFF;
    buf[1] = (code >> 16) & 0xFF;
    buf[2] = (code >> 8) & 0xFF;
    buf[3] = code & 0xFF;
    return send_frame(fd, FRAME_EXIT, buf, 4);
}

/* --- Minimal JSON parser (good enough for our protocol) --- */

static int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = (unsigned char)p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

/* Encode a BMP codepoint as UTF-8. Returns bytes written (1-3). */
static size_t utf8_encode(int cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* Binary-safe JSON string extraction.
 * Decodes standard escapes plus \uXXXX (as UTF-8). The result may contain
 * embedded NULs (e.g. from \u0000) — pass the returned length to any consumer
 * instead of using strlen. Returns bytes written, or -1 if the key is missing. */
static ssize_t json_find_string_bin(const char *json, const char *key, char *out, size_t out_sz) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_sz) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; p++; break;
                case 'r': out[i++] = '\r'; p++; break;
                case 't': out[i++] = '\t'; p++; break;
                case 'b': out[i++] = '\b'; p++; break;
                case 'f': out[i++] = '\f'; p++; break;
                case '\\': out[i++] = '\\'; p++; break;
                case '"': out[i++] = '"'; p++; break;
                case '/': out[i++] = '/'; p++; break;
                case 'u': {
                    p++;
                    int cp = hex4(p);
                    if (cp < 0) break;
                    p += 4;
                    size_t need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : 3;
                    if (i + need > out_sz) goto done;
                    i += utf8_encode(cp, out + i);
                    break;
                }
                default: out[i++] = *p; p++; break;
            }
        } else {
            out[i++] = *p++;
        }
    }
done:
    return (ssize_t)i;
}

/* Null-terminated string extraction for fields that cannot contain NULs
 * (token, cwd, cmd, path). Reserves 1 byte for the terminator. */
static const char *json_find_string(const char *json, const char *key, char *out, size_t out_sz) {
    if (out_sz == 0) return NULL;
    ssize_t n = json_find_string_bin(json, key, out, out_sz - 1);
    if (n < 0) return NULL;
    out[n] = '\0';
    return out;
}

/* Find a JSON array "key": ["val1", "val2", ...] and return items */
static int json_find_string_array(const char *json, const char *key, char ***out, int *count) {
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '[') return -1;
    p++;

    *count = 0;
    *out = NULL;
    int cap = 0;

    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '"') return -1;
        p++;
        const char *start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && *(p+1)) p++;
            p++;
        }
        size_t len = p - start;
        if (*p == '"') p++;

        if (*count >= cap) {
            cap = cap ? cap * 2 : 16;
            *out = realloc(*out, cap * sizeof(char *));
        }
        (*out)[*count] = malloc(len + 1);
        memcpy((*out)[*count], start, len);
        (*out)[*count][len] = '\0';
        (*count)++;
    }
    return 0;
}

/* --- Command handlers --- */

static void handle_git(int cfd, const char *json) {
    char cwd[4096] = "/";
    char **args = NULL;
    int argc = 0;

    /* stdin may be binary (e.g. NUL-separated paths for `update-index -z --stdin`)
     * and arbitrarily large. Size the buffer from the JSON length upper bound. */
    size_t stdin_cap = strlen(json) + 1;
    char *stdin_buf = malloc(stdin_cap);
    if (!stdin_buf) {
        send_error(cfd, "malloc failed");
        return;
    }
    ssize_t stdin_len = json_find_string_bin(json, "stdin", stdin_buf, stdin_cap);
    if (stdin_len < 0) stdin_len = 0;

    json_find_string(json, "cwd", cwd, sizeof(cwd));
    json_find_string_array(json, "args", &args, &argc);

    /* Build argv: ["git", args...] */
    char **argv = calloc(argc + 2, sizeof(char *));
    argv[0] = "git";
    for (int i = 0; i < argc; i++) argv[i + 1] = args[i];
    argv[argc + 1] = NULL;

    int pipe_out[2], pipe_err[2], pipe_in[2];
    if (pipe(pipe_out) < 0 || pipe(pipe_err) < 0 || pipe(pipe_in) < 0) {
        send_error(cfd, "pipe() failed");
        goto cleanup;
    }

    pid_t pid = fork();
    if (pid < 0) {
        send_error(cfd, "fork() failed");
        goto cleanup;
    }

    if (pid == 0) {
        /* Child — exec git */
        close(pipe_out[0]);
        close(pipe_err[0]);
        close(pipe_in[1]);
        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        dup2(pipe_err[1], STDERR_FILENO);
        close(pipe_in[0]);
        close(pipe_out[1]);
        close(pipe_err[1]);

        /* Strip SSH_ASKPASS and DISPLAY — WSL ssh uses native keys */
        unsetenv("SSH_ASKPASS");
        unsetenv("DISPLAY");
        unsetenv("GIT_ASKPASS");

        if (chdir(cwd) < 0) {
            fprintf(stderr, "chdir(%s): %s\n", cwd, strerror(errno));
            _exit(128);
        }
        execvp("git", argv);
        fprintf(stderr, "exec(git): %s\n", strerror(errno));
        _exit(127);
    }

    /* Parent — write stdin to child if provided, then relay stdout/stderr */
    close(pipe_out[1]);
    close(pipe_err[1]);
    close(pipe_in[0]);
    if (stdin_len > 0) {
        write_exact(pipe_in[1], stdin_buf, (size_t)stdin_len);
    }
    close(pipe_in[1]); /* Signal EOF to child's stdin */

    /* Use non-blocking reads with select */
    fd_set fds;
    int maxfd = (pipe_out[0] > pipe_err[0] ? pipe_out[0] : pipe_err[0]) + 1;
    int out_open = 1, err_open = 1;
    char buf[65536];

    while (out_open || err_open) {
        FD_ZERO(&fds);
        if (out_open) FD_SET(pipe_out[0], &fds);
        if (err_open) FD_SET(pipe_err[0], &fds);

        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        int r = select(maxfd, &fds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue; /* timeout, keep waiting */

        if (out_open && FD_ISSET(pipe_out[0], &fds)) {
            ssize_t n = read(pipe_out[0], buf, sizeof(buf));
            if (n > 0) send_frame(cfd, FRAME_STDOUT, buf, n);
            else out_open = 0;
        }
        if (err_open && FD_ISSET(pipe_err[0], &fds)) {
            ssize_t n = read(pipe_err[0], buf, sizeof(buf));
            if (n > 0) send_frame(cfd, FRAME_STDERR, buf, n);
            else err_open = 0;
        }
    }

    close(pipe_out[0]);
    close(pipe_err[0]);

    int status;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    send_exit(cfd, exit_code);

cleanup:
    if (args) {
        for (int i = 0; i < argc; i++) free(args[i]);
        free(args);
    }
    free(argv);
    free(stdin_buf);
}

static void handle_readfile(int cfd, const char *json) {
    char path[4096] = "";
    json_find_string(json, "path", path, sizeof(path));

    if (path[0] == '\0') {
        send_error(cfd, "missing path");
        return;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char msg[4200];
        snprintf(msg, sizeof(msg), "open(%s): %s", path, strerror(errno));
        send_error(cfd, msg);
        return;
    }

    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (send_frame(cfd, FRAME_STDOUT, buf, n) < 0) break;
    }
    close(fd);
    send_exit(cfd, 0);
}

static void handle_writefile(int cfd, const char *json) {
    char path[4096] = "";
    json_find_string(json, "path", path, sizeof(path));

    if (path[0] == '\0') {
        send_error(cfd, "missing path");
        return;
    }

    /* The file content comes as subsequent STDIN frames, or inline in "data" field */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        char msg[4200];
        snprintf(msg, sizeof(msg), "open(%s): %s", path, strerror(errno));
        send_error(cfd, msg);
        return;
    }

    /* Read STDIN frames until we get one with 0 length (EOF signal) or connection drops */
    uint8_t type;
    uint8_t *payload;
    uint32_t len;
    while (read_frame(cfd, &type, &payload, &len) == 0) {
        if (type == FRAME_STDIN) {
            if (len == 0) { free(payload); break; }  /* EOF */
            write_exact(fd, payload, len);
            free(payload);
        } else {
            free(payload);
            break;
        }
    }

    close(fd);
    send_exit(cfd, 0);
}

static void handle_stat(int cfd, const char *json) {
    char path[4096] = "";
    json_find_string(json, "path", path, sizeof(path));

    struct stat st;
    int exists = (stat(path, &st) == 0);

    char result[256];
    if (exists) {
        snprintf(result, sizeof(result),
                 "{\"exists\":true,\"size\":%lld,\"isDir\":%s}",
                 (long long)st.st_size,
                 S_ISDIR(st.st_mode) ? "true" : "false");
    } else {
        snprintf(result, sizeof(result), "{\"exists\":false,\"size\":0,\"isDir\":false}");
    }
    send_frame(cfd, FRAME_STAT_RESULT, result, strlen(result));
    send_exit(cfd, 0);
}

static void handle_pathexists(int cfd, const char *json) {
    char path[4096] = "";
    json_find_string(json, "path", path, sizeof(path));

    struct stat st;
    int exists = (stat(path, &st) == 0);

    char result[64];
    snprintf(result, sizeof(result), "{\"exists\":%s}", exists ? "true" : "false");
    send_frame(cfd, FRAME_STAT_RESULT, result, strlen(result));
    send_exit(cfd, 0);
}

static void handle_unlink(int cfd, const char *json) {
    char path[4096] = "";
    json_find_string(json, "path", path, sizeof(path));

    if (path[0] == '\0') {
        send_error(cfd, "missing path");
        return;
    }

    if (unlink(path) < 0 && errno != ENOENT) {
        char msg[4200];
        snprintf(msg, sizeof(msg), "unlink(%s): %s", path, strerror(errno));
        send_error(cfd, msg);
        return;
    }
    send_exit(cfd, 0);
}

/* --- Connection handler --- */

static void *handle_client(void *arg) {
    int cfd = (int)(intptr_t)arg;

    uint8_t type;
    uint8_t *payload;
    uint32_t len;

    if (read_frame(cfd, &type, &payload, &len) < 0 || type != FRAME_INIT) {
        close(cfd);
        return NULL;
    }

    /* Null-terminate the JSON payload */
    char *json = malloc(len + 1);
    memcpy(json, payload, len);
    json[len] = '\0';
    free(payload);

    /* Verify token */
    char token[TOKEN_LEN * 2 + 1] = "";
    json_find_string(json, "token", token, sizeof(token));
    if (strcmp(token, g_token) != 0) {
        send_error(cfd, "invalid token");
        free(json);
        close(cfd);
        return NULL;
    }

    /* Dispatch command */
    char cmd[64] = "";
    json_find_string(json, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "git") == 0) {
        handle_git(cfd, json);
    } else if (strcmp(cmd, "readfile") == 0) {
        handle_readfile(cfd, json);
    } else if (strcmp(cmd, "writefile") == 0) {
        handle_writefile(cfd, json);
    } else if (strcmp(cmd, "stat") == 0) {
        handle_stat(cfd, json);
    } else if (strcmp(cmd, "pathexists") == 0) {
        handle_pathexists(cfd, json);
    } else if (strcmp(cmd, "unlink") == 0) {
        handle_unlink(cfd, json);
    } else {
        send_error(cfd, "unknown command");
    }

    free(json);
    close(cfd);
    return NULL;
}

/* --- Main --- */

int main(int argc, char *argv[]) {
    int daemonize = 0;
    (void)argc;
    for (int i = 1; argv[i]; i++) {
        if (strcmp(argv[i], "--daemonize") == 0 || strcmp(argv[i], "-d") == 0)
            daemonize = 1;
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    generate_token(g_token, TOKEN_LEN * 2);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = 0,  /* OS picks a port */
    };

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }

    socklen_t alen = sizeof(addr);
    getsockname(sfd, (struct sockaddr *)&addr, &alen);
    int port = ntohs(addr.sin_port);

    if (listen(sfd, 16) < 0) { perror("listen"); return 1; }

    write_info_file(port);

    if (daemonize) {
        /* Fork into background. Parent exits immediately so wsl.exe returns. */
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) {
            /* Parent: info file is written, daemon is ready. Exit cleanly. */
            printf("%d\n", pid);
            return 0;
        }
        /* Child: become session leader, close stdio */
        setsid();
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    } else {
        printf("wsl-git-daemon listening on 127.0.0.1:%d\n", port);
        printf("info: %s\n", INFO_PATH);
        fflush(stdout);
    }

    while (g_running) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int cfd = accept(sfd, (struct sockaddr *)&client, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        /* Only accept connections from localhost */
        if (ntohl(client.sin_addr.s_addr) != INADDR_LOOPBACK) {
            close(cfd);
            continue;
        }

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, handle_client, (void *)(intptr_t)cfd);
        pthread_attr_destroy(&attr);
    }

    unlink(INFO_PATH);
    close(sfd);
    printf("wsl-git-daemon stopped\n");
    return 0;
}
