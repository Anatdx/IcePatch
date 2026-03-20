#include "kpimg_inspect.hpp"
#include "kallsyms.hpp"
#include "kpm.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include "kpboot/lib/sha/sha256.h"
}

namespace apd {
namespace {

constexpr std::array<std::uint8_t, 4> kArm64Magic = {'A', 'R', 'M', 0x64};
constexpr std::array<char, 16> kUncompressedPrefix = {
    'U', 'N', 'C', 'O', 'M', 'P', 'R', 'E', 'S', 'S', 'E', 'D', '_', 'I', 'M', 'G'};
constexpr std::array<std::uint8_t, 8> kKpMagic = {'K', 'P', '1', '1', '5', '8', 0x00, 0x00};

std::vector<std::uint8_t> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    throw std::runtime_error("failed to open file: " + path);
  }

  const auto end_pos = in.tellg();
  if (end_pos < 0) {
    throw std::runtime_error("failed to read file size: " + path);
  }

  std::vector<std::uint8_t> data(static_cast<std::size_t>(end_pos));
  in.seekg(0, std::ios::beg);
  if (!data.empty()) {
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  }

  if (!in) {
    throw std::runtime_error("failed to read file content: " + path);
  }

  return data;
}

void WriteFile(const std::string& path, const std::vector<std::uint8_t>& data, std::size_t size) {
  if (size > data.size()) {
    throw std::runtime_error("write size out of range");
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open output file: " + path);
  }
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(size));
  if (!out) {
    throw std::runtime_error("failed to write output file: " + path);
  }
}

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& data, std::size_t off) {
  if (off + 4 > data.size()) {
    throw std::runtime_error("out-of-range le32 read");
  }
  return static_cast<std::uint32_t>(data[off]) |
         (static_cast<std::uint32_t>(data[off + 1]) << 8) |
         (static_cast<std::uint32_t>(data[off + 2]) << 16) |
         (static_cast<std::uint32_t>(data[off + 3]) << 24);
}

std::uint64_t ReadLe64(const std::vector<std::uint8_t>& data, std::size_t off) {
  if (off + 8 > data.size()) {
    throw std::runtime_error("out-of-range le64 read");
  }
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(data[off + i]) << (i * 8);
  }
  return v;
}

