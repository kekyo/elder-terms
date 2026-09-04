#include "hyperlink-settings-editor.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static constexpr char link_widget_index_key[] =
    "elder-terms-link-widget-index";

struct HyperlinkSettingsEditorState {
  SettingsStore *store = nullptr;
  std::string id_prefix;
  std::function<void()> changed;
  bool synchronizing = false;
  int selected_rule = -1;
  unsigned int next_rule_number = 1;
  GtkWidget *root = nullptr;
  GtkWidget *enabled_combo = nullptr;
  GtkWidget *reset_button = nullptr;
  GtkWidget *rule_list = nullptr;
  GtkWidget *remove_button = nullptr;
  GtkWidget *move_up_button = nullptr;
  GtkWidget *move_down_button = nullptr;
  GtkWidget *editor = nullptr;
  GtkWidget *id_entry = nullptr;
  GtkWidget *source_combo = nullptr;
  GtkWidget *regex_entry = nullptr;
  GtkWidget *command_entry = nullptr;
  GtkWidget *arguments_box = nullptr;
  GtkWidget *argument_add_button = nullptr;
  GtkWidget *validation_combo = nullptr;
  GtkWidget *path_entry = nullptr;
  GtkWidget *validation_label = nullptr;
};

static void assign_accessible_id(GtkWidget *widget, const std::string &id) {
  gtk_widget_set_name(widget, id.c_str());
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(widget),
                                   "accessible-id") != nullptr) {
    g_object_set(widget, "accessible-id", id.c_str(), nullptr);
  }
  AtkObject *accessible = gtk_widget_get_accessible(widget);
  if (accessible != nullptr) {
    atk_object_set_accessible_id(accessible, id.c_str());
  }
}

static std::string widget_id(const HyperlinkSettingsEditorState *state,
                             const std::string &suffix) {
  return state->id_prefix + "_" + suffix;
}

static GtkWidget *create_label(const char *text) {
  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  return label;
}

static GtkWidget *create_entry(HyperlinkSettingsEditorState *state,
                               const std::string &suffix) {
  GtkWidget *entry = gtk_entry_new();
  assign_accessible_id(entry, widget_id(state, suffix));
  gtk_widget_set_hexpand(entry, TRUE);
  return entry;
}

static GtkWidget *create_combo(HyperlinkSettingsEditorState *state,
                               const std::string &suffix) {
  GtkWidget *combo = gtk_combo_box_text_new();
  assign_accessible_id(combo, widget_id(state, suffix));
  gtk_widget_set_hexpand(combo, TRUE);
  return combo;
}

