#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <cardio.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

namespace elder_terms {

/**
 * Identifies the tray transport currently selected by the launcher.
 */
enum class TrayBackendKind {
  /** No usable tray transport is available. */
  none,
  /** Freedesktop StatusNotifierItem over D-Bus. */
  status_notifier_item,
  /** Legacy GtkStatusIcon/XEmbed transport. */
  xembed,
};

/**
 * Identifies whether tray discovery can retain a hidden application.
 */
enum class TrayBackendAvailabilityState {
  /** Tray discovery or registration is still in progress. */
  pending,
  /** No current tray host can retain the application. */
  unavailable,
  /** A current tray host can retain the application. */
  available,
};

/**
 * Describes the tray transports available in the current session.
 */
struct TrayBackendAvailability {
  /** A StatusNotifier watcher and host are available. */
  bool has_status_notifier_item;
  /** The current display can provide GtkStatusIcon/XEmbed. */
  bool has_xembed;
};

/**
 * Describes platform context supplied with a tray activation.
 */
struct TrayActivationContext {
  /** Event timestamp, or no value when the backend did not provide one. */
  std::optional<std::uint32_t> activation_time;
};

/**
 * Stores one icon pixmap in StatusNotifierItem ARGB_8888 byte order.
 */
struct TrayIconPixmap {
  /** Pixmap width in pixels. */
  int width;
  /** Pixmap height in pixels. */
  int height;
  /** Consecutive ARGB bytes. */
  std::vector<std::uint8_t> argb_pixels;
};

/**
 * Receives events produced by a tray backend.
 */
struct TrayBackendCallbacks {
  /** Opens or presents the launcher window. */
  std::function<void(const TrayActivationContext &)> activate;
  /** Opens the application-level settings page. */
  std::function<void()> application_settings;
  /** Opens the application information page. */
  std::function<void()> about;
  /** Requests an explicit application quit. */
  std::function<void()> quit;
  /** Reports changes to tray discovery and host availability. */
  std::function<void(TrayBackendAvailabilityState)> availability_changed;
};

/**
 * Configures one tray backend instance.
 */
struct TrayBackendOptions {
  /** Registered GApplication that owns the D-Bus connection. */
  GApplication *application;
  /** Cardio dispatcher integrated into the GLib application loop. */
  cardio::dispatcher *dispatcher;
  /** Stable StatusNotifierItem identifier. */
  std::string identifier;
  /** User-visible title and tooltip. */
  std::string title;
  /** Icon theme name without an extension. */
  std::string icon_name;
  /** Backend event callbacks. */
  TrayBackendCallbacks callbacks;
};

/** Opaque tray backend lifetime state. */
struct TrayBackendState;

/**
 * Returns the exact GApplication identifier used by the launcher.
 *
 * @returns Valid reverse-DNS application identifier.
 */
const char *launcher_application_id();

/**
 * Chooses the preferred tray transport.
 *
 * @param availability Available transports.
 * @returns StatusNotifierItem first, then XEmbed, then none.
 */
TrayBackendKind
select_tray_backend_kind(const TrayBackendAvailability &availability);

/**
 * Builds activation context from a backend event timestamp.
 *
 * @param timestamp Event timestamp, or zero when unavailable.
 * @returns Activation context with zero normalized to no timestamp.
 */
TrayActivationContext
build_tray_activation_context(std::uint32_t timestamp);

/**
 * Converts RGB or RGBA pixbuf bytes to StatusNotifierItem ARGB bytes.
 *
 * @param pixels Source pixel bytes.
 * @param width Source width.
 * @param height Source height.
 * @param rowstride Bytes between source rows.
 * @param channel_count Three for RGB or four for RGBA.
 * @returns Converted bytes, or an empty vector for invalid input.
 */
std::vector<std::uint8_t>
convert_tray_icon_pixels_to_argb(const std::uint8_t *pixels,
                                 int width, int height, int rowstride,
                                 int channel_count);

/**
 * Builds a StatusNotifierItem `a(iiay)` icon pixmap value.
 *
 * @param pixmaps Valid ARGB pixmaps.
 * @returns Floating GVariant containing all valid pixmaps.
 */
GVariant *
build_tray_icon_pixmap_variant(const std::vector<TrayIconPixmap> &pixmaps);

/**
 * Starts asynchronous tray discovery and registration.
 *
 * @param options Application, dispatcher, identity, and callback settings.
 * @returns Opaque backend state. The selected backend may remain unavailable.
 */
TrayBackendState *create_tray_backend(TrayBackendOptions options);

/**
 * Stops tray registration and releases all backend resources.
 *
 * @param state Backend state, or null.
 */
void destroy_tray_backend(TrayBackendState *state);

/**
 * Returns the currently selected backend kind.
 *
 * @param state Backend state, or null.
 * @returns Current transport kind.
 */
TrayBackendKind tray_backend_kind(const TrayBackendState *state);

/**
 * Returns the current tray discovery and host availability.
 *
 * @param state Backend state, or null.
 * @returns Current availability, or unavailable for a null state.
 */
TrayBackendAvailabilityState
tray_backend_availability(const TrayBackendState *state);

} // namespace elder_terms
