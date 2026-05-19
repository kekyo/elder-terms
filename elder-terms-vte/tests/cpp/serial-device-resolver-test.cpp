#include "../../src/terminal-sessions/serial-session/serial-device-resolver.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace elder_terms_serial_device_resolver_test {

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static std::filesystem::path temporary_directory() {
  const auto timestamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("elder-terms-serial-resolver-" + std::to_string(timestamp));
}

static void write_file(const std::filesystem::path &path,
                       const std::string &content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  expect_true(file.good(), "failed to create test file");
  file << content;
  expect_true(file.good(), "failed to write test file");
}

static void create_tty_symlink(const std::filesystem::path &path,
                               const std::filesystem::path &target =
                                   "/dev/null") {
  std::filesystem::create_directories(path.parent_path());
  std::filesystem::create_symlink(target, path);
}

static elder_terms::SerialDeviceResolverOptions
resolver_options(const std::filesystem::path &root) {
  return {
      .dev_root = root / "dev",
      .sysfs_root = root / "sys",
  };
}

static void test_resolves_by_path_exact_path() {
  const std::filesystem::path root = temporary_directory();
  const elder_terms::SerialDeviceResolverOptions options =
      resolver_options(root);
  const std::filesystem::path device_path =
      options.dev_root / "serial" / "by-path" / "pci-usb-serial";
  create_tty_symlink(device_path);

  const elder_terms::SerialDeviceResolveResult result =
      elder_terms::resolve_serial_device(device_path.string(), options);
  std::filesystem::remove_all(root);

  expect_true(result.resolved, "by-path device should resolve");
  expect_true(result.path == device_path,
              "by-path device should return the configured path");
}

static void test_resolves_by_id_basename() {
  const std::filesystem::path root = temporary_directory();
  const elder_terms::SerialDeviceResolverOptions options =
      resolver_options(root);
  const std::filesystem::path device_path =
      options.dev_root / "serial" / "by-id" / "usb-serial-A";
  create_tty_symlink(device_path);

  const elder_terms::SerialDeviceResolveResult result =
      elder_terms::resolve_serial_device("usb-serial-A", options);
  std::filesystem::remove_all(root);

  expect_true(result.resolved, "by-id basename should resolve");
  expect_true(result.path == device_path,
              "by-id basename should return the by-id path");
}

static void test_resolves_sysfs_serial_identifier() {
  const std::filesystem::path root = temporary_directory();
  const elder_terms::SerialDeviceResolverOptions options =
      resolver_options(root);
  const std::filesystem::path device_path = options.dev_root / "ttyUSB0";
  create_tty_symlink(device_path);
  write_file(options.sysfs_root / "class" / "tty" / "ttyUSB0" / "device" /
                 "uevent",
             "ID_SERIAL=vendor_model_full\nID_SERIAL_SHORT=short-serial\n");

  const elder_terms::SerialDeviceResolveResult result =
      elder_terms::resolve_serial_device("short-serial", options);
  std::filesystem::remove_all(root);

  expect_true(result.resolved, "sysfs serial identifier should resolve");
  expect_true(result.path == device_path,
              "sysfs serial identifier should return the tty path");
}

static void test_resolves_absolute_tty_character_device_path() {
  const std::filesystem::path root = temporary_directory();
  const elder_terms::SerialDeviceResolverOptions options =
      resolver_options(root);
  const std::filesystem::path device_path = options.dev_root / "ttyUSB1";
  create_tty_symlink(device_path);

  const elder_terms::SerialDeviceResolveResult result =
      elder_terms::resolve_serial_device(device_path.string(), options);
  std::filesystem::remove_all(root);

  expect_true(result.resolved, "absolute tty character device should resolve");
  expect_true(result.path == device_path,
              "absolute tty character device should return the input path");
}

static void test_rejects_multiple_identifier_matches() {
  const std::filesystem::path root = temporary_directory();
  const elder_terms::SerialDeviceResolverOptions options =
      resolver_options(root);
  create_tty_symlink(options.dev_root / "ttyUSB2");
  create_tty_symlink(options.dev_root / "ttyUSB3", "/dev/zero");
  write_file(options.sysfs_root / "class" / "tty" / "ttyUSB2" / "device" /
                 "uevent",
             "ID_SERIAL_SHORT=duplicate\n");
  write_file(options.sysfs_root / "class" / "tty" / "ttyUSB3" / "device" /
                 "uevent",
             "ID_SERIAL_SHORT=duplicate\n");

  const elder_terms::SerialDeviceResolveResult result =
      elder_terms::resolve_serial_device("duplicate", options);
  std::filesystem::remove_all(root);

  expect_true(!result.resolved, "multiple identifier matches should not resolve");
  expect_true(!result.warnings.empty(),
              "multiple identifier matches should emit a warning");
}

static void test_rejects_missing_device() {
  const std::filesystem::path root = temporary_directory();
  const elder_terms::SerialDeviceResolverOptions options =
      resolver_options(root);
  std::filesystem::create_directories(options.dev_root);

  const elder_terms::SerialDeviceResolveResult result =
      elder_terms::resolve_serial_device("missing", options);
  std::filesystem::remove_all(root);

  expect_true(!result.resolved, "missing device should not resolve");
  expect_true(!result.warnings.empty(), "missing device should emit a warning");
}

} // namespace elder_terms_serial_device_resolver_test

int main() {
  try {
    elder_terms_serial_device_resolver_test::test_resolves_by_path_exact_path();
    elder_terms_serial_device_resolver_test::test_resolves_by_id_basename();
    elder_terms_serial_device_resolver_test::
        test_resolves_sysfs_serial_identifier();
    elder_terms_serial_device_resolver_test::
        test_resolves_absolute_tty_character_device_path();
    elder_terms_serial_device_resolver_test::
        test_rejects_multiple_identifier_matches();
    elder_terms_serial_device_resolver_test::test_rejects_missing_device();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
