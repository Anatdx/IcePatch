#include "module.hpp"

#include "assets.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "installer.hpp"
#include "metamodule.hpp"
#include "restorecon.hpp"
#include "utils.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <limits.h>
#include <sstream>

#ifdef APD_USE_LUA
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}
#endif

namespace apd {

namespace {

bool ForeachModule(ModuleType type, const std::function<bool(const std::string&)>& fn);

void PrintInstallerBanner() {
  std::fprintf(stdout, "%s\n", kInstallerBanner);
  std::fflush(stdout);
}

bool EnsureBootCompleted() {
  return GetProp("sys.boot_completed") == "1";
}

bool ValidateModuleId(const std::string& id) {
  if (id.empty() || id.size() > 64) {
    return false;
  }
  for (char c : id) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
        c == '>' || c == '|') {
      return false;
    }
  }
  if (id[0] == '.' || id.find("..") != std::string::npos) {
    return false;
  }
  return true;
}

std::map<std::string, std::string> ParseProps(const std::string& content) {
  std::map<std::string, std::string> props;
  std::stringstream ss(content);
  std::string line;
  while (std::getline(ss, line)) {
    auto pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    std::string key = Trim(line.substr(0, pos));
    std::string value = Trim(line.substr(pos + 1));
    props[key] = value;
  }
  return props;
}

std::string EscapeJson(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

std::string FindLuaInterpreter() {
  const char* candidates[] = {"/data/adb/ap/bin/lua", "/system/bin/lua", "/system/xbin/lua", "lua"};
  for (const char* path : candidates) {
    if (access(path, X_OK) == 0) {
      return path;
    }
  }
  return "";
}

#ifdef APD_USE_LUA
bool RunLuaString(const std::string& script, bool wait) {
  auto run = [&]() -> bool {
    lua_State* L = luaL_newstate();
    if (!L) {
      return false;
    }
    luaL_openlibs(L);
    int rc = luaL_loadstring(L, script.c_str());
    if (rc == LUA_OK) {
      rc = lua_pcall(L, 0, 0, 0);
    }
    if (rc != LUA_OK) {
      const char* err = lua_tostring(L, -1);
      LOGW("lua error: %s", err ? err : "unknown");
    }
    lua_close(L);
    return rc == LUA_OK;
  };

  if (!wait) {
    pid_t pid = fork();
    if (pid < 0) {
      return false;
    }
    if (pid == 0) {
      _exit(run() ? 0 : 1);
    }
    return true;
  }
  return run();
}
#endif

bool ExecScript(const std::string& path, bool wait) {
  pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    setpgid(0, 0);
    SwitchCgroups();
    setenv("ASH_STANDALONE", "1", 1);
    setenv("APATCH", "true", 1);
    setenv("APATCH_VER", kVersionName, 1);
    setenv("APATCH_VER_CODE", kVersionCode, 1);
    std::string path_env = std::getenv("PATH") ? std::getenv("PATH") : "";
    path_env += ":";
    path_env += kBinaryDir;
    setenv("PATH", path_env.c_str(), 1);
    chdir(std::string(path).substr(0, path.find_last_of('/')).c_str());
    execl(kBusyboxPath, kBusyboxPath, "sh", path.c_str(), nullptr);
    _exit(127);
  }
  if (!wait) {
    return true;
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool RunLuaScript(const std::string& module_id, const std::string& function, bool on_each_module,
                  const std::string& arg, bool wait) {
#ifndef APD_USE_LUA
  std::string lua = FindLuaInterpreter();
  if (lua.empty()) {
    LOGW("lua interpreter not found");
    return false;
  }
#endif

  EnsureDirExists("/data/adb/config");

  std::string script;
  script += "modules = {}\n";
  script += "function info(msg) io.stderr:write('[Lua] '..tostring(msg)..'\\n') end\n";
  script += "function warn(msg) io.stderr:write('[Lua] '..tostring(msg)..'\\n') end\n";
  script +=
      "function setConfig(name, content)\n"
      "  local f = io.open('/data/adb/config/'..name,'w')\n"
      "  if f then f:write(content); f:close() end\n"
      "end\n";
  script +=
      "function getConfig(name)\n"
      "  local f = io.open('/data/adb/config/'..name,'r')\n"
      "  if not f then return '' end\n"
      "  local c = f:read('*a') or ''\n"
      "  f:close()\n"
      "  return c\n"
      "end\n";
  script +=
      "function install_module(zip)\n"
      "  os.execute('/data/adb/apd module install \"'..zip..'\"')\n"
      "end\n";
  script +=
      "local function add_module(id, path)\n"
      "  package.cpath = path..'/?.so;'..package.cpath\n"
      "  local ok, mod = pcall(dofile, path..'/'..id..'.lua')\n"
      "  if ok and type(mod) == 'table' then modules[id] = mod end\n"
      "end\n";

  ForeachModule(ModuleType::kAll, [&](const std::string& module_path) {
    std::string id = module_path.substr(module_path.find_last_of('/') + 1);
    std::string lua_file = module_path + "/" + id + ".lua";
    if (FileExists(lua_file)) {
      script += "add_module('" + EscapeJson(id) + "','" + EscapeJson(module_path) + "')\n";
    }
    return true;
  });

  if (on_each_module) {
    script += "for id, m in pairs(modules) do\n";
    script += "  local f = m['" + EscapeJson(function) + "']\n";
    script += "  if type(f) == 'function' then f('" + EscapeJson(arg) + "') end\n";
    script += "end\n";
  } else {
    script += "local m = modules['" + EscapeJson(module_id) + "']\n";
    script += "if not m then error('module not found') end\n";
    script += "local f = m['" + EscapeJson(function) + "']\n";
    script += "if type(f) ~= 'function' then error('function not found') end\n";
    script += "f()\n";
  }

#ifdef APD_USE_LUA
  return RunLuaString(script, wait);
#else
  std::string runner = "/data/adb/ap/.apd_lua_runner.lua";
  if (!WriteFile(runner, script, false)) {
    LOGW("write lua runner failed");
    return false;
  }

  CommandResult res = ExecCommand({lua, runner}, false);
  if (wait) {
    return res.exit_code == 0;
  }
  return true;
#endif
}

bool ForeachModule(ModuleType type, const std::function<bool(const std::string&)>& fn) {
  const char* base = (type == ModuleType::kUpdated) ? kModuleUpdateDir : kModuleDir;
  DIR* dir = opendir(base);
  if (!dir) {
    return false;
  }
  struct dirent* entry = nullptr;
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    std::string path = std::string(base) + entry->d_name;
    if (!DirExists(path)) {
      continue;
    }
    if (type == ModuleType::kActive) {
      if (FileExists(path + "/" + kDisableFileName) || FileExists(path + "/" + kRemoveFileName)) {
        continue;
      }
    }
    if (!fn(path)) {
      closedir(dir);
      return false;
    }
  }
  closedir(dir);
  return true;
}

}  // namespace

