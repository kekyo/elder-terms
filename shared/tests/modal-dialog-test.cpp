#include <elder-terms/modal-dialog.h>
#include <elder-terms/modal-color-button.h>

#include <iostream>
#include <stdexcept>
#include <gtk/gtk.h>

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static void mark_destroyed(GtkWidget *, gpointer data) {
  *static_cast<bool *>(data) = true;
}

static void restore_application_focus(GObject *parent, GParamSpec *, gpointer data) {
  if (gtk_widget_get_sensitive(GTK_WIDGET(parent))) gtk_widget_grab_focus(GTK_WIDGET(data));
}

static GtkWidget *find_color_dialog(GtkWindow *parent) {
  auto *windows = gtk_window_list_toplevels();
  GtkWidget *result = nullptr;
  int count = 0;
  for (auto *item = windows; item != nullptr; item = item->next) {
    auto *window = GTK_WIDGET(item->data);
    if (GTK_IS_COLOR_CHOOSER_DIALOG(window) &&
        gtk_window_get_transient_for(GTK_WINDOW(window)) == parent) {
      result = window;
      ++count;
    }
  }
  g_list_free(windows);
  require(count == 1, "Repeated color selection must show exactly one chooser");
  return result;
}

static void count_color_changes(GtkColorButton *, gpointer data) {
  ++*static_cast<int *>(data);
}

static void destroy_color_parent(GObject *, GParamSpec *, gpointer data) {
  gtk_widget_destroy(GTK_WIDGET(data));
}

static void check_color_selection() {
  auto *parent = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *button = elder_terms::create_modal_color_button();
  gtk_container_add(GTK_CONTAINER(parent), button);
  gtk_widget_show_all(parent);
  const GdkRGBA original{0.0, 0.0, 0.0, 1.0};
  const GdkRGBA selected{1.0, 0.0, 0.0, 1.0};
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(button), &original);
  int changes = 0;
  g_signal_connect(button, "color-set", G_CALLBACK(count_color_changes), &changes);
  gtk_button_clicked(GTK_BUTTON(button));
  gtk_button_clicked(GTK_BUTTON(button));
  auto *dialog = find_color_dialog(GTK_WINDOW(parent));
  require(!gtk_window_get_modal(GTK_WINDOW(dialog)), "Color selection must not use GTK modal");
  require(!gtk_widget_get_sensitive(parent), "Color selection must disable its parent");
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(dialog), &selected);
  gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  GdkRGBA actual{};
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &actual);
  require(gdk_rgba_equal(&original, &actual) && changes == 0,
      "Cancelling must preserve the color without a change notification");
  require(gtk_widget_get_sensitive(parent), "Cancelling must restore the parent");

  gtk_button_clicked(GTK_BUTTON(button));
  dialog = find_color_dialog(GTK_WINDOW(parent));
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(dialog), &selected);
  gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &actual);
  require(gdk_rgba_equal(&selected, &actual) && changes == 1,
      "Accepting must update the color and notify exactly once");
  require(gtk_widget_get_sensitive(parent), "Accepting must restore the parent");

  gtk_button_clicked(GTK_BUTTON(button));
  dialog = find_color_dialog(GTK_WINDOW(parent));
  bool destroyed = false;
  g_signal_connect(dialog, "destroy", G_CALLBACK(mark_destroyed), &destroyed);
  gtk_widget_destroy(button);
  require(destroyed && gtk_widget_get_sensitive(parent),
      "Removing the color button must close its chooser and restore the parent");
  require(changes == 1, "Removing the color button must not accept a color");
  button = elder_terms::create_modal_color_button();
  gtk_container_add(GTK_CONTAINER(parent), button);
  gtk_widget_show_all(parent);
  gtk_button_clicked(GTK_BUTTON(button));
  dialog = find_color_dialog(GTK_WINDOW(parent));
  destroyed = false;
  g_signal_connect(dialog, "destroy", G_CALLBACK(mark_destroyed), &destroyed);
  bool parent_destroyed = false;
  g_signal_connect(parent, "destroy", G_CALLBACK(mark_destroyed), &parent_destroyed);
  g_signal_connect(button, "notify::rgba", G_CALLBACK(destroy_color_parent), parent);
  g_signal_connect(button, "color-set", G_CALLBACK(count_color_changes), &changes);
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(dialog), &selected);
  gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  require(parent_destroyed && destroyed,
      "Settings destruction during color notification must close the chooser safely");
  require(changes == 1, "Destroyed settings must not receive a color selection");
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  try {
    check_color_selection();
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

    auto *focus_parent = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    auto *previous = gtk_entry_new();
    auto *preferred = gtk_entry_new();
    gtk_container_add(GTK_CONTAINER(focus_parent), box);
    gtk_box_pack_start(GTK_BOX(box), previous, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), preferred, TRUE, TRUE, 0);
    gtk_widget_show_all(focus_parent);
    gtk_widget_grab_focus(previous);
    g_signal_connect(focus_parent, "notify::sensitive", G_CALLBACK(restore_application_focus), preferred);
    auto *focus_dialog = gtk_dialog_new();
    elder_terms::show_modal_dialog(focus_dialog, GTK_WINDOW(focus_parent));
    gtk_widget_destroy(focus_dialog);
    require(gtk_window_get_focus(GTK_WINDOW(focus_parent)) == preferred,
        "Closing a dialog must preserve the application's preferred focus");
    gtk_widget_destroy(focus_parent);

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
