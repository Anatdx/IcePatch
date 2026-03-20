#include "cli.hpp"

#include "apd.hpp"
#include "assets.hpp"
#include "bootimg.hpp"
#include "defs.hpp"
#include "event.hpp"
#include "kallsyms.hpp"
#include "kpimg_inspect.hpp"
#include "kpm.hpp"
#include "log.hpp"
#include "module.hpp"
#include "package.hpp"
#include "restorecon.hpp"
#include "sepolicy.hpp"
#include "supercall.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace apd {

namespace {
constexpr const char *kPackageConfigHeader =
    "pkg,exclude,allow,uid,to_uid,sctx";
constexpr const char *kApVersionPath = "/data/adb/ap/version";
constexpr const char *kApSuPath = "/data/adb/ap/su_path";
constexpr const char *kApLinkPath = "/data/adb/ap/bin/apd";
constexpr const char *kKpatchPath = "/data/adb/kpatch";
constexpr const char *kLegacySuPath = "/system/bin/su";

const char *BoolText(bool value);

bool EndsWith(const std::string &value, const std::string &suffix) {
  if (value.size() < suffix.size()) {
    return false;
  }
  return value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
         0;
}

std::string Basename(const std::string &path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

std::string Dirname(const std::string &path) {
  const size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return ".";
  }
  if (pos == 0) {
    return "/";
  }
  return path.substr(0, pos);
}

std::string ReadSelfPath() {
  char buf[PATH_MAX] = {0};
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len <= 0) {
    return "";
  }
  buf[len] = '\0';
  return std::string(buf);
}

bool ParseIntValue(const std::string &raw, int *out) {
  if (out == nullptr) {
    return false;
  }
  try {
    size_t idx = 0;
    int value = std::stoi(raw, &idx);
    if (idx != raw.size()) {
      return false;
    }
    *out = value;
    return true;
  } catch (...) {
    return false;
  }
}

bool CopyFileWithMode(const std::string &from, const std::string &to,
                      mode_t mode, std::string *out_error) {
  if (!FileExists(from)) {
    if (out_error != nullptr) {
      *out_error = "missing source file: " + from;
    }
    return false;
  }
  if (!EnsureDirExists(Dirname(to))) {
    if (out_error != nullptr) {
      *out_error = "failed to create parent dir for: " + to;
    }
    return false;
  }
  const std::string tmp = to + ".tmp";
  unlink(tmp.c_str());
  std::ifstream ifs(from, std::ios::binary);
  if (!ifs) {
    if (out_error != nullptr) {
      *out_error = "failed to open source file: " + from;
    }
    return false;
  }
  std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
  if (!ofs) {
    if (out_error != nullptr) {
      *out_error = "failed to open temporary file: " + tmp;
    }
    return false;
  }
  ofs << ifs.rdbuf();
  ofs.flush();
  if (!ofs) {
    unlink(tmp.c_str());
    if (out_error != nullptr) {
      *out_error = "failed to write temporary file: " + tmp;
    }
    return false;
  }
  if (chmod(tmp.c_str(), mode) != 0) {
    unlink(tmp.c_str());
    if (out_error != nullptr) {
      *out_error = "chmod failed for: " + tmp;
    }
    return false;
  }
  if (rename(tmp.c_str(), to.c_str()) != 0) {
    unlink(tmp.c_str());
    if (out_error != nullptr) {
      *out_error = "failed to replace destination file: " + to;
    }
    return false;
  }
  return true;
}

void PrintApStatus() {
  const bool has_daemon = FileExists(kDaemonPath);
  const bool has_busybox = FileExists(kBusyboxPath);
  const bool has_magiskpolicy = FileExists(kMagiskPolicyPath);
  const bool has_resetprop = FileExists(kResetpropPath);
  const bool installed =
      has_daemon && has_busybox && has_magiskpolicy && has_resetprop;
  std::fprintf(stdout, "ap_installed             %s\n", BoolText(installed));
  std::fprintf(stdout, "ap_has_daemon            %s\n", BoolText(has_daemon));
  std::fprintf(stdout, "ap_has_busybox           %s\n", BoolText(has_busybox));
  std::fprintf(stdout, "ap_has_magiskpolicy      %s\n",
               BoolText(has_magiskpolicy));
  std::fprintf(stdout, "ap_has_resetprop         %s\n",
               BoolText(has_resetprop));
  std::fprintf(stdout, "ap_version_code          %s\n",
               Trim(ReadFile(kApVersionPath)).c_str());
  std::fprintf(stdout, "ap_su_path               %s\n",
               Trim(ReadFile(kApSuPath)).c_str());
}

