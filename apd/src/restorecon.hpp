#pragma once

#include <string>

namespace apd {

bool Restorecon();
bool RestoreSyscon(const std::string& dir);

}  // namespace apd
