/*
 * lcm_tcp_listener.c - px2-300d LCM TCP JSON frame listener
 *
 * Listens on <ip>:<port> (default 127.0.0.1:6776), accepts one
 * JSON object per line, extracts a base64-encoded frame buffer,
 * and writes it to the 128x64 LCM via /dev/lcm ioctl.
 *
 * Threading model:
 *   main       - watchdog. Checks listener / display heartbeats
 *                every 2 seconds; respawns a dead thread.
 *   listener   - socket accept + JSON parse + base64 decode.
 *                Pushes decoded frame into a shared buffer.
 *   display    - waits on a condvar, writes the latest frame
 *                to /dev/lcm with the kernel ioctl ABI.
 *
 * Usage:
 *   sudo ./lcm_tcp_listener [-b bind_ip] [-p port]
 *   sudo ./lcm_tcp_listener -b 0.0.0.0 -p 6776
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "base64.h"
#include "lcm_uapi.h"
#include "mincjson.h"

/* ---------- constants ---------- */
#define FRAME_SIZE          (128 * 64 / 8)   /* 1024 bytes */
#define LCM_DEVICE          "/dev/lcm"
#define DEFAULT_BIND_IP     "127.0.0.1"
#define DEFAULT_PORT        6776
#define HB_CHECK_INTERVAL   2   /* seconds */
#define RECV_LINE_MAX       65536
#define DISPLAY_HB_TICK_SEC 1   /* max idle seconds before display thread ticks heartbeat */

/* ---------- shared state ---------- */
static uint8_t  g_frame[FRAME_SIZE];
static int      g_frame_ready;          /* 1 when listener has pushed a new frame */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond  = PTHREAD_COND_INITIALIZER;
static volatile int    g_shutdown;

/* config (written once at startup, read-only afterwards) */
static char g_bind_ip[64] = DEFAULT_BIND_IP;
static int  g_port        = DEFAULT_PORT;

/* thread handles & heartbeats */
static pthread_t g_listener_tid;
static pthread_t g_display_tid;
static volatile int g_listener_hb;
static volatile int g_display_hb;

/* socket used by listener thread - created at thread start */
static int g_listener_sock = -1;

/* pending backlight request (-1 = no change, 0/1/2 = set) */
static int g_pending_backlight = -1;

/* ---------- helpers ---------- */

static int g_verbose;   /* LCM_VERBOSE=1 时启用 DEBUG 日志 */

static void log_msg(const char *level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%s] %s\n", level, buf);
}

#define LOG_INFO(...)  log_msg("INFO",  __VA_ARGS__)
#define LOG_WARN(...)  log_msg("WARN",  __VA_ARGS__)
#define LOG_ERR(...)   log_msg("ERROR", __VA_ARGS__)
#define LOG_DEBUG(...) do { if (g_verbose) log_msg("DEBUG", __VA_ARGS__); } while (0)

static void on_signal(int sig)
{
    (void)sig;
    g_shutdown = 1;
    /* shutdown the listen socket so accept() returns */
    if (g_listener_sock >= 0) {
        shutdown(g_listener_sock, SHUT_RDWR);
    }
    pthread_cond_broadcast(&g_cond);
}

/* Read one '\n'-terminated line from fd into dst (size bytes).
 * Returns: number of bytes written (excluding NUL) on success,
 *          -1 on EAGAIN / timeout (empty line),
 *          -2 on connection closed,
 *          -3 on buffer overflow.
 */
static int read_line(int fd, char *dst, size_t size, long timeout_ms)
{
    size_t got = 0;
    while (got + 1 < size) {
        char c;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval tv;
        struct timeval *ptv = NULL;
        if (timeout_ms > 0) {
            tv.tv_sec  = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            ptv = &tv;
        }

        int rc = select(fd + 1, &rfds, NULL, NULL, ptv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rc == 0) return -1;  /* timeout */

        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -2;
        if (c == '\n') break;
        if (c == '\r') continue;
        dst[got++] = c;
    }
    dst[got] = '\0';
    if (got + 1 >= size) return -3;
    return (int)got;
}

/* ---------- display thread ---------- */

