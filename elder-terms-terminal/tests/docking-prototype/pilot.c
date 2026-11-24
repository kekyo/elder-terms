#include "session.h"
#include <stdio.h>
#include <string.h>

typedef struct Window Window;
typedef struct Tab {
    char name;
    Window *owner;
    PilotSession *session;
    GtkWidget *row, *pick, *close;
} Tab;

struct Window {
    unsigned id;
    GtkWidget *win, *stack, *list, *title, *native;
    GList *tabs;
    Tab *active;
};

static GPtrArray *windows;
static Tab tabs[3];
static unsigned live;
static GtkTargetEntry formats[] = {
    {"application/x-elder-terms-dock-pilot", GTK_TARGET_SAME_APP, 1},
    {"text/uri-list", 0, 2}
};

static Window *open_window(void);

static void rect(Window *w, GtkWidget *widget, const char *name)
{
    GdkRectangle area;
    if (!gtk_widget_get_mapped(widget) ||
        !gtk_widget_translate_coordinates(widget, w->win, 0, 0, &area.x, &area.y)) return;
    area.width = gtk_widget_get_allocated_width(widget);
    area.height = gtk_widget_get_allocated_height(widget);
    GdkRectangle *last = g_object_get_data(G_OBJECT(widget), "reported-area");
    if (last && g_object_get_data(G_OBJECT(widget), "reported-in") == w &&
        gdk_rectangle_equal(last, &area)) return;
    if (!last) {
        last = g_new(GdkRectangle, 1);
        g_object_set_data_full(G_OBJECT(widget), "reported-area", last, g_free);
    }
    *last = area;
    g_object_set_data(G_OBJECT(widget), "reported-in", w);
    printf("rect %u %s %d %d %d %d\n", w->id, name, area.x, area.y, area.width, area.height);
}

static void allocated(GtkWidget *widget, GtkAllocation *area, Window *w)
{
    (void)widget;
    (void)area;
    rect(w, w->title, "title");
    rect(w, w->native, "native");
    rect(w, w->stack, "content");
    for (GList *node = w->tabs; node; node = node->next) {
        Tab *tab = node->data;
        char name[12];
        snprintf(name, sizeof(name), "tab%c", tab->name);
        rect(w, tab->pick, name);
        snprintf(name, sizeof(name), "close%c", tab->name);
        rect(w, tab->close, name);
    }
}

static void refresh(Window *w)
{
    if (!w->tabs) {
        gtk_widget_destroy(w->win);
        return;
    }
    if (!w->active) w->active = w->tabs->data;
    gboolean sidebar = w->tabs->next != NULL;
    gtk_widget_set_visible(w->list, sidebar);
    GtkWidget *page = pilot_session_widget(w->active->session);
    gtk_stack_set_visible_child(GTK_STACK(w->stack), page);
    gtk_widget_grab_focus(page);
    printf("state %u tabs=", w->id);
    for (GList *node = w->tabs; node; node = node->next) {
        Tab *t = node->data;
        putchar(t->name);
    }
    gtk_list_box_select_row(GTK_LIST_BOX(w->list), GTK_LIST_BOX_ROW(w->active->row));
    printf(" active=%c sidebar=%d\n", w->active->name, sidebar);
    gtk_widget_queue_resize(w->win);
}

static void detach(Tab *t)
{
    Window *w = t->owner;
    GList *node = g_list_find(w->tabs, t);
    if (w->active == t) {
        GList *next = node->next ? node->next : node->prev;
        w->active = next ? next->data : NULL;
    }
    w->tabs = g_list_delete_link(w->tabs, node);
    t->owner = NULL;
    gtk_container_remove(GTK_CONTAINER(w->list), t->row);
    gtk_container_remove(GTK_CONTAINER(w->stack), pilot_session_widget(t->session));
}

static void attach(Window *w, Tab *t)
{
    t->owner = w;
    w->tabs = g_list_append(w->tabs, t);
    gtk_container_add(GTK_CONTAINER(w->list), t->row);
    gtk_container_add(GTK_CONTAINER(w->stack), pilot_session_widget(t->session));
}

static void release(Tab *t)
{
    gtk_widget_destroy(t->row);
    g_object_unref(t->row);
    pilot_session_close(t->session);
    t->session = NULL;
}

static void destroyed(GtkWidget *widget, Window *w)
{
    (void)widget;
    while (w->tabs) {
        Tab *t = w->tabs->data;
        detach(t);
        release(t);
    }
    w->win = NULL;
    printf("closed %u\n", w->id);
    if (!--live) gtk_main_quit();
}

