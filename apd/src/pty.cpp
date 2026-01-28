#include "pty.hpp"

#include "log.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <thread>

#include <pthread.h>

namespace apd {

namespace {

struct TermiosState {
  termios original {};
  bool has = false;
};

bool SetStdinRaw(TermiosState* state) {
  if (!isatty(STDIN_FILENO)) {
    return true;
  }
  termios t {};
  if (tcgetattr(STDIN_FILENO, &t) != 0) {
    return false;
  }
  state->original = t;
  state->has = true;
  cfmakeraw(&t);
  return tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) == 0;
}

void RestoreStdin(const TermiosState& state) {
  if (!state.has) {
    return;
  }
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &state.original);
}

bool WriteAll(int fd, const void* buf, size_t len) {
  const char* p = static_cast<const char*>(buf);
  size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = write(fd, p, remaining);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    remaining -= static_cast<size_t>(n);
    p += n;
  }
  return true;
}

void PumpFd(int from, int to) {
  char buf[4096];
  while (true) {
    ssize_t n = read(from, buf, sizeof(buf));
    if (n == 0) {
      return;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    if (!WriteAll(to, buf, static_cast<size_t>(n))) {
      return;
    }
  }
}

void UpdateWinSize(int target_fd) {
  struct winsize ws {};
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
    ioctl(target_fd, TIOCSWINSZ, &ws);
  }
}

void WatchSigwinchAsync(int target_fd) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGWINCH);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  std::thread([target_fd, set]() mutable {
    for (;;) {
      int sig = 0;
      if (sigwait(&set, &sig) != 0) {
        break;
      }
      UpdateWinSize(target_fd);
    }
  }).detach();
}

}  // namespace

bool PreparePty() {
  bool tty_in = isatty(STDIN_FILENO);
  bool tty_out = isatty(STDOUT_FILENO);
  bool tty_err = isatty(STDERR_FILENO);
  if (!tty_in && !tty_out && !tty_err) {
    return true;
  }

  int ptmx = posix_openpt(O_RDWR | O_NOCTTY);
  if (ptmx < 0) {
    LOGW("posix_openpt failed");
    return false;
  }
  if (grantpt(ptmx) != 0 || unlockpt(ptmx) != 0) {
    close(ptmx);
    LOGW("grantpt/unlockpt failed");
    return false;
  }
  char* slave_name = ptsname(ptmx);
  if (!slave_name) {
    close(ptmx);
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(ptmx);
    return false;
  }
  if (pid > 0) {
    // parent: bridge stdio <-> pty
    TermiosState term_state;
    if (!SetStdinRaw(&term_state)) {
      LOGW("set stdin raw failed");
    }
    WatchSigwinchAsync(ptmx);
    UpdateWinSize(ptmx);

    std::thread stdin_pump([ptmx]() { PumpFd(STDIN_FILENO, ptmx); });
    PumpFd(ptmx, STDOUT_FILENO);
    stdin_pump.join();
    RestoreStdin(term_state);
    close(ptmx);

    int status = 0;
    waitpid(pid, &status, 0);
    _exit(status);
  }

  setsid();
  int slave = open(slave_name, O_RDWR);
  if (slave < 0) {
    close(ptmx);
    return false;
  }
  if (tty_in) {
    dup2(slave, STDIN_FILENO);
  }
  if (tty_out) {
    dup2(slave, STDOUT_FILENO);
  }
  if (tty_err) {
    dup2(slave, STDERR_FILENO);
  }
  close(slave);
  close(ptmx);
  return true;
}

}  // namespace apd