static void attach_editor_row(GtkWidget *grid, int row, const char *label,
                              GtkWidget *control) {
  gtk_grid_attach(GTK_GRID(grid), create_label(label), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
}

static const char *active_combo_id(GtkWidget *combo, const char *fallback) {
  const char *active = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
  return active == nullptr ? fallback : active;
}

static HyperlinkActionRule *selected_rule(
    HyperlinkSettingsEditorState *state) {
  if (state == nullptr || state->store == nullptr ||
      state->selected_rule < 0 ||
      static_cast<std::size_t>(state->selected_rule) >=
          state->store->hyperlink_rules.size()) {
    return nullptr;
  }
  return &state->store->hyperlink_rules[state->selected_rule];
}

static void clear_container(GtkWidget *container) {
  GList *children = gtk_container_get_children(GTK_CONTAINER(container));
  for (GList *child = children; child != nullptr; child = child->next) {
    gtk_widget_destroy(GTK_WIDGET(child->data));
  }
  g_list_free(children);
}

static int widget_index(GtkWidget *widget) {
  return GPOINTER_TO_INT(
             g_object_get_data(G_OBJECT(widget), link_widget_index_key)) -
         1;
}

static void set_entry_validation(GtkWidget *entry, bool valid,
                                 const std::string &reason) {
  gtk_entry_set_icon_from_icon_name(
      GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY,
      valid ? nullptr : "dialog-error-symbolic");
  gtk_entry_set_icon_tooltip_text(GTK_ENTRY(entry),
                                  GTK_ENTRY_ICON_SECONDARY,
                                  valid ? nullptr : reason.c_str());
}

static bool rule_id_is_unique(const HyperlinkSettingsEditorState *state,
                              const HyperlinkActionRule *selected) {
  return std::count_if(
             state->store->hyperlink_rules.begin(),
             state->store->hyperlink_rules.end(),
             [selected](const HyperlinkActionRule &rule) {
               return rule.id == selected->id;
             }) == 1;
}

static bool all_rules_are_valid(
    const HyperlinkSettingsEditorState *state) {
  if (state == nullptr || state->store == nullptr) {
    return false;
  }
  for (const HyperlinkActionRule &rule : state->store->hyperlink_rules) {
    std::string reason;
    if (!hyperlink_action_rule_is_valid(rule, &reason) ||
        std::count_if(state->store->hyperlink_rules.begin(),
                      state->store->hyperlink_rules.end(),
                      [&rule](const HyperlinkActionRule &candidate) {
                        return candidate.id == rule.id;
                      }) != 1) {
      return false;
    }
  }
  return true;
}

static void update_validation(HyperlinkSettingsEditorState *state) {
  const HyperlinkActionRule *rule = selected_rule(state);
  if (rule == nullptr) {
    set_entry_validation(state->id_entry, true, {});
    set_entry_validation(state->regex_entry, true, {});
    set_entry_validation(state->command_entry, true, {});
    set_entry_validation(state->path_entry, true, {});
    gtk_label_set_text(GTK_LABEL(state->validation_label), "");
    return;
  }

  std::string id_reason;
  bool id_valid = hyperlink_action_rule_id_is_valid(rule->id, &id_reason);
  if (id_valid && !rule_id_is_unique(state, rule)) {
    id_valid = false;
    id_reason = _("Identifier must be unique");
  }
  set_entry_validation(state->id_entry, id_valid, id_reason);

  std::string reason;
  const bool rule_valid = hyperlink_action_rule_is_valid(*rule, &reason);
  set_entry_validation(state->regex_entry, rule_valid, reason);
  set_entry_validation(state->command_entry,
                       !rule->command.empty() || rule_valid, reason);
  set_entry_validation(
      state->path_entry,
      rule->path_validation == HyperlinkPathValidation::none || rule_valid,
      reason);
  const std::string display_reason = id_valid ? reason : id_reason;
  gtk_label_set_text(GTK_LABEL(state->validation_label),
                     id_valid && rule_valid ? ""
                                            : display_reason.c_str());
}

static void update_button_sensitivity(HyperlinkSettingsEditorState *state) {
  const bool has_selection = selected_rule(state) != nullptr;
  gtk_widget_set_sensitive(state->remove_button, has_selection);
  gtk_widget_set_sensitive(state->move_up_button,
                           has_selection && state->selected_rule > 0);
  gtk_widget_set_sensitive(
      state->move_down_button,
      has_selection &&
          static_cast<std::size_t>(state->selected_rule + 1) <
              state->store->hyperlink_rules.size());
  gtk_widget_set_sensitive(
      state->reset_button,
      state->store->hyperlink_settings_configured ? TRUE : FALSE);
}

static void notify_changed(HyperlinkSettingsEditorState *state) {
  if (!state->synchronizing && state->changed) {
    state->changed();
  }
}

static void mark_rules_changed(HyperlinkSettingsEditorState *state) {
  state->store->hyperlink_settings_configured = true;
  state->store->hyperlink_settings_dirty = true;
  update_validation(state);
  update_button_sensitivity(state);
  notify_changed(state);
}

static void on_argument_changed(GtkEditable *editable, gpointer data);
static void on_argument_move_up_clicked(GtkButton *button, gpointer data);
static void on_argument_move_down_clicked(GtkButton *button, gpointer data);
static void on_argument_remove_clicked(GtkButton *button, gpointer data);

static GtkWidget *create_argument_button(
    HyperlinkSettingsEditorState *state, std::size_t index,
    const char *label, const char *suffix, const char *tooltip,
    GCallback callback) {
  GtkWidget *button = gtk_button_new_with_label(label);
  assign_accessible_id(
      button,
      widget_id(state, "link_argument_" + std::to_string(index) + suffix));
  gtk_widget_set_tooltip_text(button, tooltip);
  g_object_set_data(G_OBJECT(button), link_widget_index_key,
                    GINT_TO_POINTER(static_cast<int>(index) + 1));
  g_signal_connect(button, "clicked", callback, state);
  return button;
}

static void rebuild_arguments(HyperlinkSettingsEditorState *state) {
  clear_container(state->arguments_box);
  const HyperlinkActionRule *rule = selected_rule(state);
  if (rule == nullptr) {
    return;
  }
  for (std::size_t index = 0; index < rule->arguments.size(); ++index) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *entry = create_entry(
        state, "link_argument_" + std::to_string(index) + "_entry");
    gtk_entry_set_text(GTK_ENTRY(entry), rule->arguments[index].c_str());
    g_object_set_data(G_OBJECT(entry), link_widget_index_key,
                      GINT_TO_POINTER(static_cast<int>(index) + 1));
    g_signal_connect(entry, "changed", G_CALLBACK(on_argument_changed),
                     state);
    gtk_box_pack_start(GTK_BOX(row), entry, TRUE, TRUE, 0);

    GtkWidget *up = create_argument_button(
        state, index, "↑", "_move_up_button", _("Move argument up"),
        G_CALLBACK(on_argument_move_up_clicked));
    GtkWidget *down = create_argument_button(
        state, index, "↓", "_move_down_button", _("Move argument down"),
        G_CALLBACK(on_argument_move_down_clicked));
    GtkWidget *remove = create_argument_button(
        state, index, "−", "_remove_button", _("Remove argument"),
        G_CALLBACK(on_argument_remove_clicked));
    gtk_widget_set_sensitive(up, index > 0);
    gtk_widget_set_sensitive(down, index + 1 < rule->arguments.size());
    gtk_box_pack_start(GTK_BOX(row), up, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), down, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), remove, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(state->arguments_box), row, FALSE, FALSE, 0);
    gtk_widget_show_all(row);
  }
}