std::map<std::string, std::string> ReadModuleProp(const std::string& module_path) {
  std::string prop = module_path + "/module.prop";
  std::string content = ReadFile(prop);
  return ParseProps(content);
}

bool HandleUpdatedModules() {
  return ForeachModule(ModuleType::kUpdated, [&](const std::string& updated_module) {
    std::string module_id = updated_module.substr(updated_module.find_last_of('/') + 1);
    std::string module_dir = std::string(kModuleDir) + module_id;
    bool disabled = FileExists(module_dir + "/" + kDisableFileName);
    bool removed = FileExists(module_dir + "/" + kRemoveFileName);
    ExecCommand({"/system/bin/sh", "-c", std::string("rm -rf ") + module_dir});
    ExecCommand({"/system/bin/sh", "-c", std::string("mv ") + updated_module + " " + module_dir});
    if (removed) {
      EnsureFileExists(module_dir + "/" + kRemoveFileName);
    } else if (disabled) {
      EnsureFileExists(module_dir + "/" + kDisableFileName);
    }
    return true;
  });
}

bool InstallModule(const std::string& zip) {
  PrintInstallerBanner();
  if (!EnsureBootCompleted()) {
    LOGE("Android is Booting!");
    return false;
  }
  EnsureBinaries();
  EnsureDirExists(kWorkingDir);
  EnsureDirExists(kBinaryDir);

  char real_zip_path[PATH_MAX] = {0};
  if (!realpath(zip.c_str(), real_zip_path)) {
    LOGE("Failed to resolve zip path");
    return false;
  }

  CommandResult prop_res =
      ExecCommand({kBusyboxPath, "unzip", "-p", real_zip_path, "module.prop"}, true);
  if (prop_res.exit_code != 0) {
    LOGE("module.prop not found in zip");
    return false;
  }
  auto props = ParseProps(prop_res.output);
  auto it = props.find("id");
  if (it == props.end()) {
    LOGE("module id missing");
    return false;
  }
  std::string module_id = Trim(it->second);
  if (!ValidateModuleId(module_id)) {
    LOGE("Invalid module id");
    return false;
  }
  bool is_metamodule = IsMetamodule(props);
  bool is_disabled = false;
  if (!is_metamodule && !CheckInstallSafety(is_disabled)) {
    LOGE("Metamodule blocks installation");
    return false;
  }
  if (is_metamodule) {
    std::string existing = GetMetamodulePath();
    if (!existing.empty()) {
      auto existing_props = ReadModuleProp(existing);
      auto existing_id = existing_props["id"];
      if (!existing_id.empty() && existing_id != module_id) {
        LOGE("Another metamodule is already installed");
        return false;
      }
    }
  }
  EnsureDirExists(kModuleDir);
  EnsureDirExists(kModuleUpdateDir);

  std::string module_dir = std::string(kModuleDir) + module_id;
  std::string module_update_dir = std::string(kModuleUpdateDir) + module_id;
  EnsureDirExists(module_dir);
  ExecCommand({"/system/bin/sh", "-c", std::string("rm -rf ") + module_update_dir});
  EnsureDirExists(module_update_dir);

  CommandResult unzip_res = ExecCommand(
      {kBusyboxPath, "unzip", "-o", "-q", real_zip_path, "-d", module_update_dir}, false);
  if (unzip_res.exit_code != 0) {
    LOGE("unzip failed");
    return false;
  }

  LOGI("- Running module installer");
  std::string install_script = GetInstallScript(
      is_metamodule, kInstallerContent,
      std::string(kInstallerContent) + "\ninstall_module\nexit 0\n");
  setenv("OUTFD", "1", 1);
  setenv("ZIPFILE", real_zip_path, 1);
  setenv("ASH_STANDALONE", "1", 1);
  setenv("APATCH", "true", 1);
  setenv("APATCH_VER", kVersionName, 1);
  setenv("APATCH_VER_CODE", kVersionCode, 1);
  std::string path_env = std::getenv("PATH") ? std::getenv("PATH") : "";
  path_env += ":";
  path_env += kBinaryDir;
  setenv("PATH", path_env.c_str(), 1);
  CommandResult inst = ExecCommand({kBusyboxPath, "sh", "-c", install_script}, false);
  if (inst.exit_code != 0) {
    LOGE("install script failed");
    return false;
  }

  std::string system_dir = module_update_dir + "/system";
  if (DirExists(system_dir)) {
    chmod(system_dir.c_str(), 0755);
    RestoreSyscon(system_dir);
  }
  if (is_metamodule) {
    EnsureSymlink(std::string(kModuleDir) + module_id);
  }

  EnsureFileExists(std::string(kWorkingDir) + kUpdateFileName);
  return true;
}

