#include "tray-backend.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <utility>

#include <gdk/gdkx.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "build-config.h"

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static constexpr char application_id[] = "net.kekyo.elder-terms";
static constexpr char status_notifier_watcher_service[] =
    "org.kde.StatusNotifierWatcher";
static constexpr char status_notifier_watcher_path[] =
    "/StatusNotifierWatcher";
static constexpr char status_notifier_watcher_interface[] =
    "org.kde.StatusNotifierWatcher";
static constexpr char status_notifier_item_path[] = "/StatusNotifierItem";
static constexpr char status_notifier_menu_path[] = "/StatusNotifierMenu";
static constexpr char status_notifier_item_interface[] =
    "org.kde.StatusNotifierItem";
static constexpr char dbus_menu_interface[] = "com.canonical.dbusmenu";
static constexpr int dbus_menu_revision = 1;
static constexpr int open_menu_item_id = 1;
static constexpr int quit_menu_item_id = 2;
static constexpr int application_settings_menu_item_id = 3;
static constexpr int about_menu_item_id = 4;
static constexpr std::array<int, 8> tray_icon_sizes = {
    16, 22, 24, 32, 48, 64, 128, 256,
};

static constexpr char status_notifier_item_xml[] = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <method name="ContextMenu">
      <arg type="i" name="x" direction="in"/>
      <arg type="i" name="y" direction="in"/>
    </method>
    <method name="Activate">
      <arg type="i" name="x" direction="in"/>
      <arg type="i" name="y" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg type="i" name="x" direction="in"/>
      <arg type="i" name="y" direction="in"/>
    </method>
    <method name="Scroll">
      <arg type="i" name="delta" direction="in"/>
      <arg type="s" name="orientation" direction="in"/>
    </method>
    <signal name="NewAttentionIcon"/>
    <signal name="NewIcon"/>
    <signal name="NewOverlayIcon"/>
    <signal name="NewStatus">
      <arg type="s" name="status"/>
    </signal>
    <signal name="NewTitle"/>
    <property type="s" name="Category" access="read"/>
    <property type="s" name="Id" access="read"/>
    <property type="s" name="Title" access="read"/>
    <property type="s" name="Status" access="read"/>
    <property type="u" name="WindowId" access="read"/>
    <property type="o" name="Menu" access="read"/>
    <property type="b" name="ItemIsMenu" access="read"/>
    <property type="s" name="IconName" access="read"/>
    <property type="a(iiay)" name="IconPixmap" access="read"/>
    <property type="s" name="OverlayIconName" access="read"/>
    <property type="a(iiay)" name="OverlayIconPixmap" access="read"/>
    <property type="s" name="IconThemePath" access="read"/>
    <property type="s" name="IconAccessibleDesc" access="read"/>
    <property type="s" name="AttentionIconName" access="read"/>
    <property type="a(iiay)" name="AttentionIconPixmap" access="read"/>
    <property type="s" name="AttentionAccessibleDesc" access="read"/>
  </interface>
</node>
)XML";

static constexpr char dbus_menu_xml[] = R"XML(
<node>
  <interface name="com.canonical.dbusmenu">
    <method name="GetLayout">
      <arg type="i" name="parentId" direction="in"/>
      <arg type="i" name="recursionDepth" direction="in"/>
      <arg type="as" name="propertyNames" direction="in"/>
      <arg type="u" name="revision" direction="out"/>
      <arg type="(ia{sv}av)" name="layout" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg type="ai" name="ids" direction="in"/>
      <arg type="as" name="propertyNames" direction="in"/>
      <arg type="a(ia{sv})" name="properties" direction="out"/>
    </method>
    <method name="Event">
      <arg type="i" name="id" direction="in"/>
      <arg type="s" name="eventId" direction="in"/>
      <arg type="v" name="data" direction="in"/>
      <arg type="u" name="timestamp" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg type="a(isvu)" name="events" direction="in"/>
      <arg type="ai" name="idErrors" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg type="i" name="id" direction="in"/>
      <arg type="b" name="needUpdate" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg type="ai" name="ids" direction="in"/>
      <arg type="ai" name="updatesNeeded" direction="out"/>
      <arg type="ai" name="idErrors" direction="out"/>
    </method>
    <signal name="LayoutUpdated">
      <arg type="u" name="revision"/>
      <arg type="i" name="parent"/>
    </signal>
    <signal name="ItemActivationRequested">
      <arg type="i" name="id"/>
      <arg type="u" name="timestamp"/>
    </signal>
    <property type="s" name="Status" access="read"/>
    <property type="s" name="TextDirection" access="read"/>
    <property type="u" name="Version" access="read"/>
    <property type="s" name="IconThemePath" access="read"/>
  </interface>
</node>
)XML";