static void sync_editor(HyperlinkSettingsEditorState *state) {
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  const HyperlinkActionRule *rule = selected_rule(state);
  const bool has_selection = rule != nullptr;
  gtk_widget_set_sensitive(state->editor, has_selection);
  gtk_entry_set_text(GTK_ENTRY(state->id_entry),
                     has_selection ? rule->id.c_str() : "");
  gtk_combo_box_set_active_id(
      GTK_COMBO_BOX(state->source_combo),
      has_selection
          ? hyperlink_recognition_source_to_string(rule->recognition_source)
          : "osc8");
  gtk_entry_set_text(GTK_ENTRY(state->regex_entry),
                     has_selection ? rule->pattern.c_str() : "");
  gtk_entry_set_text(GTK_ENTRY(state->command_entry),
                     has_selection ? rule->command.c_str() : "");
  gtk_combo_box_set_active_id(
      GTK_COMBO_BOX(state->validation_combo),
      has_selection
          ? hyperlink_path_validation_to_string(rule->path_validation)
          : "none");
  gtk_entry_set_text(GTK_ENTRY(state->path_entry),
                     has_selection ? rule->path_template.c_str() : "");
  gtk_widget_set_sensitive(
      state->path_entry,
      has_selection && rule->path_validation ==
                           HyperlinkPathValidation::existing_local_path);
  rebuild_arguments(state);
  state->synchronizing = previous_synchronizing;
  update_button_sensitivity(state);
  update_validation(state);
}

