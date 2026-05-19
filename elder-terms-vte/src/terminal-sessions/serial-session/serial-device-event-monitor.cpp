#include "serial-device-event-monitor.h"

#include <libudev.h>

#include <gio/gio.h>
#include <glib-unix.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace elder_terms {

static bool contains_slash(const std::string &value) {
  return value.find('/') != std::string::npos;
}

static bool starts_with(const std::string &value, const std::string &prefix) {
  return value.rfind(prefix, 0) == 0;
}

static bool path_is_existing_directory(const std::filesystem::path &path) {
  std::error_code error;
  return std::filesystem::is_directory(path, error) && !error;
}

static void remove_source(guint *source_id) {
  if (*source_id != 0) {
    g_source_remove(*source_id);
    *source_id = 0;
  }
}

static std::vector<std::filesystem::path>
serial_device_watch_directories(const std::string &selector,
                                const SerialDeviceEventMonitorOptions &options) {
  std::vector<std::filesystem::path> directories;
  const std::filesystem::path selector_path(selector);
  const std::filesystem::path by_path_root =
      options.dev_root / "serial" / "by-path";
  const std::string by_path_prefix =
      by_path_root.lexically_normal().string() + "/";
  const std::string normalized_selector =
      selector_path.lexically_normal().string();

  if (starts_with(normalized_selector, by_path_prefix)) {
    directories.push_back(selector_path.parent_path());
    directories.push_back(selector_path.parent_path().parent_path());
    return directories;
  }

  if (!contains_slash(selector)) {
    directories.push_back(options.dev_root / "serial" / "by-id");
    directories.push_back(options.dev_root / "serial");
    return directories;
  }

  if (selector_path.is_absolute()) {
    directories.push_back(selector_path.parent_path());
  }
  return directories;
}

static bool serial_udev_action_is_relevant(const char *action) {
  if (action == nullptr) {
    return true;
  }
  return std::strcmp(action, "add") == 0 || std::strcmp(action, "change") == 0 ||
         std::strcmp(action, "remove") == 0;
}

class SerialDeviceEventMonitor::Impl {
private:
  std::string selector;
  SerialDeviceEventMonitorOptions options;
  SerialDeviceEventCallback callback;
  udev *udev_context = nullptr;
  udev_monitor *udev_device_monitor = nullptr;
  guint udev_watch_id = 0;
  guint callback_idle_id = 0;
  bool running = false;
  std::vector<GFileMonitor *> file_monitors;

  void schedule_callback() {
    if (!running || callback_idle_id != 0) {
      return;
    }

    callback_idle_id = g_idle_add(SerialDeviceEventMonitor::Impl::on_callback_idle,
                                  this);
  }

  bool handle_udev_ready(GIOCondition condition) {
    if ((condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0) {
      return false;
    }
    if ((condition & G_IO_IN) == 0) {
      return running;
    }

    while (running) {
      udev_device *device = udev_monitor_receive_device(udev_device_monitor);
      if (device == nullptr) {
        break;
      }

      const char *subsystem = udev_device_get_subsystem(device);
      const char *action = udev_device_get_action(device);
      if (subsystem != nullptr && std::strcmp(subsystem, "tty") == 0 &&
          serial_udev_action_is_relevant(action)) {
        schedule_callback();
      }
      udev_device_unref(device);
    }
    return running;
  }

  void start_udev_monitor() {
    udev_context = udev_new();
    if (udev_context == nullptr) {
      std::cerr << "Warning: serial device udev monitor unavailable: "
                << "udev_new failed" << '\n';
      return;
    }

    udev_device_monitor =
        udev_monitor_new_from_netlink(udev_context, "udev");
    if (udev_device_monitor == nullptr) {
      std::cerr << "Warning: serial device udev monitor unavailable: "
                << "udev_monitor_new_from_netlink failed" << '\n';
      return;
    }

    if (udev_monitor_filter_add_match_subsystem_devtype(
            udev_device_monitor, "tty", nullptr) < 0) {
      std::cerr << "Warning: serial device udev monitor filter failed" << '\n';
    }
    if (udev_monitor_enable_receiving(udev_device_monitor) < 0) {
      std::cerr << "Warning: serial device udev monitor unavailable: "
                << "udev_monitor_enable_receiving failed" << '\n';
      return;
    }

    const int fd = udev_monitor_get_fd(udev_device_monitor);
    if (fd < 0) {
      std::cerr << "Warning: serial device udev monitor unavailable: "
                << std::strerror(errno) << '\n';
      return;
    }

    udev_watch_id = g_unix_fd_add(
        fd, static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL),
        SerialDeviceEventMonitor::Impl::on_udev_ready, this);
  }

