#include "kpm.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace apd {
namespace {

constexpr const char* kKpmInfoSectionName = ".kpm.info";
constexpr std::array<unsigned char, 4> kElfMagic = {0x7F, 'E', 'L', 'F'};

constexpr std::size_t kEiClass = 4;
constexpr std::size_t kEiData = 5;
constexpr std::uint8_t kElfClass64 = 2;
constexpr std::uint8_t kElfData2Lsb = 1;

constexpr std::uint16_t kEtRel = 1;
constexpr std::uint16_t kEmAarch64 = 183;
constexpr std::uint32_t kShtNoBits = 8;
constexpr std::uint64_t kShfAlloc = 0x2;

struct Elf64Ehdr {
  unsigned char e_ident[16];
  std::uint16_t e_type;
  std::uint16_t e_machine;
  std::uint32_t e_version;
  std::uint64_t e_entry;
  std::uint64_t e_phoff;
  std::uint64_t e_shoff;
  std::uint32_t e_flags;
  std::uint16_t e_ehsize;
  std::uint16_t e_phentsize;
  std::uint16_t e_phnum;
  std::uint16_t e_shentsize;
  std::uint16_t e_shnum;
  std::uint16_t e_shstrndx;
};

struct Elf64Shdr {
  std::uint32_t sh_name;
  std::uint32_t sh_type;
  std::uint64_t sh_flags;
  std::uint64_t sh_addr;
  std::uint64_t sh_offset;
  std::uint64_t sh_size;
  std::uint32_t sh_link;
  std::uint32_t sh_info;
  std::uint64_t sh_addralign;
  std::uint64_t sh_entsize;
};

std::vector<std::uint8_t> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    throw std::runtime_error("failed to open file: " + path);
  }
  const auto end_pos = in.tellg();
  if (end_pos < 0) {
    throw std::runtime_error("failed to stat file: " + path);
  }
  std::vector<std::uint8_t> data(static_cast<std::size_t>(end_pos));
  in.seekg(0, std::ios::beg);
  if (!data.empty()) {
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  }
  if (!in) {
    throw std::runtime_error("failed to read file: " + path);
  }
  return data;
}

bool IsRangeValid(std::size_t buffer_size, std::uint64_t off, std::uint64_t len) {
  return off <= static_cast<std::uint64_t>(buffer_size) &&
         len <= static_cast<std::uint64_t>(buffer_size) - off;
}

bool ReadSectionHeader(const std::vector<std::uint8_t>& kpm,
                       const Elf64Ehdr& ehdr,
                       std::size_t section_index,
                       Elf64Shdr* out_shdr) {
  if (out_shdr == nullptr || section_index >= ehdr.e_shnum) {
    return false;
  }
  const std::uint64_t shoff = ehdr.e_shoff + section_index * sizeof(Elf64Shdr);
  if (!IsRangeValid(kpm.size(), shoff, sizeof(Elf64Shdr))) {
    return false;
  }
  std::memcpy(out_shdr, kpm.data() + static_cast<std::ptrdiff_t>(shoff), sizeof(Elf64Shdr));
  return true;
}

void SetError(const std::string& msg, std::string* out_error) {
  if (out_error != nullptr) {
    *out_error = msg;
  }
}

void ParseModInfoLine(const std::string& token, const char* key, std::string* out) {
  const std::string prefix = std::string(key) + "=";
  if (token.size() <= prefix.size() || token.compare(0, prefix.size(), prefix) != 0) {
    return;
  }
  if (!out->empty()) {
    return;
  }
  *out = token.substr(prefix.size());
}

}  // namespace

