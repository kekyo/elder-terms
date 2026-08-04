#include <elder-terms/serial-device.h>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace elder_terms_serial_device_test {

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static std::filesystem::path temporary_directory() {
  const auto timestamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("elder-terms-serial-device-" + std::to_string(getpid()) + "-" +
          std::to_string(timestamp));
}

static void write_file(const std::filesystem::path &path,
                       const std::string &content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  expect_true(output.good(), "failed to create serial metadata fixture");
  output << content;
  expect_true(output.good(), "failed to write serial metadata fixture");
}

struct SerialFixture {
  std::filesystem::path root = temporary_directory();
  elder_terms::SerialDevicePaths paths{
      .dev_root = root / "dev",
      .by_id_root = root / "by-id",
      .by_path_root = root / "by-path",
      .sys_class_tty_root = root / "sys-class-tty",
  };
  int master_fd = -1;
  std::filesystem::path actual_node;
  std::filesystem::path exact_target;
  std::filesystem::path by_id_target;
  std::filesystem::path by_path_target;

  SerialFixture() {
    std::filesystem::create_directories(paths.dev_root);
    std::filesystem::create_directories(paths.by_id_root);
    std::filesystem::create_directories(paths.by_path_root);
    std::filesystem::create_directories(paths.sys_class_tty_root);

    master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    expect_true(master_fd >= 0, "failed to create serial PTY master");
    expect_true(grantpt(master_fd) == 0, "failed to grant serial PTY");
    expect_true(unlockpt(master_fd) == 0, "failed to unlock serial PTY");
    char *slave_name = ptsname(master_fd);
    expect_true(slave_name != nullptr, "failed to resolve serial PTY slave");
    actual_node = slave_name;

    exact_target = paths.dev_root / "ttyUSB0";
    by_id_target = paths.by_id_root / "usb-Elder_Terms_Demo-A";
    by_path_target =
        paths.by_path_root / "pci-0000:00:14.0-usb-0:1.2:1.0-port0";
    std::filesystem::create_symlink(actual_node, exact_target);
    std::filesystem::create_symlink(exact_target, by_id_target);
    std::filesystem::create_symlink(exact_target, by_path_target);

    const std::filesystem::path metadata_root =
        paths.sys_class_tty_root / actual_node.filename() / "device";
    write_file(metadata_root / "product", "Elder Terms Demo\n");
    write_file(metadata_root / "serial", "FT12345678901234\n");
  }

