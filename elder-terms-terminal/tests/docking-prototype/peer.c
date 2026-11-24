// Independent application offering a file copy and refusing a dock.
// It shares neither GTK's drag context nor the pilot's sessions.
#include <gtk/gtk.h>

static void provide_file(GtkWidget *widget, GdkDragContext *context,
    GtkSelectionData *data, guint info, guint time, gpointer unused)
{
    (void)widget; (void)context; (void)info; (void)time; (void)unused;
    char *uris[] = {"file:///tmp/dock-pilot.txt", NULL};
    gtk_selection_data_set_uris(data, uris);
}

static gboolean refuse(GtkWidget *widget, GdkDragContext *ctx, gint x, gint y,
    guint time, gpointer unused)
{
    (void)widget; (void)x; (void)y; (void)unused;
    gtk_drag_finish(ctx, FALSE, FALSE, time);
    g_print("refused\n");
    return TRUE;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w), "Dock pilot peer");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *file = gtk_button_new_with_label("File");
    GtkWidget *reject = gtk_button_new_with_label("Refuse");
    gtk_box_pack_start(GTK_BOX(box), file, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), reject, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(w), box);
    gtk_window_set_default_size(GTK_WINDOW(w), 240, 90);
    GtkTargetEntry uri = {"text/uri-list", 0, 0};
    GtkTargetEntry tab = {"application/x-elder-terms-dock-pilot", 0, 0};
    gtk_drag_source_set(file, GDK_BUTTON1_MASK, &uri, 1, GDK_ACTION_COPY);
    gtk_drag_dest_set(reject, GTK_DEST_DEFAULT_MOTION, &tab, 1, GDK_ACTION_MOVE);
    g_signal_connect(file, "drag-data-get", G_CALLBACK(provide_file), NULL);
    g_signal_connect(reject, "drag-drop", G_CALLBACK(refuse), NULL);
    g_signal_connect(w, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_widget_show_all(w);
    gtk_main();
    return 0;
}
