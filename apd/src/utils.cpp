#include "utils.hpp"

#include "defs.hpp"
#include "log.hpp"
#include "supercall.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <cctype>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace apd {

bool FileExists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool DirExists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool EnsureFileExists(const std::string& path) {
  int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
  if (fd >= 0) {
    close(fd);
    return true;
  }
  if (errno == EEXIST && FileExists(path)) {
    return true;
  }
  return false;
}

bool EnsureDirExists(const std::string& path) {
  if (DirExists(path)) {
    return true;
  }
  size_t pos = 0;
  while ((pos = path.find('/', pos + 1)) != std::string::npos) {
    std::string sub = path.substr(0, pos);
    if (sub.empty()) {
      continue;
    }
    mkdir(sub.c_str(), 0755);
  }
  return mkdir(path.c_str(), 0755) == 0 || DirExists(path);
}

bool EnsureBinary(const std::string& path) {
  return chmod(path.c_str(), 0755) == 0;
}

std::string GetProp(const std::string& key) {
#ifdef ANDROID
  std::string cmd = "getprop " + key;
  CommandResult res = ExecCommand({"/system/bin/sh", "-c", cmd}, true);
  return Trim(res.output);
#else
  (void)key;
  return "";
#endif
}

CommandResult ExecCommand(const std::vector<std::string>& argv, bool capture_output) {
  CommandResult result;
  if (argv.empty()) {
    return result;
  }

  int pipefd[2] = {-1, -1};
  if (capture_output && pipe(pipefd) != 0) {
    return result;
  }

  pid_t pid = fork();
  if (pid == 0) {
    if (capture_output) {
      dup2(pipefd[1], STDOUT_FILENO);
      close(pipefd[0]);
      close(pipefd[1]);
    }
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& arg : argv) {
      args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    execv(args[0], args.data());
    _exit(127);
  }

  if (pid < 0) {
    return result;
  }

  if (capture_output) {
    close(pipefd[1]);
    char buffer[4096];
    ssize_t n = 0;
    std::string out;
    while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
      out.append(buffer, n);
    }
    close(pipefd[0]);
    result.output = out;
  }

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
}

bool IsSafeMode(const std::string& superkey) {
  const auto persist = GetProp("persist.sys.safemode");
  const auto ro = GetProp("ro.sys.safemode");
  if (persist == "1" || ro == "1") {
    LOGI("safemode: true (prop)");
    return true;
  }
  if (superkey.empty()) {
    LOGW("[IsSafeMode] No superkey, assume false");
    return false;
  }
  long ret = ScSuGetSafemode(superkey);
  LOGI("kernel_safemode: %ld", ret);
  return ret == 1;
}

bool SwitchMntNs(int pid) {
#ifdef ANDROID
  std::string path = "/proc/" + std::to_string(pid) + "/ns/mnt";
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  int rc = setns(fd, CLONE_NEWNS);
  close(fd);
  return rc == 0;
#else
  (void)pid;
  return true;
#endif
}

static void SwitchCgroupOne(const char* grp, pid_t pid) {
  std::string path = std::string(grp) + "/cgroup.procs";
  int fd = open(path.c_str(), O_WRONLY | O_APPEND);
  if (fd < 0) {
    return;
  }
  std::string pid_str = std::to_string(pid);
  write(fd, pid_str.c_str(), pid_str.size());
  close(fd);
}

void SwitchCgroups() {
  pid_t pid = getpid();
  SwitchCgroupOne("/acct", pid);
  SwitchCgroupOne("/dev/cg2_bpf", pid);
  SwitchCgroupOne("/sys/fs/cgroup", pid);
  if (GetProp("ro.config.per_app_memcg") != "false") {
    SwitchCgroupOne("/dev/memcg/apps", pid);
  }
}

void Umask(unsigned int mask) {
  ::umask(mask);
}

bool HasMagisk() {
  CommandResult res = ExecCommand({"/system/bin/sh", "-c", "which magisk"}, false);
  return res.exit_code == 0;
}

const char* GetTmpPath() {
  if (DirExists(kTempDirLegacy)) {
    return kTempDirLegacy;
  }
  if (DirExists(kTempDir)) {
    return kTempDir;
  }
  return "";
}

std::string ReadFile(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) {
    return "";
  }
  std::stringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

bool WriteFile(const std::string& path, const std::string& data, bool append) {
  std::ofstream ofs;
  ofs.open(path, append ? std::ios::app : std::ios::out | std::ios::trunc);
  if (!ofs) {
    return false;
  }
  ofs << data;
  return true;
}

std::vector<std::string> SplitLines(const std::string& input) {
  std::vector<std::string> lines;
  std::stringstream ss(input);
  std::string line;
  while (std::getline(ss, line)) {
    lines.push_back(line);
  }
  return lines;
}

std::string Trim(const std::string& input) {
  size_t start = 0;
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
    start++;
  }
  size_t end = input.size();
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    end--;
  }
  return input.substr(start, end - start);
}

}  // namespace apd
