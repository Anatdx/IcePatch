#pragma once

namespace apd {
constexpr const char* kResetpropPath = "/data/adb/ap/bin/resetprop";
constexpr const char* kBusyboxPath = "/data/adb/ap/bin/busybox";
constexpr const char* kMagiskPolicyPath = "/data/adb/ap/bin/magiskpolicy";

bool EnsureBinaries();
}  // namespace apd
