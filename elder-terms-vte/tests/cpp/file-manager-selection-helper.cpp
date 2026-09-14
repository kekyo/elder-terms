#include <atspi/atspi.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using Accessible = std::unique_ptr<AtspiAccessible, decltype(&g_object_unref)>;
using StateSet = std::unique_ptr<AtspiStateSet, decltype(&g_object_unref)>;
using Text = std::unique_ptr<gchar, decltype(&g_free)>;

static void check_error(GError *error) {
  if (!error) return;
  const std::string message(error->message);
  g_error_free(error);
  throw std::runtime_error(message);
}

static void inspect_selection(AtspiAccessible *item, bool selected_parent, int depth) {
  GError *error = nullptr;
  const auto role = atspi_accessible_get_role(item, &error);
  check_error(error);
  StateSet states(atspi_accessible_get_state_set(item), g_object_unref);
  if (!states) throw std::runtime_error("Missing accessible state set");
  const bool selected = atspi_state_set_contains(states.get(), ATSPI_STATE_SELECTED);
  // A selected grid cell owns its filename label. Selected tabs or sidebar
  // entries must not make unrelated descendants count as selected files.
  const bool selected_file = selected_parent || (role == ATSPI_ROLE_TABLE_CELL && selected);
  Text name(atspi_accessible_get_name(item, &error), g_free);
  check_error(error);
  std::cerr << std::string(depth, ' ') << static_cast<int>(role) << '\t'
            << selected << '\t' << (name ? name.get() : "") << '\n';
  if (selected_file && name && *name) std::cout << name.get() << '\n';
  const int count = atspi_accessible_get_child_count(item, &error);
  check_error(error);
  for (int index = 0; index < count; ++index) {
    Accessible child(atspi_accessible_get_child_at_index(item, index, &error), g_object_unref);
    check_error(error);
    if (child) inspect_selection(child.get(), selected_file, depth + 1);
  }
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  if (atspi_init() != 0) return 3;
  int status = 1;
  try {
    const auto process_id = std::stoul(argv[1]);
    Accessible desktop(atspi_get_desktop(0), g_object_unref);
    if (!desktop) throw std::runtime_error("Missing accessibility desktop");
    GError *error = nullptr;
    const int count = atspi_accessible_get_child_count(desktop.get(), &error);
    check_error(error);
    for (int index = 0; index < count; ++index) {
      Accessible app(atspi_accessible_get_child_at_index(desktop.get(), index, &error), g_object_unref);
      check_error(error);
      if (!app) continue;
      const auto actual_process_id = atspi_accessible_get_process_id(app.get(), &error);
      check_error(error);
      if (actual_process_id == process_id) {
        inspect_selection(app.get(), false, 0);
        status = 0;
        break;
      }
    }
    if (status) std::cerr << "File manager missing from accessibility desktop\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  atspi_exit();
  return status;
}
