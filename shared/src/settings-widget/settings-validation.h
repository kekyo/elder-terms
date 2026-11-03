// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <gtk/gtk.h>
#include <string>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Presents an input error on a control, preserving its ordinary help text.
 * @param widget Control owned by its GTK parent; nullptr is ignored.
 * @param reason Validation reason, or empty to restore ordinary presentation.
 * @remarks State is owned by the widget. Entry error icons are not activatable.
 */
void set_settings_input_error(GtkWidget *widget, const std::string &reason);

/**
 * Associates editable controls with a setting, including controls in a row.
 * @param widget Control or container owned by GTK.
 * @param key Setting edited by the controls.
 */
void bind_settings_field(GtkWidget *widget, const SettingKey &key);

/**
 * Adds a standard error indicator beside a control without an entry icon.
 * @param control Unparented control, whose ownership passes to the returned row.
 * @returns Control or containing row to attach to the settings layout.
 */
GtkWidget *decorate_settings_control(GtkWidget *control);

/**
 * Refreshes field constraints and error indicators without replacing input drafts.
 * @param root Settings container.
 * @param issues Active validation failures associated with settings.
 */
void refresh_settings_validation(GtkWidget *root, const std::vector<SettingValidationIssue> &issues);

/**
 * Collects explanations from a control and its descendants.
 * @param root Control or container, or nullptr.
 * @returns Distinct localized explanations separated by newlines.
 */
std::string settings_validation_summary(GtkWidget *root);

/**
 * Marks one argument input in an ordered list of rows beginning with an entry.
 * @param rows Container of argument rows.
 * @param index Zero-based argument position; ignored when reason is empty.
 * @param reason Explanation, or empty to clear all argument errors.
 */
void set_settings_argument_error(GtkWidget *rows, std::size_t index, const std::string &reason);

/**
 * Explains a rejected entry draft using its existing setting definition.
 * @param store Settings whose schema validates the entry.
 * @param key Setting edited by the entry.
 * @param widget Input control containing the rejected draft.
 * @returns Validation reason; no settings are changed.
 */
std::string settings_rejected_input_reason(const SettingsStore &store, const SettingKey &key, GtkWidget *widget);

/**
 * Reads the current explanation attached to a control.
 * @param widget Control, or nullptr.
 * @returns Localized error message, or empty when the control is valid.
 */
std::string settings_input_error(GtkWidget *widget);

} // namespace elder_terms
