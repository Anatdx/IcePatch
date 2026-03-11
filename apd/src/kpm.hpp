#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace apd {

struct KpmInfo {
  std::string name;
  std::string version;
  std::string license;
  std::string author;
  std::string description;
};

bool ParseKpmInfo(const std::vector<std::uint8_t>& kpm, KpmInfo* out_info, std::string* error_message);
bool ParseKpmInfoPath(const std::string& kpm_path, KpmInfo* out_info, std::string* error_message);
bool PrintKpmInfoPath(const std::string& kpm_path);

}  // namespace apd

