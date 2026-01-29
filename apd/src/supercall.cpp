#include "supercall.hpp"

#include "log.hpp"
#include "package.hpp"
#include "utils.hpp"

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace apd {

namespace {
constexpr long kMajor = 0;
constexpr long kMinor = 11;
constexpr long kPatch = 1;

constexpr long kSupercallNr = 45;
constexpr long kSupercallKlog = 0x1004;
constexpr long kSupercallKernelPatchVer = 0x1008;
constexpr long kSupercallKernelVer = 0x1009;
constexpr long kSupercallSu = 0x1010;
constexpr long kSupercallKstorageWrite = 0x1041;
constexpr long kSupercallSuGrantUid = 0x1100;
constexpr long kSupercallSuRevokeUid = 0x1101;
constexpr long kSupercallSuNums = 0x1102;
constexpr long kSupercallSuList = 0x1103;
constexpr long kSupercallSuResetPath = 0x1111;
constexpr long kSupercallSuGetSafemode = 0x1112;

constexpr int kKstorageExcludeListGroup = 1;

long VerAndCmd(long cmd) {
  long version_code = (kMajor << 16) + (kMinor << 8) + kPatch;
  return (version_code << 32) | (0x1158 << 16) | (cmd & 0xFFFF);
}

long ScKstorageWrite(const std::string& key, int gid, long long did, void* data, int offset,
                     int dlen) {
  if (key.empty()) {
    return -EINVAL;
  }
  return syscall(kSupercallNr, key.c_str(), VerAndCmd(kSupercallKstorageWrite), static_cast<long>(gid),
                 static_cast<long>(did), data,
                 static_cast<long>(((static_cast<long long>(offset) << 32) | dlen)));
}

long ScSetApModExclude(const std::string& key, long long uid, int exclude) {
  return ScKstorageWrite(key, kKstorageExcludeListGroup, uid, &exclude, 0, sizeof(int));
}

std::string ReadFileString(const std::string& path) {
  return ReadFile(path);
}

std::string ToScontext(const std::string& sctx) {
  if (sctx.size() >= sizeof(SuProfile::scontext)) {
    return sctx.substr(0, sizeof(SuProfile::scontext));
  }
  return sctx;
}

void SetEnvVar(const char* key, const char* value) {
  setenv(key, value, 1);
}

}  // namespace

long ScSuGetSafemode(const std::string& key) {
  if (key.empty()) {
    LOGW("[ScSuGetSafemode] empty key");
    return 0;
  }
  return syscall(kSupercallNr, key.c_str(), VerAndCmd(kSupercallSuGetSafemode));
}

long ScSu(const std::string& key, const SuProfile& profile) {
  if (key.empty()) {
    return -EINVAL;
  }
  return syscall(kSupercallNr, key.c_str(), VerAndCmd(kSupercallSu), &profile);
}

long ScSuGrantUid(const std::string& key, const SuProfile& profile) {
  if (key.empty()) {
    return -EINVAL;
  }
  return syscall(kSupercallNr, key.c_str(), VerAndCmd(kSupercallSuGrantUid), &profile);
}

long ScSuRevokeUid(const std::string& key, int uid) {
  if (key.empty()) {
    return -EINVAL;
  }
  return syscall(kSupercallNr, key.c_str(), VerAndCmd(kSupercallSuRevokeUid), uid);
}

long ScSuResetPath(const std::string& key, const std::string& path) {
  if (key.empty() || path.empty()) {
    return -EINVAL;
  }
  return syscall(kSupercallNr, key.c_str(), VerAndCmd(kSupercallSuResetPath), path.c_str());
}

int ScKpVer(const std::string& key) {
  if (key.empty()) {
    return -EINVAL;
  }
  return static_cast<int>(syscall(kSupercallNr, key.c_str(), VerAndCmd(kSupercallKernelPatchVer)));
}

int ScKVer(const std::string& key) {
  if (key.empty()) {
    return -EINVAL;
  }
  return static_cast<int>(syscall(kSupercallNr, key.c_str(), VerAndCmd(kSupercallKernelVer)));
}

long ScKlog(const std::string& key, const std::string& msg) {
  if (key.empty() || msg.empty()) {
    return -EINVAL;
  }
  return syscall(kSupercallNr, key.c_str(), VerAndCmd(kSupercallKlog), msg.c_str());
}

long ScSuUidNums(const std::string& key) {
  if (key.empty()) {
    return -EINVAL;
  }
  return syscall(kSupercallNr, key.c_str(), VerAndCmd(kSupercallSuNums));
}

long ScSuAllowUids(const std::string& key, std::vector<int>& uids) {
  if (key.empty() || uids.empty()) {
    return -EINVAL;
  }
  return syscall(kSupercallNr, key.c_str(), VerAndCmd(kSupercallSuList), uids.data(),
                 static_cast<int>(uids.size()));
}

void RefreshApPackageList(const std::string& key) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);

  long num = ScSuUidNums(key);
  if (num < 0) {
    LOGE("[RefreshApPackageList] get number of UIDs: %ld", num);
    return;
  }
  std::vector<int> uids(static_cast<size_t>(num), 0);
  long n = ScSuAllowUids(key, uids);
  if (n < 0) {
    LOGE("[RefreshApPackageList] get su list: %ld", n);
    return;
  }
  for (int uid : uids) {
    if (uid == 0 || uid == 2000) {
      LOGW("[RefreshApPackageList] skip critical uid: %d", uid);
      continue;
    }
    long rc = ScSuRevokeUid(key, uid);
    if (rc != 0) {
      LOGE("[RefreshApPackageList] revoke uid %d: %ld", uid, rc);
    }
  }

  if (!SynchronizePackageUid()) {
    LOGE("[RefreshApPackageList] synchronize package uids failed");
  }

  auto configs = ReadApPackageConfig();
  for (const auto& cfg : configs) {
    if (cfg.allow == 1 && cfg.exclude == 0) {
      SuProfile profile{};
      profile.uid = cfg.uid;
      profile.to_uid = cfg.to_uid;
      std::string sctx = ToScontext(cfg.sctx);
      std::memcpy(profile.scontext, sctx.c_str(), sctx.size());
      long result = ScSuGrantUid(key, profile);
      LOGI("[RefreshApPackageList] Loading %s: %ld", cfg.pkg.c_str(), result);
    }
    if (cfg.allow == 0 && cfg.exclude == 1) {
      long result = ScSetApModExclude(key, cfg.uid, 1);
      LOGI("[RefreshApPackageList] Loading exclude %s: %ld", cfg.pkg.c_str(), result);
    }
  }
}

