#include "event.hpp"

#include "assets.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "metamodule.hpp"
#include "module.hpp"
#include "restorecon.hpp"
#include "supercall.hpp"
#include "utils.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <poll.h>
#include <atomic>
#include <thread>

namespace apd {

namespace {
bool RunStage(const std::string& stage, const std::string& superkey, bool block) {
  Umask(0);
  if (HasMagisk()) {
    LOGW("Magisk detected, skip %s", stage.c_str());
    return true;
  }
  if (IsSafeMode(superkey)) {
    LOGW("safe mode, skip %s scripts", stage.c_str());
    DisableAllModules();
    return true;
  }
  if (!ExecMetamoduleStage(stage, block)) {
    LOGW("metamodule stage %s failed", stage.c_str());
  }
  if (!ExecCommonScripts(stage + ".d", block)) {
    LOGW("common %s scripts failed", stage.c_str());
  }
  if (!ExecStageScript(stage, block)) {
    LOGW("%s scripts failed", stage.c_str());
  }
  if (!ExecStageLua(stage, block, superkey)) {
    LOGW("%s lua failed", stage.c_str());
  }
  return true;
}

}  // namespace

bool OnPostDataFs(const std::string& superkey) {
  Umask(0);
  InitLoadPackageUidConfig(superkey);
  InitLoadSuPath(superkey);

  ForkForResult("/data/adb/ap/bin/magiskpolicy",
                {"/data/adb/ap/bin/magiskpolicy", "--magisk", "--live"}, superkey);

  PrivilegeApdProfile(superkey);

  if (HasMagisk()) {
    LOGW("Magisk detected, skip post-fs-data!");
    return true;
  }

  if (!DirExists(kLogDir)) {
    EnsureDirExists(kLogDir);
    chmod(kLogDir, 0700);
  }
  std::string rotate_cmd = std::string("rm -rf ") + kLogDir + "*.old.log; "
                          "for file in " + std::string(kLogDir) +
                          "*; do mv \"$file\" \"$file.old.log\"; done";
  ExecCommand({"/system/bin/sh", "-c", rotate_cmd}, false);

  std::string logcat_path = std::string(kLogDir) + "locat.log";
  std::string dmesg_path = std::string(kLogDir) + "dmesg.log";
  std::string logcat_cmd =
      "timeout -s 9 120s logcat -b main,system,crash -f " + logcat_path +
      " logcatcher-bootlog:S &";
  std::string dmesg_cmd = "timeout -s 9 120s dmesg -w > " + dmesg_path + " &";
  ExecCommand({"/system/bin/sh", "-c", logcat_cmd}, false);
  ExecCommand({"/system/bin/sh", "-c", dmesg_cmd}, false);

  EnsureDirExists(kLogDir);

  const char* kpver = std::getenv("KERNELPATCH_VERSION");
  if (kpver && kpver[0] != '\0') {
    std::printf("KERNELPATCH_VERSION: %s\n", kpver);
  } else {
    std::printf("KERNELPATCH_VERSION not found\n");
  }
  const char* kver = std::getenv("KERNEL_VERSION");
  if (kver && kver[0] != '\0') {
    std::printf("KERNEL_VERSION: %s\n", kver);
  } else {
    std::printf("KERNEL_VERSION not found\n");
  }

  bool safe_mode = IsSafeMode(superkey);
  if (!safe_mode) {
    ExecCommonScripts("post-fs-data.d", true);
  }

  EnsureBinaries();

  if (DirExists(kModuleUpdateDir)) {
    HandleUpdatedModules();
    ExecCommand({"/system/bin/sh", "-c", std::string("rm -rf ") + kModuleUpdateDir});
  }

  if (safe_mode) {
    LOGW("safe mode, skip post-fs-data scripts and disable all modules!");
    DisableAllModules();
    return true;
  }
  PruneModules();
  Restorecon();
  LoadSepolicyRule();

  ExecMetamoduleMount(kModuleDir);
  ExecStageScript("post-fs-data", true);
  ExecStageLua("post-fs-data", true, superkey);
  LoadSystemProp();

  std::string update_flag = std::string(kWorkingDir) + kUpdateFileName;
  unlink(update_flag.c_str());

  RunStage("post-mount", superkey, true);
  chdir("/");

  return true;
}

bool OnServices(const std::string& superkey) {
  LOGI("on_services triggered!");
  RunStage("service", superkey, false);
  return true;
}

bool OnBootCompleted(const std::string& superkey) {
  LOGI("on_boot_completed triggered!");
  RunStage("boot-completed", superkey, false);
  ExecCommand({kDaemonPath, "uid-listener"});
  return true;
}

bool StartUidListener() {
  LOGI("start uid listener");
  const std::string superkey = "su";
  static std::atomic<bool> need_refresh(false);

  auto signal_handler = [](int) { need_refresh.store(true); };
  std::signal(SIGTERM, signal_handler);
  std::signal(SIGINT, signal_handler);
  std::signal(SIGPWR, signal_handler);

  int fd = inotify_init1(IN_NONBLOCK);
  if (fd < 0) {
    LOGW("inotify init failed");
    return false;
  }
  int wd = inotify_add_watch(fd, "/data/system", IN_MOVED_TO | IN_CLOSE_WRITE);
  if (wd < 0) {
    close(fd);
    LOGW("inotify watch failed");
    return false;
  }

  bool debounce = false;
  char buffer[4096] = {0};
  while (true) {
    if (need_refresh.load()) {
      RefreshApPackageList(superkey);
      break;
    }

    struct pollfd pfd = {fd, POLLIN, 0};
    int pr = poll(&pfd, 1, 1000);
    if (pr <= 0) {
      continue;
    }
    ssize_t len = read(fd, buffer, sizeof(buffer));
    if (len <= 0) {
      continue;
    }
    size_t offset = 0;
    while (offset < static_cast<size_t>(len)) {
      auto* ev = reinterpret_cast<struct inotify_event*>(buffer + offset);
      if (ev->len > 0 && ev->name) {
        std::string name = ev->name;
        if (name == "packages.list.tmp" || name == "packages.list") {
          if (!debounce) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            debounce = true;
            RefreshApPackageList(superkey);
            debounce = false;
          }
        }
      }
      offset += sizeof(struct inotify_event) + ev->len;
    }
  }
  inotify_rm_watch(fd, wd);
  close(fd);
  return true;
}

}  // namespace apd
