#include "session.h"
#include <boxrig/gtk3.h>
#include <boxrig/posix.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

struct PilotSession {
    GSource source;
    gpointer fd_tag;
    GIOCondition fd_events;
    boxrig_terminal *terminal;
    boxrig_ui_model *model;
    BoxrigGtk3TerminalWidget *widget;
    boxrig_posix_pty *pty;
    boxrig_session *session;
    char label;
    gboolean frame_requested;
    PilotShellEnded ended;
    gpointer data;
};

static void request_frame(void *context)
{
    PilotSession *s = context;
    s->frame_requested = TRUE;
    g_source_set_ready_time(&s->source, 0);
}

static gboolean has_allocation(PilotSession *s)
{
    return gtk_widget_get_mapped(GTK_WIDGET(s->widget)) &&
        gtk_widget_get_allocated_width(GTK_WIDGET(s->widget)) > 1 &&
        gtk_widget_get_allocated_height(GTK_WIDGET(s->widget)) > 1;
}

static gboolean prepare(GSource *source, gint *timeout)
{
    PilotSession *s = (PilotSession *)source;
    uint32_t interests = boxrig_session_interests(s->session);
    GIOCondition events = G_IO_ERR | G_IO_HUP;
    if (interests & BOXRIG_SESSION_INTEREST_WRITE) events |= G_IO_OUT;
    if (interests & BOXRIG_SESSION_INTEREST_READ) events |= G_IO_IN;
    if (events != s->fd_events) {
        g_source_modify_unix_fd(source, s->fd_tag, events);
        s->fd_events = events;
    }
    *timeout = -1;
    return (s->frame_requested && has_allocation(s)) ||
        ((interests & BOXRIG_SESSION_INTEREST_DISPATCH) &&
         !(interests & BOXRIG_SESSION_INTEREST_WRITE));
}

static gboolean check(GSource *source)
{
    PilotSession *s = (PilotSession *)source;
    return g_source_query_unix_fd(source, s->fd_tag) != 0;
}

static void update_layout(PilotSession *s)
{
    boxrig_ui_layout_action action;
    while (boxrig_gtk3_terminal_widget_take_layout_action(s->widget, &action)) {
        if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
            size_t rows = action.value.grid_resize.rows;
            size_t cols = action.value.grid_resize.columns;
            if (boxrig_session_resize(s->session, rows, cols) != BOXRIG_SESSION_OK ||
                boxrig_gtk3_terminal_widget_complete_grid_resize(s->widget,
                    action.revision, true) != BOXRIG_UI_OK) g_error("grid resize");
            printf("grid %c %zu %zu\n", s->label, rows, cols);
        } else if (action.kind == BOXRIG_UI_LAYOUT_ACTION_CONTENT_SIZE) {
            bool applied;
            boxrig_ui_layout_result result;
            GtkWidget *w = GTK_WIDGET(s->widget);
            if (boxrig_gtk3_terminal_widget_complete_size_command(s->widget,
                action.revision, BOXRIG_UI_SIZE_ACCEPTED,
                gtk_widget_get_allocated_width(w) * BOXRIG_UI_FIXED_ONE,
                gtk_widget_get_allocated_height(w) * BOXRIG_UI_FIXED_ONE,
                &applied, &result) != BOXRIG_UI_OK) g_error("content resize");
        }
    }
}