static void update_next_rule_number(HyperlinkSettingsEditorState *state) {
  for (const HyperlinkActionRule &rule : state->store->hyperlink_rules) {
    constexpr char prefix[] = "rule";
    if (!rule.id.starts_with(prefix) || rule.id.size() == sizeof(prefix) - 1) {
      continue;
    }
    const std::string suffix = rule.id.substr(sizeof(prefix) - 1);
    if (!std::all_of(suffix.begin(), suffix.end(), [](unsigned char value) {
          return std::isdigit(value) != 0;
        })) {
      continue;
    }
    const guint64 number = g_ascii_strtoull(suffix.c_str(), nullptr, 10);
    if (number < std::numeric_limits<unsigned int>::max()) {
      state->next_rule_number =
          std::max(state->next_rule_number,
                   static_cast<unsigned int>(number) + 1);
    }
  }
}

static void rebuild_rule_list(HyperlinkSettingsEditorState *state) {
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  clear_container(state->rule_list);
  if (state->store->hyperlink_rules.empty()) {
    state->selected_rule = -1;
  } else if (state->selected_rule < 0 ||
             static_cast<std::size_t>(state->selected_rule) >=
                 state->store->hyperlink_rules.size()) {
    state->selected_rule = 0;
  }

  GtkListBoxRow *selected_row = nullptr;
  for (std::size_t index = 0; index < state->store->hyperlink_rules.size();
       ++index) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label =
        gtk_label_new(state->store->hyperlink_rules[index].id.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_container_add(GTK_CONTAINER(row), label);
    g_object_set_data(G_OBJECT(row), link_widget_index_key,
                      GINT_TO_POINTER(static_cast<int>(index) + 1));
    gtk_container_add(GTK_CONTAINER(state->rule_list), row);
    if (static_cast<int>(index) == state->selected_rule) {
      selected_row = GTK_LIST_BOX_ROW(row);
    }
  }
  gtk_widget_show_all(state->rule_list);
  if (selected_row != nullptr) {
    gtk_list_box_select_row(GTK_LIST_BOX(state->rule_list), selected_row);
  }
  update_next_rule_number(state);
  state->synchronizing = previous_synchronizing;
  sync_editor(state);
}

static void on_enabled_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  if (state->synchronizing) {
    return;
  }
  state->store->hyperlink_actions_enabled =
      std::string(active_combo_id(state->enabled_combo, "enabled")) ==
      "enabled";
  mark_rules_changed(state);
}

static void on_reset_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  reset_hyperlink_actions(state->store);
  state->selected_rule = 0;
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->enabled_combo),
                              "enabled");
  state->synchronizing = previous_synchronizing;
  rebuild_rule_list(state);
  notify_changed(state);
}

static void on_rule_selected(GtkListBox *, GtkListBoxRow *row,
                             gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  if (state->synchronizing) {
    return;
  }
  state->selected_rule =
      row == nullptr ? -1 : widget_index(GTK_WIDGET(row));
  sync_editor(state);
}

static void on_id_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  HyperlinkActionRule *rule = selected_rule(state);
  if (state->synchronizing || rule == nullptr) {
    return;
  }
  rule->id = gtk_entry_get_text(GTK_ENTRY(state->id_entry));
  mark_rules_changed(state);
  rebuild_rule_list(state);
}

static void on_source_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  HyperlinkActionRule *rule = selected_rule(state);
  if (state->synchronizing || rule == nullptr) {
    return;
  }
  rule->recognition_source =
      std::string(active_combo_id(state->source_combo, "osc8")) ==
              "terminal-text"
          ? HyperlinkRecognitionSource::terminal_text
          : HyperlinkRecognitionSource::osc8;
  mark_rules_changed(state);
}