void WriteLe64(std::vector<std::uint8_t>* data, std::size_t off, std::uint64_t value) {
  if (off + 8 > data->size()) {
    throw std::runtime_error("out-of-range le64 write");
  }
  for (int i = 0; i < 8; ++i) {
    (*data)[off + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
  }
}

void WriteLe32(std::vector<std::uint8_t>* data, std::size_t off, std::uint32_t value) {
  if (off + 4 > data->size()) {
    throw std::runtime_error("out-of-range le32 write");
  }
  (*data)[off + 0] = static_cast<std::uint8_t>(value & 0xFFu);
  (*data)[off + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  (*data)[off + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  (*data)[off + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

void MaybeDisablePiMap(std::vector<std::uint8_t>* image, int kernel_major, int kernel_minor) {
  if (kernel_major < 6 || kernel_minor < 7) {
    return;
  }
  static constexpr std::array<std::uint8_t, 12> kPattern = {
      0xE6, 0x03, 0x16, 0xAA, 0xE7, 0x03, 0x1F, 0x2A, 0x34, 0x11, 0x88, 0x9A};
  static constexpr std::array<std::uint8_t, 12> kReplace = {
      0xE6, 0x03, 0x16, 0xAA, 0xE7, 0x03, 0x1F, 0x2A, 0xF4, 0x03, 0x09, 0xAA};

  auto it = std::search(image->begin(), image->end(), kPattern.begin(), kPattern.end());
  if (it == image->end()) {
    return;
  }
  std::copy(kReplace.begin(), kReplace.end(), it);
}

void PatchMapAreaPac(std::vector<std::uint8_t>* image, std::size_t tcp_init_sock_offset, std::size_t map_max_size) {
  static constexpr std::uint32_t kInsnNop = 0xD503201Fu;
  static constexpr std::uint32_t kInsnPacFirst = 0xD503233Fu;
  static constexpr std::uint32_t kInsnPacMask = 0xFFFFFD1Fu;
  static constexpr std::uint32_t kInsnPacPattern = 0xD503211Fu;

  const std::size_t image_size = image->size();
  if (tcp_init_sock_offset + 4 > image_size) {
    return;
  }

  bool saw_first_pac = false;
  std::size_t last_pac_pos = 0;
  int pac_count = 0;

  for (std::size_t i = 0; i + 4 <= map_max_size; i += 4) {
    const std::size_t off = tcp_init_sock_offset + i;
    if (off + 4 > image_size) {
      break;
    }
    const std::uint32_t insn = ReadLe32(*image, off);
    if (!saw_first_pac && insn == kInsnPacFirst && i < 20) {
      saw_first_pac = true;
    }
    if ((insn & kInsnPacMask) == kInsnPacPattern) {
      last_pac_pos = i;
      ++pac_count;
      WriteLe32(image, off, kInsnNop);
    }
  }

  if (!saw_first_pac) {
    return;
  }
  if ((pac_count & 1) == 0) {
    return;
  }

  for (std::size_t j = map_max_size; j + 4 <= map_max_size * 2; j += 4) {
    const std::size_t off = tcp_init_sock_offset + j;
    if (off + 4 > image_size) {
      break;
    }
    const std::uint32_t insn = ReadLe32(*image, off);
    if ((insn & kInsnPacMask) == kInsnPacPattern) {
      WriteLe32(image, off, kInsnNop);
      return;
    }
  }
  (void)last_pac_pos;
}

std::string ReadFixedCString(const std::vector<std::uint8_t>& data, std::size_t off, std::size_t len) {
  if (off + len > data.size()) {
    throw std::runtime_error("out-of-range string read");
  }
  const char* ptr = reinterpret_cast<const char*>(data.data() + off);
  std::size_t size = 0;
  while (size < len && ptr[size] != '\0') {
    ++size;
  }
  return std::string(ptr, ptr + size);
}

std::size_t AlignUp(std::size_t value, std::size_t align) {
  return (value + align - 1) & ~(align - 1);
}

std::size_t AlignDown(std::size_t value, std::size_t align) {
  return value & ~(align - 1);
}

std::uint32_t EncodeArm64B(std::uint32_t from_off, std::uint32_t to_off) {
  const std::int64_t delta = static_cast<std::int64_t>(to_off) - static_cast<std::int64_t>(from_off);
  if ((delta % 4) != 0) {
    throw std::runtime_error("branch delta is not instruction-aligned");
  }
  const std::int64_t imm26 = delta / 4;
  if (imm26 < -(1LL << 25) || imm26 >= (1LL << 25)) {
    throw std::runtime_error("branch target out of range for B imm26");
  }
  return 0x14000000u | (static_cast<std::uint32_t>(imm26) & 0x03FFFFFFu);
}

struct Arm64ImageInfo {
  bool uefi = false;
  std::uint64_t load_offset = 0;
  std::uint64_t kernel_size = 0;
  std::uint8_t page_shift = 12;
  std::uint32_t primary_entry_offset = 0;
};

struct KpHeaderInfo {
  std::uint8_t major = 0;
  std::uint8_t minor = 0;
  std::uint8_t patch = 0;
  std::uint64_t config_flags = 0;
  std::string compile_time;
};

Arm64ImageInfo ParseArm64ImageHeader(const std::vector<std::uint8_t>& kimg) {
  if (kimg.size() < 64) {
    throw std::runtime_error("kernel image too small");
  }

  if (!std::equal(kArm64Magic.begin(), kArm64Magic.end(), kimg.begin() + 56)) {
    throw std::runtime_error("invalid arm64 image magic");
  }

  Arm64ImageInfo info;
  info.uefi = (kimg[0] == 'M' && kimg[1] == 'Z');

  const std::size_t branch_off = info.uefi ? 4 : 0;
  const std::uint32_t branch = ReadLe32(kimg, branch_off);
  if ((branch & 0xFC000000u) != 0x14000000u) {
    throw std::runtime_error("invalid primary branch instruction");
  }

  const std::uint32_t imm26 = branch & 0x03FFFFFFu;
  info.primary_entry_offset = (imm26 << 2) + static_cast<std::uint32_t>(info.uefi ? 4 : 0);

  info.load_offset = ReadLe64(kimg, 8);
  info.kernel_size = ReadLe64(kimg, 16);

  const std::uint8_t flags = static_cast<std::uint8_t>(ReadLe64(kimg, 24) & 0x0Fu);
  const std::uint8_t page_encoding = static_cast<std::uint8_t>((flags & 0x6u) >> 1);
  if (page_encoding == 2) {
    info.page_shift = 14;
  } else if (page_encoding == 3) {
    info.page_shift = 16;
  } else {
    info.page_shift = 12;
  }

  if ((flags & 0x1u) != 0) {
    throw std::runtime_error("big-endian arm64 image is not supported");
  }

  return info;
}

std::optional<std::size_t> FindKpPreset(const std::vector<std::uint8_t>& kimg) {
  auto it = std::search(kimg.begin(), kimg.end(), kKpMagic.begin(), kKpMagic.end());
  while (it != kimg.end()) {
    const std::size_t off = static_cast<std::size_t>(std::distance(kimg.begin(), it));

    const std::size_t kimg_size_off = off + 72;
    if (kimg_size_off + 8 <= kimg.size()) {
      const std::uint64_t saved_kimg_size = ReadLe64(kimg, kimg_size_off);
      if (AlignUp(static_cast<std::size_t>(saved_kimg_size), 4096) == off) {
        return off;
      }
    }

    it = std::search(it + 1, kimg.end(), kKpMagic.begin(), kKpMagic.end());
  }
  return std::nullopt;
}

constexpr std::size_t kPresetKimgSizeOffset = 72;
constexpr std::size_t kPresetKernelVersionOffset = 64;
constexpr std::size_t kPresetHeaderBackupOffset = 208;
constexpr std::size_t kHeaderBackupLen = 8;
constexpr std::size_t kPresetSuperKeyOffset = 216;
constexpr std::size_t kSuperKeyLen = 64;
constexpr std::size_t kPresetMapOffset = 128;
constexpr std::size_t kPresetMapMaxSizeOffset = 136;
constexpr std::size_t kPresetKallsymsLookupOffset = 144;
constexpr std::size_t kPresetPagingInitOffset = 152;
constexpr std::size_t kPresetPrintkOffset = 160;
constexpr std::size_t kPresetMapSymbolOffset = 168;
constexpr std::size_t kPresetMapSymbolLen = 40;
constexpr std::size_t kPresetPatchConfigOffset = 376;
constexpr std::size_t kPresetPatchConfigLen = 512;
constexpr std::size_t kPresetRootSuperKeyOffset = kPresetSuperKeyOffset + kSuperKeyLen;
constexpr std::size_t kRootSuperKeyLen = 32;
constexpr std::size_t kSetupPreserveLen = 64;
constexpr std::size_t kPresetAdditionalOffset = kPresetPatchConfigOffset + kPresetPatchConfigLen;
constexpr std::size_t kPresetAdditionalLen = 512;
constexpr std::size_t kPresetKpimgSizeOffset = 80;
constexpr std::size_t kPresetExtraSizeOffset = 120;
constexpr std::size_t kPatchExtraItemLen = 128;
constexpr std::size_t kExtraAlign = 16;
constexpr std::size_t kExtraNameLen = 32;
constexpr std::size_t kExtraEventLen = 32;
constexpr std::uint32_t kPolicySlotMagic = 0x4b504f4cU;
constexpr std::uint16_t kPolicySlotVersion = 1;
constexpr std::uint16_t kPolicySlotSize = 0x20;
constexpr std::size_t kPolicyAdditionalTextOffset = kPolicySlotSize;

constexpr int kPolicyProfileMinimal = 0;
constexpr int kPolicyProfileRootful = 1;
constexpr int kPolicyProfileKpmSupport = 2;
constexpr int kPolicyProfileFull = 3;

constexpr std::uint32_t kPolicyFlagKcfiBypass = (1u << 0);
constexpr std::uint32_t kPolicyFlagTaskObserver = (1u << 1);
constexpr std::uint32_t kPolicyFlagSelinuxBypass = (1u << 2);
constexpr std::uint32_t kPolicyFlagSupercall = (1u << 3);
constexpr std::uint32_t kPolicyFlagKstorage = (1u << 4);
constexpr std::uint32_t kPolicyFlagSu = (1u << 5);
constexpr std::uint32_t kPolicyFlagSuCompat = (1u << 6);
constexpr std::uint32_t kPolicyFlagAndroidUser = (1u << 7);
constexpr std::uint32_t kPolicyFlagFsApi = (1u << 8);

struct PolicySlot {
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  std::uint16_t size = 0;
  std::uint32_t profile = 0;
  std::uint32_t feature_flags = 0;
  std::uint32_t option_flags = 0;
  std::uint32_t reserved0 = 0;
  std::uint32_t reserved1 = 0;
  std::uint32_t reserved2 = 0;
};

std::string ToLowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool IsPolicyAddition(const std::string& addition) {
  const std::size_t eq = addition.find('=');
  if (eq == std::string::npos || eq == 0) {
    return false;
  }
  const std::string key = ToLowerAscii(addition.substr(0, eq));
  return key == "policy" || key == "mode" || key == "profile" ||
         key == "hook.profile" || key == "no_su" ||
         key.rfind("feature.", 0) == 0;
}

bool ParsePolicyBoolText(const std::string& value, bool* out) {
  if (out == nullptr) {
    return false;
  }
  const std::string lower = ToLowerAscii(value);
  if (lower == "1" || lower == "y" || lower == "yes" || lower == "on" ||
      lower == "true" || lower == "enable" || lower == "enabled") {
    *out = true;
    return true;
  }
  if (lower == "0" || lower == "n" || lower == "no" || lower == "off" ||
      lower == "false" || lower == "disable" || lower == "disabled") {
    *out = false;
    return true;
  }
  return false;
}

std::uint32_t PolicyFlagsForProfile(int profile) {
  switch (profile) {
    case kPolicyProfileMinimal:
      return kPolicyFlagKcfiBypass | kPolicyFlagSupercall | kPolicyFlagKstorage;
    case kPolicyProfileRootful:
      return kPolicyFlagKcfiBypass | kPolicyFlagTaskObserver |
             kPolicyFlagSelinuxBypass | kPolicyFlagSupercall |
             kPolicyFlagKstorage | kPolicyFlagSu | kPolicyFlagSuCompat |
             kPolicyFlagAndroidUser;
    case kPolicyProfileKpmSupport:
      return kPolicyFlagKcfiBypass | kPolicyFlagSupercall | kPolicyFlagKstorage;
    case kPolicyProfileFull:
      return PolicyFlagsForProfile(kPolicyProfileRootful) | kPolicyFlagFsApi;
    default:
      return 0;
  }
}

std::uint32_t NormalizePolicyFlags(std::uint32_t flags) {
  if ((flags & (kPolicyFlagSuCompat | kPolicyFlagAndroidUser)) != 0) {
    flags |= kPolicyFlagSu;
  }
  if ((flags & kPolicyFlagAndroidUser) != 0) {
    flags |= kPolicyFlagSuCompat;
  }
  if ((flags & kPolicyFlagSu) != 0) {
    flags |= kPolicyFlagSupercall;
    flags |= kPolicyFlagKstorage;
  }
  if ((flags & kPolicyFlagSupercall) == 0) {
    flags &= ~(kPolicyFlagSu | kPolicyFlagSuCompat | kPolicyFlagAndroidUser);
  }
  return flags;
}

std::uint32_t FeatureFlagFromName(const std::string& lower_name) {
  if (lower_name == "kcfi" || lower_name == "kcfi_bypass") {
    return kPolicyFlagKcfiBypass;
  }
  if (lower_name == "task" || lower_name == "task_observer") {
    return kPolicyFlagTaskObserver;
  }
  if (lower_name == "selinux" || lower_name == "selinux_bypass") {
    return kPolicyFlagSelinuxBypass;
  }
  if (lower_name == "supercall") {
    return kPolicyFlagSupercall;
  }
  if (lower_name == "kstorage") {
    return kPolicyFlagKstorage;
  }
  if (lower_name == "su") {
    return kPolicyFlagSu;
  }
  if (lower_name == "su_compat") {
    return kPolicyFlagSuCompat;
  }
  if (lower_name == "android_user") {
    return kPolicyFlagAndroidUser;
  }
  if (lower_name == "fsapi" || lower_name == "fs_api") {
    return kPolicyFlagFsApi;
  }
  return 0;
}

bool ApplyPolicyProfileName(const std::string& lower_profile, std::uint32_t* flags,
                            std::uint32_t* profile) {
  if (flags == nullptr || profile == nullptr || lower_profile.empty()) {
    return false;
  }
  auto set_profile = [&](std::uint32_t next_profile) {
    *profile = next_profile;
    *flags = PolicyFlagsForProfile(static_cast<int>(next_profile));
  };
  if (lower_profile == "minimal") {
    set_profile(static_cast<std::uint32_t>(kPolicyProfileMinimal));
    return true;
  }
  if (lower_profile == "legacy" || lower_profile == "rootful") {
    set_profile(static_cast<std::uint32_t>(kPolicyProfileRootful));
    return true;
  }
  if (lower_profile == "kpm" || lower_profile == "kpm-support" ||
      lower_profile == "kpm_support") {
    set_profile(static_cast<std::uint32_t>(kPolicyProfileKpmSupport));
    return true;
  }
  if (lower_profile == "full") {
    set_profile(static_cast<std::uint32_t>(kPolicyProfileFull));
    return true;
  }
  if (lower_profile == "no-su" || lower_profile == "no_su" || lower_profile == "nosu") {
    set_profile(static_cast<std::uint32_t>(kPolicyProfileMinimal));
    return true;
  }
  return false;
}

void ApplyPolicyAddition(const std::string& addition, std::uint32_t* flags,
                         std::uint32_t* profile) {
  if (flags == nullptr || profile == nullptr) {
    return;
  }
  const std::size_t eq = addition.find('=');
  if (eq == std::string::npos || eq == 0 || eq + 1 >= addition.size()) {
    return;
  }
  const std::string key = ToLowerAscii(addition.substr(0, eq));
  const std::string value = ToLowerAscii(addition.substr(eq + 1));

  if (key == "policy" || key == "profile" || key == "hook.profile") {
    ApplyPolicyProfileName(value, flags, profile);
    return;
  }
  if (key == "mode") {
    if (value == "no-su") {
      *flags &= ~(kPolicyFlagTaskObserver | kPolicyFlagSelinuxBypass |
                  kPolicyFlagSu | kPolicyFlagSuCompat | kPolicyFlagAndroidUser);
      return;
    }
    ApplyPolicyProfileName(value, flags, profile);
    return;
  }
  if (key == "no_su") {
    bool enabled = false;
    if (ParsePolicyBoolText(value, &enabled) && enabled) {
      *flags &= ~(kPolicyFlagTaskObserver | kPolicyFlagSelinuxBypass |
                  kPolicyFlagSu | kPolicyFlagSuCompat | kPolicyFlagAndroidUser);
    }
    return;
  }
  if (key.rfind("feature.", 0) == 0) {
    const std::uint32_t feature = FeatureFlagFromName(key.substr(8));
    bool enabled = false;
    if (feature == 0 || !ParsePolicyBoolText(value, &enabled)) {
      return;
    }
    if (enabled) {
      *flags |= feature;
    } else {
      *flags &= ~feature;
    }
  }
}

PolicySlot BuildPolicySlot(const std::vector<std::string>& additions) {
  PolicySlot slot{};
  slot.magic = kPolicySlotMagic;
  slot.version = kPolicySlotVersion;
  slot.size = kPolicySlotSize;
  slot.profile = static_cast<std::uint32_t>(kPolicyProfileMinimal);

  std::uint32_t flags = PolicyFlagsForProfile(kPolicyProfileMinimal);
  for (const auto& addition : additions) {
    ApplyPolicyAddition(addition, &flags, &slot.profile);
  }
  flags = NormalizePolicyFlags(flags);
  slot.feature_flags = flags;
  return slot;
}

void WritePolicySlot(std::vector<std::uint8_t>* data, std::size_t off, const PolicySlot& slot) {
  if (data == nullptr || off + kPolicySlotSize > data->size()) {
    throw std::runtime_error("policy slot write out of range");
  }
  WriteLe32(data, off + 0, slot.magic);
  (*data)[off + 4] = static_cast<std::uint8_t>(slot.version & 0xFFu);
  (*data)[off + 5] = static_cast<std::uint8_t>((slot.version >> 8) & 0xFFu);
  (*data)[off + 6] = static_cast<std::uint8_t>(slot.size & 0xFFu);
  (*data)[off + 7] = static_cast<std::uint8_t>((slot.size >> 8) & 0xFFu);
  WriteLe32(data, off + 8, slot.profile);
  WriteLe32(data, off + 12, slot.feature_flags);
  WriteLe32(data, off + 16, slot.option_flags);
  WriteLe32(data, off + 20, slot.reserved0);
  WriteLe32(data, off + 24, slot.reserved1);
  WriteLe32(data, off + 28, slot.reserved2);
}

std::optional<PolicySlot> ParsePolicySlot(const std::vector<std::uint8_t>& data, std::size_t off) {
  if (off + kPolicySlotSize > data.size()) {
    return std::nullopt;
  }
  if (ReadLe32(data, off + 0) != kPolicySlotMagic) {
    return std::nullopt;
  }
  PolicySlot slot{};
  slot.magic = ReadLe32(data, off + 0);
  slot.version = static_cast<std::uint16_t>(data[off + 4] | (data[off + 5] << 8));
  slot.size = static_cast<std::uint16_t>(data[off + 6] | (data[off + 7] << 8));
  slot.profile = ReadLe32(data, off + 8);
  slot.feature_flags = ReadLe32(data, off + 12);
  slot.option_flags = ReadLe32(data, off + 16);
  slot.reserved0 = ReadLe32(data, off + 20);
  slot.reserved1 = ReadLe32(data, off + 24);
  slot.reserved2 = ReadLe32(data, off + 28);
  return slot;
}

std::size_t PolicyTextOffset(const std::optional<PolicySlot>& slot) {
  if (!slot.has_value()) {
    return 0;
  }
  const std::size_t size = slot->size;
  if (size >= kPolicySlotSize && size <= kPresetAdditionalLen) {
    return size;
  }
  return kPolicyAdditionalTextOffset;
}

constexpr std::int32_t kExtraTypeNone = 0;
constexpr std::int32_t kExtraTypeKpm = 1;
constexpr std::int32_t kExtraTypeShell = 2;
constexpr std::int32_t kExtraTypeExec = 3;
constexpr std::int32_t kExtraTypeRaw = 4;
constexpr std::int32_t kExtraTypeAndroidRc = 5;

std::string HexEncode(const std::uint8_t* data, std::size_t len) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    out[i * 2] = kHex[(data[i] >> 4) & 0x0F];
    out[i * 2 + 1] = kHex[data[i] & 0x0F];
  }
  return out;
}

std::string ReadCStringBounded(const std::vector<std::uint8_t>& data, std::size_t off, std::size_t len) {
  if (off + len > data.size()) {
    throw std::runtime_error("out-of-range cstring read");
  }
  std::size_t n = 0;
  while (n < len && data[off + n] != 0) {
    ++n;
  }
  return std::string(reinterpret_cast<const char*>(data.data() + static_cast<std::ptrdiff_t>(off)), n);
}

std::vector<std::string> ParseAdditionalEntries(const std::vector<std::uint8_t>& data, std::size_t off) {
  if (off + kPresetAdditionalLen > data.size()) {
    throw std::runtime_error("additional range out of bounds");
  }
  std::vector<std::string> out;
  const auto slot = ParsePolicySlot(data, off);
  std::size_t pos = off + PolicyTextOffset(slot);
  while (pos < off + kPresetAdditionalLen) {
    const std::uint8_t len = data[pos++];
    if (len == 0) {
      break;
    }
    if (pos + len > off + kPresetAdditionalLen) {
      throw std::runtime_error("invalid additional entry length");
    }
    out.emplace_back(reinterpret_cast<const char*>(data.data() + static_cast<std::ptrdiff_t>(pos)),
                     static_cast<std::size_t>(len));
    pos += len;
  }
  return out;
}

std::int32_t ParseExtraTypeName(const std::string& extra_type) {
  if (extra_type == "kpm") return kExtraTypeKpm;
  if (extra_type == "shell") return kExtraTypeShell;
  if (extra_type == "exec") return kExtraTypeExec;
  if (extra_type == "raw") return kExtraTypeRaw;
  if (extra_type == "android_rc") return kExtraTypeAndroidRc;
  return kExtraTypeNone;
}

const char* ExtraTypeName(std::int32_t extra_type) {
  switch (extra_type) {
    case kExtraTypeKpm:
      return "kpm";
    case kExtraTypeShell:
      return "shell";
    case kExtraTypeExec:
      return "exec";
    case kExtraTypeRaw:
      return "raw";
    case kExtraTypeAndroidRc:
      return "android_rc";
    default:
      return "none";
  }
}

struct ParsedExtraItem {
  std::int32_t priority = 0;
  std::int32_t args_size = 0;
  std::int32_t con_size = 0;
  std::int32_t type = 0;
  std::string name;
  std::string event;
  std::string args;
  std::vector<std::uint8_t> content;
};

struct OutputExtraItem {
  std::int32_t priority = 0;
  std::int32_t type = kExtraTypeNone;
  std::string name;
  std::string event;
  std::vector<std::uint8_t> args_data;
  std::vector<std::uint8_t> content;
};

std::string Basename(const std::string& path) {
  const auto pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

std::vector<std::uint8_t> PadAlignedCopy(const std::vector<std::uint8_t>& in) {
  std::vector<std::uint8_t> out = in;
  out.resize(AlignUp(out.size(), kExtraAlign), 0);
  return out;
}

std::vector<std::uint8_t> PadAlignedString(const std::string& in) {
  std::vector<std::uint8_t> out(in.begin(), in.end());
  out.resize(AlignUp(out.size(), kExtraAlign), 0);
  return out;
}

std::vector<OutputExtraItem> BuildOutputExtraItems(const std::vector<ParsedExtraItem>& existing,
                                                   const std::vector<PatchExtraConfig>& extra_configs) {
  std::vector<OutputExtraItem> out;
  out.reserve(extra_configs.size());
  for (const auto& cfg : extra_configs) {
    OutputExtraItem item;
    if (cfg.is_path) {
      item.type = ParseExtraTypeName(cfg.extra_type);
      if (item.type == kExtraTypeNone) {
        throw std::runtime_error("extra type is required for --embed-extra-path");
      }
      const auto raw_content = ReadFile(cfg.path);
      if (!cfg.set_name.empty()) {
        item.name = cfg.set_name;
      } else if (item.type == kExtraTypeKpm) {
        KpmInfo kpm_info{};
        std::string error;
        if (!ParseKpmInfo(raw_content, &kpm_info, &error) || kpm_info.name.empty()) {
          throw std::runtime_error("failed to read kpm info from " + cfg.path + ": " + error);
        }
        item.name = kpm_info.name;
      } else {
        item.name = Basename(cfg.path);
      }
      item.event = cfg.set_event;
      item.priority = cfg.priority;
      item.args_data = PadAlignedString(cfg.set_args);
      item.content = PadAlignedCopy(raw_content);
    } else {
      const auto it = std::find_if(existing.begin(), existing.end(), [&](const ParsedExtraItem& old_item) {
        return old_item.name == cfg.existing_name;
      });
      if (it == existing.end()) {
        throw std::runtime_error("embedded extra not found: " + cfg.existing_name);
      }
      if (cfg.detach) {
        continue;
      }
      item.priority = it->priority;
      item.type = it->type;
      item.name = it->name;
      item.event = it->event;
      item.args_data = PadAlignedString(it->args);
      item.content = it->content;
      if (!cfg.extra_type.empty()) {
        item.type = ParseExtraTypeName(cfg.extra_type);
        if (item.type == kExtraTypeNone) {
          throw std::runtime_error("invalid extra type: " + cfg.extra_type);
        }
      }
      if (!cfg.set_name.empty()) {
        item.name = cfg.set_name;
      }
      if (!cfg.set_event.empty()) {
        item.event = cfg.set_event;
      }
      if (!cfg.set_args.empty()) {
        item.args_data = PadAlignedString(cfg.set_args);
      }
      if (cfg.priority != 0) {
        item.priority = cfg.priority;
      }
    }
    if (item.name.size() >= kExtraNameLen) {
      throw std::runtime_error("extra name too long: " + item.name);
    }
    if (item.event.size() >= kExtraEventLen) {
      throw std::runtime_error("extra event too long: " + item.event);
    }
    out.push_back(std::move(item));
  }

  std::sort(out.begin(), out.end(), [](const OutputExtraItem& a, const OutputExtraItem& b) {
    return a.priority > b.priority;
  });
  return out;
}

std::size_t ComputeExtraBlobSize(const std::vector<OutputExtraItem>& items) {
  std::size_t size = kPatchExtraItemLen;
  for (const auto& item : items) {
    size += kPatchExtraItemLen;
    size += item.args_data.size();
    size += item.content.size();
  }
  return size;
}

std::vector<ParsedExtraItem> ParseExtraItems(const std::vector<std::uint8_t>& image, std::size_t preset_off) {
  std::vector<ParsedExtraItem> items;
  const std::size_t kpimg_size = static_cast<std::size_t>(ReadLe64(image, preset_off + kPresetKpimgSizeOffset));
  const std::size_t extra_size = static_cast<std::size_t>(ReadLe64(image, preset_off + kPresetExtraSizeOffset));
  if (extra_size == 0) {
    return items;
  }
  const std::size_t extra_start = preset_off + kpimg_size;
  if (extra_start + extra_size > image.size()) {
    throw std::runtime_error("extra range out of bounds");
  }

  std::size_t pos = extra_start;
  const std::size_t end = extra_start + extra_size;
  while (pos + kPatchExtraItemLen <= end) {
    if (image[pos + 0] != 'k' || image[pos + 1] != 'p' || image[pos + 2] != 'e') {
      break;
    }
    ParsedExtraItem item;
    item.priority = static_cast<std::int32_t>(ReadLe32(image, pos + 4));
    item.args_size = static_cast<std::int32_t>(ReadLe32(image, pos + 8));
    item.con_size = static_cast<std::int32_t>(ReadLe32(image, pos + 12));
    item.type = static_cast<std::int32_t>(ReadLe32(image, pos + 16));
    if (item.type == kExtraTypeNone) {
      break;
    }
    if (item.args_size < 0 || item.con_size < 0) {
      throw std::runtime_error("invalid extra item size");
    }
    item.name = ReadCStringBounded(image, pos + 20, kExtraNameLen);
    item.event = ReadCStringBounded(image, pos + 52, kExtraEventLen);
    const std::size_t args_off = pos + kPatchExtraItemLen;
    const std::size_t con_off = args_off + static_cast<std::size_t>(item.args_size);
    const std::size_t next = con_off + static_cast<std::size_t>(item.con_size);
    if (next > end) {
      throw std::runtime_error("extra item body out of bounds");
    }
    if (item.args_size > 0) {
      item.args = ReadCStringBounded(image, args_off, static_cast<std::size_t>(item.args_size));
    }
    if (item.con_size > 0) {
      item.content.assign(image.begin() + static_cast<std::ptrdiff_t>(con_off),
                          image.begin() + static_cast<std::ptrdiff_t>(next));
    }
    items.push_back(std::move(item));
    pos = next;
  }
  return items;
}

std::array<std::uint8_t, kRootSuperKeyLen> Sha256KeyDigest(const std::string& key) {
  SHA256_CTX ctx{};
  sha256_init(&ctx);
  sha256_update(&ctx, reinterpret_cast<const BYTE*>(key.data()), key.size());
  std::array<std::uint8_t, SHA256_BLOCK_SIZE> digest{};
  sha256_final(&ctx, digest.data());
  std::array<std::uint8_t, kRootSuperKeyLen> out{};
  std::copy_n(digest.begin(), kRootSuperKeyLen, out.begin());
  return out;
}

KpHeaderInfo ParseKpHeaderAt(const std::vector<std::uint8_t>& data, std::size_t offset) {
  if (offset + 64 > data.size()) {
    throw std::runtime_error("kp header out of range");
  }
  if (!std::equal(kKpMagic.begin(), kKpMagic.end(), data.begin() + static_cast<std::ptrdiff_t>(offset))) {
    throw std::runtime_error("invalid kp magic");
  }

  KpHeaderInfo info;
  info.patch = data[offset + 9];
  info.minor = data[offset + 10];
  info.major = data[offset + 11];
  info.config_flags = ReadLe64(data, offset + 16);
  info.compile_time = ReadFixedCString(data, offset + 24, 24);
  return info;
}

std::uint32_t ToVersionCode(const KpHeaderInfo& info) {
  return (static_cast<std::uint32_t>(info.major) << 16) |
         (static_cast<std::uint32_t>(info.minor) << 8) |
         static_cast<std::uint32_t>(info.patch);
}

void PrintKey(const char* name, const std::string& value) {
  std::cout << std::left << std::setw(24) << name << value << '\n';
}

void PrintHex(const char* name, std::uint64_t value) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::nouppercase << value;
  PrintKey(name, oss.str());
}

void PrintPresetDetails(const std::vector<std::uint8_t>& image, std::size_t preset_off) {
  if (preset_off + kPresetAdditionalOffset + kPresetAdditionalLen > image.size()) {
    throw std::runtime_error("preset detail range out of bounds");
  }

  PrintKey("superkey", ReadFixedCString(image, preset_off + kPresetSuperKeyOffset, kSuperKeyLen));
  const std::string root_key_hex =
      HexEncode(image.data() + static_cast<std::ptrdiff_t>(preset_off + kPresetRootSuperKeyOffset), kRootSuperKeyLen);
  PrintKey("root_superkey", root_key_hex);

  const auto policy_slot = ParsePolicySlot(image, preset_off + kPresetAdditionalOffset);
  PrintKey("policy_slot", policy_slot.has_value() ? "true" : "false");
  if (policy_slot.has_value()) {
    PrintHex("policy_slot_magic", policy_slot->magic);
    PrintKey("policy_slot_version", std::to_string(policy_slot->version));
    PrintKey("policy_slot_size", std::to_string(policy_slot->size));
    PrintKey("policy_slot_profile", std::to_string(policy_slot->profile));
    PrintHex("policy_slot_flags", policy_slot->feature_flags);
    PrintHex("policy_slot_options", policy_slot->option_flags);
  }

  const auto additional = ParseAdditionalEntries(image, preset_off + kPresetAdditionalOffset);
  PrintKey("additional_num", std::to_string(additional.size()));
  for (std::size_t i = 0; i < additional.size(); ++i) {
    std::ostringstream key;
    key << "additional[" << i << "]";
    PrintKey(key.str().c_str(), additional[i]);
  }

  const auto extras = ParseExtraItems(image, preset_off);
  PrintKey("extra_num", std::to_string(extras.size()));
  for (std::size_t i = 0; i < extras.size(); ++i) {
    const auto& item = extras[i];
    std::ostringstream k;
    k << "extra[" << i << "].type";
    PrintKey(k.str().c_str(), ExtraTypeName(item.type));
    k.str("");
    k.clear();
    k << "extra[" << i << "].name";
    PrintKey(k.str().c_str(), item.name);
    k.str("");
    k.clear();
    k << "extra[" << i << "].event";
    PrintKey(k.str().c_str(), item.event);
    k.str("");
    k.clear();
    k << "extra[" << i << "].priority";
    PrintKey(k.str().c_str(), std::to_string(item.priority));
    k.str("");
    k.clear();
    k << "extra[" << i << "].args";
    PrintKey(k.str().c_str(), item.args);
    k.str("");
    k.clear();
    k << "extra[" << i << "].con_size";
    PrintHex(k.str().c_str(), static_cast<std::uint64_t>(item.con_size));
    if (item.type == kExtraTypeKpm) {
      KpmInfo kpm_info{};
      std::string error;
      if (ParseKpmInfo(item.content, &kpm_info, &error)) {
        k.str("");
        k.clear();
        k << "extra[" << i << "].kpm.version";
        PrintKey(k.str().c_str(), kpm_info.version);
        k.str("");
        k.clear();
        k << "extra[" << i << "].kpm.license";
        PrintKey(k.str().c_str(), kpm_info.license);
        k.str("");
        k.clear();
        k << "extra[" << i << "].kpm.author";
        PrintKey(k.str().c_str(), kpm_info.author);
        k.str("");
        k.clear();
        k << "extra[" << i << "].kpm.description";
        PrintKey(k.str().c_str(), kpm_info.description);
      } else {
        k.str("");
        k.clear();
        k << "extra[" << i << "].kpm.error";
        PrintKey(k.str().c_str(), error);
      }
    }
  }
}

}  // namespace

bool InspectKernelImage(const std::string& image_path) {
  try {
    const auto all = ReadFile(image_path);

    bool has_uncompressed_prefix = false;
    std::size_t img_off = 0;
    if (all.size() >= 20 && std::memcmp(all.data(), kUncompressedPrefix.data(), kUncompressedPrefix.size()) == 0) {
      has_uncompressed_prefix = true;
      img_off = 20;
    }

    if (img_off >= all.size()) {
      throw std::runtime_error("empty kernel image payload");
    }

    std::vector<std::uint8_t> kimg(all.begin() + static_cast<std::ptrdiff_t>(img_off), all.end());

    const Arm64ImageInfo info = ParseArm64ImageHeader(kimg);
    const auto kp_off = FindKpPreset(kimg);

    PrintKey("file", image_path);
    PrintKey("container_size", std::to_string(all.size()));
    PrintKey("kernel_image_size", std::to_string(kimg.size()));
    PrintKey("uncompressed_prefix", has_uncompressed_prefix ? "true" : "false");
    PrintKey("arm64_uefi", info.uefi ? "true" : "false");
    PrintHex("load_offset", info.load_offset);
    PrintHex("kernel_size_header", info.kernel_size);
    PrintKey("page_shift", std::to_string(info.page_shift));
    PrintHex("primary_entry_offset", info.primary_entry_offset);

    if (kp_off.has_value()) {
      const std::size_t preset = *kp_off;
      const KpHeaderInfo kp_info = ParseKpHeaderAt(kimg, preset);

      PrintKey("kernelpatch_patched", "true");
      PrintHex("kernelpatch_offset", preset);

      std::ostringstream ver;
      ver << static_cast<int>(kp_info.major) << '.' << static_cast<int>(kp_info.minor) << '.'
          << static_cast<int>(kp_info.patch);
      PrintKey("kernelpatch_version", ver.str());
      PrintHex("kernelpatch_config", kp_info.config_flags);
      PrintKey("kernelpatch_compile", kp_info.compile_time.empty() ? "<empty>" : kp_info.compile_time);
    } else {
      PrintKey("kernelpatch_patched", "false");
    }

    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "inspect failed: %s\n", e.what());
    return false;
  }
}

bool InspectKernelPatchImage(const std::string& kpimg_path) {
  try {
    const auto kpimg = ReadFile(kpimg_path);
    const KpHeaderInfo kp_info = ParseKpHeaderAt(kpimg, 0);

    PrintKey("file", kpimg_path);
    PrintKey("kpimg_size", std::to_string(kpimg.size()));

    std::ostringstream ver;
    ver << static_cast<int>(kp_info.major) << '.' << static_cast<int>(kp_info.minor) << '.'
        << static_cast<int>(kp_info.patch);
    PrintKey("kernelpatch_version", ver.str());
    PrintHex("kernelpatch_version_code", ToVersionCode(kp_info));
    PrintHex("kernelpatch_config", kp_info.config_flags);
    PrintKey("kernelpatch_compile", kp_info.compile_time.empty() ? "<empty>" : kp_info.compile_time);
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "inspect kpimg failed: %s\n", e.what());
    return false;
  }
}

