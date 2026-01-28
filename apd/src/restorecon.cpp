#include "restorecon.hpp"

#include "defs.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <dirent.h>
#include <cstring>
#include <sys/stat.h>
#include <sys/xattr.h>

namespace apd {

namespace {
constexpr const char* kSystemCon = "u:object_r:system_file:s0";
constexpr const char* kAdbCon = "u:object_r:adb_data_file:s0";
constexpr const char* kUnlabelCon = "u:object_r:unlabeled:s0";
constexpr const char* kSelinuxXattr = "security.selinux";

bool LSetFileCon(const std::string& path, const char* con) {
  return lsetxattr(path.c_str(), kSelinuxXattr, con, std::strlen(con), 0) == 0;
}

std::string LGetFileCon(const std::string& path) {
  char buf[256] = {0};
  ssize_t len = lgetxattr(path.c_str(), kSelinuxXattr, buf, sizeof(buf) - 1);
  if (len <= 0) {
    return "";
  }
  buf[len] = '\0';
  return std::string(buf);
}

void RestoreSysconIfUnlabeled(const std::string& dir) {
  DIR* dp = opendir(dir.c_str());
  if (!dp) {
    return;
  }
  struct dirent* entry = nullptr;
  while ((entry = readdir(dp)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    std::string path = dir + "/" + entry->d_name;
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
      continue;
    }
    std::string con = LGetFileCon(path);
    if (con.empty() || con == kUnlabelCon) {
      LSetFileCon(path, kSystemCon);
    }
    if (S_ISDIR(st.st_mode)) {
      RestoreSysconIfUnlabeled(path);
    }
  }
  closedir(dp);
}

}  // namespace

bool RestoreSyscon(const std::string& dir) {
  DIR* dp = opendir(dir.c_str());
  if (!dp) {
    return false;
  }
  struct dirent* entry = nullptr;
  while ((entry = readdir(dp)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    std::string path = dir + "/" + entry->d_name;
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
      continue;
    }
    LSetFileCon(path, kSystemCon);
    if (S_ISDIR(st.st_mode)) {
      RestoreSyscon(path);
    }
  }
  closedir(dp);
  return true;
}

bool Restorecon() {
  LSetFileCon(kDaemonPath, kAdbCon);
  RestoreSysconIfUnlabeled(kModuleDir);
  return true;
}

}  // namespace apd
