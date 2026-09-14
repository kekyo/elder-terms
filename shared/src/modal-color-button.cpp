#include <elder-terms/modal-color-button.h>
#include <elder-terms/modal-dialog.h>

#include <string>
#include <utility>

namespace elder_terms {

static constexpr char color_state_key[] = "elder-terms-modal-color-button-state";

struct ModalColorButtonState {
  GtkWidget *dialog = nullptr;
  bool destroyed = false;
};

static ModalColorButtonState *color_state(GtkWidget *button) {
  return static_cast<ModalColorButtonState *>(
      g_object_get_data(G_OBJECT(button), color_state_key));
}

static void on_color_dialog_destroy(GtkWidget *dialog, gpointer data) {
  auto *state = color_state(GTK_WIDGET(data));
  if (state->dialog == dialog) state->dialog = nullptr;
}

static void on_color_dialog_response(GtkDialog *dialog, gint response,
                                     gpointer data) {
  auto *button = GTK_WIDGET(data);
  auto *state = color_state(button);
  // Updating the button can notify observers that destroy its owner and chooser.
  g_object_ref(dialog);
  const bool accepted = response == GTK_RESPONSE_OK;
  if (accepted && !state->destroyed) {
    GdkRGBA color{};
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dialog), &color);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(button), &color);
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  // Closing the dialog restores its parent and may dispose the button through
  // application callbacks. connect_object keeps the object alive during this
  // handler, and destroyed prevents notifying already disposed settings data.
  if (accepted && !state->destroyed) g_signal_emit_by_name(button, "color-set");
  g_object_unref(dialog);
}

static void on_color_button_destroy(GtkWidget *, gpointer data) {
  auto *state = static_cast<ModalColorButtonState *>(data);
  state->destroyed = true;
  auto *dialog = std::exchange(state->dialog, nullptr);
  if (dialog != nullptr) gtk_widget_destroy(dialog);
}

static void on_color_button_clicked(GtkButton *button) {
  auto *widget = GTK_WIDGET(button);
  auto *state = color_state(widget);
  if (state->destroyed) return;
  if (state->dialog != nullptr) {
    present_modal_dialog(state->dialog);
    return;
  }
  auto *top = gtk_widget_get_toplevel(widget);
  auto *parent = GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr;
  auto *dialog = gtk_color_chooser_dialog_new(
      gtk_color_button_get_title(GTK_COLOR_BUTTON(button)), parent);
  state->dialog = dialog;
  const auto id = std::string(gtk_widget_get_name(widget)) + "_dialog";
  gtk_widget_set_name(dialog, id.c_str());
  atk_object_set_accessible_id(gtk_widget_get_accessible(dialog), id.c_str());
  GdkRGBA color{};
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &color);
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(dialog), &color);
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(dialog),
      gtk_color_chooser_get_use_alpha(GTK_COLOR_CHOOSER(button)));
  gboolean show_editor = FALSE;
  g_object_get(button, "show-editor", &show_editor, nullptr);
  g_object_set(dialog, "show-editor", show_editor, nullptr);
  g_signal_connect_object(dialog, "response", G_CALLBACK(on_color_dialog_response),
      G_OBJECT(button), static_cast<GConnectFlags>(0));
  g_signal_connect_object(dialog, "destroy", G_CALLBACK(on_color_dialog_destroy),
      G_OBJECT(button), static_cast<GConnectFlags>(0));
  show_modal_dialog(dialog, parent);
}

static void initialize_modal_color_button_class(gpointer type_class, gpointer) {
  // GtkButton::clicked runs its class handler before ordinary signal handlers.
  // Replace only that public virtual method: chaining to GtkColorButton would
  // open its unmanaged chooser. Retain GTK's swatch, accessibility and DnD.
  GTK_BUTTON_CLASS(type_class)->clicked = on_color_button_clicked;
}

GtkWidget *create_modal_color_button() {
  static const GType type = g_type_register_static_simple(
      GTK_TYPE_COLOR_BUTTON, "ElderTermsModalColorButton",
      sizeof(GtkColorButtonClass), initialize_modal_color_button_class,
      sizeof(GtkColorButton), nullptr, static_cast<GTypeFlags>(0));
  auto *button = GTK_WIDGET(g_object_new(type, nullptr));
  auto *state = new ModalColorButtonState();
  g_object_set_data_full(G_OBJECT(button), color_state_key, state,
      [](gpointer data) { delete static_cast<ModalColorButtonState *>(data); });
  g_signal_connect(button, "destroy", G_CALLBACK(on_color_button_destroy), state);
  return button;
}

} // namespace elder_terms
