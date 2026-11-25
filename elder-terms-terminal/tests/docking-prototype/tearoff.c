#include "tearoff.h"

struct TearoffGesture {
    GtkWidget *button;
    GtkWindow *window;
    TearoffUndock undock;
    void *context;
    gulong key_handler;
    double start_x;
    double start_y;
    gboolean enabled;
    gboolean pressed;
    gboolean dragging;
    double previous_opacity;
};

static void finish(TearoffGesture *gesture)
{
    if (gesture->window) {
        if (gesture->key_handler)
            g_signal_handler_disconnect(gesture->window, gesture->key_handler);
        g_object_remove_weak_pointer(G_OBJECT(gesture->window), (gpointer *)&gesture->window);
    }
    gesture->window = NULL;
    gesture->key_handler = 0;
    gesture->pressed = FALSE;
    if (gesture->dragging)
        gtk_widget_set_opacity(gesture->button, gesture->previous_opacity);
    gesture->dragging = FALSE;
}

static gboolean escape(GtkWidget *widget, GdkEventKey *event, TearoffGesture *gesture)
{
    (void)widget;
    if (!gesture->pressed || event->keyval != GDK_KEY_Escape) return FALSE;
    finish(gesture);
    return TRUE;
}

static gboolean press(GtkWidget *widget, GdkEventButton *event, TearoffGesture *gesture)
{
    (void)widget;
    if (!gesture->enabled || event->button != GDK_BUTTON_PRIMARY ||
        event->type != GDK_BUTTON_PRESS) return FALSE;
    if (gesture->pressed) return TRUE;
    GtkWidget *toplevel = gtk_widget_get_toplevel(gesture->button);
    if (!GTK_IS_WINDOW(toplevel)) return FALSE;
    GtkWindow *window = GTK_WINDOW(toplevel);
    gesture->window = window;
    g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&gesture->window);
    gesture->key_handler = g_signal_connect(window, "key-press-event", G_CALLBACK(escape), gesture);
    gesture->start_x = event->x;
    gesture->start_y = event->y;
    gesture->pressed = TRUE;
    gesture->dragging = FALSE;
    return TRUE;
}

static gboolean motion(GtkWidget *widget, GdkEventMotion *event, TearoffGesture *gesture)
{
    if (!gesture->pressed) return FALSE;
    if (!gesture->dragging &&
        gtk_drag_check_threshold(widget, gesture->start_x, gesture->start_y, event->x, event->y)) {
        gesture->dragging = TRUE;
        gesture->previous_opacity = gtk_widget_get_opacity(widget);
        gtk_widget_set_opacity(widget, 0.65);
    }
    return TRUE;
}

static gboolean release(GtkWidget *widget, GdkEventButton *event, TearoffGesture *gesture)
{
    if (!gesture->pressed || event->button != GDK_BUTTON_PRIMARY) return FALSE;
    int x, y;
    gboolean translated = gesture->window &&
        gtk_widget_translate_coordinates(widget, GTK_WIDGET(gesture->window),
            (int)event->x, (int)event->y, &x, &y);
    gboolean outside = translated &&
        (x < 0 || y < 0 ||
         x >= gtk_widget_get_allocated_width(GTK_WIDGET(gesture->window)) ||
         y >= gtk_widget_get_allocated_height(GTK_WIDGET(gesture->window)));
    gboolean clicked = !gesture->dragging &&
        event->x >= 0 && event->y >= 0 &&
        event->x < gtk_widget_get_allocated_width(widget) &&
        event->y < gtk_widget_get_allocated_height(widget);
    gboolean dragging = gesture->dragging;
    finish(gesture);
    if (dragging && outside) gesture->undock(gesture->context);
    else if (clicked) g_signal_emit_by_name(widget, "clicked");
    return TRUE;
}

static void dispose(gpointer data)
{
    TearoffGesture *gesture = data;
    if (gesture->window) {
        if (gesture->key_handler)
            g_signal_handler_disconnect(gesture->window, gesture->key_handler);
        g_object_remove_weak_pointer(G_OBJECT(gesture->window), (gpointer *)&gesture->window);
    }
    g_free(gesture);
}

static gboolean grab_broken(GtkWidget *widget, GdkEventGrabBroken *event, TearoffGesture *gesture)
{
    (void)widget;
    (void)event;
    if (gesture->pressed) finish(gesture);
    return FALSE;
}

static void unmapped(GtkWidget *widget, TearoffGesture *gesture)
{
    (void)widget;
    if (gesture->pressed) finish(gesture);
}

TearoffGesture *tearoff_gesture_bind(
    GtkWidget *button, TearoffUndock undock, void *context)
{
    TearoffGesture *gesture = g_new0(TearoffGesture, 1);
    gesture->button = button;
    gesture->undock = undock;
    gesture->context = context;
    gtk_widget_add_events(button, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK);
    g_signal_connect(button, "button-press-event", G_CALLBACK(press), gesture);
    g_signal_connect(button, "motion-notify-event", G_CALLBACK(motion), gesture);
    g_signal_connect(button, "button-release-event", G_CALLBACK(release), gesture);
    g_signal_connect(button, "grab-broken-event", G_CALLBACK(grab_broken), gesture);
    g_signal_connect(button, "unmap", G_CALLBACK(unmapped), gesture);
    g_object_set_data_full(G_OBJECT(button), "tearoff-gesture", gesture, dispose);
    return gesture;
}

void tearoff_gesture_set_enabled(TearoffGesture *gesture, gboolean enabled)
{
    gesture->enabled = enabled;
    if (!enabled && gesture->pressed) finish(gesture);
}
