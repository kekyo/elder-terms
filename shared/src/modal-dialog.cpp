#include <elder-terms/modal-dialog.h>

#include <algorithm>
#include <vector>

namespace elder_terms {

static constexpr char modal_state_key[] = "elder-terms-modal-dialog-state";

// Each window owns its node. Edges are non-owning and are removed on hide or
// destroy, before GTK releases the corresponding window objects.
struct ModalDialogState {
  GtkWidget *window;
  ModalDialogState *parent = nullptr;
  std::vector<ModalDialogState *> children{};
  GWeakRef previous_focus{};
  bool active = false;
  bool destroyed = false;
  bool parent_was_visible = false;
  bool parent_was_sensitive = false;
};

static ModalDialogState *modal_state(GtkWidget *window) {
  return window == nullptr ? nullptr : static_cast<ModalDialogState *>(
      g_object_get_data(G_OBJECT(window), modal_state_key));
}

static bool closing_window(const ModalDialogState *state) {
  for (auto *current = state; current != nullptr; current = current->parent) {
    if (current->destroyed || gtk_widget_in_destruction(current->window)) return true;
  }
  return false;
}

bool has_modal_dialog(GtkWidget *window) {
  const auto *state = modal_state(window);
  return state != nullptr && !state->destroyed && !state->children.empty();
}

void present_modal_dialog(GtkWidget *window) {
  if (window == nullptr) return;
  auto *state = modal_state(window);
  if (state != nullptr) {
    if (closing_window(state)) return;
    // Reopening an older sibling also makes it the parent's focus target.
    for (auto *current = state; current->parent != nullptr; current = current->parent) {
      auto &siblings = current->parent->children;
      std::erase(siblings, current);
      siblings.push_back(current);
    }
    while (!state->children.empty()) state = state->children.back();
    if (closing_window(state)) return;
    window = state->window;
  }
  if (!gtk_widget_in_destruction(window)) {
    gtk_window_present_with_time(GTK_WINDOW(window), gtk_get_current_event_time());
  }
}

static void release_modal_parent(ModalDialogState *state) {
  state->active = false;
  auto *parent = state->parent;
  if (parent == nullptr) return;
  state->parent = nullptr;
  std::erase(parent->children, state);
  if (closing_window(parent)) return;

  // Sensitivity notifications may run application callbacks that destroy the
  // parent. Keep its node alive until restoration is finished.
  g_object_ref(parent->window);
  if (parent->children.empty()) {
    auto *focus = static_cast<GtkWidget *>(g_weak_ref_get(&parent->previous_focus));
    g_weak_ref_set(&parent->previous_focus, nullptr);
    gtk_widget_set_sensitive(parent->window, parent->parent_was_sensitive);
    if (!closing_window(parent) && parent->children.empty()) {
      if (focus != nullptr && !gtk_widget_in_destruction(focus) &&
          gtk_widget_is_sensitive(focus) &&
          gtk_widget_get_toplevel(focus) == parent->window) {
        gtk_widget_grab_focus(focus);
      }
      if (parent->parent_was_visible && gtk_widget_get_visible(parent->window)) {
        present_modal_dialog(parent->window);
      }
    }
    if (focus != nullptr) g_object_unref(focus);
  } else {
    present_modal_dialog(parent->window);
  }
  g_object_unref(parent->window);
}

static void on_modal_hide(GtkWidget *, gpointer data) {
  release_modal_parent(static_cast<ModalDialogState *>(data));
}

static void on_modal_destroy(GtkWidget *, gpointer data) {
  auto *state = static_cast<ModalDialogState *>(data);
  state->destroyed = true;
  // Detach before destroying children: their callbacks can in turn dispose
  // application-owned widgets or attempt to restore a parent.
  while (!state->children.empty()) {
    auto *child = state->children.back();
    state->children.pop_back();
    child->parent = nullptr;
    gtk_widget_destroy(child->window);
  }
  release_modal_parent(state);
}

static gboolean on_modal_focus_in(GtkWidget *window, GdkEventFocus *, gpointer) {
  if (!has_modal_dialog(window)) return GDK_EVENT_PROPAGATE;
  present_modal_dialog(window);
  return GDK_EVENT_STOP;
}

static void destroy_modal_state(gpointer data) {
  auto *state = static_cast<ModalDialogState *>(data);
  g_weak_ref_clear(&state->previous_focus);
  delete state;
}

static ModalDialogState *ensure_modal_state(GtkWidget *window) {
  auto *state = modal_state(window);
  if (state != nullptr) return state;
  state = new ModalDialogState{.window = window};
  g_weak_ref_init(&state->previous_focus, nullptr);
  g_object_set_data_full(G_OBJECT(window), modal_state_key, state, destroy_modal_state);
  g_signal_connect(window, "hide", G_CALLBACK(on_modal_hide), state);
  g_signal_connect(window, "destroy", G_CALLBACK(on_modal_destroy), state);
  g_signal_connect(window, "focus-in-event", G_CALLBACK(on_modal_focus_in), nullptr);
  return state;
}

void show_modal_dialog(GtkWidget *dialog, GtkWindow *parent) {
  g_return_if_fail(GTK_IS_WINDOW(dialog));
  g_return_if_fail(parent == nullptr || GTK_IS_WINDOW(parent));
  auto *state = ensure_modal_state(dialog);
  if (state->destroyed || gtk_widget_in_destruction(dialog)) return;
  if (state->active) {
    present_modal_dialog(dialog);
    return;
  }
  auto *parent_state = parent == nullptr ? nullptr : ensure_modal_state(GTK_WIDGET(parent));
  for (auto *ancestor = parent_state; ancestor != nullptr; ancestor = ancestor->parent) {
    g_return_if_fail(ancestor != state);
  }
  if (parent_state != nullptr && closing_window(parent_state)) {
    gtk_widget_destroy(dialog);
    return;
  }
  g_object_ref(dialog);
  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  state->active = true;
  state->parent = parent_state;
  if (parent_state != nullptr) {
    if (parent_state->children.empty()) {
      parent_state->parent_was_sensitive = gtk_widget_get_sensitive(GTK_WIDGET(parent));
      parent_state->parent_was_visible = gtk_widget_get_visible(GTK_WIDGET(parent));
      g_weak_ref_set(&parent_state->previous_focus, gtk_window_get_focus(parent));
    }
    parent_state->children.push_back(state);
    gtk_widget_set_sensitive(GTK_WIDGET(parent), FALSE);
  }
  if (!state->destroyed) {
    gtk_widget_show_all(dialog);
    present_modal_dialog(dialog);
  }
  g_object_unref(dialog);
}

} // namespace elder_terms
