#pragma once

#include <string>

namespace apd {

bool OnPostDataFs(const std::string& superkey);
bool OnServices(const std::string& superkey);
bool OnBootCompleted(const std::string& superkey);
bool StartUidListener();

}  // namespace apd