bool PrintKernelPatchVersion(const std::string& kpimg_path) {
  try {
    const auto kpimg = ReadFile(kpimg_path);
    const KpHeaderInfo kp_info = ParseKpHeaderAt(kpimg, 0);
    std::printf("%x\n", ToVersionCode(kp_info));
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "version failed: %s\n", e.what());
    return false;
  }
}

bool PrintKernelPatchListImage(const std::string& image_path) {
  try {
    if (!InspectKernelImage(image_path)) {
      return false;
    }
    const auto all = ReadFile(image_path);
    std::size_t img_off = 0;
    if (all.size() >= 20 && std::memcmp(all.data(), kUncompressedPrefix.data(), kUncompressedPrefix.size()) == 0) {
      img_off = 20;
    }
    if (img_off >= all.size()) {
      throw std::runtime_error("empty kernel image payload");
    }
    std::vector<std::uint8_t> kimg(all.begin() + static_cast<std::ptrdiff_t>(img_off), all.end());
    const auto kp_off = FindKpPreset(kimg);
    if (!kp_off.has_value()) {
      return true;
    }
    PrintPresetDetails(kimg, *kp_off);
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "list image failed: %s\n", e.what());
    return false;
  }
}

bool PrintKernelPatchListKpimg(const std::string& kpimg_path) {
  try {
    if (!InspectKernelPatchImage(kpimg_path)) {
      return false;
    }
    const auto kpimg = ReadFile(kpimg_path);
    ParseKpHeaderAt(kpimg, 0);
    PrintPresetDetails(kpimg, 0);
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "list kpimg failed: %s\n", e.what());
    return false;
  }
}

