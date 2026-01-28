#pragma once

#include <map>
#include <string>
#include <vector>

namespace apd {

enum class ModuleType {
  kAll,
  kActive,
  kUpdated,
};

struct ModuleInfo {
  std::string id;
  std::string name;
  std::string version;
  std::string version_code;
  std::string author;
  std::string description;
  bool enabled = true;
  bool update = false;
  bool remove = false;
  bool web = false;
  bool action = false;
  bool mount = false;
  bool metamodule = false;
};

bool InstallModule(const std::string& zip);
bool UninstallModule(const std::string& id);
bool EnableModule(const std::string& id);
bool DisableModule(const std::string& id);
bool ListModules();
bool RunAction(const std::string& id);
bool RunLua(const std::string& id, const std::string& function, bool on_each_module, bool wait);
bool ExecStageScript(const std::string& stage, bool block);
bool ExecStageLua(const std::string& stage, bool wait, const std::string& superkey);
bool ExecCommonScripts(const std::string& dir, bool wait);
bool LoadSepolicyRule();
bool LoadSystemProp();
bool PruneModules();
bool HandleUpdatedModules();
bool DisableAllModules();

std::map<std::string, std::string> ReadModuleProp(const std::string& module_path);

}  // namespace apd