static void move(Window *src, Window *dst, Tab *only)
{
    if (!src->win || (dst && !dst->win) || (only && only->owner != src)) return;
    if (dst == src) {
        printf("drop self %u\n", src->id);
        return;
    }
    if (!dst) {
        if (!only || !src->tabs->next) return;
        dst = open_window();
    }
    Tab *selected = only ? only : src->active;
    do {
        Tab *t = only ? only : src->tabs->data;
        detach(t);
        attach(dst, t);
    } while (!only && src->tabs);
    dst->active = selected;
    refresh(src);
    refresh(dst);
    gtk_window_present(GTK_WINDOW(dst->win));
}

typedef struct Move { Window *src, *dst; Tab *only; } Move;

static gboolean finish_move(gpointer context)
{
    Move *pending = context;
    move(pending->src, pending->dst, pending->only);
    return G_SOURCE_REMOVE;
}

static void queue_move(Window *src, Window *dst, Tab *only)
{
    Move *pending = g_new(Move, 1);
    *pending = (Move){src, dst, only};
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, finish_move, pending, g_free);
}

static Window *source_window(GtkWidget *widget, Tab **tab)
{
    *tab = g_object_get_data(G_OBJECT(widget), "tab");
    return *tab ? (*tab)->owner : g_object_get_data(G_OBJECT(widget), "window");
}

static void drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer unused)
{
    (void)unused;
    Tab *tab;
    Window *src = source_window(widget, &tab);
    printf("drag-begin %u %c\n", src->id, tab ? tab->name : '*');
    gtk_drag_set_icon_name(context, "utilities-terminal", 8, 8);
}

static void provide(GtkWidget *widget, GdkDragContext *ctx, GtkSelectionData *data,
    guint info, guint time, gpointer unused)
{
    (void)time; (void)unused;
    Tab *tab;
    Window *src = source_window(widget, &tab);
    // The Xdnd root-window target positively identifies a desktop drop.
    if (info == 3) {
        if (tab && gdk_drag_context_get_protocol(ctx) == GDK_DRAG_PROTO_ROOTWIN) {
            printf("desktop-drop %u %c\n", src->id, tab->name);
            queue_move(src, NULL, tab);
        }
        return;
    }
    char payload[32];
    int n = snprintf(payload, sizeof(payload), "%u/%c", src->id, tab ? tab->name : '*');
    gtk_selection_data_set(data, gtk_selection_data_get_target(data), 8, (guchar *)payload, n);
}

static gboolean drag_failed(GtkWidget *widget, GdkDragContext *ctx,
    GtkDragResult result, gpointer unused)
{
    (void)ctx; (void)unused;
    Tab *tab;
    Window *src = source_window(widget, &tab);
    GEnumClass *values = g_type_class_ref(GTK_TYPE_DRAG_RESULT);
    printf("drag-failed %u %c %s\n", src->id, tab ? tab->name : '*',
        g_enum_get_value(values, result)->value_nick);
    g_type_class_unref(values);

    return TRUE;
}

static void source(GtkWidget *widget)
{
    GtkTargetEntry targets[] = {
        formats[0], {"application/x-rootwindow-drop", 0, 3}
    };
    gboolean row = g_object_get_data(G_OBJECT(widget), "tab") != NULL;
    gtk_drag_source_set(widget, GDK_BUTTON1_MASK, targets, row ? 2 : 1, GDK_ACTION_MOVE);
    g_signal_connect(widget, "drag-begin", G_CALLBACK(drag_begin), NULL);
    g_signal_connect(widget, "drag-data-get", G_CALLBACK(provide), NULL);
    g_signal_connect(widget, "drag-failed", G_CALLBACK(drag_failed), NULL);
}

static gboolean drop(GtkWidget *widget, GdkDragContext *ctx, gint x, gint y,
    guint time, gpointer unused)
{
    (void)x; (void)y; (void)unused;
    GdkAtom type = gtk_drag_dest_find_target(widget, ctx, NULL);
    if (type == GDK_NONE) return FALSE;
    gtk_drag_get_data(widget, ctx, type, time);
    return TRUE;
}

static void receive(GtkWidget *widget, GdkDragContext *ctx, gint x, gint y,
    GtkSelectionData *data, guint info, guint time, Window *dst)
{
    (void)widget; (void)x; (void)y;
    gboolean accepted = FALSE;
    if (info == 2) {
        gchar **uris = gtk_selection_data_get_uris(data);
        accepted = uris && uris[0];
        printf("file-drop %u %u\n", dst->id, uris ? g_strv_length(uris) : 0);
        g_strfreev(uris);
    } else {
        GtkWidget *widget_src = gtk_drag_get_source_widget(ctx);
        if (widget_src && gtk_selection_data_get_length(data) > 0) {
            Tab *tab;
            Window *src = source_window(widget_src, &tab);
            if (src && src->win) {
                queue_move(src, dst, tab);
                accepted = TRUE;
            }
        }
    }
    gtk_drag_finish(ctx, accepted, FALSE, time);
}

