/* linux-host primitives: the lean-bsp contract on top of POSIX. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "bsp_api.h"
#include "bsp_linux.h"

static char **saved_argv;
static int uart_in = STDIN_FILENO;
static int uart_out = STDOUT_FILENO;

static const bsp_capabilities_t capabilities = {
    .board_name = "linux-host",
    .n_uarts = 1,
    .n_leds = 0,
    .n_buttons = 0,
    .has_cycle_counter = true,
    .n_nvm_regions = 0,
    .nvm_regions = NULL,
};

const bsp_capabilities_t *bsp_get_capabilities(void) { return &capabilities; }

static void fail(const char *what) {
    perror(what);
    exit(EXIT_FAILURE);
}

/* ---- UART 0 ----
 * The process's own stdin/stdout by default. With LEAN_BSP_UART0=pty it is a pseudo-terminal
 * instead, whose device path is written to the file named by LEAN_BSP_UART0_PTY_FILE: that is
 * how `run --detach` hands a host-side tool its com_device. */

static void write_pty_path(const char *path) {
    const char *file = getenv("LEAN_BSP_UART0_PTY_FILE");
    if (!file) return;
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", file) >= (int)sizeof tmp) fail("linux-host: LEAN_BSP_UART0_PTY_FILE too long");
    FILE *f = fopen(tmp, "w");
    if (!f || fprintf(f, "%s\n", path) < 0 || fclose(f) != 0) fail("linux-host: writing LEAN_BSP_UART0_PTY_FILE");
    if (rename(tmp, file) != 0) fail("linux-host: writing LEAN_BSP_UART0_PTY_FILE");  /* atomic for the reader */
}

static void uart0_open_pty(void) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) fail("linux-host: pseudo-terminal");
    struct termios tio;
    if (tcgetattr(master, &tio) == 0) {
        cfmakeraw(&tio);  /* a UART carries bytes, not lines */
        tcsetattr(master, TCSANOW, &tio);
    }
    const char *slave = ptsname(master);
    if (!slave) fail("linux-host: ptsname");
    write_pty_path(slave);
    if (dup2(master, STDIN_FILENO) < 0 || dup2(master, STDOUT_FILENO) < 0) fail("linux-host: dup2");
    if (master > STDOUT_FILENO) close(master);
    /* bsp_reset() re-executes the program: the new image must keep this pseudo-terminal, not open another */
    setenv("LEAN_BSP_UART0", "inherit", 1);
}

void bsp_uart_tx(unsigned port, const void *buf, size_t len) {
    if (port != 0) return;
    fflush(stdout);  /* keep printf output and raw UART bytes in order */
    const char *p = buf;
    while (len) {
        ssize_t n = write(uart_out, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += n;
        len -= (size_t)n;
    }
}

size_t bsp_uart_rx(unsigned port, void *buf, size_t len) {
    if (port != 0 || len == 0) return 0;
    struct pollfd pfd = {.fd = uart_in, .events = POLLIN};
    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) return 0;
    ssize_t n = read(uart_in, buf, len);
    return n > 0 ? (size_t)n : 0;
}

/* ---- LEDs and buttons: linux-host has none ---- */

void bsp_led_set(unsigned id, bool on) {
    (void)id;
    (void)on;
}

bool bsp_button_get(unsigned id) {
    (void)id;
    return false;
}

/* ---- time: the "cycle" counter counts nanoseconds, so bsp_cpu_freq_hz() is its 1 GHz rate ---- */

uint32_t bsp_cycle_count(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec);
}

uint32_t bsp_cpu_freq_hz(void) { return 1000000000u; }

void bsp_delay_ms(uint32_t ms) {
    struct timespec ts = {.tv_sec = ms / 1000u, .tv_nsec = (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

void bsp_clear_caches(void) {}

void bsp_reset(void) {
    fflush(NULL);
    execv("/proc/self/exe", saved_argv);
    perror("linux-host: bsp_reset");
    _exit(EXIT_FAILURE);
}

/* ---- faults: report through app_on_fault, then die the default way ---- */

static void on_fault(int sig) {
    app_on_fault(sig == SIGABRT ? APP_FAULT_ASSERT : APP_FAULT_HARD, 0);
    raise(sig);  /* SA_RESETHAND restored the default action, SA_NODEFER lets it fire now */
}

static void install_fault_handlers(void) {
    static const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sa.sa_flags = SA_RESETHAND | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) sigaction(signals[i], &sa, NULL);
}

/* ---- tick: a thread standing in for the tick ISR, started only if the application uses it ---- */

static void *tick_thread(void *arg) {
    (void)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (uint32_t tick_ms = 1;; tick_ms++) {
        next.tv_nsec += 1000000L;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {
        }
        app_tick(tick_ms);
    }
    return NULL;
}

void bsp_start_tick(void) {
    if (app_tick == lean_bsp_default_app_tick) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, tick_thread, NULL) != 0) fail("linux-host: tick thread");
    pthread_detach(thread);
}

void bsp_init(int argc, char *argv[]) {
    (void)argc;
    saved_argv = argv;
    const char *uart0 = getenv("LEAN_BSP_UART0");
    if (uart0 && strcmp(uart0, "pty") == 0) uart0_open_pty();
    setvbuf(stdout, NULL, _IONBF, 0);  /* stdout is UART 0: bytes leave as soon as they are written */
    install_fault_handlers();
    bsp_heap_usage_reset();
}
