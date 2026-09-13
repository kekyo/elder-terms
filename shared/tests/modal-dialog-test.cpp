#include <elder-terms/modal-dialog.h>

#include <iostream>
#include <stdexcept>
#include <gtk/gtk.h>

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static void mark_destroyed(GtkWidget *, gpointer data) {
  *static_cast<bool *>(data) = true;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  try {
    auto *parent = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto *entry = gtk_entry_new();
    gtk_container_add(GTK_CONTAINER(parent), entry);
    gtk_widget_show_all(parent);
    gtk_widget_grab_focus(entry);
    auto *settings = gtk_dialog_new();
    elder_terms::show_modal_dialog(settings, GTK_WINDOW(parent));
    require(!gtk_window_get_modal(GTK_WINDOW(settings)), "GTK modal must be disabled");
    require(!gtk_widget_get_sensitive(parent), "Parent must be disabled");
    require(gtk_widget_get_sensitive(settings), "Dialog must remain interactive");
    require(gtk_window_get_transient_for(GTK_WINDOW(settings)) == GTK_WINDOW(parent), "Transient parent must be set");
    require(gtk_window_get_destroy_with_parent(GTK_WINDOW(settings)), "Dialog must follow parent destruction");

    auto *chooser = gtk_dialog_new();
    elder_terms::show_modal_dialog(chooser, GTK_WINDOW(settings));
    elder_terms::show_modal_dialog(chooser, GTK_WINDOW(settings));
    elder_terms::present_modal_dialog(parent);
    require(!gtk_widget_get_sensitive(settings), "Nested dialog must disable settings");
    gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_ACCEPT);
    require(!gtk_widget_get_sensitive(settings), "Response without closing must keep parent disabled");
    gtk_widget_destroy(chooser);
    require(gtk_widget_get_sensitive(settings), "Closing nested dialog must restore settings");
    require(!gtk_widget_get_sensitive(parent), "Closing nested dialog must not restore root");

    auto *notice = gtk_dialog_new();
    elder_terms::show_modal_dialog(notice, GTK_WINDOW(parent));
    gtk_widget_destroy(settings);
    require(!gtk_widget_get_sensitive(parent), "Remaining sibling must keep parent disabled");
    gtk_widget_destroy(notice);
    require(gtk_widget_get_sensitive(parent), "Last dialog must restore parent");
    require(!elder_terms::has_modal_dialog(parent), "Closed dialogs must be unregistered");
    require(gtk_window_get_focus(GTK_WINDOW(parent)) == entry, "Original focus must be restored");

    auto *reusable = gtk_dialog_new();
    elder_terms::show_modal_dialog(reusable, GTK_WINDOW(parent));
    gtk_widget_hide(reusable);
    require(gtk_widget_get_sensitive(parent), "Hiding a dialog must restore parent");
    elder_terms::show_modal_dialog(reusable, GTK_WINDOW(parent));
    require(!gtk_widget_get_sensitive(parent), "Showing again must disable parent again");
    gtk_widget_destroy(entry);
    gtk_widget_destroy(reusable);
    require(gtk_widget_get_sensitive(parent), "Destroyed focus widget must not prevent restoration");

    gtk_widget_set_sensitive(parent, FALSE);
    auto *disabled_child = gtk_dialog_new();
    elder_terms::show_modal_dialog(disabled_child, GTK_WINDOW(parent));
    gtk_widget_destroy(disabled_child);
    require(!gtk_widget_get_sensitive(parent), "Originally disabled parent must stay disabled");
    gtk_widget_set_sensitive(parent, TRUE);
    gtk_widget_hide(parent);
    auto *hidden_child = gtk_dialog_new();
    elder_terms::show_modal_dialog(hidden_child, GTK_WINDOW(parent));
    gtk_widget_destroy(hidden_child);
    require(!gtk_widget_get_visible(parent), "Hidden parent must not be presented on close");

    bool settings_destroyed = false;
    bool chooser_destroyed = false;
    settings = gtk_dialog_new();
    chooser = gtk_dialog_new();
    g_signal_connect(settings, "destroy", G_CALLBACK(mark_destroyed), &settings_destroyed);
    g_signal_connect(chooser, "destroy", G_CALLBACK(mark_destroyed), &chooser_destroyed);
    elder_terms::show_modal_dialog(settings, GTK_WINDOW(parent));
    elder_terms::show_modal_dialog(chooser, GTK_WINDOW(settings));
    gtk_widget_destroy(parent);
    require(settings_destroyed && chooser_destroyed, "Parent destruction must close descendants");

    auto *orphan = gtk_dialog_new();
    elder_terms::show_modal_dialog(orphan, nullptr);
    require(!gtk_window_get_modal(GTK_WINDOW(orphan)), "Parentless dialog must not acquire GTK modal state");
    gtk_widget_destroy(orphan);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