static void destination(GtkWidget *widget, Window *w)
{
    gtk_drag_dest_set(widget, GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_HIGHLIGHT,
        formats, G_N_ELEMENTS(formats), GDK_ACTION_MOVE | GDK_ACTION_COPY);
    g_signal_connect(widget, "drag-drop", G_CALLBACK(drop), NULL);
    g_signal_connect(widget, "drag-data-received", G_CALLBACK(receive), w);
}

static gboolean key_press(GtkWidget *widget, GdkEventKey *e, Window *w)
{
    (void)widget;
    if ((e->state & gtk_accelerator_get_default_mod_mask()) != (GDK_CONTROL_MASK | GDK_SHIFT_MASK) ||
        gdk_keyval_to_lower(e->keyval) != GDK_KEY_u) return FALSE;
    move(w, NULL, w->active);
    return TRUE;
}

static Window *open_window(void)
{
    Window *w = g_new0(Window, 1);
    g_ptr_array_add(windows, w);
    ++live;
    w->id = windows->len;
    w->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char name[40];
    snprintf(name, sizeof(name), "Dock pilot %u", w->id);
    gtk_window_set_default_size(GTK_WINDOW(w->win), 440, 260);
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    w->title = gtk_button_new_with_label(name);
    snprintf(name, sizeof(name), "dock-title-%u", w->id);
    gtk_widget_set_name(w->title, name);
    g_object_set_data(G_OBJECT(w->title), "window", w);
    source(w->title);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->title);
    w->native = gtk_label_new(NULL);
    gtk_widget_set_size_request(w->native, 90, 30);
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(header), w->native);
    gtk_window_set_titlebar(GTK_WINDOW(w->win), header);
    gtk_window_set_title(GTK_WINDOW(w->win), gtk_button_get_label(GTK_BUTTON(w->title)));
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    w->list = gtk_list_box_new();
    gtk_widget_set_size_request(w->list, 120, -1);
    gtk_widget_set_no_show_all(w->list, TRUE);
    w->stack = gtk_stack_new();
    gtk_stack_set_homogeneous(GTK_STACK(w->stack), FALSE);
    gtk_box_pack_start(GTK_BOX(body), w->list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), w->stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(w->win), body);
    destination(body, w);
    destination(header, w);
    g_signal_connect(w->win, "key-press-event", G_CALLBACK(key_press), w);
    g_signal_connect(w->win, "size-allocate", G_CALLBACK(allocated), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(destroyed), w);
    gtk_widget_show_all(w->win);
    return w;
}

static void select_tab(GtkButton *button, Tab *t)
{
    (void)button;
    t->owner->active = t;
    refresh(t->owner);
}

static void close_tab(GtkButton *button, Tab *t)
{
    (void)button;
    Window *w = t->owner;
    detach(t);
    release(t);
    refresh(w);
}

static void add_tab(Window *w, Tab *t, char label)
{
    t->name = label;
    t->session = pilot_session_open(label);
    t->row = gtk_list_box_row_new();
    g_object_ref_sink(t->row);
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    char caption[] = "Session A";
    caption[8] = label;
    t->pick = gtk_button_new_with_label(caption);
    t->close = gtk_button_new_with_label("×");
    gtk_style_context_add_class(gtk_widget_get_style_context(t->close), "close-tab");
    gtk_widget_set_tooltip_text(t->close, "Close terminal");
    gtk_box_pack_start(GTK_BOX(line), t->pick, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(line), t->close, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(t->row), line);
    g_object_set_data(G_OBJECT(t->pick), "tab", t);
    source(t->pick);
    g_signal_connect(t->pick, "clicked", G_CALLBACK(select_tab), t);
    g_signal_connect(t->close, "clicked", G_CALLBACK(close_tab), t);
    gtk_widget_show_all(t->row);
    gtk_widget_show(pilot_session_widget(t->session));
    attach(w, t);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    gtk_init(&argc, &argv);
    printf("backend %s\n", G_OBJECT_TYPE_NAME(gdk_display_get_default()));
    GtkCssProvider *style = gtk_css_provider_new();
    gtk_css_provider_load_from_data(style,
        ".close-tab { opacity: 0; } row:hover .close-tab, .close-tab:focus { opacity: 1; }"
        "#dock-title-1 { background: #146da3; color: white; }"
        "#dock-title-2 { background: #b35a19; color: white; }"
        "#dock-title-3 { background: #398349; color: white; }"
        "#dock-title-4 { background: #8b4789; color: white; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(style);
    windows = g_ptr_array_new_with_free_func(g_free);
    Window *first = open_window();
    add_tab(first, &tabs[0], 'A');
    add_tab(first, &tabs[1], 'B');
    refresh(first);
    Window *second = open_window();
    add_tab(second, &tabs[2], 'C');
    refresh(second);
    gtk_main();
    g_ptr_array_unref(windows);
    return 0;
}
