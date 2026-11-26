#include "../../src/tab-tearoff.h"
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/extensions/Xfixes.h>
#include <stdlib.h>

static gboolean has_undock_badge(Display *display)
{
    XFixesCursorImage *image = XFixesGetCursorImage(display);
    g_assert_nonnull(image);
    int matching = 0;
    for (unsigned long index = 0; index < image->width * image->height; ++index) {
        unsigned long pixel = image->pixels[index];
        int red = (pixel >> 16) & 0xff;
        int green = (pixel >> 8) & 0xff;
        int blue = pixel & 0xff;
        if (abs(red - 66) <= 2 && abs(green - 82) <= 2 && abs(blue - 112) <= 2)
            ++matching;
    }
    XFree(image);
    return matching >= 20;
}

static void undock(void *context)
{
    ++*(int *)context;
}

static void emit_button(GtkWidget *button, const char *signal, double x, double y)
{
    GdkEventButton event = {0};
    gboolean handled = FALSE;
    event.type = g_strcmp0(signal, "button-press-event") == 0 ? GDK_BUTTON_PRESS : GDK_BUTTON_RELEASE;
    event.button = GDK_BUTTON_PRIMARY;
    event.x = x;
    event.y = y;
    g_signal_emit_by_name(button, signal, &event, &handled);
    g_assert_true(handled);
}

static void emit_motion(GtkWidget *button, double x, double y)
{
    GdkEventMotion event = {0};
    gboolean handled = FALSE;
    event.type = GDK_MOTION_NOTIFY;
    event.x = x;
    event.y = y;
    g_signal_emit_by_name(button, "motion-notify-event", &event, &handled);
    g_assert_true(handled);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *button = gtk_button_new_with_label("Session A");
    gtk_container_add(GTK_CONTAINER(window), button);
    gtk_window_set_default_size(GTK_WINDOW(window), 320, 180);
    gtk_widget_show_all(window);
    while (gtk_events_pending()) gtk_main_iteration();

    int undocked = 0;
    TearoffGesture *gesture = tearoff_gesture_bind(button, undock, &undocked);
    tearoff_gesture_set_enabled(gesture, TRUE);
    GdkWindow *native = gtk_widget_get_window(button);
    Display *display = gdk_x11_display_get_xdisplay(gdk_window_get_display(native));
    XWarpPointer(display, None, gdk_x11_window_get_xid(native), 0, 0, 0, 0, 8, 8);
    XSync(display, False);
    while (gtk_events_pending()) gtk_main_iteration();
    GdkCursor *original = gdk_window_get_cursor(native);
    if (original) g_object_ref(original);

    emit_button(button, "button-press-event", 5, 5);
    emit_motion(button, 40, 40);
    g_assert_nonnull(gdk_window_get_cursor(native));
    g_assert_true(gdk_window_get_cursor(native) != original);
    XSync(display, False);
    g_assert_true(has_undock_badge(display));
    emit_button(button, "button-release-event", 40, 40);
    g_assert_true(gdk_window_get_cursor(native) == original);
    XSync(display, False);
    g_assert_false(has_undock_badge(display));
    g_assert_cmpint(undocked, ==, 0);

    emit_button(button, "button-press-event", 5, 5);
    emit_motion(button, 40, 40);
    GdkEventKey escape = {0};
    gboolean handled = FALSE;
    escape.type = GDK_KEY_PRESS;
    escape.keyval = GDK_KEY_Escape;
    g_signal_emit_by_name(window, "key-press-event", &escape, &handled);
    g_assert_true(handled);
    g_assert_true(gdk_window_get_cursor(native) == original);
    XSync(display, False);
    g_assert_false(has_undock_badge(display));

    if (original) g_object_unref(original);
    gtk_widget_destroy(window);
    return 0;
}