  void watch_directory(const std::filesystem::path &directory,
                       std::set<std::string> *watched_directories) {
    if (directory.empty()) {
      return;
    }

    const std::string key = directory.lexically_normal().string();
    if (watched_directories->find(key) != watched_directories->end()) {
      return;
    }
    watched_directories->insert(key);

    if (!path_is_existing_directory(directory)) {
      return;
    }

    GFile *file = g_file_new_for_path(key.c_str());
    GError *error = nullptr;
    GFileMonitor *monitor =
        g_file_monitor_directory(file, G_FILE_MONITOR_NONE, nullptr, &error);
    g_object_unref(file);

    if (monitor == nullptr) {
      if (error != nullptr) {
        std::cerr << "Warning: serial device file monitor unavailable: "
                  << error->message << '\n';
        g_error_free(error);
      }
      return;
    }

    g_signal_connect(monitor, "changed",
                     G_CALLBACK(SerialDeviceEventMonitor::Impl::on_file_changed),
                     this);
    file_monitors.push_back(monitor);
  }

  void start_file_monitors() {
    std::set<std::string> watched_directories;
    for (const std::filesystem::path &directory :
         serial_device_watch_directories(selector, options)) {
      watch_directory(directory, &watched_directories);
    }
  }

  void stop_file_monitors() {
    for (GFileMonitor *monitor : file_monitors) {
      g_signal_handlers_disconnect_by_data(monitor, this);
      (void)g_file_monitor_cancel(monitor);
      g_object_unref(monitor);
    }
    file_monitors.clear();
  }

  void stop_udev_monitor() {
    remove_source(&udev_watch_id);
    if (udev_device_monitor != nullptr) {
      udev_monitor_unref(udev_device_monitor);
      udev_device_monitor = nullptr;
    }
    if (udev_context != nullptr) {
      udev_unref(udev_context);
      udev_context = nullptr;
    }
  }

  static gboolean on_callback_idle(gpointer user_data) {
    auto *self = static_cast<SerialDeviceEventMonitor::Impl *>(user_data);
    self->callback_idle_id = 0;
    SerialDeviceEventCallback pending_callback = self->callback;
    if (self->running && pending_callback) {
      pending_callback();
    }
    return G_SOURCE_REMOVE;
  }

  static gboolean on_udev_ready(gint, GIOCondition condition,
                                gpointer user_data) {
    auto *self = static_cast<SerialDeviceEventMonitor::Impl *>(user_data);
    const bool keep = self->handle_udev_ready(condition);
    if (!keep) {
      self->udev_watch_id = 0;
    }
    return keep ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
  }

  static void on_file_changed(GFileMonitor *, GFile *, GFile *,
                              GFileMonitorEvent, gpointer user_data) {
    auto *self = static_cast<SerialDeviceEventMonitor::Impl *>(user_data);
    self->schedule_callback();
  }

public:
  Impl(std::string selector, SerialDeviceEventMonitorOptions options,
       SerialDeviceEventCallback callback)
      : selector(std::move(selector)),
        options(std::move(options)),
        callback(std::move(callback)) {
  }

  ~Impl() {
    stop();
  }

  void start() {
    if (running) {
      return;
    }

    running = true;
#ifdef ELDER_TERMS_ENABLE_TEST_DOUBLES
    if (!options.enable_system_sources) {
      return;
    }
#endif
    start_udev_monitor();
    start_file_monitors();
  }

  void stop() {
    running = false;
    remove_source(&callback_idle_id);
    stop_file_monitors();
    stop_udev_monitor();
  }

#ifdef ELDER_TERMS_ENABLE_TEST_DOUBLES
  void notify_device_event_for_test() {
    schedule_callback();
  }
#endif
};

SerialDeviceEventMonitor::SerialDeviceEventMonitor(
    std::string selector, SerialDeviceEventCallback callback)
    : SerialDeviceEventMonitor(std::move(selector),
                               SerialDeviceEventMonitorOptions{},
                               std::move(callback)) {
}

SerialDeviceEventMonitor::SerialDeviceEventMonitor(
    std::string selector, SerialDeviceEventMonitorOptions options,
    SerialDeviceEventCallback callback)
    : impl(std::make_unique<Impl>(std::move(selector), std::move(options),
                                  std::move(callback))) {
}

SerialDeviceEventMonitor::~SerialDeviceEventMonitor() = default;

void SerialDeviceEventMonitor::start() {
  impl->start();
}

void SerialDeviceEventMonitor::stop() {
  impl->stop();
}

#ifdef ELDER_TERMS_ENABLE_TEST_DOUBLES
void SerialDeviceEventMonitor::notify_device_event_for_test() {
  impl->notify_device_event_for_test();
}
#endif

} // namespace elder_terms
