#include "assets.hpp"

#include "utils.hpp"

namespace apd {
bool EnsureBinaries() {
  return EnsureBinary(kResetpropPath) && EnsureBinary(kBusyboxPath) && EnsureBinary(kMagiskPolicyPath);
}
}  // namespace apd