bool ParseKpmInfo(const std::vector<std::uint8_t>& kpm, KpmInfo* out_info, std::string* error_message) {
  if (out_info == nullptr) {
    SetError("invalid output pointer", error_message);
    return false;
  }
  *out_info = KpmInfo{};

  if (kpm.size() < sizeof(Elf64Ehdr)) {
    SetError("kpm image too small", error_message);
    return false;
  }

  Elf64Ehdr ehdr{};
  std::memcpy(&ehdr, kpm.data(), sizeof(ehdr));

  if (std::memcmp(ehdr.e_ident, kElfMagic.data(), kElfMagic.size()) != 0 ||
      ehdr.e_ident[kEiClass] != kElfClass64 || ehdr.e_ident[kEiData] != kElfData2Lsb) {
    SetError("invalid ELF header", error_message);
    return false;
  }
  if (ehdr.e_type != kEtRel || ehdr.e_machine != kEmAarch64 || ehdr.e_shentsize != sizeof(Elf64Shdr)) {
    SetError("unsupported ELF type/arch", error_message);
    return false;
  }
  if (ehdr.e_shnum == 0) {
    SetError("missing ELF section table", error_message);
    return false;
  }
  if (ehdr.e_shstrndx >= ehdr.e_shnum) {
    SetError("invalid section-string table index", error_message);
    return false;
  }

  const std::uint64_t sh_table_size = static_cast<std::uint64_t>(ehdr.e_shnum) * sizeof(Elf64Shdr);
  if (!IsRangeValid(kpm.size(), ehdr.e_shoff, sh_table_size)) {
    SetError("ELF section table out of bounds", error_message);
    return false;
  }

  Elf64Shdr shstr_shdr{};
  if (!ReadSectionHeader(kpm, ehdr, ehdr.e_shstrndx, &shstr_shdr)) {
    SetError("failed to read section-string table header", error_message);
    return false;
  }
  if (!IsRangeValid(kpm.size(), shstr_shdr.sh_offset, shstr_shdr.sh_size)) {
    SetError("section-string table out of bounds", error_message);
    return false;
  }

  const char* shstr_base =
      reinterpret_cast<const char*>(kpm.data() + static_cast<std::ptrdiff_t>(shstr_shdr.sh_offset));
  const std::size_t shstr_size = static_cast<std::size_t>(shstr_shdr.sh_size);

  bool found_kpm_info = false;
  Elf64Shdr info_shdr{};
  for (std::size_t i = 1; i < ehdr.e_shnum; ++i) {
    Elf64Shdr shdr{};
    if (!ReadSectionHeader(kpm, ehdr, i, &shdr)) {
      SetError("invalid section header", error_message);
      return false;
    }

    if (shdr.sh_type != kShtNoBits && !IsRangeValid(kpm.size(), shdr.sh_offset, shdr.sh_size)) {
      SetError("section payload out of bounds", error_message);
      return false;
    }
    if (shdr.sh_name >= shstr_size) {
      continue;
    }
    const char* sec_name = shstr_base + shdr.sh_name;
    const std::size_t remain = shstr_size - static_cast<std::size_t>(shdr.sh_name);
    const void* end_ptr = std::memchr(sec_name, '\0', remain);
    if (end_ptr == nullptr) {
      continue;
    }
    if ((shdr.sh_flags & kShfAlloc) != 0 && std::strcmp(sec_name, kKpmInfoSectionName) == 0) {
      info_shdr = shdr;
      found_kpm_info = true;
      break;
    }
  }

  if (!found_kpm_info) {
    SetError("no .kpm.info section", error_message);
    return false;
  }
  if (!IsRangeValid(kpm.size(), info_shdr.sh_offset, info_shdr.sh_size)) {
    SetError(".kpm.info section out of bounds", error_message);
    return false;
  }

  const char* info_base = reinterpret_cast<const char*>(kpm.data() + static_cast<std::ptrdiff_t>(info_shdr.sh_offset));
  std::size_t remain = static_cast<std::size_t>(info_shdr.sh_size);
  const char* pos = info_base;
  while (remain > 0) {
    const void* zero = std::memchr(pos, '\0', remain);
    if (zero == nullptr) {
      break;
    }
    const auto* token_end = static_cast<const char*>(zero);
    const std::size_t token_len = static_cast<std::size_t>(token_end - pos);
    if (token_len > 0) {
      std::string token(pos, token_len);
      ParseModInfoLine(token, "name", &out_info->name);
      ParseModInfoLine(token, "version", &out_info->version);
      ParseModInfoLine(token, "license", &out_info->license);
      ParseModInfoLine(token, "author", &out_info->author);
      ParseModInfoLine(token, "description", &out_info->description);
    }
    const std::size_t consumed = token_len + 1;
    pos += static_cast<std::ptrdiff_t>(consumed);
    remain -= consumed;
  }
  return true;
}

bool ParseKpmInfoPath(const std::string& kpm_path, KpmInfo* out_info, std::string* error_message) {
  try {
    const auto data = ReadFile(kpm_path);
    return ParseKpmInfo(data, out_info, error_message);
  } catch (const std::exception& e) {
    SetError(e.what(), error_message);
    return false;
  }
}

bool PrintKpmInfoPath(const std::string& kpm_path) {
  KpmInfo info{};
  std::string error;
  if (!ParseKpmInfoPath(kpm_path, &info, &error)) {
    std::fprintf(stderr, "kpm inspect failed: %s\n", error.c_str());
    return false;
  }
  std::fprintf(stdout, "[kpm]\n");
  std::fprintf(stdout, "name=%s\n", info.name.c_str());
  std::fprintf(stdout, "version=%s\n", info.version.c_str());
  std::fprintf(stdout, "license=%s\n", info.license.c_str());
  std::fprintf(stdout, "author=%s\n", info.author.c_str());
  std::fprintf(stdout, "description=%s\n", info.description.c_str());
  return true;
}

}  // namespace apd