static void *display_thread(void *arg)
{
    (void)arg;
    LOG_INFO("display thread started");

    int fd = open(LCM_DEVICE, O_RDWR);
    if (fd < 0) {
        LOG_ERR("open %s: %s", LCM_DEVICE, strerror(errno));
        pthread_exit(NULL);
    }

    ioctl(fd, WIX_LCM_IOC_PON, 0);
    int bl = WIX_LCM_BL_ON;
    ioctl(fd, WIX_LCM_IOC_BL_SET, &bl);

    while (!g_shutdown) {
        /* tick heartbeat even while idle — so watchdog can tell
         * "alive and waiting for work" apart from "truly stuck" */
        g_display_hb++;

        pthread_mutex_lock(&g_mutex);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += DISPLAY_HB_TICK_SEC;
        while (!g_frame_ready && !g_shutdown) {
            pthread_cond_timedwait(&g_cond, &g_mutex, &ts);
            if (!g_frame_ready && !g_shutdown) {
                /* timed out — bump hb and re-arm */
                g_display_hb++;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += DISPLAY_HB_TICK_SEC;
            }
        }

        if (g_shutdown) {
            pthread_mutex_unlock(&g_mutex);
            break;
        }

        uint8_t local[FRAME_SIZE];
        memcpy(local, g_frame, FRAME_SIZE);
        g_frame_ready = 0;
        pthread_mutex_unlock(&g_mutex);

        /* push 8 pages to the LCM */
        for (int page = 0; page < 8; page++) {
            struct lcm_info info;
            memset(&info, 0, sizeof(info));
            info.ctrl   = WIX_LCM_CMD_RESET;
            info.page   = page;
            info.column = 0;
            info.size   = 128;
            memcpy(info.data, local + page * 128, 128);
            if (ioctl(fd, IOCTL_DISPLAY_COMMAND, &info) < 0) {
                LOG_WARN("page %d ioctl: %s", page, strerror(errno));
            }
        }

        /* apply any pending backlight change from listener thread */
        int bl_req = -1;
        pthread_mutex_lock(&g_mutex);
        if (g_pending_backlight >= 0) {
            bl_req = g_pending_backlight;
            g_pending_backlight = -1;
        }
        pthread_mutex_unlock(&g_mutex);
        if (bl_req >= 0 && bl_req <= 2) {
            ioctl(fd, WIX_LCM_IOC_BL_SET, &bl_req);
            LOG_DEBUG("backlight set to %d", bl_req);
        }

        g_display_hb++;
    }

    int bl_off = WIX_LCM_BL_OFF;
    ioctl(fd, WIX_LCM_IOC_BL_SET, &bl_off);
    ioctl(fd, WIX_LCM_IOC_POFF, 0);
    close(fd);
    LOG_INFO("display thread exiting");
    return NULL;
}

/* ---------- listener thread ---------- */

static int push_frame(const uint8_t *data, size_t len, int clear)
{
    if (len != FRAME_SIZE) {
        LOG_WARN("frame must be %d bytes (got %zu)", FRAME_SIZE, len);
        return -1;
    }

    pthread_mutex_lock(&g_mutex);
    memcpy(g_frame, data, FRAME_SIZE);
    g_frame_ready = 1;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);

    g_listener_hb++;
    (void)clear;
    return 0;
}

static void handle_client(int client_fd)
{
    char line[RECV_LINE_MAX];
    int rc = read_line(client_fd, line, sizeof(line), 30000);
    if (rc <= 0) {
        if (rc == -2) LOG_WARN("client disconnected before sending JSON");
        else if (rc == -3) LOG_WARN("line too long, dropping");
        return;
    }

    LOG_DEBUG("recv %d bytes JSON", rc);

    mcj_obj *obj = mcj_parse(line);
    if (!obj) {
        LOG_WARN("JSON parse failed");
        return;
    }

    /* optional control fields */
    int clear = mcj_get_bool(obj, "clear", 0);
    int bl    = mcj_get_int (obj, "backlight", -1);

    /* queue backlight request for the display thread to apply
     * (only the display thread opens /dev/lcm, so we serialize
     *  all ioctls through it to avoid multi-threaded device contention). */
    if (bl >= 0 && bl <= 2) {
        pthread_mutex_lock(&g_mutex);
        g_pending_backlight = bl;
        pthread_mutex_unlock(&g_mutex);
    }

    if (clear) {
        uint8_t blank[FRAME_SIZE];
        memset(blank, 0, FRAME_SIZE);
        push_frame(blank, FRAME_SIZE, 1);
        mcj_obj_free(obj);
        return;
    }

    const char *frame_b64 = mcj_get_string(obj, "frame");
    if (!frame_b64) {
        LOG_WARN("missing 'frame' field in JSON");
        mcj_obj_free(obj);
        return;
    }

    uint8_t decoded[FRAME_SIZE + 16];
    int n = base64_decode(frame_b64, strlen(frame_b64), decoded, sizeof(decoded));
    if (n != FRAME_SIZE) {
        LOG_WARN("base64 decoded %d bytes (expected %d)", n, FRAME_SIZE);
        mcj_obj_free(obj);
        return;
    }

    push_frame(decoded, FRAME_SIZE, 0);
    mcj_obj_free(obj);
}

