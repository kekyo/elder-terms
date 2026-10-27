/* elder-terms: A terminal application, inspired by personal computing in the 1990s.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * Test-only interposition of public APIs; no production behavior is replaced. */
#define _GNU_SOURCE
#include <boxrig/gtk3/terminal_widget.h>
#include <boxrig/session.h>
#include <boxrig/posix.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

typedef struct observation {
    const char *kind;
    uint64_t start, end, cpu;
    int sequence;
    size_t value, history;
    uint64_t revision;
    int status, pending;
    unsigned int depth;
} observation;

typedef struct interval {
    uint64_t start, cpu;
    unsigned int depth;
} interval;

typedef struct marker_reader {
    char text[64];
    size_t length, matched;
    bool collecting;
} marker_reader;

/* A bounded buffer avoids synchronous output and allocation in measured calls.
 * Overflow is reported explicitly and makes a run unsuitable for analysis. */
static observation observations[524288];
static size_t observation_count, dropped;
static bool initialized, enabled, detailed;
static pid_t recording_pid;
static unsigned int depth;
static int sequence = -1, pty_fd = -1;
static int key_sequence;
static boxrig_terminal *observed_terminal;
static marker_reader feed_marker, receive_marker;
static interval draw_interval;
static int draw_sequence;
static GdkEventFunc original_event_handler;

