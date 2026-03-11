#include "kallsyms.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <functional>
#include <string>
#include <stdexcept>
#include <vector>

#include <zlib.h>

namespace apd {
namespace {

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

std::vector<std::size_t> FindBanners(const std::vector<std::uint8_t>& image) {
  static constexpr char kPrefix[] = "Linux version ";
  const auto* begin = reinterpret_cast<const char*>(image.data());
  const auto* end = begin + static_cast<std::ptrdiff_t>(image.size());
  const auto* pos = begin;
  std::vector<std::size_t> offsets;
  while (pos < end) {
    const void* found = std::search(pos, end, std::begin(kPrefix), std::end(kPrefix) - 1);
    if (found == end) {
      break;
    }
    const auto* p = static_cast<const char*>(found);
    const std::size_t prefix_len = std::strlen(kPrefix);
    if (p + static_cast<std::ptrdiff_t>(prefix_len + 2) < end &&
        std::isdigit(static_cast<unsigned char>(p[prefix_len])) && p[prefix_len + 1] == '.') {
      offsets.push_back(static_cast<std::size_t>(p - begin));
    }
    pos = p + 1;
  }
  return offsets;
}

bool ParseVersion(const char* banner, int* major, int* minor, int* patch) {
  static constexpr char kPrefix[] = "Linux version ";
  const char* rel = banner + std::strlen(kPrefix);
  char* end = nullptr;
  long ma = std::strtol(rel, &end, 10);
  if (!end || *end != '.') {
    return false;
  }
  long mi = std::strtol(end + 1, &end, 10);
  if (!end || *end != '.') {
    return false;
  }
  long pa = std::strtol(end + 1, &end, 10);
  if (ma < 0 || mi < 0 || pa < 0) {
    return false;
  }
  *major = static_cast<int>(ma);
  *minor = static_cast<int>(mi);
  *patch = static_cast<int>(pa);
  return true;
}

std::optional<std::size_t> FindTokenTable(const std::vector<std::uint8_t>& image) {
  std::array<std::uint8_t, 20> nums{};
  for (int i = 0; i < 10; ++i) {
    nums[static_cast<std::size_t>(i * 2)] = static_cast<std::uint8_t>('0' + i);
  }

  std::array<std::uint8_t, 20> letters{};
  for (int i = 0; i < 10; ++i) {
    letters[static_cast<std::size_t>(i * 2)] = static_cast<std::uint8_t>('a' + i);
  }

  std::size_t start = 0;
  while (start < image.size()) {
    auto num_it = std::search(image.begin() + static_cast<std::ptrdiff_t>(start), image.end(), nums.begin(), nums.end());
    if (num_it == image.end()) {
      return std::nullopt;
    }
    const std::size_t num_off = static_cast<std::size_t>(num_it - image.begin());
    const std::size_t num_end = num_off + nums.size();
    if (num_end + 1 >= image.size()) {
      return std::nullopt;
    }
    if (image[num_end] == 0 || image[num_end + 1] == 0) {
      start = num_off + 1;
      continue;
    }

    auto let_it = std::search(image.begin() + static_cast<std::ptrdiff_t>(num_end), image.end(), letters.begin(), letters.end());
    if (let_it == image.end()) {
      return std::nullopt;
    }
    const std::size_t let_off = static_cast<std::size_t>(let_it - image.begin());
    if (let_off - num_end > 128) {
      start = num_off + 1;
      continue;
    }

    std::size_t pos = num_off;
    int zeros = 0;
    while (pos > 0 && zeros < static_cast<int>('0' + 1)) {
      --pos;
      if (image[pos] == 0) {
        ++zeros;
      }
    }
    std::size_t table = pos + 1;
    table = (table + 3) & ~static_cast<std::size_t>(3);
    return table;
  }
  return std::nullopt;
}

std::optional<std::pair<std::size_t, bool>> FindTokenIndex(const std::vector<std::uint8_t>& image,
                                                            std::size_t token_table_offset) {
  if (token_table_offset >= image.size()) {
    return std::nullopt;
  }

  std::array<std::uint8_t, 512> le{};
  std::array<std::uint8_t, 512> be{};

  std::size_t cur = token_table_offset;
  for (int i = 0; i < 256; ++i) {
    if (cur >= image.size()) {
      return std::nullopt;
    }
    const std::uint16_t idx = static_cast<std::uint16_t>(cur - token_table_offset);
    le[static_cast<std::size_t>(i * 2)] = static_cast<std::uint8_t>(idx & 0xFFu);
    le[static_cast<std::size_t>(i * 2 + 1)] = static_cast<std::uint8_t>((idx >> 8) & 0xFFu);
    be[static_cast<std::size_t>(i * 2)] = static_cast<std::uint8_t>((idx >> 8) & 0xFFu);
    be[static_cast<std::size_t>(i * 2 + 1)] = static_cast<std::uint8_t>(idx & 0xFFu);

    while (cur < image.size() && image[cur] != 0) {
      ++cur;
    }
    if (cur >= image.size()) {
      return std::nullopt;
    }
    ++cur;
  }

  auto it_le = std::search(image.begin(), image.end(), le.begin(), le.end());
  if (it_le != image.end()) {
    return std::make_pair(static_cast<std::size_t>(it_le - image.begin()), true);
  }
  auto it_be = std::search(image.begin(), image.end(), be.begin(), be.end());
  if (it_be != image.end()) {
    return std::make_pair(static_cast<std::size_t>(it_be - image.begin()), false);
  }
  return std::nullopt;
}

std::vector<std::string> BuildTokenTableStrings(const std::vector<std::uint8_t>& image,
                                                std::size_t token_table_offset) {
  std::vector<std::string> out;
  out.reserve(256);
  std::size_t pos = token_table_offset;
  for (int i = 0; i < 256; ++i) {
    if (pos >= image.size()) {
      throw std::runtime_error("token table out of range");
    }
    const char* start = reinterpret_cast<const char*>(image.data() + static_cast<std::ptrdiff_t>(pos));
    const char* end = start;
    while (pos < image.size() && image[pos] != 0) {
      ++pos;
      ++end;
    }
    if (pos >= image.size()) {
      throw std::runtime_error("unterminated token table string");
    }
    out.emplace_back(start, end);
    ++pos;  // skip NUL
  }
  return out;
}

int FindSymbolIndex(const std::vector<std::uint8_t>& image,
                    std::size_t names_offset,
                    std::size_t markers_offset,
                    int num_syms,
                    const std::vector<std::string>& tokens,
                    const std::string& target);

std::uint32_t ReadU32(const std::vector<std::uint8_t>& image, std::size_t off, bool little_endian) {
  if (off + 4 > image.size()) {
    throw std::runtime_error("out-of-range u32 read");
  }
  if (little_endian) {
    return static_cast<std::uint32_t>(image[off]) |
           (static_cast<std::uint32_t>(image[off + 1]) << 8) |
           (static_cast<std::uint32_t>(image[off + 2]) << 16) |
           (static_cast<std::uint32_t>(image[off + 3]) << 24);
  }
  return static_cast<std::uint32_t>(image[off + 3]) |
         (static_cast<std::uint32_t>(image[off + 2]) << 8) |
         (static_cast<std::uint32_t>(image[off + 1]) << 16) |
         (static_cast<std::uint32_t>(image[off]) << 24);
}

std::int32_t ReadI32(const std::vector<std::uint8_t>& image, std::size_t off, bool little_endian) {
  return static_cast<std::int32_t>(ReadU32(image, off, little_endian));
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& image, std::size_t off, bool little_endian) {
  if (off + 8 > image.size()) {
    throw std::runtime_error("out-of-range u64 read");
  }
  if (little_endian) {
    return static_cast<std::uint64_t>(image[off]) |
           (static_cast<std::uint64_t>(image[off + 1]) << 8) |
           (static_cast<std::uint64_t>(image[off + 2]) << 16) |
           (static_cast<std::uint64_t>(image[off + 3]) << 24) |
           (static_cast<std::uint64_t>(image[off + 4]) << 32) |
           (static_cast<std::uint64_t>(image[off + 5]) << 40) |
           (static_cast<std::uint64_t>(image[off + 6]) << 48) |
           (static_cast<std::uint64_t>(image[off + 7]) << 56);
  }
  return static_cast<std::uint64_t>(image[off + 7]) |
         (static_cast<std::uint64_t>(image[off + 6]) << 8) |
         (static_cast<std::uint64_t>(image[off + 5]) << 16) |
         (static_cast<std::uint64_t>(image[off + 4]) << 24) |
         (static_cast<std::uint64_t>(image[off + 3]) << 32) |
         (static_cast<std::uint64_t>(image[off + 2]) << 40) |
         (static_cast<std::uint64_t>(image[off + 1]) << 48) |
         (static_cast<std::uint64_t>(image[off]) << 56);
}

void WriteU64(std::vector<std::uint8_t>* image, std::size_t off, std::uint64_t value, bool little_endian) {
  if (off + 8 > image->size()) {
    throw std::runtime_error("out-of-range u64 write");
  }
  if (little_endian) {
    (*image)[off] = static_cast<std::uint8_t>(value & 0xFFu);
    (*image)[off + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    (*image)[off + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    (*image)[off + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
    (*image)[off + 4] = static_cast<std::uint8_t>((value >> 32) & 0xFFu);
    (*image)[off + 5] = static_cast<std::uint8_t>((value >> 40) & 0xFFu);
    (*image)[off + 6] = static_cast<std::uint8_t>((value >> 48) & 0xFFu);
    (*image)[off + 7] = static_cast<std::uint8_t>((value >> 56) & 0xFFu);
    return;
  }
  (*image)[off + 7] = static_cast<std::uint8_t>(value & 0xFFu);
  (*image)[off + 6] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  (*image)[off + 5] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  (*image)[off + 4] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
  (*image)[off + 3] = static_cast<std::uint8_t>((value >> 32) & 0xFFu);
  (*image)[off + 2] = static_cast<std::uint8_t>((value >> 40) & 0xFFu);
  (*image)[off + 1] = static_cast<std::uint8_t>((value >> 48) & 0xFFu);
  (*image)[off] = static_cast<std::uint8_t>((value >> 56) & 0xFFu);
}

std::uint64_t ReadUnsignedElem(const std::vector<std::uint8_t>& image,
                               std::size_t off,
                               int elem_size,
                               bool little_endian) {
  if (elem_size == 4) {
    return ReadU32(image, off, little_endian);
  }
  if (elem_size == 8) {
    return ReadU64(image, off, little_endian);
  }
  throw std::runtime_error("unsupported element size");
}

std::int64_t ReadSignedElem(const std::vector<std::uint8_t>& image,
                            std::size_t off,
                            int elem_size,
                            bool little_endian) {
  if (elem_size == 4) {
    return ReadI32(image, off, little_endian);
  }
  if (elem_size == 8) {
    return static_cast<std::int64_t>(ReadU64(image, off, little_endian));
  }
  throw std::runtime_error("unsupported signed element size");
}

bool TryApplyArm64Relocations(std::vector<std::uint8_t>* image) {
  constexpr std::uint64_t kElf64KernelMinVa = 0xffffff8008080000ULL;
  constexpr std::uint64_t kElf64KernelMaxVa = 0xffffffffffffffffULL;
  constexpr int kArm64ReloMinNum = 4000;
  constexpr std::uint32_t kRelaAbs64 = 0x101;
  constexpr std::uint32_t kRelaPrel64 = 0x403;

  if (image->size() < 24) {
    return false;
  }

  std::size_t cand = 0;
  int rela_num = 0;
  std::uint64_t kernel_va = kElf64KernelMaxVa;
  while (cand + 24 <= image->size()) {
    const std::uint64_t r_offset = ReadU64(*image, cand, true);
    const std::uint64_t r_info = ReadU64(*image, cand + 8, true);
    const std::uint64_t r_addend = ReadU64(*image, cand + 16, true);
    const std::uint32_t r_type = static_cast<std::uint32_t>(r_info & 0xffffffffULL);
    if ((r_offset & 0xffff000000000000ULL) == 0xffff000000000000ULL &&
        (r_type == kRelaAbs64 || r_type == kRelaPrel64)) {
      if ((r_addend & 0xfffULL) == 0 && r_addend >= kElf64KernelMinVa && r_addend < kernel_va) {
        kernel_va = r_addend;
      }
      cand += 24;
      ++rela_num;
    } else if (rela_num > 0 && r_offset == 0 && r_info == 0 && r_addend == 0) {
      cand += 24;
      ++rela_num;
    } else {
      if (rela_num >= kArm64ReloMinNum) {
        break;
      }
      cand += 8;
      rela_num = 0;
      kernel_va = kElf64KernelMaxVa;
    }
  }
  if (rela_num < kArm64ReloMinNum || kernel_va == kElf64KernelMaxVa) {
    return false;
  }

  const std::size_t cand_start = cand - static_cast<std::size_t>(rela_num) * 24;
  std::size_t cand_end = cand;
  while (cand_end >= cand_start + 24) {
    const std::size_t entry = cand_end - 24;
    const std::uint64_t r_offset = ReadU64(*image, entry, true);
    const std::uint64_t r_info = ReadU64(*image, entry + 8, true);
    const std::uint64_t r_addend = ReadU64(*image, entry + 16, true);
    if (!(r_offset == 0 && r_info == 0 && r_addend == 0)) {
      break;
    }
    cand_end -= 24;
  }
  if ((cand_end - cand_start) / 24 < static_cast<std::size_t>(kArm64ReloMinNum)) {
    return false;
  }

  bool modified = false;
  for (std::size_t off = cand_start; off + 24 <= cand_end; off += 24) {
    const std::uint64_t r_offset = ReadU64(*image, off, true);
    const std::uint64_t r_info = ReadU64(*image, off + 8, true);
    std::uint64_t r_addend = ReadU64(*image, off + 16, true);
    if (r_offset == 0 && r_info == 0 && r_addend == 0) {
      continue;
    }
    if (r_offset <= kernel_va || r_offset >= kElf64KernelMaxVa - image->size()) {
      continue;
    }

    const std::size_t target = static_cast<std::size_t>(r_offset - kernel_va);
    if (target + 8 > image->size()) {
      continue;
    }
    const std::uint32_t r_type = static_cast<std::uint32_t>(r_info & 0xffffffffULL);
    if (r_type == kRelaAbs64) {
      r_addend += kernel_va;
    }
    const std::uint64_t value = ReadU64(*image, target, true);
    if (value == r_addend) {
      continue;
    }
    WriteU64(image, target, value + r_addend, true);
    modified = true;
  }

  return modified;
}

std::optional<std::pair<std::size_t, std::size_t>> FindApproxRelativeOffsets(
    const std::vector<std::uint8_t>& image,
    bool little_endian) {
  constexpr std::size_t kElem = 4;
  constexpr int kMinNeqSyms = 25600;

  int sym_num = 0;
  std::int64_t prev = 0;
  std::size_t cand = 0;
  for (; cand + static_cast<std::size_t>(kMinNeqSyms) * kElem < image.size(); cand += kElem) {
    const std::int64_t cur = ReadI32(image, cand, little_endian);
    if (cur == prev) {
      continue;
    }
    if (cur > prev) {
      prev = cur;
      if (sym_num++ >= kMinNeqSyms) {
        break;
      }
    } else {
      prev = 0;
      sym_num = 0;
    }
  }
  if (sym_num < kMinNeqSyms || cand < static_cast<std::size_t>(kMinNeqSyms) * kElem) {
    return std::nullopt;
  }

  cand -= static_cast<std::size_t>(kMinNeqSyms) * kElem;
  while (cand >= kElem && ReadI32(image, cand, little_endian) != 0) {
    cand -= kElem;
  }

  constexpr int kMaxZeroPrefix = 10;
  int zero_count = 0;
  while (cand >= kElem) {
    if (ReadI32(image, cand, little_endian) != 0) {
      break;
    }
    if (zero_count++ >= kMaxZeroPrefix) {
      break;
    }
    cand -= kElem;
  }
  cand += kElem;
  const std::size_t start = cand;

  std::int64_t end_prev = 0;
  std::size_t end = start;
  for (; end + kElem <= image.size(); end += kElem) {
    const std::int64_t cur = ReadI32(image, end, little_endian);
    if (cur < end_prev) {
      break;
    }
    end_prev = cur;
  }
  if (end <= start) {
    return std::nullopt;
  }
  return std::make_pair(start, end);
}

std::optional<std::pair<std::size_t, std::size_t>> FindApproxAddresses(const std::vector<std::uint8_t>& image,
                                                                        bool little_endian,
                                                                        int elem_size) {
  constexpr int kMinNeqSyms = 25600;
  if (elem_size != 4 && elem_size != 8) {
    return std::nullopt;
  }

  int sym_num = 0;
  std::uint64_t prev = 0;
  std::size_t cand = 0;
  const std::size_t need = static_cast<std::size_t>(kMinNeqSyms) * static_cast<std::size_t>(elem_size);
  while (cand + need < image.size()) {
    const std::uint64_t address = ReadUnsignedElem(image, cand, elem_size, little_endian);
    if (sym_num == 0) {
      if ((address & 0xFFu) != 0) {
        cand += static_cast<std::size_t>(elem_size);
        continue;
      }
      if (elem_size == 4 && (address & 0xFF800000u) != 0xFF800000u) {
        cand += static_cast<std::size_t>(elem_size);
        continue;
      }
      if (elem_size == 8 && (address & 0xFFFF000000000000ULL) != 0xFFFF000000000000ULL) {
        cand += static_cast<std::size_t>(elem_size);
        continue;
      }
      prev = address;
      ++sym_num;
      cand += static_cast<std::size_t>(elem_size);
      continue;
    }
    if (address >= prev) {
      prev = address;
      if (sym_num++ >= kMinNeqSyms) {
        break;
      }
    } else {
      sym_num = 0;
      prev = 0;
    }
    cand += static_cast<std::size_t>(elem_size);
  }

  if (sym_num < kMinNeqSyms || cand < need) {
    return std::nullopt;
  }

  cand -= need;
  const std::size_t start = cand;
  std::uint64_t end_prev = 0;
  while (cand + static_cast<std::size_t>(elem_size) <= image.size()) {
    const std::uint64_t address = ReadUnsignedElem(image, cand, elem_size, little_endian);
    if (address < end_prev) {
      break;
    }
    end_prev = address;
    cand += static_cast<std::size_t>(elem_size);
  }
  if (cand <= start) {
    return std::nullopt;
  }
  return std::make_pair(start, cand);
}

struct ApproxTableResult {
  std::size_t start = 0;
  std::size_t end = 0;
  bool has_relative_base = false;
  int elem_size = 0;
};

std::optional<ApproxTableResult> FindApproxTable(const std::vector<std::uint8_t>& image,
                                                 bool little_endian) {
  if (const auto rel = FindApproxRelativeOffsets(image, little_endian); rel.has_value()) {
    ApproxTableResult out;
    out.start = rel->first;
    out.end = rel->second;
    out.has_relative_base = true;
    out.elem_size = 4;
    return out;
  }
  if (const auto addr64 = FindApproxAddresses(image, little_endian, 8); addr64.has_value()) {
    ApproxTableResult out;
    out.start = addr64->first;
    out.end = addr64->second;
    out.has_relative_base = false;
    out.elem_size = 8;
    return out;
  }
  if (const auto addr32 = FindApproxAddresses(image, little_endian, 4); addr32.has_value()) {
    ApproxTableResult out;
    out.start = addr32->first;
    out.end = addr32->second;
    out.has_relative_base = false;
    out.elem_size = 4;
    return out;
  }
  return std::nullopt;
}

int FindNumSymsGuess(const std::vector<std::uint8_t>& image,
                     std::size_t names_offset,
                     int approx_num_syms,
                     bool little_endian) {
  constexpr int kGap = 10;
  if (approx_num_syms <= 0) {
    return 0;
  }
  const std::size_t search_floor = names_offset > 4096 ? (names_offset - 4096) : 0;
  std::size_t cand = names_offset;
  while (cand >= search_floor + 4) {
    cand -= 4;
    const int nsyms = ReadI32(image, cand, little_endian);
    if (nsyms == 0) {
      continue;
    }
    if (nsyms > approx_num_syms && (nsyms - approx_num_syms) > kGap) {
      continue;
    }
    if (nsyms < approx_num_syms && (approx_num_syms - nsyms) > kGap) {
      continue;
    }
    return nsyms;
  }
  return std::max(approx_num_syms - kGap, 1);
}

std::optional<std::size_t> CorrectOffsetTableStartByBanner(const std::vector<std::uint8_t>& image,
                                                            std::size_t approx_start,
                                                            std::size_t names_offset,
                                                            std::size_t markers_offset,
                                                            int num_syms,
                                                            const std::vector<std::string>& tokens,
                                                            const std::vector<std::size_t>& banner_offsets,
                                                            bool little_endian,
                                                            int elem_size) {
  if (num_syms <= 0 || banner_offsets.empty()) {
    return std::nullopt;
  }
  const int banner_index =
      FindSymbolIndex(image, names_offset, markers_offset, num_syms, tokens, "linux_banner");
  if (banner_index < 0) {
    return std::nullopt;
  }
  const std::size_t idx_off = static_cast<std::size_t>(banner_index) * static_cast<std::size_t>(elem_size);
  const std::size_t search_end =
      std::min<std::size_t>(approx_start + 4096 + static_cast<std::size_t>(elem_size), image.size());
  for (const std::size_t banner_off : banner_offsets) {
    if (banner_off > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
      continue;
    }
    const std::int64_t target = static_cast<std::int64_t>(static_cast<std::int32_t>(banner_off));
    for (std::size_t pos = approx_start;
         pos + idx_off + static_cast<std::size_t>(elem_size) <= image.size() && pos < search_end;
         pos += static_cast<std::size_t>(elem_size)) {
      const std::uint64_t base = ReadUnsignedElem(image, pos, elem_size, little_endian);
      const std::uint64_t value = ReadUnsignedElem(image, pos + idx_off, elem_size, little_endian);
      const std::int64_t offset = static_cast<std::int64_t>(value) - static_cast<std::int64_t>(base);
      if (offset == target) {
        return pos;
      }
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> FindMarkers(const std::vector<std::uint8_t>& image,
                                       std::size_t token_table_offset,
                                       bool little_endian,
                                       int elem_size) {
  constexpr int kMinMarker = 100;
  if (elem_size != 4 && elem_size != 8) {
    return std::nullopt;
  }
  if (token_table_offset < 0x10000) {
    return std::nullopt;
  }
  std::size_t cand = token_table_offset;
  std::uint64_t last = static_cast<std::uint64_t>(image.size());
  int count = 0;
  while (cand > 0x10000) {
    const std::uint64_t marker = ReadUnsignedElem(image, cand, elem_size, little_endian);
    if (last > marker) {
      ++count;
      if (marker == 0 && count > kMinMarker) {
        return cand;
      }
    } else {
      count = 0;
      last = static_cast<std::uint64_t>(image.size());
    }
    last = marker;
    if (cand < static_cast<std::size_t>(elem_size)) {
      break;
    }
    cand -= static_cast<std::size_t>(elem_size);
  }
  return std::nullopt;
}

std::optional<std::size_t> FindNames(const std::vector<std::uint8_t>& image,
                                     std::size_t markers_offset,
                                     const std::vector<std::string>& tokens,
                                     int marker_elem_size,
                                     bool little_endian) {
  constexpr int kUseMarker = 5;
  if (marker_elem_size != 4 && marker_elem_size != 8) {
    return std::nullopt;
  }

  for (std::size_t cand = 0x4000; cand < markers_offset; ++cand) {
    std::size_t pos = cand;
    int remain = kUseMarker;
    for (int i = 0;; ++i) {
      if (pos >= markers_offset) {
        break;
      }
      int len = image[pos++];
      if (len > 0x7F) {
        if (pos >= markers_offset) {
          break;
        }
        len = (len & 0x7F) + (static_cast<int>(image[pos++]) << 7);
      }
      if (len <= 0 || len >= 512) {
        break;
      }
      pos += static_cast<std::size_t>(len);
      if (pos >= markers_offset) {
        break;
      }
      if (i != 0 && (i & 0xFF) == 0xFF) {
        const std::size_t mark_off =
            markers_offset + static_cast<std::size_t>((i >> 8) + 1) * static_cast<std::size_t>(marker_elem_size);
        if (mark_off + static_cast<std::size_t>(marker_elem_size) > image.size()) {
          break;
        }
        const std::uint64_t mark_len = ReadUnsignedElem(image, mark_off, marker_elem_size, little_endian);
        if (pos - cand != static_cast<std::size_t>(mark_len)) {
          break;
        }
        if (--remain == 0) {
          return cand;
        }
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string> DecompressNameAt(const std::vector<std::uint8_t>& image,
                                            std::size_t* pos,
                                            const std::vector<std::string>& tokens) {
  if (*pos >= image.size()) {
    return std::nullopt;
  }
  int len = image[*pos];
  ++(*pos);
  if (len > 0x7F) {
    if (*pos >= image.size()) {
      return std::nullopt;
    }
    len = (len & 0x7F) + (static_cast<int>(image[*pos]) << 7);
    ++(*pos);
  }
  if (len <= 0 || len >= 512 || *pos + static_cast<std::size_t>(len) > image.size()) {
    return std::nullopt;
  }
  std::string out;
  for (int i = 0; i < len; ++i) {
    const std::uint8_t idx = image[*pos + static_cast<std::size_t>(i)];
    std::string token = tokens[idx];
    if (i == 0 && !token.empty()) {
      token.erase(token.begin());  // strip symbol type
    }
    out += token;
  }
  *pos += static_cast<std::size_t>(len);
  return out;
}

std::optional<std::pair<char, std::string>> DecompressNameWithTypeAt(const std::vector<std::uint8_t>& image,
                                                                      std::size_t* pos,
                                                                      const std::vector<std::string>& tokens) {
  if (*pos >= image.size()) {
    return std::nullopt;
  }
  int len = image[*pos];
  ++(*pos);
  if (len > 0x7F) {
    if (*pos >= image.size()) {
      return std::nullopt;
    }
    len = (len & 0x7F) + (static_cast<int>(image[*pos]) << 7);
    ++(*pos);
  }
  if (len <= 0 || len >= 512 || *pos + static_cast<std::size_t>(len) > image.size()) {
    return std::nullopt;
  }

  std::string out;
  char type = '?';
  for (int i = 0; i < len; ++i) {
    const std::uint8_t idx = image[*pos + static_cast<std::size_t>(i)];
    std::string token = tokens[idx];
    if (i == 0) {
      if (!token.empty()) {
        type = token[0];
        token.erase(token.begin());
      }
    }
    out += token;
  }
  *pos += static_cast<std::size_t>(len);
  return std::make_pair(type, out);
}

int FindSymbolIndex(const std::vector<std::uint8_t>& image,
                    std::size_t names_offset,
                    std::size_t markers_offset,
                    int num_syms,
                    const std::vector<std::string>& tokens,
                    const std::string& target) {
  std::size_t pos = names_offset;
  for (int i = 0; i < num_syms && pos < markers_offset; ++i) {
    auto name = DecompressNameAt(image, &pos, tokens);
    if (!name.has_value()) {
      return -1;
    }
    if (*name == target) {
      return i;
    }
  }
  return -1;
}

bool IsSuffixedMatch(const std::string& actual, const std::string& target) {
  if (actual.size() <= target.size() || actual.compare(0, target.size(), target) != 0) {
    return false;
  }
  const char c = actual[target.size()];
  if (c != '.' && c != '$') {
    return false;
  }
  return actual.find(".cfi_jt") == std::string::npos;
}

int GetOffsetAtIndex(const std::vector<std::uint8_t>& image,
                     std::size_t start,
                     int index,
                     bool little_endian,
                     bool has_relative_base,
                     int elem_size,
                     std::uint64_t kernel_base) {
  if (elem_size != 4 && elem_size != 8) {
    return -1;
  }
  const std::size_t off = start + static_cast<std::size_t>(index) * static_cast<std::size_t>(elem_size);
  if (off + static_cast<std::size_t>(elem_size) > image.size()) {
    return -1;
  }
  const std::uint64_t raw = ReadUnsignedElem(image, off, elem_size, little_endian);
  if (has_relative_base) {
    const std::uint64_t base = ReadUnsignedElem(image, start, elem_size, little_endian);
    if (elem_size == 4) {
      return static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(raw - base)));
    }
    const auto diff = static_cast<std::int64_t>(raw) - static_cast<std::int64_t>(base);
    if (diff < std::numeric_limits<int>::min() || diff > std::numeric_limits<int>::max()) {
      return -1;
    }
    return static_cast<int>(diff);
  }
  if (raw < kernel_base) {
    return -1;
  }
  const auto offset = raw - kernel_base;
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    return -1;
  }
  return static_cast<int>(offset);
}

void ResolveImportantSymbols(KallsymsProbeResult* res,
                             const std::vector<std::uint8_t>& image,
                             std::size_t names_offset,
                             std::size_t markers_offset,
                             int num_syms,
                             const std::vector<std::string>& tokens) {
  if (res->offset_table_start == 0 || num_syms <= 0) {
    return;
  }

  struct OffsetPick {
    int exact = -1;
    int suffixed = -1;
  };

  auto resolve_pick = [](const OffsetPick& pick, bool allow_suffix) {
    if (pick.exact >= 0) {
      return pick.exact;
    }
    if (allow_suffix && pick.suffixed >= 0) {
      return pick.suffixed;
    }
    return -1;
  };

  OffsetPick kallsyms_lookup_name{};
  OffsetPick paging_init{};
  OffsetPick printk{};
  OffsetPick underscore_printk{};
  OffsetPick panic{};
  OffsetPick rest_init{};
  OffsetPick cgroup_init{};
  OffsetPick kernel_init{};
  OffsetPick copy_process{};
  OffsetPick cgroup_post_fork{};
  OffsetPick avc_denied{};
  OffsetPick slow_avc_audit{};
  OffsetPick input_handle_event{};
  OffsetPick report_cfi_failure{};
  OffsetPick cfi_slowpath_diag{};
  OffsetPick cfi_slowpath{};
  OffsetPick memblock_reserve{};
  OffsetPick memblock_free{};
  OffsetPick memblock_mark_nomap{};
  OffsetPick memblock_phys_alloc_try_nid{};
  OffsetPick memblock_virt_alloc_try_nid{};
  OffsetPick memblock_alloc_try_nid{};
  OffsetPick tcp_init_sock{};

  std::size_t pos = names_offset;
  for (int i = 0; i < num_syms && pos < markers_offset; ++i) {
    auto name = DecompressNameAt(image, &pos, tokens);
    if (!name.has_value()) {
      break;
    }
    const int offset =
        GetOffsetAtIndex(image,
                         res->offset_table_start,
                         i,
                         res->little_endian_token_index,
                         res->has_relative_base,
                         res->offset_table_elem_size,
                         res->kernel_base);
    if (offset < 0) {
      continue;
    }

    const auto pick_if = [&](const std::string& target, OffsetPick* pick, bool allow_suffix) {
      if (pick->exact < 0 && *name == target) {
        pick->exact = offset;
      }
      if (allow_suffix && pick->suffixed < 0 && IsSuffixedMatch(*name, target)) {
        pick->suffixed = offset;
      }
    };

    pick_if("kallsyms_lookup_name", &kallsyms_lookup_name, false);
    pick_if("paging_init", &paging_init, false);
    pick_if("printk", &printk, false);
    pick_if("_printk", &underscore_printk, false);
    pick_if("panic", &panic, false);
    pick_if("rest_init", &rest_init, true);
    pick_if("cgroup_init", &cgroup_init, true);
    pick_if("kernel_init", &kernel_init, true);
    pick_if("copy_process", &copy_process, true);
    pick_if("cgroup_post_fork", &cgroup_post_fork, false);
    pick_if("avc_denied", &avc_denied, true);
    pick_if("slow_avc_audit", &slow_avc_audit, true);
    pick_if("input_handle_event", &input_handle_event, false);
    pick_if("report_cfi_failure", &report_cfi_failure, false);
    pick_if("__cfi_slowpath_diag", &cfi_slowpath_diag, false);
    pick_if("__cfi_slowpath", &cfi_slowpath, false);
    pick_if("memblock_reserve", &memblock_reserve, false);
    pick_if("memblock_free", &memblock_free, false);
    pick_if("memblock_mark_nomap", &memblock_mark_nomap, false);
    pick_if("memblock_phys_alloc_try_nid", &memblock_phys_alloc_try_nid, false);
    pick_if("memblock_virt_alloc_try_nid", &memblock_virt_alloc_try_nid, false);
    pick_if("memblock_alloc_try_nid", &memblock_alloc_try_nid, false);
    pick_if("tcp_init_sock", &tcp_init_sock, false);
  }

  res->kallsyms_lookup_name_offset = resolve_pick(kallsyms_lookup_name, false);
  res->paging_init_offset = resolve_pick(paging_init, false);
  res->printk_offset = resolve_pick(printk, false);
  if (res->printk_offset < 0) {
    res->printk_offset = resolve_pick(underscore_printk, false);
  }

  res->panic_offset = resolve_pick(panic, false);
  res->rest_init_offset = resolve_pick(rest_init, true);
  if (res->rest_init_offset < 0) {
    res->cgroup_init_offset = resolve_pick(cgroup_init, true);
  }
  res->kernel_init_offset = resolve_pick(kernel_init, true);

  res->copy_process_offset = resolve_pick(copy_process, true);
  if (res->copy_process_offset < 0) {
    res->cgroup_post_fork_offset = resolve_pick(cgroup_post_fork, false);
  }
  res->avc_denied_offset = resolve_pick(avc_denied, true);
  res->slow_avc_audit_offset = resolve_pick(slow_avc_audit, true);
  res->input_handle_event_offset = resolve_pick(input_handle_event, false);
  res->report_cfi_failure_offset = resolve_pick(report_cfi_failure, false);
  res->cfi_slowpath_diag_offset = resolve_pick(cfi_slowpath_diag, false);
  res->cfi_slowpath_offset = resolve_pick(cfi_slowpath, false);

  res->memblock_reserve_offset = resolve_pick(memblock_reserve, false);
  res->memblock_free_offset = resolve_pick(memblock_free, false);
  res->memblock_mark_nomap_offset = resolve_pick(memblock_mark_nomap, false);
  res->memblock_phys_alloc_try_nid_offset = resolve_pick(memblock_phys_alloc_try_nid, false);
  res->memblock_virt_alloc_try_nid_offset = resolve_pick(memblock_virt_alloc_try_nid, false);
  res->memblock_alloc_try_nid_offset = resolve_pick(memblock_alloc_try_nid, false);
  if (res->memblock_phys_alloc_try_nid_offset < 0) {
    res->memblock_phys_alloc_try_nid_offset = res->memblock_alloc_try_nid_offset;
  }
  if (res->memblock_virt_alloc_try_nid_offset < 0) {
    res->memblock_virt_alloc_try_nid_offset = res->memblock_alloc_try_nid_offset;
  }
  res->tcp_init_sock_offset = resolve_pick(tcp_init_sock, false);

  if (res->kallsyms_lookup_name_offset >= 0 &&
      res->paging_init_offset >= 0 &&
      res->printk_offset >= 0 &&
      res->memblock_reserve_offset >= 0 &&
      res->memblock_free_offset >= 0 &&
      (res->memblock_phys_alloc_try_nid_offset >= 0 || res->memblock_alloc_try_nid_offset >= 0) &&
      (res->memblock_virt_alloc_try_nid_offset >= 0 || res->memblock_alloc_try_nid_offset >= 0) &&
      res->tcp_init_sock_offset >= 0) {
    res->has_symbol_offsets = true;
  }
}

}  // namespace

KallsymsProbeResult ProbeKallsymsLayout(const std::vector<std::uint8_t>& image) {
  KallsymsProbeResult res;
  std::vector<std::uint8_t> work = image;

  const auto banner_offsets = FindBanners(work);
  if (banner_offsets.empty()) {
    res.message = "linux banner not found";
    return res;
  }

  int major = 0;
  int minor = 0;
  int patch = 0;
  const char* banner_ptr =
      reinterpret_cast<const char*>(work.data() + static_cast<std::ptrdiff_t>(banner_offsets.back()));
  if (!ParseVersion(banner_ptr, &major, &minor, &patch)) {
    res.message = "failed to parse kernel version from banner";
    return res;
  }
  res.version_major = major;
  res.version_minor = minor;
  res.version_patch = patch;

  const auto token_table = FindTokenTable(work);
  if (!token_table.has_value()) {
    res.message = "kallsyms token_table probe failed";
    return res;
  }
  res.token_table_offset = *token_table;

  const auto token_index = FindTokenIndex(work, *token_table);
  if (!token_index.has_value()) {
    res.message = "kallsyms token_index probe failed";
    return res;
  }
  res.token_index_offset = token_index->first;
  res.little_endian_token_index = token_index->second;
  const auto tokens = BuildTokenTableStrings(work, *token_table);

  if (res.little_endian_token_index) {
    TryApplyArm64Relocations(&work);
  }

  std::array<int, 2> marker_candidates = {4, 8};
  if (major < 4 || (major == 4 && minor < 20)) {
    marker_candidates = {8, 4};
  }
  std::optional<std::size_t> markers = std::nullopt;
  int marker_elem_size = 0;
  for (int elem : marker_candidates) {
    markers = FindMarkers(work, *token_table, res.little_endian_token_index, elem);
    if (markers.has_value()) {
      marker_elem_size = elem;
      break;
    }
  }
  if (!markers.has_value()) {
    res.message = "kallsyms markers probe failed";
    return res;
  }
  res.markers_offset = *markers;
  res.markers_elem_size = marker_elem_size;

  const auto names = FindNames(work, *markers, tokens, marker_elem_size, res.little_endian_token_index);
  if (!names.has_value()) {
    res.message = "kallsyms names probe failed";
    return res;
  }
  res.names_offset = *names;

  const auto approx = FindApproxTable(work, res.little_endian_token_index);
  if (approx.has_value()) {
    res.approx_addr_or_offset_start = approx->start;
    res.approx_addr_or_offset_end = approx->end;
    res.offset_table_elem_size = approx->elem_size;
    res.approx_num_syms =
        static_cast<int>((approx->end - approx->start) / static_cast<std::size_t>(approx->elem_size));
    res.has_relative_base = approx->has_relative_base;
  }

  if (res.approx_num_syms > 0) {
    res.num_syms = FindNumSymsGuess(work, *names, res.approx_num_syms, res.little_endian_token_index);
    res.offset_table_start = res.approx_addr_or_offset_start;
    if (const auto corrected = CorrectOffsetTableStartByBanner(work,
                                                               res.approx_addr_or_offset_start,
                                                               *names,
                                                               *markers,
                                                               res.num_syms,
                                                               tokens,
                                                               banner_offsets,
                                                               res.little_endian_token_index,
                                                               res.offset_table_elem_size);
        corrected.has_value()) {
      res.offset_table_start = *corrected;
    }
    if (!res.has_relative_base && res.offset_table_start != 0) {
      res.kernel_base =
          ReadUnsignedElem(work, res.offset_table_start, res.offset_table_elem_size, res.little_endian_token_index);
    }
    res.kallsyms_lookup_name_index =
        FindSymbolIndex(work, *names, *markers, res.num_syms, tokens, "kallsyms_lookup_name");
    if (res.kallsyms_lookup_name_index >= 0) {
      res.kallsyms_lookup_name_offset = GetOffsetAtIndex(work,
                                                         res.offset_table_start,
                                                         res.kallsyms_lookup_name_index,
                                                         res.little_endian_token_index,
                                                         res.has_relative_base,
                                                         res.offset_table_elem_size,
                                                         res.kernel_base);
    }
    ResolveImportantSymbols(&res, work, *names, *markers, res.num_syms, tokens);
  }

  res.ok = true;
  res.message = res.has_symbol_offsets ? "probe ok (offset set ready)" : "probe ok (partial offsets)";
  return res;
}

bool PrintKallsymsProbe(const std::string& image_path) {
  try {
    auto image = ReadFile(image_path);
    auto res = ProbeKallsymsLayout(image);
    std::printf("file                     %s\n", image_path.c_str());
    std::printf("probe_ok                 %s\n", res.ok ? "true" : "false");
    std::printf("kernel_version           %d.%d.%d\n", res.version_major, res.version_minor, res.version_patch);
    if (res.token_table_offset != 0) {
      std::printf("token_table_offset       0x%zx\n", res.token_table_offset);
    }
    if (res.token_index_offset != 0) {
      std::printf("token_index_offset       0x%zx\n", res.token_index_offset);
      std::printf("token_index_endian       %s\n", res.little_endian_token_index ? "little" : "big");
    }
    if (res.markers_offset != 0) {
      std::printf("markers_offset           0x%zx\n", res.markers_offset);
      if (res.markers_elem_size != 0) {
        std::printf("markers_elem_size        %d\n", res.markers_elem_size);
      }
    }
    if (res.names_offset != 0) {
      std::printf("names_offset             0x%zx\n", res.names_offset);
    }
    if (res.approx_addr_or_offset_start != 0 || res.approx_addr_or_offset_end != 0) {
      std::printf("approx_offsets_start     0x%zx\n", res.approx_addr_or_offset_start);
      std::printf("approx_offsets_end       0x%zx\n", res.approx_addr_or_offset_end);
      std::printf("approx_num_syms          %d\n", res.approx_num_syms);
      std::printf("num_syms_guess           %d\n", res.num_syms);
      if (res.offset_table_start != 0) {
        std::printf("offset_table_start       0x%zx\n", res.offset_table_start);
      }
      if (res.offset_table_elem_size != 0) {
        std::printf("offset_table_elem_size   %d\n", res.offset_table_elem_size);
      }
      std::printf("offset_mode_relative     %s\n", res.has_relative_base ? "true" : "false");
      if (!res.has_relative_base && res.kernel_base != 0) {
        std::printf("kernel_base              0x%llx\n", static_cast<unsigned long long>(res.kernel_base));
      }
    }
    if (res.kallsyms_lookup_name_index >= 0) {
      std::printf("lookup_name_index        %d\n", res.kallsyms_lookup_name_index);
    }
    if (res.kallsyms_lookup_name_offset >= 0) {
      std::printf("lookup_name_offset       0x%x\n", res.kallsyms_lookup_name_offset);
    }
    if (res.paging_init_offset >= 0) {
      std::printf("paging_init_offset       0x%x\n", res.paging_init_offset);
    }
    if (res.printk_offset >= 0) {
      std::printf("printk_offset            0x%x\n", res.printk_offset);
    }
    if (res.tcp_init_sock_offset >= 0) {
      std::printf("tcp_init_sock_offset     0x%x\n", res.tcp_init_sock_offset);
    }
    std::printf("offset_set_ready         %s\n", res.has_symbol_offsets ? "true" : "false");
    std::printf("message                  %s\n", res.message.c_str());
    return res.ok;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "ksym-probe failed: %s\n", e.what());
    return false;
  }
}

bool DumpKallsyms(const std::string& image_path) {
  try {
    auto image = ReadFile(image_path);
    const auto res = ProbeKallsymsLayout(image);
    if (!res.ok || !res.has_symbol_offsets || res.num_syms <= 0 || res.names_offset == 0 || res.markers_offset == 0 ||
        res.offset_table_start == 0 || res.offset_table_elem_size == 0) {
      std::fprintf(stderr, "dump failed: kallsyms layout is not ready (%s)\n", res.message.c_str());
      return false;
    }

    std::vector<std::uint8_t> work = image;
    if (res.little_endian_token_index) {
      TryApplyArm64Relocations(&work);
    }
    const auto tokens = BuildTokenTableStrings(work, res.token_table_offset);
    std::size_t pos = res.names_offset;
    for (int i = 0; i < res.num_syms && pos < res.markers_offset; ++i) {
      auto sym = DecompressNameWithTypeAt(work, &pos, tokens);
      if (!sym.has_value()) {
        std::fprintf(stderr, "dump warning: truncated names at index %d\n", i);
        break;
      }
      const int offset = GetOffsetAtIndex(work,
                                          res.offset_table_start,
                                          i,
                                          res.little_endian_token_index,
                                          res.has_relative_base,
                                          res.offset_table_elem_size,
                                          res.kernel_base);
      std::fprintf(stdout, "0x%08x %c %s\n", offset, sym->first, sym->second.c_str());
    }
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "dump failed: %s\n", e.what());
    return false;
  }
}

bool DumpIkconfig(const std::string& image_path) {
  try {
    const auto image = ReadFile(image_path);
    static constexpr char kStartMagic[] = "IKCFG_ST";
    static constexpr char kEndMagic[] = "IKCFG_ED";
    const auto start_it = std::search(image.begin(), image.end(), kStartMagic, kStartMagic + 8);
    if (start_it == image.end()) {
      std::fprintf(stderr, "ikconfig not found: IKCFG_ST missing\n");
      return false;
    }
    const auto end_it = std::search(image.begin(), image.end(), kEndMagic, kEndMagic + 8);
    if (end_it == image.end()) {
      std::fprintf(stderr, "ikconfig not found: IKCFG_ED missing\n");
      return false;
    }
    const std::size_t start = static_cast<std::size_t>(std::distance(image.begin(), start_it)) + 8;
    const std::size_t end = static_cast<std::size_t>(std::distance(image.begin(), end_it));
    if (start >= end || end > image.size()) {
      std::fprintf(stderr, "ikconfig range invalid\n");
      return false;
    }
    std::vector<std::uint8_t> compressed(image.begin() + static_cast<std::ptrdiff_t>(start),
                                         image.begin() + static_cast<std::ptrdiff_t>(end));

    auto try_inflate = [&](int window_bits, std::string* out_text) -> bool {
      z_stream strm{};
      strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed.data()));
      strm.avail_in = static_cast<uInt>(compressed.size());
      if (inflateInit2(&strm, window_bits) != Z_OK) {
        return false;
      }
      std::vector<std::uint8_t> out(256 * 1024);
      int rc = Z_OK;
      while (rc != Z_STREAM_END) {
        if (strm.total_out >= out.size()) {
          out.resize(out.size() * 2);
        }
        strm.next_out = reinterpret_cast<Bytef*>(out.data() + static_cast<std::ptrdiff_t>(strm.total_out));
        strm.avail_out = static_cast<uInt>(out.size() - strm.total_out);
        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
          inflateEnd(&strm);
          return false;
        }
      }
      inflateEnd(&strm);
      out.resize(strm.total_out);
      out_text->assign(reinterpret_cast<const char*>(out.data()), out.size());
      return true;
    };

    std::string text;
    if (!try_inflate(16 + MAX_WBITS, &text) && !try_inflate(MAX_WBITS, &text) && !try_inflate(-MAX_WBITS, &text)) {
      std::fprintf(stderr, "ikconfig decompress failed\n");
      return false;
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "ikconfig dump failed: %s\n", e.what());
    return false;
  }
}

}  // namespace apd
