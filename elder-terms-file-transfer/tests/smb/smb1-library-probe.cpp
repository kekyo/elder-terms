// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

extern "C" {
#include <bdsm/bdsm.h>
}
#include <arpa/inet.h>
#include <algorithm>
#include <clocale>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

int main(int argc, char **argv) {
  try {
    require(argc == 3 || argc == 4, "Usage: smb1-library-probe <IPv4> <TCP|NBT> [names]");
    require(setenv("LC_ALL", "C.UTF-8", 1) == 0 && std::setlocale(LC_ALL, ""), "UTF-8 locale unavailable");
    const std::unique_ptr<smb_session, decltype(&smb_session_destroy)> session(
        smb_session_new(), smb_session_destroy);
    require(session != nullptr, "Session allocation failed");
    const auto check = [&](int status, const std::string &operation) {
      if (status != 0) throw std::runtime_error(operation + ": result=" + std::to_string(status) +
          " NT=" + std::to_string(smb_session_get_nt_status(session.get())));
      std::cout << "PASS " << operation << std::endl;
    };
    in_addr address{};
    require(inet_pton(AF_INET, argv[1], &address) == 1, "Invalid IPv4 address");
    check(smb_session_connect(session.get(), "ELDERTEST", address.s_addr,
        std::string(argv[2]) == "NBT" ? SMB_TRANSPORT_NBT : SMB_TRANSPORT_TCP), "connect");
    smb_session_set_creds(session.get(), "WORKGROUP", "elder", std::getenv("ELDER_SMB_PASSWORD"));
    check(smb_session_login(session.get()), "login");
    require(smb_session_is_guest(session.get()) == 0, "Unexpected guest authentication");
    smb_share_list shares = nullptr;
    size_t count = 0;
    check(smb_share_get_list(session.get(), &shares, &count), "share enumeration");
    for (size_t index = 0; index < count; ++index)
      std::cout << "SHARE " << smb_share_list_at(shares, index) << '\n';
    smb_share_list_destroy(shares);
    smb_tid tree{};
    if (argc == 4) {
      check(smb_tree_connect(session.get(), "names", &tree), "name test tree connect");
      const auto entries = smb_find(session.get(), tree, "\\*");
      std::vector<std::string> names;
      for (auto entry = entries; entry != nullptr; entry = smb_stat_list_next(entry)) {
        const auto *name = smb_stat_name(entry);
        require(name != nullptr, "Listing explicitly reports an unconvertible name");
        names.emplace_back(name);
        std::cout << "NAME " << name << '\n';
      }
      smb_stat_list_destroy(entries);
      const auto status = smb_session_get_nt_status(session.get());
      std::cout << "LIST count=" << names.size() << " NT=" << status << std::endl;
      for (const auto *name : {"before.txt", "絵文字😀.txt", "after.txt"})
        require(std::find(names.begin(), names.end(), name) != names.end(),
            "Directory listing silently omitted " + std::string(name));
      std::cout << "PASS SMB1 complete directory listing" << std::endl;
      return 0;
    }
    check(smb_tree_connect(session.get(), "probe", &tree), "tree connect");
    std::vector<char> data(150000);
    for (size_t index = 0; index < data.size(); ++index) data[index] = static_cast<char>(index % 251);
    for (const auto &path : {std::string("\\ascii.txt"), std::string("\\日本語.txt")}) {
      smb_fd file{};
      check(smb_fopen(session.get(), tree, path.c_str(), SMB_MOD_RW, &file), "create " + path);
      for (size_t offset = 0; offset < data.size();) {
        const auto written = smb_fwrite(session.get(), file, data.data() + offset, data.size() - offset);
        require(written > 0 && static_cast<size_t>(written) <= data.size() - offset, "Write made no progress");
        offset += static_cast<size_t>(written);
      }
      smb_fclose(session.get(), file);
      check(smb_fopen(session.get(), tree, path.c_str(), SMB_MOD_RO, &file), "reopen " + path);
      std::vector<char> received(data.size());
      for (size_t offset = 0; offset < received.size();) {
        const auto size = smb_fread(session.get(), file, received.data() + offset, received.size() - offset);
        require(size > 0 && static_cast<size_t>(size) <= received.size() - offset, "Read ended early");
        offset += static_cast<size_t>(size);
      }
      smb_fclose(session.get(), file);
      require(received == data, "Read-back content differs");
      const auto moved = path + ".moved";
      check(smb_file_mv(session.get(), tree, path.c_str(), moved.c_str()), "rename " + path);
      check(smb_file_rm(session.get(), tree, moved.c_str()), "delete " + moved);
    }
    check(smb_directory_create(session.get(), tree, "\\新しいフォルダー"), "mkdir");
    require(smb_directory_create(session.get(), tree, "\\新しいフォルダー") != 0,
        "Directory creation must reject an existing directory");
    check(smb_directory_rm(session.get(), tree, "\\新しいフォルダー"), "rmdir");
    std::cout << "PASS SMB1 public file operations" << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << std::endl;
    return 1;
  }
}
