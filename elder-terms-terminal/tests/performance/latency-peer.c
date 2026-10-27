/* elder-terms: A terminal application, inspired by personal computing in the 1990s.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT. */

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define KEY_CAPACITY 16384u

typedef struct key_record {
    uint64_t received_ns;
    uint64_t written_ns;
    uint64_t output_bytes;
    uint64_t output_lines;
    uint64_t output_updates;
    uint64_t missed_ticks;
} key_record;

typedef struct peer_state {
    const char *report_path;
    const char *mode;
    const char *load;
    unsigned int period_ms;
    unsigned int lines_per_tick;
    unsigned int rows;
    uint64_t output_bytes;
    uint64_t output_lines;
    uint64_t output_updates;
    uint64_t missed_ticks;
    unsigned int sequence;
    key_record keys[KEY_CAPACITY];
} peer_state;

static uint64_t now_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static bool parse_uint(const char *text, unsigned int minimum, unsigned int maximum,
    unsigned int *result)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < minimum || value > maximum) return false;
    *result = (unsigned int)value;
    return true;
}

static bool write_bytes(peer_state *state, const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(STDOUT_FILENO, data + offset, length - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += (size_t)written;
        state->output_bytes += (uint64_t)written;
    }
    return true;
}

static bool write_text(peer_state *state, const char *value)
{
    return write_bytes(state, value, strlen(value));
}

