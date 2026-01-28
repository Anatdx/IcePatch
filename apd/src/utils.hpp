#pragma once

#include <string>
#include <vector>

namespace apd {

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

bool EnsureFileExists(const std::string& path);
bool EnsureDirExists(const std::string& path);
bool EnsureBinary(const std::string& path);
std::string GetProp(const std::string& key);
CommandResult ExecCommand(const std::vector<std::string>& argv, bool capture_output = false);
bool IsSafeMode(const std::string& superkey);
bool SwitchMntNs(int pid);
void SwitchCgroups();
void Umask(unsigned int mask);
bool HasMagisk();
const char* GetTmpPath();
bool FileExists(const std::string& path);
bool DirExists(const std::string& path);
std::string ReadFile(const std::string& path);
bool WriteFile(const std::string& path, const std::string& data, bool append = false);
std::vector<std::string> SplitLines(const std::string& input);
std::string Trim(const std::string& input);

}  // namespace apd
