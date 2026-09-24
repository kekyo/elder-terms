#include "hotkey-setup.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <gdk/gdk.h>
#include <glib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <sys/stat.h>
#include <unistd.h>

namespace elder_terms {

static constexpr char sway_begin[] = "# elder-terms setup begin";
static constexpr char sway_end[] = "# elder-terms setup end";
static constexpr char labwc_marker[] = "elder-terms setup";
static constexpr auto modifier_mask = static_cast<GdkModifierType>(
    GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SHIFT_MASK | GDK_SUPER_MASK);

static std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Cannot read " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

static std::string read_base_config(
    const std::filesystem::path &user_path,
    const std::vector<std::filesystem::path> &system_paths) {
  if (std::filesystem::exists(user_path)) {
    return read_file(user_path);
  }
  for (const auto &path : system_paths) {
    if (std::filesystem::is_regular_file(path)) {
      return read_file(path);
    }
  }
  return {};
}

static void write_config(const std::filesystem::path &path,
                         const std::string &contents) {
  const auto parent = path.parent_path();
  std::filesystem::create_directories(parent);
  struct stat original {};
  if (lstat(path.c_str(), &original) == 0) {
    if (!S_ISREG(original.st_mode) || original.st_uid != geteuid()) {
      throw std::runtime_error("Refusing to replace an unsafe config file: " +
                               path.string());
    }
    if (read_file(path) == contents) {
      return;
    }
  } else if (errno != ENOENT) {
    throw std::runtime_error("Cannot inspect " + path.string() + ": " +
                             std::strerror(errno));
  }
  std::string temporary = path.string() + ".tmp-XXXXXX";
  const int descriptor = mkstemp(temporary.data());
  if (descriptor < 0) {
    throw std::runtime_error("Cannot create " + path.string() + ": " +
                             std::strerror(errno));
  }
  bool descriptor_open = true;
  try {
    const mode_t mode = S_ISREG(original.st_mode)
                            ? original.st_mode & 0777 : 0600;
    if (fchmod(descriptor, mode) != 0) {
      throw std::runtime_error("Cannot set config permissions");
    }
    std::size_t offset = 0;
    while (offset < contents.size()) {
      const auto count = write(descriptor, contents.data() + offset,
                               contents.size() - offset);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        throw std::runtime_error("Cannot write " + path.string());
      }
      offset += static_cast<std::size_t>(count);
    }
    if (close(descriptor) != 0) {
      descriptor_open = false;
      throw std::runtime_error("Cannot close " + path.string());
    }
    descriptor_open = false;
    if (rename(temporary.c_str(), path.c_str()) != 0) {
      throw std::runtime_error("Cannot replace " + path.string() + ": " +
                               std::strerror(errno));
    }
  } catch (...) {
    if (descriptor_open) {
      (void)close(descriptor);
    }
    (void)unlink(temporary.c_str());
    throw;
  }
}

static std::string shell_quote(const std::string &value) {
  if (value.find_first_of("\r\n") != std::string::npos) {
    throw std::runtime_error("A hotkey command contains an unsupported line break");
  }
  gchar *quoted = g_shell_quote(value.c_str());
  if (quoted == nullptr) {
    throw std::runtime_error("Cannot quote hotkey command");
  }
  const std::string result(quoted);
  g_free(quoted);
  return result;
}

static std::string shell_command(
    const std::vector<std::string> &arguments) {
  if (arguments.empty()) {
    throw std::runtime_error("Hotkey command is empty");
  }
  std::string result;
  for (const auto &argument : arguments) {
    if (!result.empty()) {
      result += ' ';
    }
    result += shell_quote(argument);
  }
  return result;
}

static std::string key_name(const KeyBinding &binding) {
  if (binding.modifiers == 0 ||
      (binding.modifiers & modifier_mask) != binding.modifiers) {
    throw std::runtime_error("A configured hotkey has unsupported modifiers");
  }
  const char *raw = gdk_keyval_name(binding.keyval);
  if (raw == nullptr || *raw == '\0') {
    throw std::runtime_error("A configured hotkey has an unsupported key");
  }
  const std::string name(raw);
  if (!std::all_of(name.begin(), name.end(), [](unsigned char value) {
        return g_ascii_isalnum(value) || value == '_';
      })) {
    throw std::runtime_error("A configured hotkey cannot be expressed by the compositor");
  }
  return name;
}