static void on_regex_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  HyperlinkActionRule *rule = selected_rule(state);
  if (state->synchronizing || rule == nullptr) {
    return;
  }
  rule->pattern = gtk_entry_get_text(GTK_ENTRY(state->regex_entry));
  mark_rules_changed(state);
}

static void on_command_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  HyperlinkActionRule *rule = selected_rule(state);
  if (state->synchronizing || rule == nullptr) {
    return;
  }
  rule->command = gtk_entry_get_text(GTK_ENTRY(state->command_entry));
  mark_rules_changed(state);
}

static void on_validation_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  HyperlinkActionRule *rule = selected_rule(state);
  if (state->synchronizing || rule == nullptr) {
    return;
  }
  rule->path_validation =
      std::string(active_combo_id(state->validation_combo, "none")) ==
              "existing-local-path"
          ? HyperlinkPathValidation::existing_local_path
          : HyperlinkPathValidation::none;
  gtk_widget_set_sensitive(
      state->path_entry,
      rule->path_validation == HyperlinkPathValidation::existing_local_path);
  mark_rules_changed(state);
}

static void on_path_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  HyperlinkActionRule *rule = selected_rule(state);
  if (state->synchronizing || rule == nullptr) {
    return;
  }
  rule->path_template = gtk_entry_get_text(GTK_ENTRY(state->path_entry));
  mark_rules_changed(state);
}

static void on_add_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  std::string id;
  do {
    id = "rule" + std::to_string(state->next_rule_number++);
  } while (std::any_of(
      state->store->hyperlink_rules.begin(),
      state->store->hyperlink_rules.end(),
      [&id](const HyperlinkActionRule &rule) { return rule.id == id; }));
  state->store->hyperlink_rules.push_back(HyperlinkActionRule{
      .id = std::move(id),
      .recognition_source = HyperlinkRecognitionSource::osc8,
      .pattern = {},
      .command = {},
      .arguments = {},
      .path_validation = HyperlinkPathValidation::none,
      .path_template = {},
  });
  state->selected_rule =
      static_cast<int>(state->store->hyperlink_rules.size()) - 1;
  mark_rules_changed(state);
  rebuild_rule_list(state);
}

static void on_remove_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  if (selected_rule(state) == nullptr) {
    return;
  }
  state->store->hyperlink_rules.erase(
      state->store->hyperlink_rules.begin() + state->selected_rule);
  if (static_cast<std::size_t>(state->selected_rule) >=
      state->store->hyperlink_rules.size()) {
    --state->selected_rule;
  }
  mark_rules_changed(state);
  rebuild_rule_list(state);
}

static void move_selected_rule(HyperlinkSettingsEditorState *state,
                               int offset) {
  const int destination = state->selected_rule + offset;
  if (selected_rule(state) == nullptr || destination < 0 ||
      static_cast<std::size_t>(destination) >=
          state->store->hyperlink_rules.size()) {
    return;
  }
  std::swap(state->store->hyperlink_rules[state->selected_rule],
            state->store->hyperlink_rules[destination]);
  state->selected_rule = destination;
  mark_rules_changed(state);
  rebuild_rule_list(state);
}

static void on_move_up_clicked(GtkButton *, gpointer data) {
  move_selected_rule(static_cast<HyperlinkSettingsEditorState *>(data), -1);
}

static void on_move_down_clicked(GtkButton *, gpointer data) {
  move_selected_rule(static_cast<HyperlinkSettingsEditorState *>(data), 1);
}

static void on_argument_add_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  HyperlinkActionRule *rule = selected_rule(state);
  if (rule == nullptr) {
    return;
  }
  rule->arguments.emplace_back();
  mark_rules_changed(state);
  rebuild_arguments(state);
}

static void on_argument_changed(GtkEditable *editable, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  HyperlinkActionRule *rule = selected_rule(state);
  const int index = widget_index(GTK_WIDGET(editable));
  if (state->synchronizing || rule == nullptr || index < 0 ||
      static_cast<std::size_t>(index) >= rule->arguments.size()) {
    return;
  }
  rule->arguments[index] = gtk_entry_get_text(GTK_ENTRY(editable));
  mark_rules_changed(state);
}