bool InstallApArtifacts(long manager_version, std::string *out_error) {
  const std::string self_path = ReadSelfPath();
  if (self_path.empty()) {
    if (out_error != nullptr) {
      *out_error = "failed to resolve /proc/self/exe";
    }
    return false;
  }
  const std::string lib_dir = Dirname(self_path);

  struct InstallItem {
    const char *src_name;
    const char *dst_path;
  };
  const InstallItem items[] = {
      {"libapd.so", kDaemonPath},
      {"libmagiskpolicy.so", kMagiskPolicyPath},
      {"libresetprop.so", kResetpropPath},
      {"libbusybox.so", kBusyboxPath},
  };

  if (!EnsureDirExists(kBinaryDir) || !EnsureDirExists(kLogDir)) {
    if (out_error != nullptr) {
      *out_error = "failed to create /data/adb/ap directories";
    }
    return false;
  }

  for (const auto &item : items) {
    const std::string src = lib_dir + "/" + item.src_name;
    if (!CopyFileWithMode(src, item.dst_path, 0755, out_error)) {
      return false;
    }
  }

  unlink(kApLinkPath);
  if (symlink(kDaemonPath, kApLinkPath) != 0 && !FileExists(kApLinkPath)) {
    if (out_error != nullptr) {
      *out_error = "failed to create apd symlink";
    }
    return false;
  }

  if (!EnsureFileExists("/data/adb/ap/package_config")) {
    if (out_error != nullptr) {
      *out_error = "failed to ensure package_config";
    }
    return false;
  }
  if (!EnsureFileExists(kApSuPath)) {
    if (out_error != nullptr) {
      *out_error = "failed to ensure su_path";
    }
    return false;
  }
  if (Trim(ReadFile(kApSuPath)).empty() &&
      !WriteFile(kApSuPath, std::string(kLegacySuPath) + "\n")) {
    if (out_error != nullptr) {
      *out_error = "failed to initialize su_path";
    }
    return false;
  }
  if (!WriteFile(kApVersionPath, std::to_string(manager_version) + "\n")) {
    if (out_error != nullptr) {
      *out_error = "failed to write version file";
    }
    return false;
  }

  ExecCommand({"/system/bin/sh", "-c",
               "restorecon /data/adb/apd 2>/dev/null; "
               "restorecon -R /data/adb/ap 2>/dev/null"},
              false);
  Restorecon();
  ExecCommand({kMagiskPolicyPath, "--magisk", "--live"}, false);
  return true;
}

bool UninstallApArtifacts(std::string *out_error) {
  const CommandResult res = ExecCommand(
      {"/system/bin/sh", "-c",
       "rm -f /data/adb/apd /data/adb/kpatch; "
       "rm -rf /data/adb/ap/bin /data/adb/ap/log /data/adb/ap/version"},
      false);
  if (res.exit_code != 0) {
    if (out_error != nullptr) {
      *out_error =
          "rm command failed with exit code " + std::to_string(res.exit_code);
    }
    return false;
  }
  return true;
}

void PrintPackageConfigCsv() {
  std::fprintf(stdout, "%s\n", kPackageConfigHeader);
  for (const auto &cfg : ReadApPackageConfig()) {
    std::fprintf(stdout, "%s,%d,%d,%d,%d,%s\n", cfg.pkg.c_str(), cfg.exclude,
                 cfg.allow, cfg.uid, cfg.to_uid, cfg.sctx.c_str());
  }
}

std::string ToLowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool IsPolicyBoolText(const std::string &value) {
  const std::string lower = ToLowerAscii(value);
  return lower == "1" || lower == "0" || lower == "y" || lower == "n" ||
         lower == "yes" || lower == "no" || lower == "on" || lower == "off" ||
         lower == "true" || lower == "false" || lower == "enable" ||
         lower == "enabled" || lower == "disable" || lower == "disabled";
}