static void *listener_thread(void *arg)
{
    (void)arg;
    LOG_INFO("listener thread starting on %s:%d", g_bind_ip, g_port);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        LOG_ERR("socket: %s", strerror(errno));
        pthread_exit(NULL);
    }

    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)g_port);
    if (inet_pton(AF_INET, g_bind_ip, &addr.sin_addr) <= 0) {
        /* fall back to binding any if the user asked 127.0.0.0 style */
        LOG_WARN("inet_pton('%s') failed, binding 0.0.0.0", g_bind_ip);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("bind %s:%d: %s", g_bind_ip, g_port, strerror(errno));
        close(sock);
        pthread_exit(NULL);
    }
    if (listen(sock, 8) < 0) {
        LOG_ERR("listen: %s", strerror(errno));
        close(sock);
        pthread_exit(NULL);
    }

    g_listener_sock = sock;

    while (!g_shutdown) {
        /* tick heartbeat every loop so watchdog sees liveness
         * even when no client is connecting */
        g_listener_hb++;

        /* use select so we can bail out cleanly on shutdown */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = { .tv_sec = 1 };
        int rc = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("select: %s", strerror(errno));
            break;
        }
        if (rc == 0) continue;  /* loop, check g_shutdown */

        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(sock, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (g_shutdown || errno == EBADF) break;
            LOG_WARN("accept: %s", strerror(errno));
            continue;
        }

        LOG_DEBUG("accepted %s:%d",
                 inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));
        handle_client(cfd);
        close(cfd);
    }

    close(sock);
    g_listener_sock = -1;
    LOG_INFO("listener thread exiting");
    return NULL;
}

/* ---------- watchdog (main thread) ---------- */

static int spawn_thread(pthread_t *tid, void *(*fn)(void *))
{
    int rc = pthread_create(tid, NULL, fn, NULL);
    if (rc != 0) {
        LOG_ERR("pthread_create: %s", strerror(rc));
        return -1;
    }
    return 0;
}

static int is_thread_alive(pthread_t tid)
{
    /* pthread_kill with signal 0; if we get ESRCH the thread is dead */
    int rc = pthread_kill(tid, 0);
    return (rc == 0);
}

static void *safe_join_reuse(pthread_t *tid, void *(*fn)(void *), const char *name)
{
    /* Always join. pthread_join on an already-exited thread returns
     * immediately and reclaims the thread's stack/TLS resources —
     * skipping it on a dead thread leaks those resources on every
     * respawn. */
    if (*tid) {
        pthread_join(*tid, NULL);
        memset(tid, 0, sizeof(*tid));
    }
    LOG_WARN("respawning %s thread", name);
    if (spawn_thread(tid, fn) != 0) {
        LOG_ERR("failed to respawn %s thread", name);
    }
    return NULL;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [-s | -b bind_ip -p port]\n"
        "  -s  Start with defaults (127.0.0.1:6776)\n"
        "  -b  Bind IP (default 127.0.0.1, use 0.0.0.0 for all interfaces)\n"
        "  -p  Listen port (default 6776)\n"
        "  -h  Show this help\n",
        argv0);
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        usage(argv[0]);
        return 0;
    }

    {
        const char *ev = getenv("LCM_VERBOSE");
        g_verbose = (ev && ev[0] && ev[0] != '0' && ev[0] != 'f' && ev[0] != 'F');
    }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            strncpy(g_bind_ip, argv[++i], sizeof(g_bind_ip) - 1);
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--start")) {
            /* start with defaults (already set at file scope) */
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (g_port <= 0 || g_port > 65535) g_port = DEFAULT_PORT;

    /* signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    LOG_INFO("px2-300d LCM TCP listener starting, binding %s:%d", g_bind_ip, g_port);

    if (spawn_thread(&g_listener_tid, listener_thread) != 0) return 1;
    if (spawn_thread(&g_display_tid, display_thread)  != 0) return 1;

    int last_l_hb = g_listener_hb;
    int last_d_hb = g_display_hb;

    while (!g_shutdown) {
        sleep(HB_CHECK_INTERVAL);
        if (g_shutdown) break;

        /* check listener */
        if (!is_thread_alive(g_listener_tid)) {
            safe_join_reuse(&g_listener_tid, listener_thread, "listener");
        } else if (g_listener_hb == last_l_hb) {
            /* no progress for 2 intervals = stuck; do not kill, just warn */
            LOG_WARN("listener thread heartbeat stalled but alive");
        }
        last_l_hb = g_listener_hb;

        /* check display */
        if (!is_thread_alive(g_display_tid)) {
            safe_join_reuse(&g_display_tid, display_thread, "display");
        } else if (g_display_hb == last_d_hb) {
            LOG_WARN("display thread heartbeat stalled but alive");
        }
        last_d_hb = g_display_hb;
    }

    LOG_INFO("shutting down...");
    pthread_cond_broadcast(&g_cond);

    /* wait for workers to finish */
    if (g_listener_tid) pthread_join(g_listener_tid, NULL);
    if (g_display_tid)  pthread_join(g_display_tid, NULL);

    LOG_INFO("all threads stopped, exiting");
    return 0;
}