struct TrayBackendImplementation {
  TrayBackendOptions options;
  TrayBackendKind kind = TrayBackendKind::none;
  TrayBackendAvailabilityState availability =
      TrayBackendAvailabilityState::pending;
  bool destroyed = false;
  GDBusConnection *connection = nullptr;
  guint item_registration_id = 0;
  guint menu_registration_id = 0;
  GtkStatusIcon *status_icon = nullptr;
  GtkWidget *status_menu = nullptr;
  gulong embedded_signal_id = 0;
  std::optional<cardio::cancellation_source> cancellation_source;
  std::optional<cardio::promise<void>> initialization_task;
};

struct TrayBackendState {
  std::shared_ptr<TrayBackendImplementation> implementation;
};

static GDBusNodeInfo *status_notifier_item_node_info() {
  static GDBusNodeInfo *node_info =
      g_dbus_node_info_new_for_xml(status_notifier_item_xml, nullptr);
  return node_info;
}

static GDBusNodeInfo *dbus_menu_node_info() {
  static GDBusNodeInfo *node_info =
      g_dbus_node_info_new_for_xml(dbus_menu_xml, nullptr);
  return node_info;
}

static GDBusInterfaceInfo *status_notifier_item_interface_info() {
  return status_notifier_item_node_info()->interfaces[0];
}

static GDBusInterfaceInfo *dbus_menu_interface_info() {
  return dbus_menu_node_info()->interfaces[0];
}

const char *launcher_application_id() {
  return application_id;
}

TrayBackendKind
select_tray_backend_kind(const TrayBackendAvailability &availability) {
  if (availability.has_status_notifier_item) {
    return TrayBackendKind::status_notifier_item;
  }
  if (availability.has_xembed) {
    return TrayBackendKind::xembed;
  }
  return TrayBackendKind::none;
}

TrayActivationContext
build_tray_activation_context(std::uint32_t timestamp) {
  return {
      .activation_time =
          timestamp == 0U
              ? std::optional<std::uint32_t>()
              : std::optional<std::uint32_t>(timestamp),
  };
}

std::vector<std::uint8_t>
convert_tray_icon_pixels_to_argb(const std::uint8_t *pixels,
                                 int width, int height, int rowstride,
                                 int channel_count) {
  if (pixels == nullptr || width <= 0 || height <= 0 || rowstride <= 0 ||
      (channel_count != 3 && channel_count != 4) ||
      rowstride < width * channel_count) {
    return {};
  }
  std::vector<std::uint8_t> result;
  result.reserve(static_cast<std::size_t>(width) *
                 static_cast<std::size_t>(height) * 4U);
  for (int y = 0; y < height; ++y) {
    const std::uint8_t *row =
        pixels + static_cast<std::size_t>(y) *
                     static_cast<std::size_t>(rowstride);
    for (int x = 0; x < width; ++x) {
      const std::uint8_t *pixel =
          row + static_cast<std::size_t>(x) *
                    static_cast<std::size_t>(channel_count);
      result.push_back(channel_count == 4 ? pixel[3] : 0xffU);
      result.push_back(pixel[0]);
      result.push_back(pixel[1]);
      result.push_back(pixel[2]);
    }
  }
  return result;
}

GVariant *
build_tray_icon_pixmap_variant(const std::vector<TrayIconPixmap> &pixmaps) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
  for (const TrayIconPixmap &pixmap : pixmaps) {
    if (pixmap.width <= 0 || pixmap.height <= 0) {
      continue;
    }
    const std::size_t expected =
        static_cast<std::size_t>(pixmap.width) *
        static_cast<std::size_t>(pixmap.height) * 4U;
    if (pixmap.argb_pixels.size() != expected) {
      continue;
    }
    GVariant *bytes = g_variant_new_fixed_array(
        G_VARIANT_TYPE_BYTE, pixmap.argb_pixels.data(),
        pixmap.argb_pixels.size(), sizeof(std::uint8_t));
    g_variant_builder_add(&builder, "(ii@ay)", pixmap.width,
                          pixmap.height, bytes);
  }
  return g_variant_builder_end(&builder);
}

static std::string tray_icon_path(int size,
                                  const std::string &icon_name) {
  return std::string(ELDER_TERMS_BUILD_ICON_THEME_PATH) +
         "/hicolor/" + std::to_string(size) + "x" +
         std::to_string(size) + "/apps/" + icon_name + ".png";
}

static std::string installed_tray_icon_path(
    int size, const std::string &icon_name) {
  return std::string(ELDER_TERMS_INSTALL_ICON_THEME_PATH) +
         "/hicolor/" + std::to_string(size) + "x" +
         std::to_string(size) + "/apps/" + icon_name + ".png";
}