static std::string sway_binding(const KeyBinding &binding) {
  std::string result;
  if ((binding.modifiers & GDK_CONTROL_MASK) != 0) result += "Ctrl+";
  if ((binding.modifiers & GDK_MOD1_MASK) != 0) result += "Mod1+";
  if ((binding.modifiers & GDK_SHIFT_MASK) != 0) result += "Shift+";
  if ((binding.modifiers & GDK_SUPER_MASK) != 0) result += "Mod4+";
  return result + key_name(binding);
}

static std::string labwc_binding(const KeyBinding &binding) {
  std::string result;
  if ((binding.modifiers & GDK_CONTROL_MASK) != 0) result += "C-";
  if ((binding.modifiers & GDK_MOD1_MASK) != 0) result += "A-";
  if ((binding.modifiers & GDK_SHIFT_MASK) != 0) result += "S-";
  if ((binding.modifiers & GDK_SUPER_MASK) != 0) result += "W-";
  return result + key_name(binding);
}

static std::filesystem::path sway_user_config(
    const ExternalHotkeySetupEnvironment &environment) {
  const auto legacy = environment.home / ".sway/config";
  if (std::filesystem::exists(legacy)) {
    return legacy;
  }
  const auto xdg = environment.config_home / "sway/config";
  if (std::filesystem::exists(xdg)) {
    return xdg;
  }
  const auto i3 = environment.home / ".i3/config";
  return std::filesystem::exists(i3) ? i3 : xdg;
}

static std::vector<std::filesystem::path> system_config_paths(
    const ExternalHotkeySetupEnvironment &environment,
    const char *application, const char *filename) {
  std::vector<std::filesystem::path> paths;
  for (const auto &directory : environment.config_dirs) {
    paths.push_back(directory / application / filename);
  }
  return paths;
}

static std::string sway_config(
    std::string contents, const std::vector<ExternalHotkeyCommand> &actions) {
  const auto begin = contents.find(sway_begin);
  const auto end = contents.find(sway_end);
  if ((begin == std::string::npos) != (end == std::string::npos) ||
      (begin != std::string::npos &&
       (end < begin || contents.find(sway_begin, begin + 1) != std::string::npos ||
        contents.find(sway_end, end + 1) != std::string::npos))) {
    throw std::runtime_error("The existing Sway setup block is incomplete");
  }
  if (begin != std::string::npos) {
    const auto after_end = contents.find('\n', end);
    contents.erase(begin, after_end == std::string::npos
                              ? contents.size() - begin : after_end + 1 - begin);
  }
  if (!contents.empty() && contents.back() != '\n') {
    contents += '\n';
  }
  contents += sway_begin;
  contents += '\n';
  for (const auto &action : actions) {
    contents += "bindsym " + sway_binding(action.binding) + " exec " +
                shell_command(action.arguments) + '\n';
  }
  contents += sway_end;
  contents += '\n';
  return contents;
}

