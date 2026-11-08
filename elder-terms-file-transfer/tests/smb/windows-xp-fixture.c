// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <lm.h>
#include <sddl.h>
#include <aclapi.h>
#include <stdio.h>
#include <wchar.h>

static const wchar_t *lsa_path = L"SYSTEM\\CurrentControlSet\\Control\\Lsa";
static const wchar_t *prefixes[] = {L"ep", L"ee", L"en", L"日本語 ", L"ed", L"er"};
static const wchar_t *directories[] = {L"share", L"empty", L"names", L"empty", L"share", L"share"};

static void check(DWORD status, const char *operation) {
  if (status != ERROR_SUCCESS) {
    fprintf(stderr, "%s failed: %lu\n", operation, (unsigned long)status);
    ExitProcess(1);
  }
}

static void require(BOOL condition, const char *operation) {
  if (!condition) check(ERROR_INVALID_DATA, operation);
}

static void join(wchar_t *result, const wchar_t *root, const wchar_t *name) {
  require(swprintf(result, MAX_PATH, L"%ls\\%ls", root, name) > 0, "Join fixture path");
}

static DWORD force_guest(BOOL *exists) {
  HKEY key;
  check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, lsa_path, 0, KEY_QUERY_VALUE, &key), "Open LSA");
  DWORD value = 0, type = 0, size = sizeof(value);
  DWORD status = RegQueryValueExW(key, L"forceguest", NULL, &type, (BYTE *)&value, &size);
  RegCloseKey(key);
  *exists = status != ERROR_FILE_NOT_FOUND;
  if (*exists) {
    check(status, "Read forceguest");
    require(type == REG_DWORD && size == sizeof(value), "forceguest type");
  }
  return value;
}

static void set_force_guest(BOOL exists, DWORD value) {
  HKEY key;
  check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, lsa_path, 0, KEY_SET_VALUE, &key), "Open LSA for restoration");
  DWORD status = exists
    ? RegSetValueExW(key, L"forceguest", 0, REG_DWORD, (const BYTE *)&value, sizeof(value))
    : RegDeleteValueW(key, L"forceguest");
  RegCloseKey(key);
  if (!exists && status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
  check(status, "Set forceguest");
}

static PSECURITY_DESCRIPTOR descriptor(const wchar_t *sddl) {
  PSECURITY_DESCRIPTOR result = NULL;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &result, NULL))
    check(GetLastError(), "Parse fixture permissions");
  return result;
}

static void setup(const wchar_t *root, const wchar_t *id, const wchar_t *user, const wchar_t *marker) {
  // Refuse collisions before creating the state file that authorizes cleanup.
  USER_INFO_1 *existing = NULL;
  DWORD status = NetUserGetInfo(NULL, user, 1, (BYTE **)&existing);
  if (existing) NetApiBufferFree(existing);
  require(status == NERR_UserNotFound, "Fixture user already exists");
  for (unsigned index = 0; index < 6; ++index) {
    wchar_t name[80];
    swprintf(name, 80, L"%ls%ls", prefixes[index], id);
    SHARE_INFO_2 *share = NULL;
    status = NetShareGetInfo(NULL, name, 2, (BYTE **)&share);
    if (share) NetApiBufferFree(share);
    require(status == NERR_NetNameNotFound, "Fixture share already exists");
  }
  BOOL existed;
  DWORD saved[2];
  saved[1] = force_guest(&existed);
  saved[0] = existed;
  wchar_t state_path[MAX_PATH];
  join(state_path, root, L"forceguest.bin");
  HANDLE state = CreateFileW(state_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  if (state == INVALID_HANDLE_VALUE) check(GetLastError(), "Create restoration state");
  DWORD written;
  require(WriteFile(state, saved, sizeof(saved), &written, NULL) && written == sizeof(saved), "Save restoration state");
  require(FlushFileBuffers(state), "Flush restoration state");
  CloseHandle(state);

  wchar_t password[256];
  DWORD length = GetEnvironmentVariableW(L"ELDER_SMB_PASSWORD", password, 256);
  require(length > 0 && length < 256, "Test password is missing");
  USER_INFO_1 account = {0};
  account.usri1_name = (wchar_t *)user;
  account.usri1_password = password;
  account.usri1_priv = USER_PRIV_USER;
  account.usri1_comment = (wchar_t *)marker;
  account.usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT | UF_DONT_EXPIRE_PASSWD;
  check(NetUserAdd(NULL, 1, (BYTE *)&account, NULL), "Create fixture user");
  SecureZeroMemory(password, sizeof(password));

  USER_INFO_4 *info = NULL;
  check(NetUserGetInfo(NULL, user, 4, (BYTE **)&info), "Read fixture user SID");
  wchar_t *sid = NULL;
  require(ConvertSidToStringSidW(info->usri4_user_sid, &sid), "Format fixture SID");
  NetApiBufferFree(info);
  wchar_t sddl[512];
  swprintf(sddl, 512, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;%ls)", sid);
  PSECURITY_DESCRIPTOR directory_sd = descriptor(sddl);
  PACL acl = NULL;
  BOOL present, defaulted;
  require(GetSecurityDescriptorDacl(directory_sd, &present, &acl, &defaulted) && present, "Read fixture DACL");
  for (unsigned index = 0; index < 3; ++index) {
    wchar_t path[MAX_PATH];
    join(path, root, directories[index]);
    require(CreateDirectoryW(path, NULL), "Create fixture directory");
    check(SetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      NULL, NULL, acl, NULL), "Grant fixture directory access");
  }
  LocalFree(directory_sd);
  for (unsigned index = 0; index < 6; ++index) {
    wchar_t name[80], path[MAX_PATH];
    swprintf(name, 80, L"%ls%ls", prefixes[index], id);
    join(path, root, directories[index]);
    swprintf(sddl, 512, L"D:(A;;GA;;;BA)(A;;%ls;;;%ls)", index == 5 ? L"GRGX" : L"GA", sid);
    PSECURITY_DESCRIPTOR share_sd = descriptor(index == 4 ? L"D:(A;;GA;;;BA)" : sddl);
    SHARE_INFO_502 share = {0};
    share.shi502_netname = name;
    share.shi502_type = STYPE_DISKTREE;
    share.shi502_remark = (wchar_t *)marker;
    share.shi502_max_uses = (DWORD)-1;
    share.shi502_path = path;
    share.shi502_security_descriptor = share_sd;
    status = NetShareAdd(NULL, 502, (BYTE *)&share, NULL);
    LocalFree(share_sd);
    check(status, "Create fixture share");
  }
  LocalFree(sid);
  set_force_guest(TRUE, 0);
  printf("{\"setup\":true,\"previousForceGuest\":%lu}\n", (unsigned long)saved[1]);
}