bool NormalizePolicySpec(const std::string &spec, std::string *out_addition,
                         std::string *out_error) {
  if (out_addition == nullptr || out_error == nullptr) {
    return false;
  }
  *out_addition = "";
  *out_error = "";

  std::string raw = ToLowerAscii(spec);
  if (raw.empty()) {
    *out_error = "empty policy";
    return false;
  }

  if (raw == "minimal" || raw == "legacy" || raw == "full") {
    *out_addition = "policy=" + raw;
    return true;
  }
  if (raw == "no-su" || raw == "no_su" || raw == "nosu") {
    *out_addition = "mode=no-su";
    return true;
  }

  const size_t eq = raw.find('=');
  if (eq == std::string::npos || eq == 0 || eq + 1 >= raw.size()) {
    *out_error = "expect minimal|legacy|full|no-su or KEY=VALUE";
    return false;
  }

  const std::string key = raw.substr(0, eq);
  const std::string value = raw.substr(eq + 1);
  if (key == "policy" || key == "profile") {
    if (value != "minimal" && value != "legacy" && value != "full") {
      *out_error = "policy/profile must be minimal|legacy|full";
      return false;
    }
    *out_addition = "policy=" + value;
    return true;
  }

  if (key == "mode") {
    if (value != "minimal" && value != "legacy" && value != "full" &&
        value != "no-su") {
      *out_error = "mode must be minimal|legacy|full|no-su";
      return false;
    }
    *out_addition = key + "=" + value;
    return true;
  }

  if (key == "no_su") {
    if (!IsPolicyBoolText(value)) {
      *out_error = "no_su must be boolean";
      return false;
    }
    *out_addition = key + "=" + value;
    return true;
  }

  if (key.rfind("feature.", 0) == 0) {
    if (key.size() <= 8) {
      *out_error = "missing feature name after feature.";
      return false;
    }
    if (!IsPolicyBoolText(value)) {
      *out_error = "feature value must be boolean";
      return false;
    }
    *out_addition = key + "=" + value;
    return true;
  }

  *out_error = "unsupported policy key: " + key;
  return false;
}

constexpr int kPolicyProfileMinimal = 0;
constexpr int kPolicyProfileRootful = 1;
constexpr int kPolicyProfileNoSu = 2;
constexpr int kPolicyProfileFull = 3;

constexpr uint32_t kPolicyFlagKcfiBypass = (1u << 0);
constexpr uint32_t kPolicyFlagTaskObserver = (1u << 1);
constexpr uint32_t kPolicyFlagSelinuxBypass = (1u << 2);
constexpr uint32_t kPolicyFlagSupercall = (1u << 3);
constexpr uint32_t kPolicyFlagKstorage = (1u << 4);
constexpr uint32_t kPolicyFlagSu = (1u << 5);
constexpr uint32_t kPolicyFlagSuCompat = (1u << 6);
constexpr uint32_t kPolicyFlagAndroidUser = (1u << 7);
constexpr uint32_t kPolicyFlagFsApi = (1u << 8);

constexpr uint32_t kPolicyNoSuMask = (1u << 1) | // task_observer
                                     (1u << 2) | // selinux_bypass
                                     (1u << 5) | // su
                                     (1u << 6) | // su_compat
                                     (1u << 7);  // android_user

uint32_t PolicyFlagsForProfile(int profile) {
  switch (profile) {
  case kPolicyProfileMinimal:
  case kPolicyProfileNoSu:
    return kPolicyFlagKcfiBypass | kPolicyFlagSupercall | kPolicyFlagKstorage;
  case kPolicyProfileRootful:
    return kPolicyFlagKcfiBypass | kPolicyFlagTaskObserver |
           kPolicyFlagSelinuxBypass | kPolicyFlagSupercall |
           kPolicyFlagKstorage | kPolicyFlagSu | kPolicyFlagSuCompat |
           kPolicyFlagAndroidUser;
  case kPolicyProfileFull:
    return PolicyFlagsForProfile(kPolicyProfileRootful) | kPolicyFlagFsApi;
  default:
    return 0;
  }
}

bool ParsePolicyProfile(const std::string &raw, int *out_profile) {
  if (out_profile == nullptr) {
    return false;
  }
  const std::string profile = ToLowerAscii(raw);
  if (profile == "minimal") {
    *out_profile = kPolicyProfileMinimal;
    return true;
  }
  if (profile == "legacy" || profile == "rootful") {
    *out_profile = kPolicyProfileRootful;
    return true;
  }
  if (profile == "full") {
    *out_profile = kPolicyProfileFull;
    return true;
  }
  if (profile == "no-su" || profile == "nosu" || profile == "no_su") {
    *out_profile = kPolicyProfileNoSu;
    return true;
  }
  return false;
}