static gboolean dispatch(GSource *source, GSourceFunc unused, gpointer data)
{
    PilotSession *s = (PilotSession *)source;
    (void)unused;
    (void)data;
    GIOCondition events = g_source_query_unix_fd(source, s->fd_tag);
    uint32_t ready = BOXRIG_SESSION_READY_NONE;
    if (events & (G_IO_IN | G_IO_HUP)) ready |= BOXRIG_SESSION_READY_READ;
    if (events & G_IO_OUT) ready |= BOXRIG_SESSION_READY_WRITE;
    g_source_set_ready_time(source, -1);
    boxrig_session_dispatch_result io;
    if ((events & (G_IO_NVAL | G_IO_ERR)) ||
        boxrig_session_dispatch(s->session, ready, &io) != BOXRIG_SESSION_OK ||
        boxrig_ui_model_synchronize(s->model) != BOXRIG_UI_OK) g_error("session dispatch");
    gboolean mapped = gtk_widget_get_mapped(GTK_WIDGET(s->widget));
    if (has_allocation(s)) {
        boxrig_ui_component_frame_result frame;
        s->frame_requested = FALSE;
        update_layout(s);
        boxrig_ui_status status = boxrig_gtk3_terminal_widget_frame(s->widget, &frame);
        if (status != BOXRIG_UI_OK) g_error("terminal frame %c: %d, %dx%d", s->label, status,
            gtk_widget_get_allocated_width(GTK_WIDGET(s->widget)), gtk_widget_get_allocated_height(GTK_WIDGET(s->widget)));
        update_layout(s);
        if (frame.additional_dispatch) s->frame_requested = TRUE;
    }
    boxrig_host_item item;
    while (boxrig_terminal_host_item_peek(s->terminal, &item)) {
        if (item.type == BOXRIG_HOST_ITEM_TITLE) {
            printf("pty %c %.*s", s->label, (int)item.payload_length, (const char *)item.payload);
            if (item.payload_length >= 5 && !memcmp(item.payload, "tick-", 5))
                printf(" visible=%d", mapped);
            putchar('\n');
        }
        if (boxrig_terminal_host_item_consume(s->terminal) != BOXRIG_STATUS_OK)
            g_error("terminal notification");
    }
    if (io.peer_closed) {
        s->ended(s->data);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs callbacks = {prepare, check, dispatch, NULL, NULL, NULL};

PilotSession *pilot_session_open(char label, gboolean fixture, PilotShellEnded ended, gpointer data)
{
    PilotSession *s = (PilotSession *)g_source_new(&callbacks, sizeof(*s));
    s->label = label;
    s->ended = ended;
    s->data = data;
    boxrig_terminal_config tc = boxrig_terminal_config_default();
    tc.rows = 12;
    tc.columns = 36;
    boxrig_session_config sc = boxrig_session_config_default();
    boxrig_gtk3_terminal_widget_config wc = boxrig_gtk3_terminal_widget_config_default();
    wc.frame_request_callback = request_frame;
    wc.frame_request_context = s;
    char name[] = {label, 0};
    // OSC title reports let the test observe bytes parsed from the real PTY.
    char *test_shell[] = {"/bin/sh", "-c",
        "printf 'Session %s (shell %s)\n' \"$0\" \"$$\"; "
        "while IFS= read -r line; do "
        "if [ \"$line\" = burst ]; then (for n in 1 2 3 4 5; do sleep 0.16; "
        "printf 'tick %s\\n\\033]0;tick-%s\\007' \"$n\" \"$n\"; done) & "
        "elif [ \"$line\" = size ]; then printf '\\033]0;size-%s\\007' \"$(stty size)\"; "
        "else printf '%s: %s\\n\\033]0;ack-%s-%s\\007' \"$0\" \"$line\" \"$$\" \"$line\"; fi; done",
        name, NULL};
    char *interactive_shell[] = {"/bin/sh", "-i", NULL};
    if (boxrig_terminal_create(&tc, &s->terminal) != BOXRIG_STATUS_OK ||
        boxrig_ui_model_create(s->terminal, &s->model) != BOXRIG_UI_OK ||
        boxrig_posix_pty_spawn(fixture ? test_shell : interactive_shell,
            "xterm-256color", tc.rows, tc.columns, &s->pty) ||
        boxrig_posix_pty_session_create(s->terminal, s->pty, &sc, &s->session) != BOXRIG_SESSION_OK ||
        boxrig_gtk3_terminal_widget_create(s->model, &wc, &s->widget) != BOXRIG_UI_OK ||
        boxrig_gtk3_terminal_widget_set_visible(s->widget, true) != BOXRIG_UI_OK)
        g_error("creating session %c", label);
    g_object_ref_sink(s->widget);
    s->fd_tag = g_source_add_unix_fd(&s->source, boxrig_posix_pty_descriptor(s->pty), G_IO_IN);
    g_source_set_priority(&s->source, G_PRIORITY_DEFAULT_IDLE);
    g_source_attach(&s->source, NULL);
    return s;
}

GtkWidget *pilot_session_widget(PilotSession *s)
{
    return GTK_WIDGET(s->widget);
}

void pilot_session_close(PilotSession *s)
{
    g_source_destroy(&s->source);
    kill(boxrig_posix_pty_child(s->pty), SIGKILL);
    int status;
    if (boxrig_posix_pty_wait(s->pty, &status)) g_error("waiting for fixture shell");
    g_object_unref(s->widget);
    boxrig_session_destroy(s->session);
    boxrig_posix_pty_destroy(s->pty);
    boxrig_ui_model_destroy(s->model);
    boxrig_terminal_destroy(s->terminal);
    g_source_unref(&s->source);
}
