#pragma once

#include <cstdarg>

namespace apd {
enum class LogLevel {
  kDebug,
  kInfo,
  kWarn,
  kError,
};

void InitLog();
void Log(LogLevel level, const char* fmt, ...);
}  // namespace apd

#define LOGD(fmt, ...) ::apd::Log(::apd::LogLevel::kDebug, fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) ::apd::Log(::apd::LogLevel::kInfo, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) ::apd::Log(::apd::LogLevel::kWarn, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) ::apd::Log(::apd::LogLevel::kError, fmt, ##__VA_ARGS__)