static uint64_t now_ns(clockid_t clock)
{
    struct timespec value;
    if (clock_gettime(clock, &value) != 0) abort();
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static void save_observations(void)
{
    if (!enabled || recording_pid != getpid() || observation_count == 0) return;
    FILE *file = fopen(getenv("ELDER_LATENCY_TRACE"), "w");
    if (file == NULL) { perror("latency trace"); return; }
    fprintf(file, "kind\tstart_ns\tend_ns\tcpu_ns\tsequence\tvalue\thistory\trevision\tstatus\tpending\tdepth\n");
    for (size_t index = 0; index < observation_count; index++) {
        const observation *item = &observations[index];
        fprintf(file, "%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
            "\t%d\t%zu\t%zu\t%" PRIu64 "\t%d\t%d\t%u\n",
            item->kind, item->start, item->end, item->cpu, item->sequence,
            item->value, item->history, item->revision, item->status, item->pending, item->depth);
    }
    if (dropped != 0) fprintf(file, "dropped\t0\t0\t0\t-1\t%zu\t0\t0\t0\t0\t0\n", dropped);
    if (fclose(file) != 0) perror("latency trace close");
}

static void initialize(void)
{
    if (initialized) return;
    initialized = true;
    enabled = getenv("ELDER_LATENCY_TRACE") != NULL;
    detailed = getenv("ELDER_LATENCY_LIGHT") == NULL;
    recording_pid = getpid();
    if (enabled && atexit(save_observations) != 0) abort();
}

static interval begin_interval(void)
{
    initialize();
    interval result = {now_ns(CLOCK_MONOTONIC), now_ns(CLOCK_THREAD_CPUTIME_ID), depth++};
    return result;
}

static void record(const char *kind, interval started, int id, size_t value,
    int status, int pending, const boxrig_terminal *terminal)
{
    const uint64_t ended = now_ns(CLOCK_MONOTONIC);
    const uint64_t cpu = now_ns(CLOCK_THREAD_CPUTIME_ID) - started.cpu;
    if (!enabled || recording_pid != getpid()) return;
    if (observation_count == sizeof(observations) / sizeof(observations[0])) { dropped++; return; }
    observations[observation_count++] = (observation){kind, started.start, ended, cpu,
        id, value, terminal ? boxrig_terminal_scrollback_rows(terminal) : 0,
        terminal ? boxrig_terminal_text_revision(terminal) : 0, status, pending, started.depth};
}

static void end_interval(const char *kind, interval started, size_t value,
    int status, int pending, const boxrig_terminal *terminal)
{
    depth--;
    if (detailed) record(kind, started, sequence, value, status, pending, terminal);
}

static void marker_bytes(marker_reader *reader, const unsigned char *bytes, size_t length,
    const char *kind, interval started, const boxrig_terminal *terminal)
{
    static const char prefix[] = "elder-latency:";
    for (size_t index = 0; index < length; index++) {
        const unsigned char byte = bytes[index];
        if (reader->collecting) {
            if (byte < 32) {
                reader->text[reader->length] = '\0';
                int id = strcmp(reader->text, "ready") == 0 ? 0 : atoi(reader->text);
                if (id >= 0) {
                    if (strcmp(kind, "parsed") == 0) sequence = id;
                    record(kind, started, id, length, 0, 0, terminal);
                    struct rusage usage;
                    if (strcmp(kind, "parsed") == 0 && getrusage(RUSAGE_SELF, &usage) == 0)
                        record("maxrss_kib", started, id, (size_t)usage.ru_maxrss, 0, 0, terminal);
                }
                reader->collecting = false;
                reader->length = 0;
            } else if (reader->length + 1 < sizeof(reader->text)) {
                reader->text[reader->length++] = (char)byte;
            } else {
                reader->collecting = false;
                reader->length = 0;
            }
        } else {
            reader->matched = byte == (unsigned char)prefix[reader->matched] ? reader->matched + 1 :
                (byte == (unsigned char)prefix[0] ? 1 : 0);
            if (reader->matched == sizeof(prefix) - 1) {
                reader->matched = 0;
                reader->collecting = true;
                reader->length = 0;
            }
        }
    }
}

/* POSIX dlsym supplies a callable address. memcpy keeps the C99 build free of
 * object/function pointer cast warnings without changing the ABI. */
#define RESOLVE(name) do { \
    if (next == NULL) { \
        void *symbol = dlsym(RTLD_NEXT, #name); \
        if (symbol == NULL || sizeof(next) != sizeof(symbol)) abort(); \
        memcpy(&next, &symbol, sizeof(next)); \
    } \
} while (0)

/** Observes accepted input bytes while preserving the public feed contract. */
boxrig_status boxrig_terminal_feed(boxrig_terminal *terminal, const uint8_t *bytes,
    size_t length, size_t *consumed)
{
    static boxrig_status (*next)(boxrig_terminal *, const uint8_t *, size_t, size_t *);
    RESOLVE(boxrig_terminal_feed);
    interval started = begin_interval();
    boxrig_status status = next(terminal, bytes, length, consumed);
    if (consumed != NULL) marker_bytes(&feed_marker, bytes, *consumed, "parsed", started, terminal);
    end_interval("feed", started, consumed ? *consumed : 0, status, 0, terminal);
    return status;
}

/** Observes the public bounded recognition operation without changing its budget. */
boxrig_status boxrig_terminal_resume_text_recognizers(boxrig_terminal *terminal,
    size_t budget, size_t *used, bool *more)
{
    static boxrig_status (*next)(boxrig_terminal *, size_t, size_t *, bool *);
    RESOLVE(boxrig_terminal_resume_text_recognizers);
    interval started = begin_interval();
    boxrig_status status = next(terminal, budget, used, more);
    end_interval("recognize", started, used ? *used : 0, status, more && *more, terminal);
    return status;
}

/** Records registrations, including application-triggered re-registrations. */
boxrig_status boxrig_terminal_register_compiled_text_recognizer(boxrig_terminal *terminal,
    boxrig_regex_program *program, boxrig_text_recognizer_scope scope,
    const boxrig_text_recognizer_limits *limits, uint64_t *id)
{
    static boxrig_status (*next)(boxrig_terminal *, boxrig_regex_program *,
        boxrig_text_recognizer_scope, const boxrig_text_recognizer_limits *, uint64_t *);
    RESOLVE(boxrig_terminal_register_compiled_text_recognizer);
    interval started = begin_interval();
    boxrig_status status = next(terminal, program, scope, limits, id);
    end_interval("register", started, id ? *id : 0, status, 0, terminal);
    return status;
}

/** Records consumed notification kinds once, without consuming extra events. */
boxrig_status boxrig_terminal_text_recognizer_event_consume(boxrig_terminal *terminal)
{
    static boxrig_status (*next)(boxrig_terminal *);
    RESOLVE(boxrig_terminal_text_recognizer_event_consume);
    boxrig_text_recognizer_event event = {0};
    bool present = detailed && boxrig_terminal_text_recognizer_event_peek(terminal, &event);
    interval started = begin_interval();
    boxrig_status status = next(terminal);
    end_interval("recognizer_event", started, present ? event.kind : 0, status, 0, terminal);
    return status;
}

/** Observes model synchronization through the application's unchanged call. */
boxrig_ui_status boxrig_ui_model_synchronize(boxrig_ui_model *model)
{
    static boxrig_ui_status (*next)(boxrig_ui_model *);
    RESOLVE(boxrig_ui_model_synchronize);
    interval started = begin_interval();
    boxrig_ui_status status = next(model);
    end_interval("model", started, 0, status, 0, boxrig_ui_model_terminal(model));
    return status;
}

static void observe_native_event(GdkEvent *event, gpointer data)
{
    if (event->type == GDK_KEY_PRESS) {
        interval started = begin_interval();
        depth--;
        int id = event->key.keyval == GDK_KEY_k ? ++key_sequence : -1;
        record("gtk_key", started, id, event->key.keyval, 0, 0, NULL);
    }
    if (original_event_handler != NULL) original_event_handler(event, data);
}

/** Preserves GTK's event handler and ownership while observing native delivery.
 * @param handler Handler installed by GTK.
 * @param data Caller-owned context passed unchanged to the handler.
 * @param notify Destructor retained by GDK without modification.
 */
void gdk_event_handler_set(GdkEventFunc handler, gpointer data, GDestroyNotify notify)
{
    static void (*next)(GdkEventFunc, gpointer, GDestroyNotify);
    RESOLVE(gdk_event_handler_set);
    initialize();
    original_event_handler = handler;
    next(handler != NULL && enabled ? observe_native_event : handler, data, notify);
}

static gboolean draw_begin(GtkWidget *widget, cairo_t *cr, gpointer context)
{
    (void)widget; (void)cr; (void)context;
    draw_interval = begin_interval();
    draw_sequence = sequence;
    return FALSE;
}

static gboolean draw_end(GtkWidget *widget, cairo_t *cr, gpointer context)
{
    (void)cr; (void)context;
    depth--;
    record("draw", draw_interval, draw_sequence,
        (size_t)(uintptr_t)gdk_window_get_toplevel(gtk_widget_get_window(widget)),
        0, 0, observed_terminal);
    boxrig_gtk_render_state state = {0};
    if (boxrig_gtk3_terminal_widget_get_render_state(
            BOXRIG_GTK3_TERMINAL_WIDGET(widget), &state) == BOXRIG_UI_OK)
        record("backend", draw_interval, draw_sequence, state.active_backend,
            state.status, state.pending, NULL);
    return FALSE;
}

/** Attaches passive GTK observers to the terminal widget created by the application. */
boxrig_ui_status boxrig_gtk3_terminal_widget_create(boxrig_ui_model *model,
    const boxrig_gtk3_terminal_widget_config *config, BoxrigGtk3TerminalWidget **widget)
{
    static boxrig_ui_status (*next)(boxrig_ui_model *, const boxrig_gtk3_terminal_widget_config *, BoxrigGtk3TerminalWidget **);
    RESOLVE(boxrig_gtk3_terminal_widget_create);
    initialize();
    boxrig_ui_status status = next(model, config, widget);
    if (status == BOXRIG_UI_OK && enabled) {
        observed_terminal = boxrig_ui_model_terminal(model);
        g_signal_connect(*widget, "draw", G_CALLBACK(draw_begin), NULL);
        g_signal_connect_after(*widget, "draw", G_CALLBACK(draw_end), NULL);
    }
    return status;
}

/** Observes one public frame, including its pending work result. */
boxrig_ui_status boxrig_gtk3_terminal_widget_frame(BoxrigGtk3TerminalWidget *widget,
    boxrig_ui_component_frame_result *result)
{
    static boxrig_ui_status (*next)(BoxrigGtk3TerminalWidget *, boxrig_ui_component_frame_result *);
    RESOLVE(boxrig_gtk3_terminal_widget_frame);
    interval started = begin_interval();
    boxrig_ui_status status = next(widget, result);
    end_interval("frame", started, (size_t)(uintptr_t)gtk_widget_get_window(GTK_WIDGET(widget)),
        status, result && result->additional_dispatch,
        boxrig_ui_view_terminal(boxrig_gtk3_terminal_widget_view(widget)));
    return status;
}

/** Records GTK/GDK submission completion, which is not physical display time. */
void gdk_window_end_draw_frame(GdkWindow *window, GdkDrawingContext *context)
{
    static void (*next)(GdkWindow *, GdkDrawingContext *);
    RESOLVE(gdk_window_end_draw_frame);
    interval started = begin_interval();
    next(window, context);
    depth--;
    record("composite", started, sequence,
        (size_t)(uintptr_t)gdk_window_get_toplevel(window), 0, 0, NULL);
}

/** Tracks the application's PTY descriptor; no I/O state is changed. */
int boxrig_posix_pty_descriptor(const struct boxrig_posix_pty *pty)
{
    static int (*next)(const struct boxrig_posix_pty *);
    RESOLVE(boxrig_posix_pty_descriptor);
    int descriptor = next(pty);
    pty_fd = descriptor;
    return descriptor;
}

/** Records only reads from the observed PTY, retaining errno and return values. */
ssize_t read(int descriptor, void *buffer, size_t capacity)
{
    static ssize_t (*next)(int, void *, size_t);
    RESOLVE(read);
    if (descriptor != pty_fd || pty_fd < 0) return next(descriptor, buffer, capacity);
    interval started = begin_interval();
    ssize_t count = next(descriptor, buffer, capacity);
    int saved_errno = errno;
    if (count > 0) marker_bytes(&receive_marker, buffer, (size_t)count, "received", started, NULL);
    end_interval("pty_read", started, count > 0 ? (size_t)count : 0, count < 0 ? saved_errno : 0, 0, NULL);
    errno = saved_errno;
    return count;
}
