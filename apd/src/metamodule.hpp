#pragma once

#include <map>
#include <string>

namespace apd {

bool IsMetamodule(const std::map<std::string, std::string>& props);
std::string GetMetamodulePath();
bool HasMetamodule();
bool CheckInstallSafety(bool& is_disabled);
bool EnsureSymlink(const std::string& module_path);
bool RemoveSymlink();
std::string GetInstallScript(bool is_metamodule, const std::string& installer_content,
                             const std::string& install_module_script);
bool ExecMetaUninstallScript(const std::string& module_id);
bool ExecMetamoduleMount(const std::string& module_dir);
bool ExecMetamoduleStage(const std::string& stage, bool block);

}  // namespace apd