bool UnpatchKernelImage(const std::string& image_path, const std::string& out_path) {
  try {
    auto all = ReadFile(image_path);

    bool has_uncompressed_prefix = false;
    std::size_t img_off = 0;
    if (all.size() >= 20 && std::memcmp(all.data(), kUncompressedPrefix.data(), kUncompressedPrefix.size()) == 0) {
      has_uncompressed_prefix = true;
      img_off = 20;
    }

    if (img_off >= all.size()) {
      throw std::runtime_error("empty kernel image payload");
    }

    std::vector<std::uint8_t> kimg(all.begin() + static_cast<std::ptrdiff_t>(img_off), all.end());
    const auto kp_off_opt = FindKpPreset(kimg);
    if (!kp_off_opt.has_value()) {
      throw std::runtime_error("not a patched kernel image");
    }

    const std::size_t kp_off = *kp_off_opt;
    if (kp_off + kPresetHeaderBackupOffset + kHeaderBackupLen > kimg.size()) {
      throw std::runtime_error("preset layout is invalid");
    }

    // Restore the original kernel entry/header bytes.
    std::copy_n(kimg.begin() + static_cast<std::ptrdiff_t>(kp_off + kPresetHeaderBackupOffset),
                kHeaderBackupLen,
                kimg.begin());

    std::size_t original_kimg_size = static_cast<std::size_t>(ReadLe64(kimg, kp_off + kPresetKimgSizeOffset));
    if (original_kimg_size == 0 || original_kimg_size > kimg.size()) {
      original_kimg_size = kp_off;
    }
    if (original_kimg_size == 0 || original_kimg_size > kimg.size()) {
      throw std::runtime_error("invalid original kernel size in preset");
    }

    const std::size_t output_size = has_uncompressed_prefix ? (img_off + original_kimg_size) : original_kimg_size;
    if (output_size > all.size()) {
      throw std::runtime_error("output size out of range");
    }

    // Update UNCOMPRESSED_IMG payload size field if present.
    if (has_uncompressed_prefix) {
      WriteLe32(&all, 16, static_cast<std::uint32_t>(original_kimg_size));
    }

    std::copy_n(kimg.begin(), static_cast<std::ptrdiff_t>(original_kimg_size), all.begin() + static_cast<std::ptrdiff_t>(img_off));
    WriteFile(out_path, all, output_size);
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "unpatch failed: %s\n", e.what());
    return false;
  }
}