  ~SerialFixture() {
    if (master_fd >= 0) {
      close(master_fd);
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
};

static void test_lists_all_identification_modes() {
  SerialFixture fixture;

  const auto exact = elder_terms::list_serial_device_choices(
      elder_terms::SerialDeviceMatchMode::exact_path, fixture.paths);
  expect_true(exact.size() == 1, "exact-path enumeration should find one PTY");
  expect_true(exact.front().target_path == fixture.exact_target.string(),
              "exact-path enumeration returned the wrong target");
  expect_true(exact.front().display_label == fixture.exact_target.string(),
              "exact-path label should be the complete device path");

  const auto stable = elder_terms::list_serial_device_choices(
      elder_terms::SerialDeviceMatchMode::stable_id, fixture.paths);
  expect_true(stable.size() == 1, "stable-id enumeration should find one link");
  expect_true(stable.front().target_path == fixture.by_id_target.string(),
              "stable-id enumeration returned the wrong target");
  expect_true(stable.front().display_label ==
                  "Elder Terms Demo [SN:FT12...1234]",
              "stable-id label should include product and abbreviated serial");
  expect_true(stable.front().usb_serial ==
                  std::optional<std::string>("FT12345678901234"),
              "stable-id choice should expose the USB serial number");

  const auto physical = elder_terms::list_serial_device_choices(
      elder_terms::SerialDeviceMatchMode::physical_port, fixture.paths);
  expect_true(physical.size() == 1,
              "physical-port enumeration should find one link");
  expect_true(physical.front().target_path == fixture.by_path_target.string(),
              "physical-port enumeration returned the wrong target");
  expect_true(physical.front().display_label ==
                  fixture.by_path_target.filename().string(),
              "physical-port label should be the persistent link name");
}

static void test_maps_one_device_between_identification_modes() {
  SerialFixture fixture;

  expect_true(
      elder_terms::resolve_serial_device_target_for_mode(
          elder_terms::SerialDeviceMatchMode::stable_id,
          fixture.exact_target.string(), fixture.paths) ==
          std::optional<std::string>(fixture.by_id_target.string()),
      "exact path should map to the stable-id target for the same device");
  expect_true(
      elder_terms::resolve_serial_device_target_for_mode(
          elder_terms::SerialDeviceMatchMode::physical_port,
          fixture.by_id_target.string(), fixture.paths) ==
          std::optional<std::string>(fixture.by_path_target.string()),
      "stable-id target should map to the physical port for the same device");
  expect_true(
      elder_terms::resolve_serial_device_target_for_mode(
          elder_terms::SerialDeviceMatchMode::exact_path,
          fixture.by_path_target.string(), fixture.paths) ==
          std::optional<std::string>(fixture.exact_target.string()),
      "physical-port target should map to the exact path for the same device");

  const std::filesystem::path absent =
      fixture.paths.by_id_root / "usb-Elder_Terms_Absent";
  expect_true(
      elder_terms::resolve_serial_device_target_for_mode(
          elder_terms::SerialDeviceMatchMode::stable_id, absent.string(),
          fixture.paths) == std::optional<std::string>(absent.string()),
      "an absent target already in the selected mode should be retained");
}

static void test_resolves_renamed_stable_id_by_usb_serial() {
  SerialFixture fixture;
  const std::filesystem::path old_target =
      fixture.paths.by_id_root / "usb-Elder_Terms_Old_Name";

  const auto result = elder_terms::resolve_serial_device(
      elder_terms::SerialDeviceMatchMode::stable_id, old_target.string(),
      std::optional<std::string>("FT12345678901234"), fixture.paths);

  expect_true(result.resolved,
              "USB serial should recover a renamed stable-id target");
  expect_true(result.path == fixture.by_id_target,
              "USB serial should resolve the current stable-id link");
}

static void test_rejects_ambiguous_usb_serial_matches() {
  SerialFixture fixture;
  const std::filesystem::path duplicate =
      fixture.paths.by_id_root / "usb-Elder_Terms_Demo-B";
  std::filesystem::create_symlink(fixture.exact_target, duplicate);
  const std::filesystem::path old_target =
      fixture.paths.by_id_root / "usb-Elder_Terms_Old_Name";

  const auto result = elder_terms::resolve_serial_device(
      elder_terms::SerialDeviceMatchMode::stable_id, old_target.string(),
      std::optional<std::string>("FT12345678901234"), fixture.paths);

  expect_true(!result.resolved,
              "ambiguous USB serial matches must not select arbitrarily");
  expect_true(!result.warnings.empty(),
              "ambiguous USB serial matches should produce a warning");
}

static void test_serializes_match_modes() {
  expect_true(elder_terms::serial_device_match_mode_to_string(
                  elder_terms::SerialDeviceMatchMode::exact_path) == "path",
              "exact-path mode should serialize as path");
  expect_true(elder_terms::parse_serial_device_match_mode("by-id") ==
                  std::optional<elder_terms::SerialDeviceMatchMode>(
                      elder_terms::SerialDeviceMatchMode::stable_id),
              "by-id should parse as stable-id mode");
  expect_true(elder_terms::parse_serial_device_match_mode("by-path") ==
                  std::optional<elder_terms::SerialDeviceMatchMode>(
                      elder_terms::SerialDeviceMatchMode::physical_port),
              "by-path should parse as physical-port mode");
  expect_true(!elder_terms::parse_serial_device_match_mode("unknown")
                   .has_value(),
              "unknown serial match modes should be rejected");
}

} // namespace elder_terms_serial_device_test

int main() {
  try {
    elder_terms_serial_device_test::test_lists_all_identification_modes();
    elder_terms_serial_device_test::
        test_maps_one_device_between_identification_modes();
    elder_terms_serial_device_test::
        test_resolves_renamed_stable_id_by_usb_serial();
    elder_terms_serial_device_test::
        test_rejects_ambiguous_usb_serial_matches();
    elder_terms_serial_device_test::test_serializes_match_modes();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