static void move_argument(HyperlinkSettingsEditorState *state,
                          GtkWidget *widget, int offset) {
  HyperlinkActionRule *rule = selected_rule(state);
  const int index = widget_index(widget);
  const int destination = index + offset;
  if (rule == nullptr || index < 0 || destination < 0 ||
      static_cast<std::size_t>(index) >= rule->arguments.size() ||
      static_cast<std::size_t>(destination) >= rule->arguments.size()) {
    return;
  }
  std::swap(rule->arguments[index], rule->arguments[destination]);
  mark_rules_changed(state);
  rebuild_arguments(state);
}

static void on_argument_move_up_clicked(GtkButton *button, gpointer data) {
  move_argument(static_cast<HyperlinkSettingsEditorState *>(data),
                GTK_WIDGET(button), -1);
}

static void on_argument_move_down_clicked(GtkButton *button, gpointer data) {
  move_argument(static_cast<HyperlinkSettingsEditorState *>(data),
                GTK_WIDGET(button), 1);
}

static void on_argument_remove_clicked(GtkButton *button, gpointer data) {
  auto *state = static_cast<HyperlinkSettingsEditorState *>(data);
  HyperlinkActionRule *rule = selected_rule(state);
  const int index = widget_index(GTK_WIDGET(button));
  if (rule == nullptr || index < 0 ||
      static_cast<std::size_t>(index) >= rule->arguments.size()) {
    return;
  }
  rule->arguments.erase(rule->arguments.begin() + index);
  mark_rules_changed(state);
  rebuild_arguments(state);
}

static GtkWidget *create_rule_panel(HyperlinkSettingsEditorState *state) {
  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_size_request(panel, 190, -1);
  gtk_box_pack_start(GTK_BOX(panel), create_label(_("Rules")), FALSE, FALSE,
                     0);
  GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  state->rule_list = gtk_list_box_new();
  assign_accessible_id(state->rule_list, widget_id(state, "link_list"));
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->rule_list),
                                  GTK_SELECTION_SINGLE);
  g_signal_connect(state->rule_list, "row-selected",
                   G_CALLBACK(on_rule_selected), state);
  gtk_container_add(GTK_CONTAINER(scroll), state->rule_list);
  gtk_box_pack_start(GTK_BOX(panel), scroll, TRUE, TRUE, 0);

  GtkWidget *buttons = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(buttons), 6);
  GtkWidget *add = gtk_button_new_with_label(_("Add"));
  assign_accessible_id(add, widget_id(state, "link_add_button"));
  g_signal_connect(add, "clicked", G_CALLBACK(on_add_clicked), state);
  state->remove_button = gtk_button_new_with_label(_("Remove"));
  assign_accessible_id(state->remove_button,
                       widget_id(state, "link_remove_button"));
  g_signal_connect(state->remove_button, "clicked",
                   G_CALLBACK(on_remove_clicked), state);
  state->move_up_button = gtk_button_new_with_label(_("Move up"));
  assign_accessible_id(state->move_up_button,
                       widget_id(state, "link_move_up_button"));
  g_signal_connect(state->move_up_button, "clicked",
                   G_CALLBACK(on_move_up_clicked), state);
  state->move_down_button = gtk_button_new_with_label(_("Move down"));
  assign_accessible_id(state->move_down_button,
                       widget_id(state, "link_move_down_button"));
  g_signal_connect(state->move_down_button, "clicked",
                   G_CALLBACK(on_move_down_clicked), state);
  gtk_grid_attach(GTK_GRID(buttons), add, 0, 0, 2, 1);
  gtk_grid_attach(GTK_GRID(buttons), state->remove_button, 0, 1, 2, 1);
  gtk_grid_attach(GTK_GRID(buttons), state->move_up_button, 0, 2, 1, 1);
  gtk_grid_attach(GTK_GRID(buttons), state->move_down_button, 1, 2, 1, 1);
  gtk_box_pack_start(GTK_BOX(panel), buttons, FALSE, FALSE, 0);
  return panel;
}