bool ResetKernelSuperKey(const std::string& image_path, const std::string& out_path, const std::string& superkey) {
  try {
    if (superkey.empty() || superkey.size() >= kSuperKeyLen) {
      throw std::runtime_error("invalid superkey length");
    }

    auto all = ReadFile(image_path);

    std::size_t img_off = 0;
    if (all.size() >= 20 && std::memcmp(all.data(), kUncompressedPrefix.data(), kUncompressedPrefix.size()) == 0) {
      img_off = 20;
    }
    if (img_off >= all.size()) {
      throw std::runtime_error("empty kernel image payload");
    }

    std::vector<std::uint8_t> kimg(all.begin() + static_cast<std::ptrdiff_t>(img_off), all.end());
    const auto kp_off_opt = FindKpPreset(kimg);
    if (!kp_off_opt.has_value()) {
      throw std::runtime_error("not a patched kernel image");
    }
    const std::size_t kp_off = *kp_off_opt;
    if (kp_off + kPresetSuperKeyOffset + kSuperKeyLen > kimg.size()) {
      throw std::runtime_error("preset layout is invalid");
    }

    std::fill_n(kimg.begin() + static_cast<std::ptrdiff_t>(kp_off + kPresetSuperKeyOffset),
                static_cast<std::ptrdiff_t>(kSuperKeyLen),
                static_cast<std::uint8_t>(0));
    std::copy(superkey.begin(),
              superkey.end(),
              kimg.begin() + static_cast<std::ptrdiff_t>(kp_off + kPresetSuperKeyOffset));

    std::copy(kimg.begin(), kimg.end(), all.begin() + static_cast<std::ptrdiff_t>(img_off));
    WriteFile(out_path, all, all.size());
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "reset-key failed: %s\n", e.what());
    return false;
  }
}

