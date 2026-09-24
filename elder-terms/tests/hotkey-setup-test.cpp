#include "hotkey-setup.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <gdk/gdkkeysyms.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <unistd.h>

static bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

static int count_text(const std::string &source, const std::string &needle) {
  int count = 0;
  for (std::size_t position = source.find(needle);
       position != std::string::npos;
       position = source.find(needle, position + needle.size())) {
    ++count;
  }
  return count;
}

int main() {
  char template_path[] = "/tmp/elder-hotkey-setup-XXXXXX";
  const char *directory = mkdtemp(template_path);
  if (directory == nullptr) {
    return 1;
  }
  const std::filesystem::path root(directory);
  const auto config_home = root / "user";
  const auto config_system = root / "system";
  const std::vector<elder_terms::ExternalHotkeyCommand> actions = {
      {
          .binding = {.keyval = GDK_KEY_F2,
                      .modifiers = GDK_CONTROL_MASK},
          .arguments = {"etctl", "open-application"},
      },
      {
          .binding = {.keyval = GDK_KEY_y,
                      .modifiers = static_cast<GdkModifierType>(
                          GDK_CONTROL_MASK | GDK_SHIFT_MASK)},
          .arguments = {"etctl", "open-connection", "Alice's $(touch unsafe)"},
      },
  };
  std::vector<std::vector<std::string>> commands;
  const auto run = [&commands, &config_home](const std::vector<std::string> &command) {
    commands.push_back(command);
    return elder_terms::ExternalHotkeyCommandResult{
        true,
        command == std::vector<std::string>({"swaymsg", "-t", "get_config"})
            ? read_file(config_home / "sway/config") : "",
    };
  };
  bool success = true;
  try {
    std::filesystem::create_directories(config_system / "sway");
    std::ofstream(config_system / "sway/config")
        << "bindsym Mod4+Return exec terminal\n";
    const elder_terms::ExternalHotkeySetupEnvironment sway{
        .wayland = true,
        .config_home = config_home,
        .home = root / "home",
        .config_dirs = {config_system},
        .sway_socket = "session-sway.sock",
        .labwc_pid = "",
    };
    const auto first = elder_terms::setup_external_hotkeys(sway, actions, run);
    const auto second = elder_terms::setup_external_hotkeys(sway, actions, run);
    const auto sway_config = read_file(config_home / "sway/config");
    success &= expect(first.success && second.success,
                      "Sway setup should succeed and be repeatable");
    success &= expect(sway_config.find("include '" +
                      (config_system / "sway/config").string() + "'") !=
                          std::string::npos,
                      "Sway setup should retain the effective system config");
    success &= expect(count_text(sway_config, "bindsym Ctrl+F2") == 1 &&
                          count_text(sway_config, "bindsym Ctrl+Shift+y") == 1,
                      "Sway setup should persist each binding once");
    success &= expect(sway_config.find("'Alice'\\''s $(touch unsafe)'") !=
                          std::string::npos,
                      "Sway setup should quote saved connection names");
    success &= expect(commands.size() == 6 &&
                          commands[0] == std::vector<std::string>({"swaymsg", "-t", "get_version"}) &&
                          commands[1] == std::vector<std::string>({"swaymsg", "reload"}) &&
                          commands[2] == std::vector<std::string>({"swaymsg", "-t", "get_config"}),
                      "Sway setup should verify IPC and reload the compositor");

    commands.clear();
    const auto run_original_config = [&commands](
                                         const std::vector<std::string> &command) {
      commands.push_back(command);
      return elder_terms::ExternalHotkeyCommandResult{
          true, command == std::vector<std::string>({"swaymsg", "-t", "get_config"})
                    ? "bindsym Mod4+Return exec terminal" : "",
      };
    };
    const auto first_session = elder_terms::setup_external_hotkeys(
        sway, actions, run_original_config);
    success &= expect(first_session.success && commands.size() == 5 &&
                          commands[3][0] == "swaymsg" &&
                          commands[3][1] == "bindsym" &&
                          commands[4][1] == "bindsym",
                      "Sway setup should activate bindings when reload keeps the old system config");

    commands.clear();
    std::filesystem::create_directories(config_system / "labwc");
    std::ofstream(config_system / "labwc/rc.xml")
        << "<?xml version=\"1.0\"?><labwc_config><keyboard>"
           "<keybind key=\"W-Return\"><action name=\"Execute\" "
           "command=\"terminal\"/></keybind></keyboard></labwc_config>";
    const elder_terms::ExternalHotkeySetupEnvironment labwc{
        .wayland = true,
        .config_home = config_home,
        .home = root / "home",
        .config_dirs = {config_system},
        .sway_socket = "",
        .labwc_pid = "12345",
    };
    const auto labwc_first =
        elder_terms::setup_external_hotkeys(labwc, actions, run);
    const auto labwc_second =
        elder_terms::setup_external_hotkeys(labwc, actions, run);
    const auto labwc_path = config_home / "labwc/rc.xml";
    const auto labwc_config = read_file(labwc_path);
    xmlDoc *document = xmlReadFile(labwc_path.c_str(), nullptr, XML_PARSE_NONET);
    success &= expect(labwc_first.success && labwc_second.success &&
                          document != nullptr,
                      "labwc setup should produce valid XML and be repeatable");
    success &= expect(count_text(labwc_config, "key=\"W-Return\"") == 1 &&
                          count_text(labwc_config, "key=\"C-F2\"") == 1 &&
                          count_text(labwc_config, "key=\"C-S-y\"") == 1,
                      "labwc setup should retain existing bindings without duplication");
    success &= expect(commands.size() == 2 &&
                          commands[0] == std::vector<std::string>({"labwc", "--reconfigure"}),
                      "labwc setup should reload the active compositor");
    if (document != nullptr) {
      xmlFreeDoc(document);
    }

    const auto openbox_home = root / "openbox-user";
    const auto openbox_system = root / "openbox-system";
    std::filesystem::create_directories(openbox_system / "labwc");
    std::ofstream(openbox_system / "labwc/rc.xml")
        << "<?xml version=\"1.0\"?><openbox_config "
           "xmlns=\"http://openbox.org/3.4/rc\"><keyboard>"
           "<keybind key=\"W-Return\"><action name=\"Execute\" "
           "command=\"terminal\"/></keybind></keyboard></openbox_config>";
    const elder_terms::ExternalHotkeySetupEnvironment openbox{
        .wayland = true,
        .config_home = openbox_home,
        .home = root / "home",
        .config_dirs = {openbox_system},
        .sway_socket = "",
        .labwc_pid = "12345",
    };
    const auto openbox_result =
        elder_terms::setup_external_hotkeys(openbox, actions, run);
    const auto openbox_path = openbox_home / "labwc/rc.xml";
    const auto openbox_contents =
        std::filesystem::exists(openbox_path) ? read_file(openbox_path) : "";
    success &= expect(openbox_result.success &&
                          openbox_contents.find("<openbox_config") !=
                              std::string::npos &&
                          count_text(openbox_contents, "key=\"C-F2\"") == 1,
                      "labwc setup should accept Openbox-style system configuration");

    commands.clear();
    const elder_terms::ExternalHotkeySetupEnvironment unsupported{
        .wayland = true,
        .config_home = config_home,
        .home = root / "home",
        .config_dirs = {config_system},
        .sway_socket = "",
        .labwc_pid = "",
    };
    const auto unavailable =
        elder_terms::setup_external_hotkeys(unsupported, actions, run);
    success &= expect(!unavailable.success && commands.empty(),
                      "Unknown Wayland compositors should be reported without changes");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    success = false;
  }
  std::filesystem::remove_all(root);
  return success ? 0 : 1;
}