static std::string labwc_config(
    const std::string &base,
    const std::vector<ExternalHotkeyCommand> &actions) {
  const std::string source = base.empty()
      ? "<?xml version=\"1.0\"?><labwc_config><keyboard><default/>"
        "</keyboard></labwc_config>"
      : base;
  if (source.size() > static_cast<std::size_t>(
                          std::numeric_limits<int>::max())) {
    throw std::runtime_error("The labwc rc.xml is too large");
  }
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document(
      xmlReadMemory(source.data(), static_cast<int>(source.size()),
                    "rc.xml", nullptr, XML_PARSE_NONET | XML_PARSE_NOBLANKS),
      xmlFreeDoc);
  xmlNode *root = document == nullptr ? nullptr : xmlDocGetRootElement(document.get());
  if (root == nullptr ||
      (!xmlStrEqual(root->name, BAD_CAST "labwc_config") &&
       !xmlStrEqual(root->name, BAD_CAST "openbox_config"))) {
    throw std::runtime_error("The labwc rc.xml is not valid labwc configuration");
  }
  xmlNode *keyboard = nullptr;
  for (xmlNode *child = root->children; child != nullptr; child = child->next) {
    if (child->type == XML_ELEMENT_NODE &&
        xmlStrEqual(child->name, BAD_CAST "keyboard")) {
      keyboard = child;
      break;
    }
  }
  if (keyboard == nullptr) {
    keyboard = xmlNewChild(root, nullptr, BAD_CAST "keyboard", nullptr);
  }
  if (keyboard == nullptr) {
    throw std::runtime_error("Cannot add labwc keyboard settings");
  }
  for (xmlNode *child = keyboard->children; child != nullptr;) {
    xmlNode *next = child->next;
    if (child->type == XML_COMMENT_NODE && child->content != nullptr &&
        xmlStrEqual(child->content, BAD_CAST labwc_marker)) {
      if (next == nullptr || next->type != XML_ELEMENT_NODE ||
          !xmlStrEqual(next->name, BAD_CAST "keybind")) {
        throw std::runtime_error("The existing labwc setup binding was changed");
      }
      xmlNode *after = next->next;
      xmlUnlinkNode(child);
      xmlFreeNode(child);
      xmlUnlinkNode(next);
      xmlFreeNode(next);
      child = after;
      continue;
    }
    child = next;
  }
  for (const auto &item : actions) {
    const auto binding = labwc_binding(item.binding);
    const auto command = shell_command(item.arguments);
    if (xmlAddChild(keyboard,
                    xmlNewDocComment(document.get(), BAD_CAST labwc_marker)) == nullptr) {
      throw std::runtime_error("Cannot mark labwc hotkey binding");
    }
    xmlNode *keybind = xmlNewChild(keyboard, nullptr, BAD_CAST "keybind", nullptr);
    xmlNode *action = keybind == nullptr
                          ? nullptr
                          : xmlNewChild(keybind, nullptr, BAD_CAST "action", nullptr);
    if (action == nullptr ||
        xmlNewProp(keybind, BAD_CAST "key", BAD_CAST binding.c_str()) == nullptr ||
        xmlNewProp(action, BAD_CAST "name", BAD_CAST "Execute") == nullptr ||
        xmlNewProp(action, BAD_CAST "command", BAD_CAST command.c_str()) == nullptr) {
      throw std::runtime_error("Cannot add labwc hotkey binding");
    }
  }
  xmlChar *memory = nullptr;
  int length = 0;
  xmlDocDumpFormatMemoryEnc(document.get(), &memory, &length, "UTF-8", 1);
  if (memory == nullptr || length <= 0) {
    throw std::runtime_error("Cannot serialize labwc configuration");
  }
  const std::string result(reinterpret_cast<const char *>(memory),
                           static_cast<std::size_t>(length));
  xmlFree(memory);
  return result;
}

ExternalHotkeySetupResult setup_external_hotkeys(
    const ExternalHotkeySetupEnvironment &environment,
    const std::vector<ExternalHotkeyCommand> &actions,
    const ExternalHotkeyCommandRunner &run) {
  if (!environment.wayland || !environment.config_home.is_absolute()) {
    return {false, "Run setup in a Wayland desktop session with a valid config directory"};
  }
  if (actions.empty()) {
    return {true, "No hotkeys are configured"};
  }
  try {
    if (!environment.sway_socket.empty() &&
        run({"swaymsg", "-t", "get_version"}).success) {
      const auto path = sway_user_config(environment);
      std::string base;
      if (std::filesystem::exists(path)) {
        base = read_file(path);
      } else {
        for (const auto &candidate : system_config_paths(
                 environment, "sway", "config")) {
          if (std::filesystem::is_regular_file(candidate)) {
            base = "include " + shell_quote(candidate.string()) + '\n';
            break;
          }
        }
      }
      write_config(path, sway_config(std::move(base), actions));
      if (!run({"swaymsg", "reload"}).success) {
        return {false, "Sway configuration was saved, but reload failed"};
      }
      const auto active = run({"swaymsg", "-t", "get_config"});
      if (!active.success) {
        return {false, "Sway did not report the active configuration"};
      }
      if (active.output.find(sway_begin) == std::string::npos) {
        // Sway reloads its original config path. When setup creates a user
        // config over a system config, activate the same bindings over IPC now.
        for (const auto &action : actions) {
          if (!run({"swaymsg", "bindsym", "--no-warn",
                    sway_binding(action.binding), "exec",
                    shell_command(action.arguments)})
                   .success) {
            return {false, "Sway saved the user configuration but rejected a live binding"};
          }
        }
        return {true, "Sway hotkeys activated; user configuration saved for sessions using the default config path"};
      }
      return {true, "Sway hotkeys configured and reloaded"};
    }
    if (!environment.labwc_pid.empty()) {
      const auto path = environment.config_home / "labwc/rc.xml";
      const auto base = read_base_config(
          path, system_config_paths(environment, "labwc", "rc.xml"));
      write_config(path, labwc_config(base, actions));
      if (!run({"labwc", "--reconfigure"}).success) {
        return {false, "labwc configuration was saved, but reload failed"};
      }
      return {true, "labwc hotkeys configured and reloaded"};
    }
    return {false, "No supported shortcut API or compositor configuration was found"};
  } catch (const std::exception &error) {
    return {false, error.what()};
  }
}

} // namespace elder_terms
