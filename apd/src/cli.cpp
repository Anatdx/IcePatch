#include "cli.hpp"

#include "apd.hpp"
#include "defs.hpp"
#include "event.hpp"
#include "log.hpp"
#include "module.hpp"
#include "sepolicy.hpp"
#include "supercall.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace apd {

namespace {
bool EndsWith(const std::string& value, const std::string& suffix) {
  if (value.size() < suffix.size()) {
    return false;
  }
  return value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string Basename(const std::string& path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

void PrintUsage() {
  const char* usage =
      "Usage:\n"
      "  apd [--superkey KEY] <command>\n"
      "Commands:\n"
      "  module <install|uninstall|enable|disable|action|lua|list>\n"
      "  post-fs-data\n"
      "  services\n"
      "  boot-completed\n"
      "  uid-listener\n"
      "  sepolicy check <policy>\n";
  std::fprintf(stdout, "%s", usage);
}

}  // namespace

int RunCli(int argc, char** argv) {
  if (argc <= 0 || argv == nullptr) {
    return 1;
  }

  std::string arg0 = argv[0] ? argv[0] : "";
  if (EndsWith(arg0, "kp") || EndsWith(arg0, "su")) {
    return RootShell(argc, argv);
  }

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintUsage();
      return 0;
    }
    if (arg == "-V" || arg == "--version") {
      std::string name = Basename(argv[0] ? argv[0] : "apd");
      std::fprintf(stdout, "%s %s\n", name.c_str(), kVersionCode);
      return 0;
    }
  }

  std::string superkey;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if ((arg == "-s" || arg == "--superkey") && i + 1 < argc) {
      superkey = argv[++i];
      continue;
    }
    args.push_back(arg);
  }

  if (!superkey.empty()) {
    PrivilegeApdProfile(superkey);
  }

  if (args.empty()) {
    PrintUsage();
    return 1;
  }

  const std::string& cmd = args[0];
  bool ok = false;

  if (cmd == "post-fs-data") {
    ok = OnPostDataFs(superkey);
  } else if (cmd == "services") {
    ok = OnServices(superkey);
  } else if (cmd == "boot-completed") {
    ok = OnBootCompleted(superkey);
  } else if (cmd == "uid-listener") {
    ok = StartUidListener();
  } else if (cmd == "module") {
    if (args.size() < 2) {
      PrintUsage();
      return 1;
    }
    const std::string& sub = args[1];
    if (sub == "install" && args.size() >= 3) {
      ok = InstallModule(args[2]);
    } else if (sub == "uninstall" && args.size() >= 3) {
      ok = UninstallModule(args[2]);
    } else if (sub == "enable" && args.size() >= 3) {
      ok = EnableModule(args[2]);
    } else if (sub == "disable" && args.size() >= 3) {
      ok = DisableModule(args[2]);
    } else if (sub == "action" && args.size() >= 3) {
      ok = RunAction(args[2]);
    } else if (sub == "lua" && args.size() >= 4) {
      ok = RunLua(args[2], args[3], false, true);
    } else if (sub == "list") {
      ok = ListModules();
    } else {
      PrintUsage();
      return 1;
    }
  } else if (cmd == "sepolicy") {
    if (args.size() >= 3 && args[1] == "check") {
      ok = CheckSepolicyRule(args[2]);
    } else {
      PrintUsage();
      return 1;
    }
  } else {
    PrintUsage();
    return 1;
  }

  if (!ok) {
    LOGE("Command failed");
    return 1;
  }
  return 0;
}

}  // namespace apd