void PrivilegeApdProfile(const std::string& key) {
  if (key.empty()) {
    return;
  }
  SuProfile profile{};
  profile.uid = static_cast<int>(getpid());
  profile.to_uid = 0;
  const char* sctx = "u:r:magisk:s0";
  std::memcpy(profile.scontext, sctx, std::strlen(sctx));
  long result = ScSu(key, profile);
  if (result != 0) {
    LOGW("[PrivilegeApdProfile] ScSu failed, fallback to GrantUid: %ld", result);
    result = ScSuGrantUid(key, profile);
  }
  LOGI("[PrivilegeApdProfile] result=%ld", result);
}

void InitLoadPackageUidConfig(const std::string& key) {
  if (key.empty()) {
    LOGW("[InitLoadPackageUidConfig] empty superkey");
    return;
  }
  auto configs = ReadApPackageConfig();
  for (const auto& cfg : configs) {
    if (cfg.allow == 1 && cfg.exclude == 0) {
      SuProfile profile{};
      profile.uid = cfg.uid;
      profile.to_uid = cfg.to_uid;
      std::string sctx = ToScontext(cfg.sctx);
      std::memcpy(profile.scontext, sctx.c_str(), sctx.size());
      long result = ScSuGrantUid(key, profile);
      LOGI("[InitLoadPackageUidConfig] %s: %ld", cfg.pkg.c_str(), result);
    }
    if (cfg.allow == 0 && cfg.exclude == 1) {
      long result = ScSetApModExclude(key, cfg.uid, 1);
      LOGI("[InitLoadPackageUidConfig] exclude %s: %ld", cfg.pkg.c_str(), result);
    }
  }
}

void InitLoadSuPath(const std::string& key) {
  if (key.empty()) {
    LOGW("[InitLoadSuPath] empty superkey");
    return;
  }
  const char* su_path_file = "/data/adb/ap/su_path";
  std::string content = ReadFileString(su_path_file);
  if (content.empty()) {
    LOGW("[InitLoadSuPath] su_path file missing");
    return;
  }
  std::string path = Trim(content);
  if (path.empty()) {
    return;
  }
  long rc = ScSuResetPath(key, path);
  if (rc == 0) {
    LOGI("[InitLoadSuPath] loaded");
  } else {
    LOGW("[InitLoadSuPath] failed rc=%ld", rc);
  }
}

void ForkForResult(const std::string& exec, const std::vector<std::string>& argv,
                   const std::string& key) {
  if (key.empty()) {
    LOGW("[ForkForResult] superkey empty");
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    LOGE("[ForkForResult] fork failed");
    return;
  }
  if (pid == 0) {
    SetEnvVar("KERNELPATCH", "true");
    char kpver[32];
    std::snprintf(kpver, sizeof(kpver), "%x", ScKpVer(key));
    SetEnvVar("KERNELPATCH_VERSION", kpver);
    char kver[32];
    std::snprintf(kver, sizeof(kver), "%x", ScKVer(key));
    SetEnvVar("KERNEL_VERSION", kver);

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& arg : argv) {
      args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    execv(exec.c_str(), args.data());
    _exit(1);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  LOGI("[ForkForResult] wait status: %d", status);
}

}  // namespace apd