bool PatchKernelImage(const std::string& image_path,
                      const std::string& kpimg_path,
                      const std::string& out_path,
                      const std::string& superkey,
                      const std::string& reuse_config_from,
                      bool dry_run,
                      bool root_superkey,
                      const std::vector<std::string>& additions,
                      const std::vector<PatchExtraConfig>& extra_configs) {
  try {
    if (superkey.empty() || superkey.size() >= kSuperKeyLen) {
      throw std::runtime_error("invalid superkey length");
    }

    const auto image_all = ReadFile(image_path);
    const auto kpimg = ReadFile(kpimg_path);
    if (kpimg.size() < 64) {
      throw std::runtime_error("kpimg too small");
    }

    std::size_t image_off = 0;
    bool has_uncompressed_prefix = false;
    if (image_all.size() >= 20 &&
        std::memcmp(image_all.data(), kUncompressedPrefix.data(), kUncompressedPrefix.size()) == 0) {
      has_uncompressed_prefix = true;
      image_off = 20;
    }
    if (image_off >= image_all.size()) {
      throw std::runtime_error("empty kernel image payload");
    }

    std::vector<std::uint8_t> kimg(image_all.begin() + static_cast<std::ptrdiff_t>(image_off), image_all.end());
    const Arm64ImageInfo image_info = ParseArm64ImageHeader(kimg);
    const auto patched_off = FindKpPreset(kimg);
    const KpHeaderInfo kp_info = ParseKpHeaderAt(kpimg, 0);
    std::vector<ParsedExtraItem> existing_extras;
    if (patched_off.has_value()) {
      existing_extras = ParseExtraItems(kimg, *patched_off);
    }
    const auto output_extras = BuildOutputExtraItems(existing_extras, extra_configs);
    const std::size_t extra_blob_size = ComputeExtraBlobSize(output_extras);

    std::vector<std::uint8_t> reused_kimg;
    std::optional<std::size_t> reused_off;
    if (!reuse_config_from.empty()) {
      const auto reused_all = ReadFile(reuse_config_from);
      std::size_t reused_img_off = 0;
      if (reused_all.size() >= 20 &&
          std::memcmp(reused_all.data(), kUncompressedPrefix.data(), kUncompressedPrefix.size()) == 0) {
        reused_img_off = 20;
      }
      if (reused_img_off >= reused_all.size()) {
        throw std::runtime_error("reuse-config image payload is empty");
      }
      reused_kimg.assign(reused_all.begin() + static_cast<std::ptrdiff_t>(reused_img_off), reused_all.end());
      reused_off = FindKpPreset(reused_kimg);
      if (!reused_off.has_value()) {
        throw std::runtime_error("reuse-config image is not patched");
      }
    }

    std::size_t ori_kimg_size = kimg.size();
    if (patched_off.has_value()) {
      const std::size_t saved_kimg_size_off = *patched_off + kPresetKimgSizeOffset;
      if (saved_kimg_size_off + 8 <= kimg.size()) {
        const std::size_t saved_kimg_size =
            static_cast<std::size_t>(ReadLe64(kimg, saved_kimg_size_off));
        if (saved_kimg_size > 0 && saved_kimg_size <= *patched_off &&
            AlignUp(saved_kimg_size, 4096) == *patched_off) {
          ori_kimg_size = saved_kimg_size;
        } else {
          ori_kimg_size = *patched_off;
        }
      } else {
        ori_kimg_size = *patched_off;
      }
    }
    const std::size_t align_kimg = AlignUp(ori_kimg_size, 4096);
    const std::size_t align_kpimg = AlignUp(kpimg.size(), 16);
    const std::size_t planned_out_size = align_kimg + align_kpimg + extra_blob_size;

    std::ostringstream kp_ver;
    kp_ver << static_cast<int>(kp_info.major) << '.'
           << static_cast<int>(kp_info.minor) << '.'
           << static_cast<int>(kp_info.patch);

    PrintKey("patch_mode", dry_run ? "dry-run" : "write");
    PrintKey("image", image_path);
    PrintKey("kpimg", kpimg_path);
    PrintKey("out", out_path);
    PrintKey("superkey_len", std::to_string(superkey.size()));
    PrintKey("root_superkey_mode", root_superkey ? "true" : "false");
    PrintKey("addition_num", std::to_string(additions.size()));
    PrintKey("extra_num", std::to_string(output_extras.size()));
    PrintKey("image_has_prefix", has_uncompressed_prefix ? "true" : "false");
    PrintKey("image_already_patched", patched_off.has_value() ? "true" : "false");
    if (patched_off.has_value()) {
      PrintHex("image_patch_offset", *patched_off);
    }
    PrintHex("image_primary_entry", image_info.primary_entry_offset);
    PrintHex("image_kernel_size_header", image_info.kernel_size);
    PrintHex("plan_ori_kimg_size", ori_kimg_size);
    PrintHex("plan_aligned_kimg", align_kimg);
    PrintHex("plan_kpimg_size", kpimg.size());
    PrintHex("plan_aligned_kpimg", align_kpimg);
    PrintHex("plan_output_size", planned_out_size);
    PrintKey("kpimg_version", kp_ver.str());
    PrintHex("kpimg_version_code", ToVersionCode(kp_info));
    if (patched_off.has_value()) {
      PrintKey("symbol_source", "reuse-from-input-preset");
    } else if (reused_off.has_value()) {
      PrintKey("symbol_source", "reuse-from-external-preset");
      PrintKey("reuse_config_from", reuse_config_from);
    } else {
      const auto probe = ProbeKallsymsLayout(kimg);
      if (probe.has_symbol_offsets) {
        PrintKey("symbol_source", "pure-rewrite-kallsyms");
      } else {
        PrintKey("symbol_source", "missing");
      }
    }

    if (!dry_run) {
      const auto probe = ProbeKallsymsLayout(kimg);
      std::vector<std::uint8_t> out_kimg(planned_out_size, 0);
      std::copy_n(kimg.begin(), static_cast<std::ptrdiff_t>(ori_kimg_size), out_kimg.begin());
      std::copy(kpimg.begin(), kpimg.end(), out_kimg.begin() + static_cast<std::ptrdiff_t>(align_kimg));

      // Rewrite minimal setup fields in appended kpimg preset area.
      const std::size_t preset_off = align_kimg;
      if (preset_off + kPresetSuperKeyOffset + kSuperKeyLen > out_kimg.size()) {
        throw std::runtime_error("output preset layout out of range");
      }
      const std::size_t align_kernel_size = AlignUp(static_cast<std::size_t>(image_info.kernel_size), 4096);
      const std::size_t start_offset = std::max(align_kernel_size, AlignUp(planned_out_size, 4096));

      WriteLe64(&out_kimg, preset_off + kPresetKimgSizeOffset, static_cast<std::uint64_t>(ori_kimg_size));
      WriteLe64(&out_kimg, preset_off + 80, static_cast<std::uint64_t>(align_kpimg));
      WriteLe64(&out_kimg, preset_off + 88, static_cast<std::uint64_t>(image_info.kernel_size));
      WriteLe64(&out_kimg, preset_off + 96, static_cast<std::uint64_t>(image_info.page_shift));
      WriteLe64(&out_kimg, preset_off + 104, static_cast<std::uint64_t>(align_kimg));
      WriteLe64(&out_kimg, preset_off + 112, static_cast<std::uint64_t>(start_offset));
      WriteLe64(&out_kimg, preset_off + 120, static_cast<std::uint64_t>(extra_blob_size));

      if (patched_off.has_value() || reused_off.has_value()) {
        // Reuse resolved symbol/mapping fields from previous preset.
        const bool use_external = !patched_off.has_value();
        const std::vector<std::uint8_t>& src_kimg = use_external ? reused_kimg : kimg;
        const std::size_t old_preset = use_external ? *reused_off : *patched_off;
        if (old_preset + kPresetPatchConfigOffset + kPresetPatchConfigLen > src_kimg.size()) {
          throw std::runtime_error("source preset layout out of range");
        }
        std::copy_n(src_kimg.begin() + static_cast<std::ptrdiff_t>(old_preset + kPresetKernelVersionOffset),
                    8,
                    out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetKernelVersionOffset));
        std::copy_n(src_kimg.begin() + static_cast<std::ptrdiff_t>(old_preset + kPresetMapOffset),
                    8,
                    out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetMapOffset));
        std::copy_n(src_kimg.begin() + static_cast<std::ptrdiff_t>(old_preset + kPresetMapMaxSizeOffset),
                    8,
                    out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetMapMaxSizeOffset));
        std::copy_n(src_kimg.begin() + static_cast<std::ptrdiff_t>(old_preset + kPresetKallsymsLookupOffset),
                    8,
                    out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetKallsymsLookupOffset));
        std::copy_n(src_kimg.begin() + static_cast<std::ptrdiff_t>(old_preset + kPresetPagingInitOffset),
                    8,
                    out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetPagingInitOffset));
        std::copy_n(src_kimg.begin() + static_cast<std::ptrdiff_t>(old_preset + kPresetPrintkOffset),
                    8,
                    out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetPrintkOffset));
        std::copy_n(src_kimg.begin() + static_cast<std::ptrdiff_t>(old_preset + kPresetMapSymbolOffset),
                    kPresetMapSymbolLen,
                    out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetMapSymbolOffset));
        std::copy_n(src_kimg.begin() + static_cast<std::ptrdiff_t>(old_preset + kPresetPatchConfigOffset),
                    kPresetPatchConfigLen,
                    out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetPatchConfigOffset));
      } else {
        // Pure rewrite path from kallsyms probe offsets.
        std::array<std::uint8_t, 8> kernel_ver = {0, static_cast<std::uint8_t>(probe.version_patch),
                                                   static_cast<std::uint8_t>(probe.version_minor),
                                                   static_cast<std::uint8_t>(probe.version_major),
                                                   0, 0, 0, 0};
        std::copy(kernel_ver.begin(), kernel_ver.end(),
                  out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetKernelVersionOffset));

        MaybeDisablePiMap(&out_kimg, probe.version_major, probe.version_minor);
        std::size_t map_offset = 0;
        if (probe.tcp_init_sock_offset >= 0) {
          const std::size_t tcp_off = static_cast<std::size_t>(probe.tcp_init_sock_offset);
          map_offset = AlignDown(tcp_off, 16);
          PatchMapAreaPac(&out_kimg, tcp_off, 0x800);
        }
        WriteLe64(&out_kimg, preset_off + kPresetMapOffset, static_cast<std::uint64_t>(map_offset));
        WriteLe64(&out_kimg, preset_off + kPresetMapMaxSizeOffset, static_cast<std::uint64_t>(0x800));
        WriteLe64(&out_kimg, preset_off + kPresetKallsymsLookupOffset,
                  static_cast<std::uint64_t>(probe.kallsyms_lookup_name_offset));
        WriteLe64(&out_kimg, preset_off + kPresetPagingInitOffset, static_cast<std::uint64_t>(probe.paging_init_offset));
        WriteLe64(&out_kimg, preset_off + kPresetPrintkOffset, static_cast<std::uint64_t>(probe.printk_offset));

        const int memblock_phys = probe.memblock_phys_alloc_try_nid_offset >= 0
                                      ? probe.memblock_phys_alloc_try_nid_offset
                                      : probe.memblock_alloc_try_nid_offset;
        const int memblock_virt = probe.memblock_virt_alloc_try_nid_offset >= 0
                                      ? probe.memblock_virt_alloc_try_nid_offset
                                      : probe.memblock_alloc_try_nid_offset;
        WriteLe64(&out_kimg, preset_off + kPresetMapSymbolOffset + 0, static_cast<std::uint64_t>(probe.memblock_reserve_offset));
        WriteLe64(&out_kimg, preset_off + kPresetMapSymbolOffset + 8, static_cast<std::uint64_t>(probe.memblock_free_offset));
        WriteLe64(&out_kimg, preset_off + kPresetMapSymbolOffset + 16, static_cast<std::uint64_t>(memblock_phys));
        WriteLe64(&out_kimg, preset_off + kPresetMapSymbolOffset + 24, static_cast<std::uint64_t>(memblock_virt));
        WriteLe64(&out_kimg, preset_off + kPresetMapSymbolOffset + 32,
                  static_cast<std::uint64_t>(probe.memblock_mark_nomap_offset >= 0 ? probe.memblock_mark_nomap_offset : 0));

        // patch_config block
        std::fill_n(out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetPatchConfigOffset),
                    static_cast<std::ptrdiff_t>(kPresetPatchConfigLen),
                    static_cast<std::uint8_t>(0));
        auto write_patch_u64 = [&](std::size_t index, int off) {
          if (off >= 0) {
            WriteLe64(&out_kimg, preset_off + kPresetPatchConfigOffset + index * 8, static_cast<std::uint64_t>(off));
          }
        };
        write_patch_u64(2, probe.panic_offset);
        write_patch_u64(3, probe.rest_init_offset);
        if (probe.rest_init_offset < 0) {
          write_patch_u64(4, probe.cgroup_init_offset);
        }
        write_patch_u64(5, probe.kernel_init_offset);
        write_patch_u64(6, probe.report_cfi_failure_offset);
        write_patch_u64(7, probe.cfi_slowpath_diag_offset);
        write_patch_u64(8, probe.cfi_slowpath_offset);
        write_patch_u64(9, probe.copy_process_offset);
        if (probe.copy_process_offset < 0) {
          write_patch_u64(10, probe.cgroup_post_fork_offset);
        }
        write_patch_u64(11, probe.avc_denied_offset);
        write_patch_u64(12, probe.slow_avc_audit_offset);
        write_patch_u64(13, probe.input_handle_event_offset);
      }

      if (patched_off.has_value()) {
        const std::size_t old_header_backup =
            *patched_off + kPresetHeaderBackupOffset;
        if (old_header_backup + kHeaderBackupLen > kimg.size()) {
          throw std::runtime_error("input preset header backup out of range");
        }
        // Repatching must preserve the original kernel header bytes saved by
        // the first patch. Copying the current image head would re-save the
        // already-patched branch instruction and break early boot.
        std::copy_n(kimg.begin() + static_cast<std::ptrdiff_t>(old_header_backup),
                    static_cast<std::ptrdiff_t>(kHeaderBackupLen),
                    out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetHeaderBackupOffset));
      } else {
        std::copy_n(kimg.begin(), static_cast<std::ptrdiff_t>(kHeaderBackupLen),
                    out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetHeaderBackupOffset));
      }

      std::fill_n(out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetSuperKeyOffset),
                  static_cast<std::ptrdiff_t>(kSuperKeyLen),
                  static_cast<std::uint8_t>(0));
      std::fill_n(out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetRootSuperKeyOffset),
                  static_cast<std::ptrdiff_t>(kRootSuperKeyLen + kSetupPreserveLen),
                  static_cast<std::uint8_t>(0));
      if (root_superkey) {
        const auto digest = Sha256KeyDigest(superkey);
        std::copy(digest.begin(),
                  digest.end(),
                  out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetRootSuperKeyOffset));
      } else {
        std::copy(superkey.begin(),
                  superkey.end(),
                  out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetSuperKeyOffset));
      }

      std::fill_n(out_kimg.begin() + static_cast<std::ptrdiff_t>(preset_off + kPresetAdditionalOffset),
                  static_cast<std::ptrdiff_t>(kPresetAdditionalLen),
                  static_cast<std::uint8_t>(0));
      const PolicySlot policy_slot = BuildPolicySlot(additions);
      WritePolicySlot(&out_kimg, preset_off + kPresetAdditionalOffset, policy_slot);
      std::size_t addition_pos = preset_off + kPresetAdditionalOffset + kPolicyAdditionalTextOffset;
      for (const auto& kv : additions) {
        if (IsPolicyAddition(kv)) {
          continue;
        }
        if (kv.find('=') == std::string::npos) {
          throw std::runtime_error("addition must be KEY=VALUE: " + kv);
        }
        if (kv.size() > 127) {
          throw std::runtime_error("addition too long: " + kv);
        }
        if (addition_pos + 1 + kv.size() > preset_off + kPresetAdditionalOffset + kPresetAdditionalLen) {
          throw std::runtime_error("no memory for addition");
        }
        out_kimg[addition_pos++] = static_cast<std::uint8_t>(kv.size());
        std::copy(kv.begin(), kv.end(), out_kimg.begin() + static_cast<std::ptrdiff_t>(addition_pos));
        addition_pos += kv.size();
      }

      std::size_t extra_pos = align_kimg + align_kpimg;
      for (const auto& item : output_extras) {
        if (extra_pos + kPatchExtraItemLen + item.args_data.size() + item.content.size() > out_kimg.size()) {
          throw std::runtime_error("extra item out of range");
        }
        std::vector<std::uint8_t> header(kPatchExtraItemLen, 0);
        header[0] = 'k';
        header[1] = 'p';
        header[2] = 'e';
        WriteLe32(&header, 4, static_cast<std::uint32_t>(item.priority));
        WriteLe32(&header, 8, static_cast<std::uint32_t>(item.args_data.size()));
        WriteLe32(&header, 12, static_cast<std::uint32_t>(item.content.size()));
        WriteLe32(&header, 16, static_cast<std::uint32_t>(item.type));
        std::copy_n(item.name.begin(),
                    std::min(item.name.size(), kExtraNameLen - 1),
                    header.begin() + 20);
        std::copy_n(item.event.begin(),
                    std::min(item.event.size(), kExtraEventLen - 1),
                    header.begin() + 52);
        std::copy(header.begin(), header.end(), out_kimg.begin() + static_cast<std::ptrdiff_t>(extra_pos));
        extra_pos += kPatchExtraItemLen;
        if (!item.args_data.empty()) {
          std::copy(item.args_data.begin(), item.args_data.end(), out_kimg.begin() + static_cast<std::ptrdiff_t>(extra_pos));
          extra_pos += item.args_data.size();
        }
        if (!item.content.empty()) {
          std::copy(item.content.begin(), item.content.end(), out_kimg.begin() + static_cast<std::ptrdiff_t>(extra_pos));
          extra_pos += item.content.size();
        }
      }
      if (extra_pos + kPatchExtraItemLen > out_kimg.size()) {
        throw std::runtime_error("extra guard out of range");
      }
      std::fill_n(out_kimg.begin() + static_cast<std::ptrdiff_t>(extra_pos),
                  static_cast<std::ptrdiff_t>(kPatchExtraItemLen),
                  static_cast<std::uint8_t>(0));

      // Redirect kernel primary branch to kpimg text area.
      const std::uint32_t text_offset = static_cast<std::uint32_t>(align_kimg + 4096);
      const std::uint32_t branch_off = image_info.uefi ? 4u : 0u;
      const std::uint32_t new_b = EncodeArm64B(branch_off, text_offset);
      WriteLe32(&out_kimg, branch_off, new_b);

      // Keep ARM64 image header kernel_size semantics consistent with kptools.
      WriteLe64(&out_kimg, 16, image_info.kernel_size);

      std::vector<std::uint8_t> out_all;
      if (has_uncompressed_prefix) {
        out_all.assign(image_all.begin(), image_all.begin() + static_cast<std::ptrdiff_t>(image_off));
        out_all.insert(out_all.end(), out_kimg.begin(), out_kimg.end());
        WriteLe32(&out_all, 16, static_cast<std::uint32_t>(out_kimg.size()));
      } else {
        out_all.swap(out_kimg);
      }

      WriteFile(out_path, out_all, out_all.size());
      std::fprintf(stderr,
                   "patch write-mode(partial) completed: symbol/map config source resolved.\n");
    }
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "patch failed: %s\n", e.what());
    return false;
  }
}

}  // namespace apd
