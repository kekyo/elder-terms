// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <gtk/gtk.h>

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Presents one launcher-owned top-level window after a tray activation.
 * @param window Window originally attached to the launcher.
 * @param timestamp X11 event time, or zero when unavailable.
 * @param activation_token Desktop activation token, or an empty string.
 */
using TrackedWindowPresenter = std::function<void(
    GtkWidget *window, std::uint32_t timestamp, const std::string &activation_token)>;

/**
 * Registers a child window with the launcher for tray presentation.
 * @param window Shown top-level GTK window; the registration ends on destroy.
 * @param presenter Callback on the GTK thread for an authorized presentation request.
 * @returns True if launch tracking was configured and D-Bus registration succeeded.
 * @remarks Call once after showing the window. Untracked launches return false.
 */
ELDER_TERMS_API bool attach_tracked_window(
    GtkWidget *window, TrackedWindowPresenter presenter);

/**
 * Presents the active modal descendant of a tracked window.
 * @param window Shown top-level GTK window.
 * @param timestamp X11 event time, or zero to obtain a fresh server time.
 * @param activation_token Desktop activation token, or an empty string.
 * @remarks Call on the GTK thread after a user activation request.
 */
ELDER_TERMS_API void present_tracked_window(
    GtkWidget *window, std::uint32_t timestamp,
    const std::string &activation_token);

} // namespace elder_terms