bool UninstallModule(const std::string& id) {
  PrintInstallerBanner();
  std::string target = std::string(kModuleDir) + id;
  if (!DirExists(target)) {
    return false;
  }
  EnsureFileExists(target + "/" + kRemoveFileName);
  EnsureFileExists(std::string(kWorkingDir) + kUpdateFileName);
  return true;
}

bool EnableModule(const std::string& id) {
  PrintInstallerBanner();
  std::string flag = std::string(kModuleDir) + id + "/" + kDisableFileName;
  unlink(flag.c_str());
  EnsureFileExists(std::string(kWorkingDir) + kUpdateFileName);
  return true;
}

bool DisableModule(const std::string& id) {
  PrintInstallerBanner();
  std::string flag = std::string(kModuleDir) + id + "/" + kDisableFileName;
  EnsureFileExists(flag);
  EnsureFileExists(std::string(kWorkingDir) + kUpdateFileName);
  return true;
}

bool ListModules() {
  std::vector<std::map<std::string, std::string>> modules;
  bool ok = ForeachModule(ModuleType::kAll, [&](const std::string& module_path) {
    auto props = ReadModuleProp(module_path);
    std::string id = props["id"];
    if (id.empty()) {
      id = module_path.substr(module_path.find_last_of('/') + 1);
      props["id"] = id;
    }
    bool enabled = !FileExists(module_path + "/" + kDisableFileName);
    bool update = FileExists(module_path + "/" + kUpdateFileName);
    bool remove = FileExists(module_path + "/" + kRemoveFileName);
    bool web = DirExists(module_path + "/" + kModuleWebDir);
    bool action = FileExists(module_path + "/" + kModuleActionScript) ||
                  FileExists(module_path + "/" + id + ".lua");

    props["enabled"] = enabled ? "true" : "false";
    props["update"] = update ? "true" : "false";
    props["remove"] = remove ? "true" : "false";
    props["web"] = web ? "true" : "false";
    props["action"] = action ? "true" : "false";
    modules.push_back(props);
    return true;
  });

  std::string json = "[";
  for (size_t i = 0; i < modules.size(); ++i) {
    if (i > 0) {
      json += ",";
    }
    json += "{";
    size_t j = 0;
    for (const auto& kv : modules[i]) {
      if (j++ > 0) {
        json += ",";
      }
      json += "\"" + EscapeJson(kv.first) + "\":";
      if (kv.second == "true" || kv.second == "false") {
        json += kv.second;
      } else {
        json += "\"" + EscapeJson(kv.second) + "\"";
      }
    }
    json += "}";
  }
  json += "]";
  std::printf("%s\n", json.c_str());
  return ok;
}