static std::string resolve_tray_icon_path(
    int size, const std::string &icon_name) {
  const std::string build_path = tray_icon_path(size, icon_name);
  if (g_file_test(build_path.c_str(), G_FILE_TEST_IS_REGULAR)) {
    return build_path;
  }
  const std::string installed_path =
      installed_tray_icon_path(size, icon_name);
  if (g_file_test(installed_path.c_str(), G_FILE_TEST_IS_REGULAR)) {
    return installed_path;
  }
  return {};
}

static std::optional<TrayIconPixmap>
load_tray_icon_pixmap(const std::string &path) {
  GError *error = nullptr;
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &error);
  if (error != nullptr) {
    g_error_free(error);
    return std::nullopt;
  }
  if (pixbuf == nullptr) {
    return std::nullopt;
  }
  const int width = gdk_pixbuf_get_width(pixbuf);
  const int height = gdk_pixbuf_get_height(pixbuf);
  const std::vector<std::uint8_t> pixels =
      convert_tray_icon_pixels_to_argb(
          gdk_pixbuf_get_pixels(pixbuf), width, height,
          gdk_pixbuf_get_rowstride(pixbuf),
          gdk_pixbuf_get_n_channels(pixbuf));
  g_object_unref(pixbuf);
  if (pixels.empty()) {
    return std::nullopt;
  }
  return TrayIconPixmap{
      .width = width,
      .height = height,
      .argb_pixels = pixels,
  };
}

static std::vector<TrayIconPixmap>
load_tray_icon_pixmaps(const std::string &icon_name) {
  std::vector<TrayIconPixmap> pixmaps;
  for (const int size : tray_icon_sizes) {
    const std::string path = resolve_tray_icon_path(size, icon_name);
    if (path.empty()) {
      continue;
    }
    std::optional<TrayIconPixmap> pixmap =
        load_tray_icon_pixmap(path);
    if (pixmap.has_value()) {
      pixmaps.push_back(std::move(*pixmap));
    }
  }
  return pixmaps;
}

static std::string resolve_tray_icon_theme_path(
    const std::string &icon_name) {
  for (const int size : tray_icon_sizes) {
    const std::string build_path = tray_icon_path(size, icon_name);
    if (g_file_test(build_path.c_str(), G_FILE_TEST_IS_REGULAR)) {
      return std::filesystem::path(build_path).parent_path().string();
    }
    const std::string installed_path =
        installed_tray_icon_path(size, icon_name);
    if (g_file_test(installed_path.c_str(), G_FILE_TEST_IS_REGULAR)) {
      return std::filesystem::path(installed_path).parent_path().string();
    }
  }
  return {};
}

static GVariant *build_menu_item_properties(const char *label) {
  GVariantBuilder properties;
  g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&properties, "{sv}", "label",
                        g_variant_new_string(label));
  g_variant_builder_add(&properties, "{sv}", "enabled",
                        g_variant_new_boolean(TRUE));
  g_variant_builder_add(&properties, "{sv}", "visible",
                        g_variant_new_boolean(TRUE));
  return g_variant_builder_end(&properties);
}

static GVariant *build_menu_item(int id, const char *label) {
  GVariantBuilder children;
  g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
  return g_variant_new("(i@a{sv}@av)", id,
                       build_menu_item_properties(label),
                       g_variant_builder_end(&children));
}

static GVariant *build_menu_layout() {
  GVariantBuilder root_properties;
  g_variant_builder_init(&root_properties, G_VARIANT_TYPE("a{sv}"));
  GVariantBuilder children;
  g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
  g_variant_builder_add_value(
      &children,
      g_variant_new_variant(
          build_menu_item(open_menu_item_id, _("Open elder-terms"))));
  g_variant_builder_add_value(
      &children,
      g_variant_new_variant(build_menu_item(
          application_settings_menu_item_id, _("Application settings"))));
  g_variant_builder_add_value(
      &children,
      g_variant_new_variant(build_menu_item(
          about_menu_item_id, _("About elder-terms"))));
  g_variant_builder_add_value(
      &children,
      g_variant_new_variant(build_menu_item(quit_menu_item_id, _("Quit"))));
  GVariant *root =
      g_variant_new("(i@a{sv}@av)", 0,
                    g_variant_builder_end(&root_properties),
                    g_variant_builder_end(&children));
  return g_variant_new("(u@(ia{sv}av))", dbus_menu_revision, root);
}

static GVariant *
build_menu_group_properties(const std::vector<int> &ids) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ia{sv})"));
  for (const int id : ids) {
    if (id == open_menu_item_id) {
      g_variant_builder_add(
          &builder, "(i@a{sv})", id,
          build_menu_item_properties(_("Open elder-terms")));
    } else if (id == application_settings_menu_item_id) {
      g_variant_builder_add(
          &builder, "(i@a{sv})", id,
          build_menu_item_properties(_("Application settings")));
    } else if (id == about_menu_item_id) {
      g_variant_builder_add(
          &builder, "(i@a{sv})", id,
          build_menu_item_properties(_("About elder-terms")));
    } else if (id == quit_menu_item_id) {
      g_variant_builder_add(&builder, "(i@a{sv})", id,
                            build_menu_item_properties(_("Quit")));
    }
  }
  return g_variant_builder_end(&builder);
}

