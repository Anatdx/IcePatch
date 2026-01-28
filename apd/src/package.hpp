#pragma once

#include <string>
#include <vector>

namespace apd {

struct PackageConfig {
  std::string pkg;
  int exclude = 0;
  int allow = 0;
  int uid = 0;
  int to_uid = 0;
  std::string sctx;
};

std::vector<PackageConfig> ReadApPackageConfig();
bool WriteApPackageConfig(const std::vector<PackageConfig>& configs);
bool SynchronizePackageUid();

}  // namespace apd
