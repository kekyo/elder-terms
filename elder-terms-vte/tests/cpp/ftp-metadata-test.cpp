#include "ftp-metadata.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace elder_terms {

static void expect(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Operation>
static void expect_failure(Operation operation, const char *message) {
  try {
    operation();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error(message);
}

static void pwd_response_unescapes_embedded_quotes() {
  expect(parse_ftp_pwd_path(
             "257 \"/reports/\"\"quoted\"\"\" is current") ==
             "/reports/\"quoted\"",
         "PWD should decode doubled quote characters");
  expect_failure(
      []() {
        (void)parse_ftp_pwd_path(
            "257 current directory unavailable");
      },
      "PWD reply without a quoted pathname should fail");
}

static void mlsd_entries_preserve_machine_readable_facts() {
  const std::optional<FtpDirectoryEntry> file = parse_ftp_mlsd_entry(
      "Type=file;Size=42;Modify=19700101000000; report final.txt");
  expect(file.has_value() && file->name == "report final.txt" &&
             file->type == FtpDirectoryEntryType::regular &&
             file->size == 42 &&
             file->modification_time_unix_seconds == 0,
         "MLSD should parse case-insensitive standard facts");

  const std::optional<FtpDirectoryEntry> directory =
      parse_ftp_mlsd_entry("type=dir;modify=20240229010203; archive");
  expect(directory.has_value() &&
             directory->type == FtpDirectoryEntryType::directory &&
             directory->modification_time_unix_seconds.has_value(),
         "MLSD should parse a valid leap-day timestamp");

  const std::optional<FtpDirectoryEntry> current =
      parse_ftp_mlsd_entry("type=cdir; /");
  const std::optional<FtpDirectoryEntry> parent =
      parse_ftp_mlsd_entry("type=pdir; ..");
  expect(current.has_value() &&
             current->type == FtpDirectoryEntryType::current_directory &&
             parent.has_value() &&
             parent->type == FtpDirectoryEntryType::parent_directory,
         "MLSD should distinguish current and parent pseudo entries");

  const std::optional<FtpDirectoryEntry> invalid_time =
      parse_ftp_mlsd_entry("type=file;size=7;modify=20230229010203; item");
  expect(invalid_time.has_value() &&
             !invalid_time->modification_time_unix_seconds.has_value(),
         "invalid MLSD timestamps should remain absent");
}

static void common_list_formats_are_available_as_a_fallback() {
  const std::optional<FtpDirectoryEntry> unix_file =
      parse_ftp_list_entry(
          "-rw-r--r-- 1 alice staff 123 Jan 02 2024 report final.txt");
  const std::optional<FtpDirectoryEntry> unix_directory =
      parse_ftp_list_entry(
          "drwxr-xr-x 2 alice staff 4096 Feb 29 12:34 archive");
  const std::optional<FtpDirectoryEntry> dos_directory =
      parse_ftp_list_entry("02-29-24  12:34PM       <DIR>          uploads");
  const std::optional<FtpDirectoryEntry> dos_file =
      parse_ftp_list_entry("02-29-24  12:35PM                 987 note.txt");

  expect(unix_file.has_value() && unix_file->name == "report final.txt" &&
             unix_file->type == FtpDirectoryEntryType::regular &&
             unix_file->size == 123,
         "UNIX LIST regular file should be parsed");
  expect(unix_directory.has_value() &&
             unix_directory->type == FtpDirectoryEntryType::directory,
         "UNIX LIST directory should be parsed");
  expect(dos_directory.has_value() &&
             dos_directory->type == FtpDirectoryEntryType::directory,
         "DOS LIST directory should be parsed");
  expect(dos_file.has_value() && dos_file->size == 987,
         "DOS LIST regular file should be parsed");
  expect(!parse_ftp_list_entry("unrecognized listing").has_value(),
         "unknown LIST format should be ignored");
}

static void command_arguments_reject_control_line_injection() {
  expect(ftp_command_argument_is_safe("/reports/final.txt"),
         "ordinary FTP pathname should be safe");
  expect(!ftp_command_argument_is_safe("alice\r\nDELE /important"),
         "FTP command arguments must reject CRLF injection");
  expect(!ftp_command_argument_is_safe(std::string("a\0b", 3)),
         "FTP command arguments must reject NUL bytes");
}

} // namespace elder_terms

int main() {
  try {
    elder_terms::pwd_response_unescapes_embedded_quotes();
    elder_terms::mlsd_entries_preserve_machine_readable_facts();
    elder_terms::common_list_formats_are_available_as_a_fallback();
    elder_terms::command_arguments_reject_control_line_injection();
  } catch (const std::exception &exception) {
    std::cerr << "ftp-metadata-test: FAIL: " << exception.what() << '\n';
    return 1;
  }
  std::cout << "ftp-metadata-test: PASS\n";
  return 0;
}
