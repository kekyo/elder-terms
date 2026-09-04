#pragma once

#include <functional>
#include <string>

#include <gtk/gtk.h>

#include <elder-terms/settings.h>

namespace elder_terms {

struct HyperlinkSettingsEditorState;

struct HyperlinkSettingsEditorOptions {
  SettingsStore *store = nullptr;
  std::string id_prefix;
  std::function<void()> changed;
};

HyperlinkSettingsEditorState *
create_hyperlink_settings_editor(HyperlinkSettingsEditorOptions options);

GtkWidget *hyperlink_settings_editor_root(
    HyperlinkSettingsEditorState *state);

void sync_hyperlink_settings_editor(HyperlinkSettingsEditorState *state);

bool hyperlink_settings_editor_is_valid(
    const HyperlinkSettingsEditorState *state);

void destroy_hyperlink_settings_editor(HyperlinkSettingsEditorState *state);

} // namespace elder_terms