static GtkWidget *create_rule_editor(HyperlinkSettingsEditorState *state) {
  state->editor = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(state->editor), 8);
  gtk_grid_set_column_spacing(GTK_GRID(state->editor), 12);
  gtk_widget_set_margin_start(state->editor, 12);
  gtk_widget_set_hexpand(state->editor, TRUE);
  gtk_widget_set_vexpand(state->editor, TRUE);

  state->id_entry = create_entry(state, "link_id_entry");
  g_signal_connect(state->id_entry, "changed", G_CALLBACK(on_id_changed),
                   state);
  attach_editor_row(state->editor, 0, _("Identifier"), state->id_entry);

  state->source_combo = create_combo(state, "link_source_combo");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->source_combo), "osc8",
                            _("OSC 8 target"));
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->source_combo),
                            "terminal-text", _("Visible terminal text"));
  g_signal_connect(state->source_combo, "changed",
                   G_CALLBACK(on_source_changed), state);
  attach_editor_row(state->editor, 1, _("Recognition source"),
                    state->source_combo);

  state->regex_entry = create_entry(state, "link_regex_entry");
  g_signal_connect(state->regex_entry, "changed",
                   G_CALLBACK(on_regex_changed), state);
  attach_editor_row(state->editor, 2, _("Regular expression"),
                    state->regex_entry);

  state->command_entry = create_entry(state, "link_command_entry");
  gtk_widget_set_tooltip_text(
      state->command_entry,
      _("The command is executed directly without a shell. Capture "
        "templates are expanded only in arguments."));
  g_signal_connect(state->command_entry, "changed",
                   G_CALLBACK(on_command_changed), state);
  attach_editor_row(state->editor, 3, _("Command"), state->command_entry);

  GtkWidget *argument_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  state->arguments_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  assign_accessible_id(state->arguments_box,
                       widget_id(state, "link_arguments_box"));
  gtk_box_pack_start(GTK_BOX(argument_panel), state->arguments_box, FALSE,
                     FALSE, 0);
  state->argument_add_button = gtk_button_new_with_label(_("Add argument"));
  assign_accessible_id(state->argument_add_button,
                       widget_id(state, "link_argument_add_button"));
  gtk_widget_set_halign(state->argument_add_button, GTK_ALIGN_START);
  g_signal_connect(state->argument_add_button, "clicked",
                   G_CALLBACK(on_argument_add_clicked), state);
  gtk_box_pack_start(GTK_BOX(argument_panel), state->argument_add_button,
                     FALSE, FALSE, 0);
  attach_editor_row(state->editor, 4, _("Arguments"), argument_panel);

  state->validation_combo = create_combo(state, "link_validation_combo");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->validation_combo),
                            "none", _("Do not validate"));
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->validation_combo),
                            "existing-local-path",
                            _("Existing local file or directory"));
  g_signal_connect(state->validation_combo, "changed",
                   G_CALLBACK(on_validation_changed), state);
  attach_editor_row(state->editor, 5, _("Path validation"),
                    state->validation_combo);

  state->path_entry = create_entry(state, "link_path_entry");
  gtk_widget_set_tooltip_text(
      state->path_entry,
      _("Path validation requires an absolute expanded path. Relative paths "
        "are rejected."));
  g_signal_connect(state->path_entry, "changed",
                   G_CALLBACK(on_path_changed), state);
  attach_editor_row(state->editor, 6, _("Path template"),
                    state->path_entry);

  state->validation_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(state->validation_label), 0.0F);
  gtk_label_set_line_wrap(GTK_LABEL(state->validation_label), TRUE);
  GtkStyleContext *context =
      gtk_widget_get_style_context(state->validation_label);
  gtk_style_context_add_class(context, GTK_STYLE_CLASS_ERROR);
  gtk_grid_attach(GTK_GRID(state->editor), state->validation_label, 0, 7, 2,
                  1);
  return state->editor;
}