const char *BoolText(bool value) { return value ? "true" : "false"; }

void PrintPolicyFlags(uint32_t flags) {
  auto print_feature = [&](const char *key, uint32_t bit) {
    std::fprintf(stdout, "%-24s%s\n", key, BoolText((flags & bit) != 0));
  };
  std::fprintf(stdout, "policy_flags             0x%08x\n", flags);
  print_feature("feature.kcfi_bypass", (1u << 0));
  print_feature("feature.task_observer", (1u << 1));
  print_feature("feature.selinux_bypass", (1u << 2));
  print_feature("feature.supercall", (1u << 3));
  print_feature("feature.kstorage", (1u << 4));
  print_feature("feature.su", (1u << 5));
  print_feature("feature.su_compat", (1u << 6));
  print_feature("feature.android_user", (1u << 7));
  print_feature("feature.fs_api", (1u << 8));
}

void PrintUsage() {
  const char *usage =
      "Usage:\n"
      "  apd [--superkey KEY] <command>\n"
      "Commands:\n"
      "  module <install|uninstall|enable|disable|action|lua|list>\n"
      "  post-fs-data\n"
      "  services\n"
      "  boot-completed\n"
      "  uid-listener\n"
      "  inspect --image <kernel_image>\n"
      "  tool inspect --image <kernel_image>\n"
      "  tool inspect --kpimg <kpimg>\n"
      "  tool list --image <kernel_image>\n"
      "  tool list --kpimg <kpimg>\n"
      "  tool list --kpm <kpm_file>\n"
      "  tool ksym-probe --image <kernel_image>\n"
      "  tool policy get\n"
      "  tool policy apply --profile <minimal|legacy|full> [--no-su]\n"
      "  tool policy apply --no-su\n"
      "  tool ap status\n"
      "  tool ap install --manager-version <version_code>\n"
      "  tool ap uninstall\n"
      "  tool package-config list\n"
      "  tool package-config set --pkg <name> --exclude <0|1> --allow <0|1> "
      "--uid <uid> --to-uid <uid> [--sctx <ctx>]\n"
      "  tool su-path get\n"
      "  tool su-path set --path <path>\n"
      "  tool dump --image <kernel_image>\n"
      "  tool flag --image <kernel_image>\n"
      "  tool boot unpack --boot <boot.img> --out-kernel <kernel>\n"
      "  tool boot repack --boot <orig_boot.img> --kernel <kernel> --out "
      "<new_boot.img>\n"
      "    note: unpack supports "
      "raw/gzip/lz4(frame+legacy)/zstd/bzip2/xz/lzma; repack supports "
      "raw/gzip/lz4(frame+legacy)/zstd/bzip2\n"
      "  tool version --kpimg <kpimg>\n"
      "  tool patch --image <kernel_image> --kpimg <kpimg> --out <output> "
      "--skey <key> [--dry-run]\n"
      "    optional: --reuse-config-from <patched_kernel_image> --root-skey "
      "--addition <KEY=VALUE>\n"
      "              --policy <minimal|legacy|full|no-su|KEY=VALUE>\n"
      "              --policy-profile <minimal|legacy|full> --policy-no-su\n"
      "              --embed-extra-path <PATH> | --embeded-extra-name <NAME>\n"
      "              --extra-type <kpm|shell|exec|raw|android_rc> --extra-name "
      "<NAME>\n"
      "              --extra-event <EVENT> --extra-args <ARGS> --extra-detach\n"
      "    note: non-dry-run needs symbol config source (patched input or "
      "reuse-config)\n"
      "  tool unpatch --image <kernel_image> --out <output>\n"
      "  tool reset-key --image <kernel_image> --out <output> --skey <key>\n"
      "  sepolicy check <policy>\n";
  std::fprintf(stdout, "%s", usage);
}

} // namespace