static void cleanup(const wchar_t *root, const wchar_t *id, const wchar_t *user, const wchar_t *marker) {
  wchar_t state_path[MAX_PATH];
  join(state_path, root, L"forceguest.bin");
  HANDLE state = CreateFileW(state_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (state == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND) {
    puts("{\"cleaned\":true}");
    return;
  }
  if (state == INVALID_HANDLE_VALUE) check(GetLastError(), "Open restoration state");
  DWORD saved[2], count;
  require(ReadFile(state, saved, sizeof(saved), &count, NULL) && count == sizeof(saved), "Read restoration state");
  CloseHandle(state);
  // Restore the machine policy even if subsequent fixture deletion fails.
  set_force_guest(saved[0] != 0, saved[1]);
  for (unsigned index = 0; index < 6; ++index) {
    wchar_t name[80], path[MAX_PATH];
    swprintf(name, 80, L"%ls%ls", prefixes[index], id);
    join(path, root, directories[index]);
    SHARE_INFO_2 *share = NULL;
    DWORD status = NetShareGetInfo(NULL, name, 2, (BYTE **)&share);
    if (status == NERR_NetNameNotFound) continue;
    check(status, "Inspect fixture share ownership");
    BOOL owned = !wcscmp(share->shi2_remark, marker) && !wcscmp(share->shi2_path, path);
    NetApiBufferFree(share);
    require(owned, "Fixture share ownership changed");
    check(NetShareDel(NULL, name, 0), "Delete fixture share");
    status = NetShareGetInfo(NULL, name, 2, (BYTE **)&share);
    if (status == ERROR_SUCCESS) NetApiBufferFree(share);
    require(status == NERR_NetNameNotFound, "Fixture share remains");
  }
  USER_INFO_1 *account = NULL;
  DWORD status = NetUserGetInfo(NULL, user, 1, (BYTE **)&account);
  if (status != NERR_UserNotFound) {
    check(status, "Inspect fixture user ownership");
    BOOL owned = !wcscmp(account->usri1_comment, marker);
    NetApiBufferFree(account);
    require(owned, "Fixture user ownership changed");
    check(NetUserDel(NULL, user), "Delete fixture user");
  }
  status = NetUserGetInfo(NULL, user, 1, (BYTE **)&account);
  if (status == ERROR_SUCCESS) NetApiBufferFree(account);
  require(status == NERR_UserNotFound, "Fixture user remains");
  BOOL exists;
  DWORD restored = force_guest(&exists);
  require(exists == (BOOL)saved[0] && restored == saved[1], "Authentication policy was not restored");
  puts("{\"cleaned\":true}");
}

int wmain(int argc, wchar_t **argv) {
  require(argc == 4, "Usage: fixture setup|cleanup directory id");
  require(wcslen(argv[3]) == 10 && wcsspn(argv[3], L"0123456789abcdef") == 10, "Invalid fixture identity");
  wchar_t user[32], marker[80];
  swprintf(user, 32, L"eu%ls", argv[3]);
  swprintf(marker, 80, L"elder-plan18-winxp-%ls", argv[3]);
  if (!wcscmp(argv[1], L"setup")) setup(argv[2], argv[3], user, marker);
  else if (!wcscmp(argv[1], L"cleanup")) cleanup(argv[2], argv[3], user, marker);
  else require(FALSE, "Unknown fixture action");
  return 0;
}
