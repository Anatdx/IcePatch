#pragma once

namespace apd {
constexpr const char* kAdbDir = "/data/adb/";
constexpr const char* kWorkingDir = "/data/adb/ap/";
constexpr const char* kBinaryDir = "/data/adb/ap/bin/";
constexpr const char* kLogDir = "/data/adb/ap/log/";

constexpr const char* kApRcPath = "/data/adb/ap/.aprc";
constexpr const char* kGlobalNamespaceFile = "/data/adb/.global_namespace_enable";
constexpr const char* kDaemonPath = "/data/adb/apd";

constexpr const char* kModuleDir = "/data/adb/modules/";
constexpr const char* kModuleUpdateDir = "/data/adb/modules_update/";

constexpr const char* kTempDir = "/debug_ramdisk";
constexpr const char* kTempDirLegacy = "/sbin";

constexpr const char* kModuleWebDir = "webroot";
constexpr const char* kModuleActionScript = "action.sh";
constexpr const char* kDisableFileName = "disable";
constexpr const char* kUpdateFileName = "update";
constexpr const char* kRemoveFileName = "remove";

constexpr const char* kMetamoduleMountScript = "metamount.sh";
constexpr const char* kMetamoduleMetaInstallScript = "metainstall.sh";
constexpr const char* kMetamoduleMetaUninstallScript = "metauninstall.sh";
constexpr const char* kMetamoduleDir = "/data/adb/metamodule/";

constexpr const char* kPtsName = "pts";

extern const char* kVersionCode;
extern const char* kVersionName;
}  // namespace apd
