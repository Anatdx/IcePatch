#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace apd {

struct SuProfile {
  int uid = 0;
  int to_uid = 0;
  char scontext[0x60]{};
};

long ScSuGetSafemode(const std::string& key);
long ScSuGrantUid(const std::string& key, const SuProfile& profile);
long ScSuRevokeUid(const std::string& key, int uid);
long ScSuResetPath(const std::string& key, const std::string& path);
int ScKpVer(const std::string& key);
int ScKVer(const std::string& key);
long ScKlog(const std::string& key, const std::string& msg);
long ScSuUidNums(const std::string& key);
long ScSuAllowUids(const std::string& key, std::vector<int>& uids);

void RefreshApPackageList(const std::string& key);
void PrivilegeApdProfile(const std::string& key);
void InitLoadPackageUidConfig(const std::string& key);
void InitLoadSuPath(const std::string& key);
void ForkForResult(const std::string& exec, const std::vector<std::string>& argv,
                   const std::string& key);

}  // namespace apd
