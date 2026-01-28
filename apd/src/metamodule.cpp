#include "metamodule.hpp"

#include "assets.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "module.hpp"
#include "utils.hpp"

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

namespace apd {

bool IsMetamodule(const std::map<std::string, std::string>& props) {
  auto it = props.find("metamodule");
  if (it == props.end()) {
    return false;
  }
  std::string value = Trim(it->second);
  return value == "1" || value == "true" || value == "TRUE";
}

std::string GetMetamodulePath() {
  std::string symlink = std::string(kMetamoduleDir);
  if (!symlink.empty() && symlink.back() == '/') {
    symlink.pop_back();
  }
  struct stat st {};
  if (lstat(symlink.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
    char buf[PATH_MAX] = {0};
    ssize_t len = readlink(symlink.c_str(), buf, sizeof(buf) - 1);
    if (len > 0) {
      buf[len] = '\0';
      std::string target = buf;
      if (DirExists(target)) {
        return target;
      }
    }
  }
  return "";
}

bool HasMetamodule() {
  return !GetMetamodulePath().empty();
}

bool CheckInstallSafety(bool& is_disabled) {
  is_disabled = false;
  std::string path = GetMetamodulePath();
  if (path.empty()) {
    return true;
  }
  bool has_metainstall = FileExists(path + "/" + kMetamoduleMetaInstallScript);
  if (!has_metainstall) {
    std::string id = path.substr(path.find_last_of('/') + 1);
    has_metainstall = FileExists(std::string(kModuleUpdateDir) + id + "/" +
                                 kMetamoduleMetaInstallScript);
  }
  if (!has_metainstall) {
    return true;
  }
  bool has_update = FileExists(path + "/" + kUpdateFileName);
  bool has_remove = FileExists(path + "/" + kRemoveFileName);
  bool has_disable = FileExists(path + "/" + kDisableFileName);
  if (!has_update && !has_remove && !has_disable) {
    return true;
  }
  is_disabled = has_disable && !has_update && !has_remove;
  return false;
}

bool EnsureSymlink(const std::string& module_path) {
  std::string symlink_path = std::string(kMetamoduleDir);
  if (!symlink_path.empty() && symlink_path.back() == '/') {
    symlink_path.pop_back();
  }
  unlink(symlink_path.c_str());
  if (symlink(module_path.c_str(), symlink_path.c_str()) != 0) {
    LOGW("Failed to create metamodule symlink");
    return false;
  }
  return true;
}

bool RemoveSymlink() {
  std::string symlink = std::string(kMetamoduleDir);
  if (!symlink.empty() && symlink.back() == '/') {
    symlink.pop_back();
  }
  unlink(symlink.c_str());
  return true;
}

std::string GetInstallScript(bool is_metamodule, const std::string& installer_content,
                             const std::string& install_module_script) {
  if (is_metamodule) {
    return install_module_script;
  }
  std::string path = GetMetamodulePath();
  if (path.empty()) {
    return install_module_script;
  }
  if (FileExists(path + "/" + kDisableFileName)) {
    return install_module_script;
  }
  std::string metainstall = ReadFile(path + "/" + kMetamoduleMetaInstallScript);
  if (metainstall.empty()) {
    return install_module_script;
  }
  return installer_content + "\n" + metainstall + "\nexit 0\n";
}

bool ExecMetaUninstallScript(const std::string& module_id) {
  std::string path = std::string(kModuleDir) + module_id + "/" + kMetamoduleMetaUninstallScript;
  if (!FileExists(path)) {
    return true;
  }
  CommandResult res = ExecCommand({kBusyboxPath, "sh", path}, false);
  return res.exit_code == 0;
}

bool ExecMetamoduleMount(const std::string& module_dir) {
  std::string path = GetMetamodulePath();
  if (path.empty()) {
    return true;
  }
  std::string script = path + "/" + kMetamoduleMountScript;
  if (!FileExists(script)) {
    return true;
  }
  CommandResult res = ExecCommand({kBusyboxPath, "sh", script, module_dir}, false);
  return res.exit_code == 0;
}

bool ExecMetamoduleStage(const std::string& stage, bool block) {
  std::string path = GetMetamodulePath();
  if (path.empty()) {
    return true;
  }
  std::string script = path + "/" + stage + ".sh";
  if (!FileExists(script)) {
    return true;
  }
  CommandResult res = ExecCommand({kBusyboxPath, "sh", script}, false);
  if (block) {
    return res.exit_code == 0;
  }
  return true;
}

}  // namespace apd
