#pragma once

#include <string>

namespace apd {

bool BootUnpackKernel(const std::string& boot_image_path, const std::string& out_kernel_path);
bool BootRepackKernel(const std::string& orig_boot_path,
                      const std::string& new_kernel_path,
                      const std::string& out_boot_path);

}  // namespace apd
