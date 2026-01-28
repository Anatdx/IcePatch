#include "package.hpp"

#include "log.hpp"
#include "utils.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <thread>

namespace apd {

namespace {
constexpr const char* kPackageConfigPath = "/data/adb/ap/package_config";
constexpr const char* kPackageConfigTmpPath = "/data/adb/ap/package_config.tmp";
constexpr const char* kPackagesListPath = "/data/system/packages.list";

std::vector<std::string> SplitCsvLine(const std::string& line) {
  std::vector<std::string> cols;
  std::string token;
  bool in_quote = false;
  for (char c : line) {
    if (c == '"' ) {
      in_quote = !in_quote;
      continue;
    }
    if (c == ',' && !in_quote) {
      cols.push_back(token);
      token.clear();
      continue;
    }
    token.push_back(c);
  }
  cols.push_back(token);
  return cols;
}

}  // namespace

std::vector<PackageConfig> ReadApPackageConfig() {
  const int max_retry = 5;
  for (int i = 0; i < max_retry; ++i) {
    std::ifstream ifs(kPackageConfigPath);
    if (!ifs) {
      LOGW("Error opening %s", kPackageConfigPath);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    std::string header;
    if (!std::getline(ifs, header)) {
      return {};
    }
    std::vector<PackageConfig> configs;
    std::string line;
    while (std::getline(ifs, line)) {
      auto cols = SplitCsvLine(line);
      if (cols.size() < 6) {
        continue;
      }
      PackageConfig cfg;
      cfg.pkg = cols[0];
      cfg.exclude = std::stoi(cols[1]);
      cfg.allow = std::stoi(cols[2]);
      cfg.uid = std::stoi(cols[3]);
      cfg.to_uid = std::stoi(cols[4]);
      cfg.sctx = cols[5];
      configs.push_back(cfg);
    }
    return configs;
  }
  return {};
}

bool WriteApPackageConfig(const std::vector<PackageConfig>& configs) {
  const int max_retry = 5;
  for (int i = 0; i < max_retry; ++i) {
    std::ofstream ofs(kPackageConfigTmpPath, std::ios::out | std::ios::trunc);
    if (!ofs) {
      LOGW("Error creating temp file");
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    ofs << "pkg,exclude,allow,uid,to_uid,sctx\n";
    for (const auto& cfg : configs) {
      ofs << cfg.pkg << "," << cfg.exclude << "," << cfg.allow << "," << cfg.uid << ","
          << cfg.to_uid << "," << cfg.sctx << "\n";
    }
    ofs.flush();
    if (!ofs) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    if (rename(kPackageConfigTmpPath, kPackageConfigPath) != 0) {
      LOGW("rename temp file failed");
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    return true;
  }
  return false;
}

bool SynchronizePackageUid() {
  LOGI("[SynchronizePackageUid] Start synchronizing root list with system packages...");
  const int max_retry = 5;
  for (int i = 0; i < max_retry; ++i) {
    std::ifstream ifs(kPackagesListPath);
    if (!ifs) {
      LOGW("Error reading packages.list");
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
      lines.push_back(line);
    }

    auto configs = ReadApPackageConfig();
    std::vector<std::string> system_packages;
    for (const auto& ln : lines) {
      std::istringstream iss(ln);
      std::string pkg;
      if (iss >> pkg) {
        system_packages.push_back(pkg);
      }
    }

    size_t original = configs.size();
    configs.erase(std::remove_if(configs.begin(), configs.end(),
                                 [&](const PackageConfig& cfg) {
                                   return std::find(system_packages.begin(), system_packages.end(),
                                                    cfg.pkg) == system_packages.end();
                                 }),
                  configs.end());
    if (original != configs.size()) {
      LOGI("Removed %zu uninstalled package configurations", original - configs.size());
    }

    bool updated = false;
    for (const auto& ln : lines) {
      std::istringstream iss(ln);
      std::string pkg;
      int uid = 0;
      if (!(iss >> pkg >> uid)) {
        continue;
      }
      for (auto& cfg : configs) {
        if (cfg.pkg == pkg) {
          if (cfg.uid % 100000 != uid % 100000) {
            int new_uid = (cfg.uid / 100000) * 100000 + (uid % 100000);
            LOGI("Updating uid for %s: %d -> %d", pkg.c_str(), cfg.uid, new_uid);
            cfg.uid = new_uid;
            updated = true;
          }
        }
      }
    }
    if (updated || original != configs.size()) {
      return WriteApPackageConfig(configs);
    }
    return true;
  }
  return false;
}

}  // namespace apd