bool RunAction(const std::string& id) {
  PrintInstallerBanner();
  std::string action = std::string(kModuleDir) + id + "/" + kModuleActionScript;
  if (!FileExists(action)) {
    LOGW("action.sh not found");
    return RunLua(id, "action", false, true);
  }
  return ExecScript(action, true);
}

bool RunLua(const std::string& id, const std::string& function, bool on_each_module, bool wait) {
  PrintInstallerBanner();
  return RunLuaScript(id, function, on_each_module, id, wait);
}

bool ExecStageScript(const std::string& stage, bool block) {
  return ForeachModule(ModuleType::kActive, [&](const std::string& module_path) {
    std::string script = module_path + "/" + stage + ".sh";
    if (!FileExists(script)) {
      return true;
    }
    return ExecScript(script, block);
  });
}

bool ExecStageLua(const std::string& stage, bool wait, const std::string& superkey) {
  std::string stage_safe = stage;
  std::replace(stage_safe.begin(), stage_safe.end(), '-', '_');
  return RunLuaScript(superkey, stage_safe, true, superkey, wait);
}

bool ExecCommonScripts(const std::string& dir, bool wait) {
  std::string script_dir = std::string(kAdbDir) + dir;
  if (!DirExists(script_dir)) {
    return true;
  }
  DIR* dp = opendir(script_dir.c_str());
  if (!dp) {
    return false;
  }
  struct dirent* entry = nullptr;
  while ((entry = readdir(dp)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    std::string path = script_dir + "/" + entry->d_name;
    if (FileExists(path)) {
      if (access(path.c_str(), X_OK) != 0) {
        continue;
      }
      ExecScript(path, wait);
    }
  }
  closedir(dp);
  return true;
}

bool LoadSepolicyRule() {
  return ForeachModule(ModuleType::kActive, [&](const std::string& module_path) {
    std::string rule = module_path + "/sepolicy.rule";
    if (!FileExists(rule)) {
      return true;
    }
    CommandResult res =
        ExecCommand({kMagiskPolicyPath, "--live", "--apply", rule}, false);
    return res.exit_code == 0;
  });
}

bool LoadSystemProp() {
  return ForeachModule(ModuleType::kActive, [&](const std::string& module_path) {
    std::string prop = module_path + "/system.prop";
    if (!FileExists(prop)) {
      return true;
    }
    CommandResult res = ExecCommand({kResetpropPath, "-n", "--file", prop}, false);
    return res.exit_code == 0;
  });
}

bool PruneModules() {
  bool ok = ForeachModule(ModuleType::kAll, [&](const std::string& module_path) {
    std::string update_flag = module_path + "/" + kUpdateFileName;
    unlink(update_flag.c_str());
    if (!FileExists(module_path + "/" + kRemoveFileName)) {
      return true;
    }
    std::string module_id = module_path.substr(module_path.find_last_of('/') + 1);
    auto props = ReadModuleProp(module_path);
    bool is_metamodule = IsMetamodule(props);
    if (is_metamodule) {
      RemoveSymlink();
    } else {
      ExecMetaUninstallScript(module_id);
    }
    std::string uninstaller = module_path + "/uninstall.sh";
    if (FileExists(uninstaller)) {
      ExecScript(uninstaller, true);
    }
    ExecCommand({"/system/bin/sh", "-c", std::string("rm -rf ") + module_path});
    return true;
  });
  return ok;
}

bool DisableAllModules() {
  if (GetProp("sys.boot_completed") == "1") {
    LOGI("System boot completed, no need to disable all modules");
    return true;
  }
  EnsureFileExists(std::string(kWorkingDir) + kUpdateFileName);
  return ForeachModule(ModuleType::kAll, [&](const std::string& module_path) {
    EnsureFileExists(module_path + "/" + kDisableFileName);
    return true;
  });
}

}  // namespace apd