static bool emit_line(peer_state *state, uint64_t index)
{
    char line[320];
    int length = 0;
    if (strcmp(state->mode, "plain") == 0) {
        length = snprintf(line, sizeof(line),
            "elder-latency %08" PRIu64 " ordinary terminal line\r\n", index);
    } else if (strcmp(state->mode, "url") == 0) {
        length = snprintf(line, sizeof(line),
            "elder-latency %08" PRIu64 " https://example.org/latency/%08" PRIu64 "\r\n",
            index, index);
    } else {
        length = snprintf(line, sizeof(line),
            "elder-latency %08" PRIu64 " wrapped ABCDEFGHIJKLMNOPQRSTUVWXYZ "
            "abcdefghijklmnopqrstuvwxyz 0123456789 "
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz\r\n", index);
    }
    if (length <= 0 || (size_t)length >= sizeof(line)) return false;
    if (!write_bytes(state, line, (size_t)length)) return false;
    state->output_lines++;
    return true;
}

static bool paint_marker(peer_state *state)
{
    char marker[128];
    unsigned int red = state->sequence == 0 ? 32u : 64u + (state->sequence % 160u);
    unsigned int green = state->sequence == 0 ? 64u : 80u + ((state->sequence / 160u) % 160u);
    unsigned int blue = state->sequence == 0 ? 96u : 80u;
    int length = snprintf(marker, sizeof(marker),
        "\0337\033[1;1H\033[48;2;%u;%u;%um        \033[0m\0338",
        red, green, blue);
    return length > 0 && (size_t)length < sizeof(marker) &&
        write_bytes(state, marker, (size_t)length);
}

static bool emit_title(peer_state *state, const char *suffix)
{
    char title[96];
    int length = snprintf(title, sizeof(title), "\033]0;elder-latency:%s\007", suffix);
    return length > 0 && (size_t)length < sizeof(title) &&
        write_bytes(state, title, (size_t)length);
}

static bool set_running(int timer, const peer_state *state, bool running)
{
    struct itimerspec interval = {0};
    if (running && strcmp(state->load, "idle") != 0) {
        interval.it_interval.tv_sec = state->period_ms / 1000u;
        interval.it_interval.tv_nsec = (long)(state->period_ms % 1000u) * 1000000L;
        interval.it_value = interval.it_interval;
    }
    return timerfd_settime(timer, 0, &interval, NULL) == 0;
}

static bool emit_tick(peer_state *state)
{
    if (strcmp(state->load, "idle") == 0) return true;
    for (unsigned int index = 0; index < state->lines_per_tick; index++) {
        if (strcmp(state->load, "append") == 0) {
            if (!emit_line(state, state->output_lines)) return false;
        } else {
            char line[320];
            int length = 0;
            if (strcmp(state->mode, "plain") == 0) {
                length = snprintf(line, sizeof(line),
                    "\0337\033[3;1H\033[Jelder-latency update %08" PRIu64
                    " ordinary terminal line\0338", state->output_updates);
            } else if (strcmp(state->mode, "url") == 0) {
                length = snprintf(line, sizeof(line),
                    "\0337\033[3;1H\033[Jelder-latency update %08" PRIu64
                    " https://example.org/latency/%08" PRIu64 "\0338",
                    state->output_updates, state->output_updates);
            } else {
                length = snprintf(line, sizeof(line),
                    "\0337\033[3;1H\033[Jelder-latency wrapped update %08" PRIu64
                    " ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz "
                    "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\0338", state->output_updates);
            }
            if (length <= 0 || (size_t)length >= sizeof(line) ||
                !write_bytes(state, line, (size_t)length)) return false;
            state->output_updates++;
        }
    }
    return paint_marker(state);
}

static bool write_report(const peer_state *state)
{
    FILE *report = fopen(state->report_path, "w");
    if (report == NULL) return false;
    fputs("key\treceived_ns\twritten_ns\toutput_bytes\toutput_lines\toutput_updates\tmissed_ticks\n", report);
    for (unsigned int index = 0; index < state->sequence; index++) {
        const key_record *key = &state->keys[index];
        fprintf(report, "%u\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\n", index + 1u,
            key->received_ns, key->written_ns, key->output_bytes,
            key->output_lines, key->output_updates, key->missed_ticks);
    }
    return fclose(report) == 0;
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: %s REPORT HISTORY_LINES plain|url|wrapped "
            "idle|append|overwrite PERIOD_MS LINES_PER_TICK\n", argv[0]);
        return 2;
    }
    peer_state state = {.report_path = argv[1], .mode = argv[3], .load = argv[4]};
    unsigned int history_lines = 0;
    if (!parse_uint(argv[2], 0, 1000000, &history_lines) ||
        !parse_uint(argv[5], 1, 60000, &state.period_ms) ||
        !parse_uint(argv[6], 1, 4096, &state.lines_per_tick) ||
        (strcmp(state.mode, "plain") != 0 && strcmp(state.mode, "url") != 0 &&
            strcmp(state.mode, "wrapped") != 0) ||
        (strcmp(state.load, "idle") != 0 && strcmp(state.load, "append") != 0 &&
            strcmp(state.load, "overwrite") != 0)) return 2;

    struct termios original;
    if (tcgetattr(STDIN_FILENO, &original) != 0) return 1;
    struct termios raw = original;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return 1;
    struct winsize size = {0};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) != 0) return 1;
    state.rows = size.ws_row == 0 ? 24u : size.ws_row;

    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timer < 0) return 1;
    for (uint64_t line = 0; line < (uint64_t)history_lines + state.rows - 1u; line++) {
        if (!emit_line(&state, line)) return 1;
    }
    /* The last line stays on the grid without causing another scroll. */
    if (!write_text(&state, "elder-latency ready")) return 1;
    if (!paint_marker(&state) || !emit_title(&state, "ready")) return 1;

    bool running = false;
    bool done = false;
    while (!done) {
        struct pollfd events[2] = {{STDIN_FILENO, POLLIN, 0}, {timer, POLLIN, 0}};
        int ready = poll(events, 2, 30000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return 1;
        if (events[0].revents & POLLIN) {
            char input[64];
            ssize_t count = read(STDIN_FILENO, input, sizeof(input));
            if (count <= 0) return 1;
            for (ssize_t index = 0; index < count; index++) {
                if (input[index] == 'g') {
                    running = true;
                    if (!set_running(timer, &state, true)) return 1;
                } else if (input[index] == 's') {
                    running = false;
                    if (!set_running(timer, &state, false)) return 1;
                } else if (input[index] == 'k') {
                    if (state.sequence == KEY_CAPACITY) return 1;
                    key_record *key = &state.keys[state.sequence];
                    key->received_ns = now_ns();
                    state.sequence++;
                    char number[32];
                    snprintf(number, sizeof(number), "%u", state.sequence);
                    if (!paint_marker(&state) || !emit_title(&state, number)) return 1;
                    key->written_ns = now_ns();
                    key->output_bytes = state.output_bytes;
                    key->output_lines = state.output_lines;
                    key->output_updates = state.output_updates;
                    key->missed_ticks = state.missed_ticks;
                } else if (input[index] == 'q') {
                    done = true;
                    break;
                }
            }
        }
        if (!done && running && (events[1].revents & POLLIN)) {
            uint64_t expirations = 0;
            if (read(timer, &expirations, sizeof(expirations)) != sizeof(expirations)) return 1;
            if (expirations > 1) state.missed_ticks += expirations - 1;
            if (!emit_tick(&state)) return 1;
        }
    }
    close(timer);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &original) != 0) return 1;
    return write_report(&state) ? 0 : 1;
}