HyperlinkSettingsEditorState *
create_hyperlink_settings_editor(HyperlinkSettingsEditorOptions options) {
  auto *state = new HyperlinkSettingsEditorState();
  state->store = options.store;
  state->id_prefix = std::move(options.id_prefix);
  state->changed = std::move(options.changed);
  state->root = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(state->root),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(state->root, TRUE);
  gtk_widget_set_vexpand(state->root, TRUE);
  assign_accessible_id(state->root, widget_id(state, "link_page"));

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(content, 16);
  gtk_widget_set_margin_bottom(content, 16);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_container_add(GTK_CONTAINER(state->root), content);

  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(top), create_label(_("Link actions")), FALSE,
                     FALSE, 0);
  state->enabled_combo = create_combo(state, "link_enabled_combo");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->enabled_combo),
                            "enabled", _("Enabled"));
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->enabled_combo),
                            "disabled", _("Disabled"));
  gtk_combo_box_set_active_id(
      GTK_COMBO_BOX(state->enabled_combo),
      state->store != nullptr && state->store->hyperlink_actions_enabled
          ? "enabled"
          : "disabled");
  g_signal_connect(state->enabled_combo, "changed",
                   G_CALLBACK(on_enabled_changed), state);
  gtk_box_pack_start(GTK_BOX(top), state->enabled_combo, FALSE, FALSE, 0);
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_pack_start(GTK_BOX(top), spacer, TRUE, TRUE, 0);
  state->reset_button = gtk_button_new_with_label(_("Restore defaults"));
  assign_accessible_id(state->reset_button,
                       widget_id(state, "link_reset_button"));
  g_signal_connect(state->reset_button, "clicked",
                   G_CALLBACK(on_reset_clicked), state);
  gtk_box_pack_start(GTK_BOX(top), state->reset_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content), top, FALSE, FALSE, 0);

  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_hexpand(paned, TRUE);
  gtk_widget_set_vexpand(paned, TRUE);
  gtk_paned_pack1(GTK_PANED(paned), create_rule_panel(state), FALSE, FALSE);
  gtk_paned_pack2(GTK_PANED(paned), create_rule_editor(state), TRUE, FALSE);
  gtk_box_pack_start(GTK_BOX(content), paned, TRUE, TRUE, 0);

  if (state->store != nullptr && !state->store->hyperlink_rules.empty()) {
    state->selected_rule = 0;
  }
  rebuild_rule_list(state);
  return state;
}

GtkWidget *hyperlink_settings_editor_root(
    HyperlinkSettingsEditorState *state) {
  return state == nullptr ? nullptr : state->root;
}

void sync_hyperlink_settings_editor(HyperlinkSettingsEditorState *state) {
  if (state == nullptr || state->store == nullptr) {
    return;
  }
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  gtk_combo_box_set_active_id(
      GTK_COMBO_BOX(state->enabled_combo),
      state->store->hyperlink_actions_enabled ? "enabled" : "disabled");
  state->selected_rule = state->store->hyperlink_rules.empty() ? -1 : 0;
  state->next_rule_number = 1;
  rebuild_rule_list(state);
  state->synchronizing = previous_synchronizing;
}

bool hyperlink_settings_editor_is_valid(
    const HyperlinkSettingsEditorState *state) {
  return all_rules_are_valid(state);
}

void destroy_hyperlink_settings_editor(HyperlinkSettingsEditorState *state) {
  if (state == nullptr) {
    return;
  }
  if (state->root != nullptr && gtk_widget_get_parent(state->root) == nullptr) {
    gtk_widget_destroy(state->root);
  }
  delete state;
}

} // namespace elder_terms
