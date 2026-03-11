#pragma once

#include <string>
#include <vector>

namespace apd {

struct PatchExtraConfig {
  bool is_path = true;
  std::string path;
  std::string existing_name;
  std::string extra_type;
  std::string set_name;
  std::string set_event;
  std::string set_args;
  bool detach = false;
  int priority = 0;
};

bool InspectKernelImage(const std::string& image_path);
bool InspectKernelPatchImage(const std::string& kpimg_path);
bool PrintKernelPatchVersion(const std::string& kpimg_path);
bool PrintKernelPatchListImage(const std::string& image_path);
bool PrintKernelPatchListKpimg(const std::string& kpimg_path);
bool UnpatchKernelImage(const std::string& image_path, const std::string& out_path);
bool ResetKernelSuperKey(const std::string& image_path, const std::string& out_path, const std::string& superkey);
bool PatchKernelImage(const std::string& image_path,
                      const std::string& kpimg_path,
                      const std::string& out_path,
                      const std::string& superkey,
                      const std::string& reuse_config_from,
                      bool dry_run,
                      bool root_superkey,
                      const std::vector<std::string>& additions,
                      const std::vector<PatchExtraConfig>& extra_configs);

}  // namespace apd