static void set_backend_availability(
    TrayBackendImplementation *implementation,
    TrayBackendAvailabilityState availability) {
  if (implementation == nullptr || implementation->destroyed ||
      implementation->availability == availability) {
    return;
  }
  implementation->availability = availability;
  if (implementation->options.callbacks.availability_changed) {
    implementation->options.callbacks.availability_changed(availability);
  }
}

static void activate_backend(
    TrayBackendImplementation *implementation,
    const TrayActivationContext &context) {
  if (implementation != nullptr && !implementation->destroyed &&
      implementation->options.callbacks.activate) {
    implementation->options.callbacks.activate(context);
  }
}

static void quit_backend(TrayBackendImplementation *implementation) {
  if (implementation != nullptr && !implementation->destroyed &&
      implementation->options.callbacks.quit) {
    implementation->options.callbacks.quit();
  }
}

static void open_application_settings_backend(
    TrayBackendImplementation *implementation) {
  if (implementation != nullptr && !implementation->destroyed &&
      implementation->options.callbacks.application_settings) {
    implementation->options.callbacks.application_settings();
  }
}

static void open_about_backend(TrayBackendImplementation *implementation) {
  if (implementation != nullptr && !implementation->destroyed &&
      implementation->options.callbacks.about) {
    implementation->options.callbacks.about();
  }
}

static void handle_menu_event(TrayBackendImplementation *implementation,
                              int item_id, const char *event_id,
                              std::uint32_t timestamp) {
  if (event_id == nullptr || std::strcmp(event_id, "clicked") != 0) {
    return;
  }
  if (item_id == open_menu_item_id) {
    activate_backend(implementation,
                     build_tray_activation_context(timestamp));
  } else if (item_id == application_settings_menu_item_id) {
    open_application_settings_backend(implementation);
  } else if (item_id == about_menu_item_id) {
    open_about_backend(implementation);
  } else if (item_id == quit_menu_item_id) {
    quit_backend(implementation);
  }
}