int RunCli(int argc, char **argv) {
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

  const std::string &cmd = args[0];
  bool ok = false;

  if (cmd == "post-fs-data") {
    ok = OnPostDataFs(superkey);
  } else if (cmd == "services") {
    ok = OnServices(superkey);
  } else if (cmd == "boot-completed") {
    ok = OnBootCompleted(superkey);
  } else if (cmd == "uid-listener") {
    ok = StartUidListener();
  } else if (cmd == "inspect") {
    if (args.size() != 3 || args[1] != "--image") {
      PrintUsage();
      return 1;
    }
    ok = InspectKernelImage(args[2]);
  } else if (cmd == "tool") {
    if (args.size() < 2) {
      PrintUsage();
      return 1;
    }
    const std::string &sub = args[1];
    if (sub == "inspect") {
      if (args.size() != 4) {
        PrintUsage();
        return 1;
      }
      if (args[2] == "--image") {
        ok = InspectKernelImage(args[3]);
      } else if (args[2] == "--kpimg") {
        ok = InspectKernelPatchImage(args[3]);
      } else {
        PrintUsage();
        return 1;
      }
    } else if (sub == "ksym-probe") {
      if (args.size() != 4 || args[2] != "--image") {
        PrintUsage();
        return 1;
      }
      ok = PrintKallsymsProbe(args[3]);
    } else if (sub == "policy") {
      if (args.size() < 3) {
        PrintUsage();
        return 1;
      }
      if (superkey.empty()) {
        std::fprintf(stderr, "policy failed: missing --superkey\n");
        return 1;
      }
      const std::string &policy_sub = args[2];
      if (policy_sub == "get") {
        if (args.size() != 3) {
          PrintUsage();
          return 1;
        }
        long flags_rc = ScPolicyGetFlags(superkey);
        if (flags_rc < 0) {
          std::fprintf(stderr, "policy get failed: %ld\n", flags_rc);
          return 1;
        }
        PrintPolicyFlags(static_cast<uint32_t>(flags_rc));
        ok = true;
      } else if (policy_sub == "apply") {
        int profile = -1;
        bool no_su = false;
        for (size_t i = 3; i < args.size();) {
          if (args[i] == "--no-su") {
            no_su = true;
            ++i;
            continue;
          }
          if (args[i] == "--profile" && i + 1 < args.size()) {
            if (!ParsePolicyProfile(args[i + 1], &profile)) {
              std::fprintf(stderr,
                           "policy apply failed: invalid profile '%s'\n",
                           args[i + 1].c_str());
              return 1;
            }
            i += 2;
            continue;
          }
          PrintUsage();
          return 1;
        }

        if (profile < 0 && !no_su) {
          std::fprintf(stderr, "policy apply failed: provide --profile "
                               "<minimal|legacy|full> or --no-su\n");
          return 1;
        }

        long before_rc = ScPolicyGetFlags(superkey);
        if (before_rc < 0) {
          std::fprintf(stderr,
                       "policy apply failed to read current flags: %ld\n",
                       before_rc);
          return 1;
        }

        uint32_t target_flags = static_cast<uint32_t>(before_rc);
        if (profile >= 0) {
          target_flags = PolicyFlagsForProfile(profile);
        }
        if (no_su) {
          target_flags &= ~kPolicyNoSuMask;
        }

        long after_rc = ScPolicyApplyFlags(superkey, target_flags);
        if (after_rc < 0) {
          std::fprintf(stderr, "policy apply failed (flags=0x%08x): %ld\n",
                       target_flags, after_rc);
          return 1;
        }

        long final_rc = ScPolicyGetFlags(superkey);
        if (final_rc < 0) {
          final_rc = after_rc;
        }
        std::fprintf(stdout, "policy_before_flags      0x%08x\n",
                     static_cast<uint32_t>(before_rc));
        std::fprintf(stdout, "policy_after_flags       0x%08x\n",
                     static_cast<uint32_t>(final_rc));
        std::fprintf(stdout, "policy_profile_applied   %s\n",
                     profile >= 0 ? BoolText(true) : BoolText(false));
        std::fprintf(stdout, "policy_no_su_applied     %s\n", BoolText(no_su));
        PrintPolicyFlags(static_cast<uint32_t>(final_rc));
        ok = true;
      } else {
        PrintUsage();
        return 1;
      }
    } else if (sub == "ap") {
      if (superkey.empty()) {
        std::fprintf(stderr, "ap failed: missing --superkey\n");
        return 1;
      }
      if (args.size() < 3) {
        PrintUsage();
        return 1;
      }
      const std::string &ap_sub = args[2];
      if (ap_sub == "status") {
        if (args.size() != 3) {
          PrintUsage();
          return 1;
        }
        PrintApStatus();
        ok = true;
      } else if (ap_sub == "install") {
        long manager_version = 0;
        for (size_t i = 3; i < args.size();) {
          if (args[i] == "--manager-version" && i + 1 < args.size()) {
            try {
              manager_version = std::stol(args[i + 1]);
            } catch (...) {
              std::fprintf(stderr, "ap install failed: invalid version '%s'\n",
                           args[i + 1].c_str());
              return 1;
            }
            i += 2;
            continue;
          }
          PrintUsage();
          return 1;
        }
        std::string error;
        ok = InstallApArtifacts(manager_version, &error);
        if (!ok) {
          std::fprintf(stderr, "ap install failed: %s\n", error.c_str());
          return 1;
        }
        PrintApStatus();
      } else if (ap_sub == "uninstall") {
        if (args.size() != 3) {
          PrintUsage();
          return 1;
        }
        std::string error;
        ok = UninstallApArtifacts(&error);
        if (!ok) {
          std::fprintf(stderr, "ap uninstall failed: %s\n", error.c_str());
          return 1;
        }
        PrintApStatus();
      } else {
        PrintUsage();
        return 1;
      }
    } else if (sub == "package-config") {
      if (superkey.empty()) {
        std::fprintf(stderr, "package-config failed: missing --superkey\n");
        return 1;
      }
      if (args.size() < 3) {
        PrintUsage();
        return 1;
      }
      const std::string &pkg_sub = args[2];
      if (pkg_sub == "list") {
        if (args.size() != 3) {
          PrintUsage();
          return 1;
        }
        PrintPackageConfigCsv();
        ok = true;
      } else if (pkg_sub == "set") {
        PackageConfig config;
        config.sctx = "u:r:untrusted_app:s0";
        for (size_t i = 3; i < args.size();) {
          if (args[i] == "--pkg" && i + 1 < args.size()) {
            config.pkg = args[i + 1];
            i += 2;
            continue;
          }
          if (args[i] == "--exclude" && i + 1 < args.size()) {
            if (!ParseIntValue(args[i + 1], &config.exclude)) {
              std::fprintf(stderr, "package-config failed: invalid exclude\n");
              return 1;
            }
            i += 2;
            continue;
          }
          if (args[i] == "--allow" && i + 1 < args.size()) {
            if (!ParseIntValue(args[i + 1], &config.allow)) {
              std::fprintf(stderr, "package-config failed: invalid allow\n");
              return 1;
            }
            i += 2;
            continue;
          }
          if (args[i] == "--uid" && i + 1 < args.size()) {
            if (!ParseIntValue(args[i + 1], &config.uid)) {
              std::fprintf(stderr, "package-config failed: invalid uid\n");
              return 1;
            }
            i += 2;
            continue;
          }
          if (args[i] == "--to-uid" && i + 1 < args.size()) {
            if (!ParseIntValue(args[i + 1], &config.to_uid)) {
              std::fprintf(stderr, "package-config failed: invalid to-uid\n");
              return 1;
            }
            i += 2;
            continue;
          }
          if (args[i] == "--sctx" && i + 1 < args.size()) {
            config.sctx = args[i + 1];
            i += 2;
            continue;
          }
          PrintUsage();
          return 1;
        }
        if (config.pkg.empty() || config.uid <= 0) {
          std::fprintf(
              stderr,
              "package-config failed: missing --pkg or invalid --uid\n");
          return 1;
        }
        if (config.allow == 1) {
          config.exclude = 0;
        }
        ok = UpsertApPackageConfig(config);
        if (!ok) {
          std::fprintf(stderr, "package-config failed: write error\n");
          return 1;
        }
        PrintPackageConfigCsv();
      } else {
        PrintUsage();
        return 1;
      }
    } else if (sub == "su-path") {
      if (superkey.empty()) {
        std::fprintf(stderr, "su-path failed: missing --superkey\n");
        return 1;
      }
      if (args.size() < 3) {
        PrintUsage();
        return 1;
      }
      const std::string &su_sub = args[2];
      if (su_sub == "get") {
        if (args.size() != 3) {
          PrintUsage();
          return 1;
        }
        std::fprintf(stdout, "su_path                 %s\n",
                     Trim(ReadFile(kApSuPath)).c_str());
        ok = true;
      } else if (su_sub == "set") {
        std::string path;
        for (size_t i = 3; i < args.size();) {
          if (args[i] == "--path" && i + 1 < args.size()) {
            path = args[i + 1];
            i += 2;
            continue;
          }
          PrintUsage();
          return 1;
        }
        if (path.empty()) {
          std::fprintf(stderr, "su-path failed: missing --path\n");
          return 1;
        }
        if (!EnsureDirExists(kWorkingDir) ||
            !WriteFile(kApSuPath, path + "\n")) {
          std::fprintf(stderr, "su-path failed: unable to write file\n");
          return 1;
        }
        const long rc = ScSuResetPath(superkey, path);
        std::fprintf(stdout, "su_path                 %s\n", path.c_str());
        std::fprintf(stdout, "su_path_reset_rc        %ld\n", rc);
        ok = rc == 0;
      } else {
        PrintUsage();
        return 1;
      }
    } else if (sub == "dump") {
      if (args.size() != 4 || args[2] != "--image") {
        PrintUsage();
        return 1;
      }
      ok = DumpKallsyms(args[3]);
    } else if (sub == "flag") {
      if (args.size() != 4 || args[2] != "--image") {
        PrintUsage();
        return 1;
      }
      ok = DumpIkconfig(args[3]);
    } else if (sub == "list") {
      if (args.size() != 4) {
        PrintUsage();
        return 1;
      }
      if (args[2] == "--image") {
        ok = PrintKernelPatchListImage(args[3]);
      } else if (args[2] == "--kpimg") {
        ok = PrintKernelPatchListKpimg(args[3]);
      } else if (args[2] == "--kpm") {
        ok = PrintKpmInfoPath(args[3]);
      } else {
        PrintUsage();
        return 1;
      }
    } else if (sub == "boot") {
      if (args.size() < 3) {
        PrintUsage();
        return 1;
      }
      const std::string &boot_sub = args[2];
      if (boot_sub == "unpack") {
        std::string boot_path;
        std::string out_kernel_path;
        for (size_t i = 3; i + 1 < args.size(); i += 2) {
          if (args[i] == "--boot") {
            boot_path = args[i + 1];
          } else if (args[i] == "--out-kernel") {
            out_kernel_path = args[i + 1];
          } else {
            PrintUsage();
            return 1;
          }
        }
        if (boot_path.empty() || out_kernel_path.empty()) {
          PrintUsage();
          return 1;
        }
        ok = BootUnpackKernel(boot_path, out_kernel_path);
      } else if (boot_sub == "repack") {
        std::string boot_path;
        std::string kernel_path;
        std::string out_path;
        for (size_t i = 3; i + 1 < args.size(); i += 2) {
          if (args[i] == "--boot") {
            boot_path = args[i + 1];
          } else if (args[i] == "--kernel") {
            kernel_path = args[i + 1];
          } else if (args[i] == "--out") {
            out_path = args[i + 1];
          } else {
            PrintUsage();
            return 1;
          }
        }
        if (boot_path.empty() || kernel_path.empty() || out_path.empty()) {
          PrintUsage();
          return 1;
        }
        ok = BootRepackKernel(boot_path, kernel_path, out_path);
      } else {
        PrintUsage();
        return 1;
      }
    } else if (sub == "version") {
      if (args.size() != 4 || args[2] != "--kpimg") {
        PrintUsage();
        return 1;
      }
      ok = PrintKernelPatchVersion(args[3]);
    } else if (sub == "patch") {
      if (args.size() < 9) {
        PrintUsage();
        return 1;
      }
      std::string image_path;
      std::string kpimg_path;
      std::string out_path;
      std::string skey;
      std::string reuse_config_from;
      bool dry_run = false;
      bool root_superkey = false;
      std::vector<std::string> additions;
      std::vector<PatchExtraConfig> extra_configs;
      PatchExtraConfig *cur_extra = nullptr;
      auto append_policy = [&](const std::string &spec) -> bool {
        std::string addition;
        std::string error;
        if (!NormalizePolicySpec(spec, &addition, &error)) {
          std::fprintf(stderr, "patch failed: invalid --policy '%s': %s\n",
                       spec.c_str(), error.c_str());
          return false;
        }
        additions.push_back(addition);
        return true;
      };
      for (size_t i = 2; i < args.size();) {
        if (args[i] == "--dry-run") {
          dry_run = true;
          ++i;
          continue;
        }
        if (args[i] == "--root-skey") {
          root_superkey = true;
          ++i;
          continue;
        }
        if (args[i] == "--extra-detach") {
          if (cur_extra == nullptr) {
            PrintUsage();
            return 1;
          }
          cur_extra->detach = true;
          ++i;
          continue;
        }
        if (args[i] == "--policy-no-su") {
          if (!append_policy("mode=no-su")) {
            return 1;
          }
          ++i;
          continue;
        }
        if (i + 1 >= args.size()) {
          PrintUsage();
          return 1;
        }
        if (args[i] == "--image") {
          image_path = args[i + 1];
        } else if (args[i] == "--kpimg") {
          kpimg_path = args[i + 1];
        } else if (args[i] == "--out") {
          out_path = args[i + 1];
        } else if (args[i] == "--skey") {
          skey = args[i + 1];
        } else if (args[i] == "--reuse-config-from") {
          reuse_config_from = args[i + 1];
        } else if (args[i] == "--addition") {
          additions.push_back(args[i + 1]);
        } else if (args[i] == "--policy") {
          if (!append_policy(args[i + 1])) {
            return 1;
          }
        } else if (args[i] == "--policy-profile") {
          if (!append_policy("policy=" + args[i + 1])) {
            return 1;
          }
        } else if (args[i] == "--embed-extra-path") {
          PatchExtraConfig cfg;
          cfg.is_path = true;
          cfg.path = args[i + 1];
          extra_configs.push_back(cfg);
          cur_extra = &extra_configs.back();
        } else if (args[i] == "--embeded-extra-name") {
          PatchExtraConfig cfg;
          cfg.is_path = false;
          cfg.existing_name = args[i + 1];
          extra_configs.push_back(cfg);
          cur_extra = &extra_configs.back();
        } else if (args[i] == "--extra-type") {
          if (cur_extra == nullptr) {
            PrintUsage();
            return 1;
          }
          cur_extra->extra_type = args[i + 1];
        } else if (args[i] == "--extra-name") {
          if (cur_extra == nullptr) {
            PrintUsage();
            return 1;
          }
          cur_extra->set_name = args[i + 1];
        } else if (args[i] == "--extra-event") {
          if (cur_extra == nullptr) {
            PrintUsage();
            return 1;
          }
          cur_extra->set_event = args[i + 1];
        } else if (args[i] == "--extra-args") {
          if (cur_extra == nullptr) {
            PrintUsage();
            return 1;
          }
          cur_extra->set_args = args[i + 1];
        } else {
          PrintUsage();
          return 1;
        }
        i += 2;
      }
      if (image_path.empty() || kpimg_path.empty() || out_path.empty() ||
          skey.empty()) {
        PrintUsage();
        return 1;
      }
      ok = PatchKernelImage(image_path, kpimg_path, out_path, skey,
                            reuse_config_from, dry_run, root_superkey,
                            additions, extra_configs);
    } else if (sub == "unpatch") {
      if (args.size() != 6) {
        PrintUsage();
        return 1;
      }
      std::string image_path;
      std::string out_path;
      for (size_t i = 2; i + 1 < args.size(); i += 2) {
        if (args[i] == "--image") {
          image_path = args[i + 1];
        } else if (args[i] == "--out") {
          out_path = args[i + 1];
        } else {
          PrintUsage();
          return 1;
        }
      }
      if (image_path.empty() || out_path.empty()) {
        PrintUsage();
        return 1;
      }
      ok = UnpatchKernelImage(image_path, out_path);
    } else if (sub == "reset-key") {
      if (args.size() != 8) {
        PrintUsage();
        return 1;
      }
      std::string image_path;
      std::string out_path;
      std::string skey;
      for (size_t i = 2; i + 1 < args.size(); i += 2) {
        if (args[i] == "--image") {
          image_path = args[i + 1];
        } else if (args[i] == "--out") {
          out_path = args[i + 1];
        } else if (args[i] == "--skey") {
          skey = args[i + 1];
        } else {
          PrintUsage();
          return 1;
        }
      }
      if (image_path.empty() || out_path.empty() || skey.empty()) {
        PrintUsage();
        return 1;
      }
      ok = ResetKernelSuperKey(image_path, out_path, skey);
    } else {
      PrintUsage();
      return 1;
    }
  } else if (cmd == "module") {
    if (args.size() < 2) {
      PrintUsage();
      return 1;
    }
    if (!SwitchMntNs(1)) {
      LOGE("Failed to switch to global mount namespace");
      return 1;
    }
    const std::string &sub = args[1];
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

} // namespace apd
