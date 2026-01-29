#include "apd.hpp"

#include "defs.hpp"
#include "log.hpp"
#include "pty.hpp"
#include "utils.hpp"

#include <cerrno>
#include <cstring>
#include <vector>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace apd {

namespace {
void PrintUsage() {
  const char* brief =
      "IcePatch\n\nUsage: <command> [options] [-] [user [argument...]]";
  std::fprintf(stdout, "%s\n", brief);
}

void SetIdentity(uid_t uid, gid_t gid) {
  setresgid(gid, gid, gid);
  setresuid(uid, uid, uid);
}

void AddPathEnv(const std::string& path) {
  const char* current = std::getenv("PATH");
  std::string value = current ? current : "";
  if (!value.empty()) {
    value += ":";
  }
  value += path;
  setenv("PATH", value.c_str(), 1);
}

}  // namespace

int RootShell(int argc, char** argv) {
  bool show_help = false;
  bool show_version = false;
  bool show_version_code = false;
  bool login_shell = false;
  bool preserve_env = false;
  bool mount_master = false;
  bool no_pty = false;
  std::string shell = "/system/bin/sh";
  std::string command;

  std::vector<std::string> free_args;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-mm") {
      mount_master = true;
      continue;
    }
    if (arg == "-cn") {
      no_pty = true;
      if (i + 1 < argc) {
        std::string joined;
        for (int j = i + 1; j < argc; ++j) {
          if (!joined.empty()) {
            joined.push_back(' ');
          }
          joined += argv[j];
        }
        command = joined;
        break;
      }
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      show_help = true;
    } else if (arg == "-v" || arg == "--version") {
      show_version = true;
    } else if (arg == "-V") {
      show_version_code = true;
    } else if (arg == "-l" || arg == "--login") {
      login_shell = true;
    } else if (arg == "-p" || arg == "--preserve-environment") {
      preserve_env = true;
    } else if (arg == "-M" || arg == "--mount-master") {
      mount_master = true;
    } else if (arg == "--no-pty") {
      no_pty = true;
    } else if (arg == "-s" || arg == "--shell") {
      if (i + 1 < argc) {
        shell = argv[++i];
      }
    } else if (arg == "-c" || arg == "--command") {
      if (i + 1 < argc) {
        std::string joined;
        for (int j = i + 1; j < argc; ++j) {
          if (!joined.empty()) {
            joined.push_back(' ');
          }
          joined += argv[j];
        }
        command = joined;
        break;
      }
    } else {
      free_args.push_back(arg);
    }
  }

  if (show_help) {
    PrintUsage();
    return 0;
  }
  if (show_version) {
    std::fprintf(stdout, "%s:IcePatch\n", kVersionName);
    return 0;
  }
  if (show_version_code) {
    std::fprintf(stdout, "%s\n", kVersionCode);
    return 0;
  }

  bool is_login = login_shell;
  size_t free_idx = 0;
  if (!free_args.empty() && free_args[0] == "-") {
    is_login = true;
    free_idx = 1;
  }

  uid_t uid = getuid();
  gid_t gid = getgid();
  if (free_idx < free_args.size()) {
    const std::string& name = free_args[free_idx];
    struct passwd* pw = getpwnam(name.c_str());
    if (pw) {
      uid = pw->pw_uid;
      gid = pw->pw_gid;
    } else {
      uid = static_cast<uid_t>(std::strtoul(name.c_str(), nullptr, 10));
    }
  }

  std::vector<const char*> exec_args;
  if (!command.empty()) {
    exec_args.push_back("-c");
    exec_args.push_back(command.c_str());
  }

  if (!preserve_env) {
    struct passwd* pw = getpwuid(uid);
    if (pw) {
      setenv("HOME", pw->pw_dir, 1);
      setenv("USER", pw->pw_name, 1);
      setenv("LOGNAME", pw->pw_name, 1);
      setenv("SHELL", shell.c_str(), 1);
    }
  }

  AddPathEnv(kBinaryDir);

  if (FileExists(kApRcPath) && std::getenv("ENV") == nullptr) {
    setenv("ENV", kApRcPath, 1);
  }

#ifdef ANDROID
  if (!no_pty) {
    PreparePty();
  }
#endif

  Umask(022);
  SwitchCgroups();
  if (FileExists(kGlobalNamespaceFile) || mount_master) {
    SwitchMntNs(1);
  }
  SetIdentity(uid, gid);

  std::vector<char*> args;
  args.reserve(exec_args.size() + 2);
  const char* arg0 = is_login ? "-" : shell.c_str();
  args.push_back(const_cast<char*>(arg0));
  for (const char* a : exec_args) {
    args.push_back(const_cast<char*>(a));
  }
  args.push_back(nullptr);

  execv(shell.c_str(), args.data());
  return errno == 0 ? 1 : errno;
}

}  // namespace apd