static void handle_status_notifier_method(
    TrayBackendImplementation *implementation, const gchar *method_name,
    GDBusMethodInvocation *invocation) {
  if (std::strcmp(method_name, "Activate") == 0 ||
      std::strcmp(method_name, "SecondaryActivate") == 0) {
    activate_backend(implementation, build_tray_activation_context(0U));
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (std::strcmp(method_name, "ContextMenu") == 0 ||
      std::strcmp(method_name, "Scroll") == 0) {
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  g_dbus_method_invocation_return_error_literal(
      invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
      "Unsupported StatusNotifierItem method");
}

static void handle_dbus_menu_method(
    TrayBackendImplementation *implementation, const gchar *method_name,
    GVariant *parameters, GDBusMethodInvocation *invocation) {
  if (std::strcmp(method_name, "GetLayout") == 0) {
    g_dbus_method_invocation_return_value(invocation,
                                          build_menu_layout());
    return;
  }
  if (std::strcmp(method_name, "GetGroupProperties") == 0) {
    GVariant *ids_value = nullptr;
    GVariant *properties_value = nullptr;
    g_variant_get(parameters, "(@ai@as)", &ids_value,
                  &properties_value);
    std::vector<int> ids;
    GVariantIter iterator;
    g_variant_iter_init(&iterator, ids_value);
    gint id = 0;
    while (g_variant_iter_next(&iterator, "i", &id)) {
      ids.push_back(id);
    }
    g_variant_unref(properties_value);
    g_variant_unref(ids_value);
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(@a(ia{sv}))",
                      build_menu_group_properties(ids)));
    return;
  }
  if (std::strcmp(method_name, "Event") == 0) {
    gint item_id = 0;
    const gchar *event_id = nullptr;
    GVariant *data = nullptr;
    guint timestamp = 0;
    g_variant_get(parameters, "(i&svu)", &item_id, &event_id, &data,
                  &timestamp);
    handle_menu_event(implementation, item_id, event_id, timestamp);
    g_variant_unref(data);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (std::strcmp(method_name, "EventGroup") == 0) {
    GVariant *events = nullptr;
    g_variant_get(parameters, "(@a(isvu))", &events);
    GVariantIter iterator;
    g_variant_iter_init(&iterator, events);
    GVariant *event = nullptr;
    while ((event = g_variant_iter_next_value(&iterator)) != nullptr) {
      gint item_id = 0;
      const gchar *event_id = nullptr;
      GVariant *data = nullptr;
      guint timestamp = 0;
      g_variant_get(event, "(i&svu)", &item_id, &event_id, &data,
                    &timestamp);
      handle_menu_event(implementation, item_id, event_id, timestamp);
      g_variant_unref(data);
      g_variant_unref(event);
    }
    g_variant_unref(events);
    GVariantBuilder errors;
    g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(ai)", &errors));
    return;
  }
  if (std::strcmp(method_name, "AboutToShow") == 0) {
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(b)", FALSE));
    return;
  }
  if (std::strcmp(method_name, "AboutToShowGroup") == 0) {
    GVariantBuilder updates;
    g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
    GVariantBuilder errors;
    g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(aiai)", &updates, &errors));
    return;
  }
  g_dbus_method_invocation_return_error_literal(
      invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
      "Unsupported DBusMenu method");
}

static void on_dbus_method_call(
    GDBusConnection *, const gchar *, const gchar *,
    const gchar *interface_name, const gchar *method_name,
    GVariant *parameters, GDBusMethodInvocation *invocation,
    gpointer user_data) {
  auto *implementation =
      static_cast<TrayBackendImplementation *>(user_data);
  if (std::strcmp(interface_name, status_notifier_item_interface) == 0) {
    handle_status_notifier_method(implementation, method_name,
                                  invocation);
    return;
  }
  if (std::strcmp(interface_name, dbus_menu_interface) == 0) {
    handle_dbus_menu_method(implementation, method_name, parameters,
                            invocation);
    return;
  }
  g_dbus_method_invocation_return_error_literal(
      invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
      "Unsupported tray interface");
}

static GVariant *status_notifier_property(
    TrayBackendImplementation *implementation, const gchar *property_name,
    GError **error) {
  if (std::strcmp(property_name, "Category") == 0) {
    return g_variant_new_string("ApplicationStatus");
  }
  if (std::strcmp(property_name, "Id") == 0) {
    return g_variant_new_string(
        implementation->options.identifier.c_str());
  }
  if (std::strcmp(property_name, "Title") == 0) {
    return g_variant_new_string(implementation->options.title.c_str());
  }
  if (std::strcmp(property_name, "Status") == 0) {
    return g_variant_new_string("Active");
  }
  if (std::strcmp(property_name, "WindowId") == 0) {
    return g_variant_new_uint32(0);
  }
  if (std::strcmp(property_name, "Menu") == 0) {
    return g_variant_new_object_path(status_notifier_menu_path);
  }
  if (std::strcmp(property_name, "ItemIsMenu") == 0) {
    return g_variant_new_boolean(FALSE);
  }
  if (std::strcmp(property_name, "IconName") == 0) {
    return g_variant_new_string(implementation->options.icon_name.c_str());
  }
  if (std::strcmp(property_name, "IconPixmap") == 0) {
    return build_tray_icon_pixmap_variant(
        load_tray_icon_pixmaps(implementation->options.icon_name));
  }
  if (std::strcmp(property_name, "OverlayIconName") == 0 ||
      std::strcmp(property_name, "AttentionIconName") == 0 ||
      std::strcmp(property_name, "AttentionAccessibleDesc") == 0) {
    return g_variant_new_string("");
  }
  if (std::strcmp(property_name, "OverlayIconPixmap") == 0 ||
      std::strcmp(property_name, "AttentionIconPixmap") == 0) {
    return build_tray_icon_pixmap_variant({});
  }
  if (std::strcmp(property_name, "IconThemePath") == 0) {
    const std::string path = resolve_tray_icon_theme_path(
        implementation->options.icon_name);
    return g_variant_new_string(path.c_str());
  }
  if (std::strcmp(property_name, "IconAccessibleDesc") == 0) {
    return g_variant_new_string(implementation->options.title.c_str());
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "Unsupported StatusNotifierItem property");
  return nullptr;
}

static GVariant *dbus_menu_property(const gchar *property_name,
                                    GError **error) {
  if (std::strcmp(property_name, "Status") == 0) {
    return g_variant_new_string("normal");
  }
  if (std::strcmp(property_name, "TextDirection") == 0) {
    return g_variant_new_string("ltr");
  }
  if (std::strcmp(property_name, "Version") == 0) {
    return g_variant_new_uint32(3);
  }
  if (std::strcmp(property_name, "IconThemePath") == 0) {
    return g_variant_new_string("");
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "Unsupported DBusMenu property");
  return nullptr;
}

static GVariant *on_dbus_get_property(
    GDBusConnection *, const gchar *, const gchar *,
    const gchar *interface_name, const gchar *property_name,
    GError **error, gpointer user_data) {
  auto *implementation =
      static_cast<TrayBackendImplementation *>(user_data);
  if (std::strcmp(interface_name, status_notifier_item_interface) == 0) {
    return status_notifier_property(implementation, property_name, error);
  }
  if (std::strcmp(interface_name, dbus_menu_interface) == 0) {
    return dbus_menu_property(property_name, error);
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "Unsupported tray property");
  return nullptr;
}

static const GDBusInterfaceVTable status_notifier_vtable = {
    on_dbus_method_call,
    on_dbus_get_property,
    nullptr,
    {nullptr},
};

static const GDBusInterfaceVTable dbus_menu_vtable = {
    on_dbus_method_call,
    on_dbus_get_property,
    nullptr,
    {nullptr},
};

static bool register_status_notifier_objects(
    TrayBackendImplementation *implementation) {
  GError *error = nullptr;
  implementation->item_registration_id =
      g_dbus_connection_register_object(
          implementation->connection, status_notifier_item_path,
          status_notifier_item_interface_info(),
          &status_notifier_vtable, implementation, nullptr, &error);
  if (error != nullptr) {
    std::cerr << "Failed to register StatusNotifierItem: "
              << error->message << '\n';
    g_error_free(error);
    return false;
  }
  implementation->menu_registration_id =
      g_dbus_connection_register_object(
          implementation->connection, status_notifier_menu_path,
          dbus_menu_interface_info(), &dbus_menu_vtable,
          implementation, nullptr, &error);
  if (error != nullptr) {
    std::cerr << "Failed to register StatusNotifier menu: "
              << error->message << '\n';
    g_error_free(error);
    return false;
  }
  return true;
}

static void unregister_status_notifier_objects(
    TrayBackendImplementation *implementation) {
  if (implementation->connection == nullptr) {
    return;
  }
  if (implementation->menu_registration_id != 0) {
    g_dbus_connection_unregister_object(
        implementation->connection,
        implementation->menu_registration_id);
    implementation->menu_registration_id = 0;
  }
  if (implementation->item_registration_id != 0) {
    g_dbus_connection_unregister_object(
        implementation->connection,
        implementation->item_registration_id);
    implementation->item_registration_id = 0;
  }
}

static cardio::promise<GVariant *> call_dbus_async(
    GDBusConnection *connection, const char *bus_name,
    const char *object_path, const char *interface_name,
    const char *method_name, GVariant *parameters,
    const GVariantType *reply_type, cardio::cancellation cancellation) {
  co_return co_await cardio::gio::submit<GVariant *>(
      [connection, bus_name, object_path, interface_name, method_name,
       parameters, reply_type](
          GCancellable *cancellable, GAsyncReadyCallback callback,
          gpointer user_data) {
        g_dbus_connection_call(
            connection, bus_name, object_path, interface_name,
            method_name, parameters, reply_type, G_DBUS_CALL_FLAGS_NONE,
            -1, cancellable, callback, user_data);
      },
      [connection](GObject *, GAsyncResult *result, GError **error) {
        return g_dbus_connection_call_finish(connection, result, error);
      },
      std::move(cancellation));
}

static cardio::promise<bool> status_notifier_host_available_async(
    TrayBackendImplementation *implementation,
    cardio::cancellation cancellation) {
  GVariant *owner_result = co_await call_dbus_async(
      implementation->connection, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "NameHasOwner",
      g_variant_new("(s)", status_notifier_watcher_service),
      G_VARIANT_TYPE("(b)"), cancellation);
  gboolean has_owner = FALSE;
  g_variant_get(owner_result, "(b)", &has_owner);
  g_variant_unref(owner_result);
  if (has_owner == FALSE) {
    co_return false;
  }

  GVariant *property_result = co_await call_dbus_async(
      implementation->connection, status_notifier_watcher_service,
      status_notifier_watcher_path, "org.freedesktop.DBus.Properties",
      "Get",
      g_variant_new("(ss)", status_notifier_watcher_interface,
                    "IsStatusNotifierHostRegistered"),
      G_VARIANT_TYPE("(v)"), std::move(cancellation));
  GVariant *property = nullptr;
  g_variant_get(property_result, "(v)", &property);
  const bool available = g_variant_get_boolean(property) != FALSE;
  g_variant_unref(property);
  g_variant_unref(property_result);
  co_return available;
}

static cardio::promise<void> register_status_notifier_async(
    TrayBackendImplementation *implementation,
    cardio::cancellation cancellation) {
  GVariant *result = co_await call_dbus_async(
      implementation->connection, status_notifier_watcher_service,
      status_notifier_watcher_path, status_notifier_watcher_interface,
      "RegisterStatusNotifierItem",
      g_variant_new("(s)", status_notifier_item_path), nullptr,
      std::move(cancellation));
  if (result != nullptr) {
    g_variant_unref(result);
  }
}

static bool can_use_xembed() {
  GdkDisplay *display = gdk_display_get_default();
  return display != nullptr && GDK_IS_X11_DISPLAY(display);
}

static bool xembed_tray_host_available() {
  GdkDisplay *display = gdk_display_get_default();
  if (display == nullptr || !GDK_IS_X11_DISPLAY(display)) {
    return false;
  }
  GdkScreen *screen = gdk_display_get_default_screen(display);
  if (screen == nullptr) {
    return false;
  }
  const std::string selection_name =
      "_NET_SYSTEM_TRAY_S" +
      std::to_string(gdk_x11_screen_get_screen_number(screen));
  const GdkAtom selection =
      gdk_atom_intern(selection_name.c_str(), TRUE);
  return selection != GDK_NONE &&
         gdk_selection_owner_get_for_display(display, selection) != nullptr;
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void on_status_icon_activate(GtkStatusIcon *, gpointer user_data) {
  activate_backend(
      static_cast<TrayBackendImplementation *>(user_data),
      build_tray_activation_context(gtk_get_current_event_time()));
}

static void on_status_icon_popup(GtkStatusIcon *status_icon, guint button,
                                 guint activate_time,
                                 gpointer user_data) {
  auto *implementation =
      static_cast<TrayBackendImplementation *>(user_data);
  gtk_menu_popup(GTK_MENU(implementation->status_menu), nullptr, nullptr,
                 gtk_status_icon_position_menu, status_icon, button,
                 activate_time);
}

static void on_status_icon_open(GtkMenuItem *, gpointer user_data) {
  activate_backend(
      static_cast<TrayBackendImplementation *>(user_data),
      build_tray_activation_context(gtk_get_current_event_time()));
}

static void on_status_icon_application_settings(GtkMenuItem *,
                                                gpointer user_data) {
  open_application_settings_backend(
      static_cast<TrayBackendImplementation *>(user_data));
}

static void on_status_icon_about(GtkMenuItem *, gpointer user_data) {
  open_about_backend(static_cast<TrayBackendImplementation *>(user_data));
}

static void on_status_icon_quit(GtkMenuItem *, gpointer user_data) {
  quit_backend(static_cast<TrayBackendImplementation *>(user_data));
}

static void on_status_icon_embedded_changed(GObject *object, GParamSpec *,
                                            gpointer user_data) {
  set_backend_availability(
      static_cast<TrayBackendImplementation *>(user_data),
      gtk_status_icon_is_embedded(GTK_STATUS_ICON(object)) != FALSE
          ? TrayBackendAvailabilityState::available
          : TrayBackendAvailabilityState::unavailable);
}

static void create_xembed_backend(
    TrayBackendImplementation *implementation) {
  implementation->kind = TrayBackendKind::xembed;
  const std::string icon_path =
      resolve_tray_icon_path(24, implementation->options.icon_name);
  implementation->status_icon =
      icon_path.empty()
          ? gtk_status_icon_new_from_icon_name(
                implementation->options.icon_name.c_str())
          : gtk_status_icon_new_from_file(icon_path.c_str());
  implementation->status_menu = gtk_menu_new();
  gtk_status_icon_set_tooltip_text(
      implementation->status_icon,
      implementation->options.title.c_str());
  gtk_status_icon_set_title(implementation->status_icon,
                            implementation->options.title.c_str());

  GtkWidget *open_item =
      gtk_menu_item_new_with_label(_("Open elder-terms"));
  GtkWidget *application_settings_item =
      gtk_menu_item_new_with_label(_("Application settings"));
  GtkWidget *about_item =
      gtk_menu_item_new_with_label(_("About elder-terms"));
  GtkWidget *quit_item = gtk_menu_item_new_with_label(_("Quit"));
  gtk_menu_shell_append(GTK_MENU_SHELL(implementation->status_menu),
                        open_item);
  gtk_menu_shell_append(GTK_MENU_SHELL(implementation->status_menu),
                        application_settings_item);
  gtk_menu_shell_append(GTK_MENU_SHELL(implementation->status_menu),
                        about_item);
  gtk_menu_shell_append(GTK_MENU_SHELL(implementation->status_menu),
                        quit_item);
  gtk_widget_show_all(implementation->status_menu);

  g_signal_connect(implementation->status_icon, "activate",
                   G_CALLBACK(on_status_icon_activate), implementation);
  g_signal_connect(implementation->status_icon, "popup-menu",
                   G_CALLBACK(on_status_icon_popup), implementation);
  g_signal_connect(open_item, "activate",
                   G_CALLBACK(on_status_icon_open), implementation);
  g_signal_connect(application_settings_item, "activate",
                   G_CALLBACK(on_status_icon_application_settings),
                   implementation);
  g_signal_connect(about_item, "activate",
                   G_CALLBACK(on_status_icon_about), implementation);
  g_signal_connect(quit_item, "activate",
                   G_CALLBACK(on_status_icon_quit), implementation);
  implementation->embedded_signal_id = g_signal_connect(
      implementation->status_icon, "notify::embedded",
      G_CALLBACK(on_status_icon_embedded_changed), implementation);
  gtk_status_icon_set_visible(implementation->status_icon, TRUE);
  set_backend_availability(
      implementation,
      xembed_tray_host_available()
          ? TrayBackendAvailabilityState::available
          : TrayBackendAvailabilityState::unavailable);
}

static void destroy_xembed_backend(
    TrayBackendImplementation *implementation) {
  if (implementation->status_menu != nullptr) {
    gtk_widget_destroy(implementation->status_menu);
    implementation->status_menu = nullptr;
  }
  if (implementation->status_icon != nullptr) {
    if (implementation->embedded_signal_id != 0) {
      g_signal_handler_disconnect(
          implementation->status_icon,
          implementation->embedded_signal_id);
      implementation->embedded_signal_id = 0;
    }
    gtk_status_icon_set_visible(implementation->status_icon, FALSE);
    g_object_unref(implementation->status_icon);
    implementation->status_icon = nullptr;
  }
}
G_GNUC_END_IGNORE_DEPRECATIONS

static void create_fallback_backend(
    TrayBackendImplementation *implementation) {
  if (implementation->destroyed) {
    return;
  }
  const TrayBackendKind selected = select_tray_backend_kind({
      .has_status_notifier_item = false,
      .has_xembed = can_use_xembed(),
  });
  if (selected == TrayBackendKind::xembed) {
    create_xembed_backend(implementation);
  } else {
    implementation->kind = selected;
    set_backend_availability(
        implementation, TrayBackendAvailabilityState::unavailable);
  }
}

static cardio::promise<void> initialize_backend_async(
    std::shared_ptr<TrayBackendImplementation> implementation) {
  try {
    const cardio::cancellation cancellation =
        implementation->cancellation_source->get_cancellation();
    const bool has_status_notifier =
        implementation->connection != nullptr &&
        co_await status_notifier_host_available_async(
            implementation.get(), cancellation);
    if (implementation->destroyed) {
      co_return;
    }
    if (!has_status_notifier) {
      create_fallback_backend(implementation.get());
      co_return;
    }
    if (!register_status_notifier_objects(implementation.get())) {
      unregister_status_notifier_objects(implementation.get());
      create_fallback_backend(implementation.get());
      co_return;
    }
    co_await register_status_notifier_async(
        implementation.get(), cancellation);
    if (implementation->destroyed) {
      co_return;
    }
    implementation->kind =
        TrayBackendKind::status_notifier_item;
    set_backend_availability(
        implementation.get(), TrayBackendAvailabilityState::available);
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    if (!implementation->destroyed) {
      std::cerr << "Failed to initialize tray backend: "
                << error.what() << '\n';
      unregister_status_notifier_objects(implementation.get());
      create_fallback_backend(implementation.get());
    }
  }
}

TrayBackendState *create_tray_backend(TrayBackendOptions options) {
  auto implementation = std::make_shared<TrayBackendImplementation>();
  implementation->options = std::move(options);
  implementation->cancellation_source.emplace();
  implementation->connection = g_application_get_dbus_connection(
      G_APPLICATION(implementation->options.application));
  if (implementation->connection != nullptr) {
    g_object_ref(implementation->connection);
  }

  auto *state = new TrayBackendState{
      .implementation = implementation,
  };
  if (implementation->options.dispatcher == nullptr) {
    create_fallback_backend(implementation.get());
    return state;
  }
  implementation->initialization_task.emplace(
      initialize_backend_async(implementation));
  return state;
}

void destroy_tray_backend(TrayBackendState *state) {
  if (state == nullptr) {
    return;
  }
  const std::shared_ptr<TrayBackendImplementation> implementation =
      state->implementation;
  implementation->destroyed = true;
  implementation->availability =
      TrayBackendAvailabilityState::unavailable;
  if (implementation->cancellation_source.has_value()) {
    (void)implementation->cancellation_source->cancel();
  }
  unregister_status_notifier_objects(implementation.get());
  destroy_xembed_backend(implementation.get());
  g_clear_object(&implementation->connection);
  delete state;
}

TrayBackendKind tray_backend_kind(const TrayBackendState *state) {
  return state == nullptr ? TrayBackendKind::none
                          : state->implementation->kind;
}

TrayBackendAvailabilityState
tray_backend_availability(const TrayBackendState *state) {
  return state == nullptr
             ? TrayBackendAvailabilityState::unavailable
             : state->implementation->availability;
}

} // namespace elder_terms
