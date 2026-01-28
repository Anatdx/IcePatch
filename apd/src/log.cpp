#include "log.hpp"

#include <cstdio>

#ifdef ANDROID
#include <android/log.h>
#endif

namespace apd {

void InitLog() {
#ifdef ANDROID
  // __android_log_set_minimum_priority is API 30+, avoid on lower API.
#endif
}

void Log(LogLevel level, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
#ifdef ANDROID
  int prio = ANDROID_LOG_INFO;
  switch (level) {
    case LogLevel::kDebug:
      prio = ANDROID_LOG_DEBUG;
      break;
    case LogLevel::kInfo:
      prio = ANDROID_LOG_INFO;
      break;
    case LogLevel::kWarn:
      prio = ANDROID_LOG_WARN;
      break;
    case LogLevel::kError:
      prio = ANDROID_LOG_ERROR;
      break;
  }
  __android_log_vprint(prio, "IcePatchD", fmt, args);
#else
  const char* prefix = "I";
  switch (level) {
    case LogLevel::kDebug:
      prefix = "D";
      break;
    case LogLevel::kInfo:
      prefix = "I";
      break;
    case LogLevel::kWarn:
      prefix = "W";
      break;
    case LogLevel::kError:
      prefix = "E";
      break;
  }
  std::fprintf(stderr, "[%s] ", prefix);
  std::vfprintf(stderr, fmt, args);
  std::fprintf(stderr, "\n");
#endif
  va_end(args);
}

}  // namespace apd
