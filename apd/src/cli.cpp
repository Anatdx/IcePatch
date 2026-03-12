#include "cli.hpp"

#include "apd.hpp"
#include "bootimg.hpp"
#include "defs.hpp"
#include "event.hpp"
#include "kallsyms.hpp"
#include "kpm.hpp"
#include "kpimg_inspect.hpp"
#include "log.hpp"
#include "module.hpp"
#include "sepolicy.hpp"
#include "supercall.hpp"
#include "utils.hpp"

#include <cstdio>
#include <algorithm>
#include <cctype>
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

std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool IsPolicyBoolText(const std::string& value) {
  const std::string lower = ToLowerAscii(value);
  return lower == "1" || lower == "0" || lower == "y" || lower == "n" ||
         lower == "yes" || lower == "no" || lower == "on" || lower == "off" ||
         lower == "true" || lower == "false" ||
         lower == "enable" || lower == "enabled" ||
         lower == "disable" || lower == "disabled";
}

bool NormalizePolicySpec(const std::string& spec,
                         std::string* out_addition,
                         std::string* out_error) {
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
    if (value != "minimal" && value != "legacy" &&
        value != "full" && value != "no-su") {
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
      "  inspect --image <kernel_image>\n"
      "  tool inspect --image <kernel_image>\n"
      "  tool inspect --kpimg <kpimg>\n"
      "  tool list --image <kernel_image>\n"
      "  tool list --kpimg <kpimg>\n"
      "  tool list --kpm <kpm_file>\n"
      "  tool ksym-probe --image <kernel_image>\n"
      "  tool dump --image <kernel_image>\n"
      "  tool flag --image <kernel_image>\n"
      "  tool boot unpack --boot <boot.img> --out-kernel <kernel>\n"
      "  tool boot repack --boot <orig_boot.img> --kernel <kernel> --out <new_boot.img>\n"
      "    note: unpack supports raw/gzip/lz4(frame+legacy)/zstd/bzip2/xz/lzma; repack supports raw/gzip/lz4(frame+legacy)/zstd/bzip2\n"
      "  tool version --kpimg <kpimg>\n"
      "  tool patch --image <kernel_image> --kpimg <kpimg> --out <output> --skey <key> [--dry-run]\n"
      "    optional: --reuse-config-from <patched_kernel_image> --root-skey --addition <KEY=VALUE>\n"
      "              --policy <minimal|legacy|full|no-su|KEY=VALUE>\n"
      "              --policy-profile <minimal|legacy|full> --policy-no-su\n"
      "              --embed-extra-path <PATH> | --embeded-extra-name <NAME>\n"
      "              --extra-type <kpm|shell|exec|raw|android_rc> --extra-name <NAME>\n"
      "              --extra-event <EVENT> --extra-args <ARGS> --extra-detach\n"
      "    note: non-dry-run needs symbol config source (patched input or reuse-config)\n"
      "  tool unpatch --image <kernel_image> --out <output>\n"
      "  tool reset-key --image <kernel_image> --out <output> --skey <key>\n"
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
    const std::string& sub = args[1];
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
      const std::string& boot_sub = args[2];
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
      PatchExtraConfig* cur_extra = nullptr;
      auto append_policy = [&](const std::string& spec) -> bool {
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
      if (image_path.empty() || kpimg_path.empty() || out_path.empty() || skey.empty()) {
        PrintUsage();
        return 1;
      }
      ok = PatchKernelImage(
          image_path, kpimg_path, out_path, skey, reuse_config_from, dry_run, root_superkey, additions, extra_configs);
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
