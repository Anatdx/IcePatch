#include "bootimg.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>
#include <zstd.h>

extern "C" {
#include "kpboot/lib/lz4/lz4.h"
#include "kpboot/lib/lz4/lz4frame.h"
#include "kpboot/lib/lz4/lz4hc.h"
#include "kpboot/lib/bz2/bzlib.h"
#include "kpboot/lib/xz/xz.h"
#include "kpboot/lib/sha/sha1.h"
#include "kpboot/lib/sha/sha256.h"
}

namespace apd {
namespace {

constexpr std::size_t kBootMagicLen = 8;
constexpr char kBootMagic[kBootMagicLen + 1] = "ANDROID!";
constexpr std::size_t kPageDefault = 4096;
constexpr std::uint32_t kLz4LegacyMagic = 0x184c2102u;
constexpr std::size_t kLz4BlockSize = 0x800000;
constexpr int kLz4HcLevel = 12;

constexpr std::size_t Align(std::size_t v, std::size_t a) {
  return (v + a - 1) & ~(a - 1);
}

#pragma pack(push, 1)
struct BootImgHdr {
  std::uint8_t magic[8];
  std::uint32_t kernel_size;
  std::uint32_t kernel_addr;
  std::uint32_t ramdisk_size;
  std::uint32_t ramdisk_addr;
  std::uint32_t second_size;
  std::uint32_t second_addr;
  std::uint32_t tags_addr;
  std::uint32_t page_size;
  std::uint32_t unused[2];
  std::uint8_t name[16];
  std::uint8_t cmdline[512];
  std::uint32_t id[8];
  std::uint8_t extra_cmdline[1024];
  std::uint32_t recovery_dtbo_size;
  std::uint64_t recovery_dtbo_offset;
  std::uint32_t dtb_size;
  std::uint64_t dtb_addr;
};

struct FdtHeader {
  std::uint32_t magic;
  std::uint32_t totalsize;
  std::uint32_t off_dt_struct;
  std::uint32_t off_dt_strings;
  std::uint32_t off_mem_rsvmap;
  std::uint32_t version;
  std::uint32_t last_comp_version;
  std::uint32_t boot_cpuid_phys;
  std::uint32_t size_dt_strings;
  std::uint32_t size_dt_struct;
};

struct AvbFooter {
  std::uint32_t reverse[16];
  std::uint32_t magic;
  std::uint32_t version;
  std::uint64_t reserved1;
  std::uint32_t data_size1;
  std::uint32_t data_size_1;
  std::uint32_t data_size2;
  std::uint32_t data_size_2;
  std::uint64_t unknown_field;
  std::uint8_t padding[24];
};
#pragma pack(pop)

constexpr std::size_t kAvbFooterSize = sizeof(AvbFooter);

struct CompressHead {
  std::uint8_t magic[8];
};

enum class CompressMethod {
  Raw = 0,
  Gzip = 1,
  Lz4Frame = 2,
  Lz4Legacy = 3,
  Zstd = 4,
  Bzip2 = 5,
  Xz = 6,
  LzmaLegacy = 7,
};

std::vector<std::uint8_t> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    throw std::runtime_error("failed to open file: " + path);
  }
  const auto end = in.tellg();
  if (end < 0) {
    throw std::runtime_error("failed to stat file: " + path);
  }
  std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
  in.seekg(0, std::ios::beg);
  if (!data.empty()) {
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  }
  if (!in) {
    throw std::runtime_error("failed to read file: " + path);
  }
  return data;
}

void WriteFile(const std::string& path, const std::vector<std::uint8_t>& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open output: " + path);
  }
  if (!data.empty()) {
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  }
  if (!out) {
    throw std::runtime_error("failed to write output: " + path);
  }
}

std::uint32_t Swap32(std::uint32_t x) {
  return ((x >> 24) & 0x000000FFu) | ((x >> 8) & 0x0000FF00u) | ((x << 8) & 0x00FF0000u) |
         ((x << 24) & 0xFF000000u);
}

std::uint32_t Fdt32ToCpu(std::uint32_t v) {
  return Swap32(v);
}

CompressMethod DetectCompressMethod(const std::vector<std::uint8_t>& data) {
  if (data.size() < 8) {
    return CompressMethod::Raw;
  }
  const auto* m = data.data();
  if (m[0] == 0x1F && (m[1] == 0x8B || m[1] == 0x9E)) return CompressMethod::Gzip;
  if (m[0] == 0x04 && m[1] == 0x22 && m[2] == 0x4D && m[3] == 0x18) return CompressMethod::Lz4Frame;
  if (m[0] == 0x03 && m[1] == 0x21 && m[2] == 0x4C && m[3] == 0x18) return CompressMethod::Lz4Frame;
  if (m[0] == 0x02 && m[1] == 0x21 && m[2] == 0x4C && m[3] == 0x18) return CompressMethod::Lz4Legacy;
  if (m[0] == 0x28 && m[1] == 0xB5 && m[2] == 0x2F && m[3] == 0xFD) return CompressMethod::Zstd;
  if (m[0] == 0x42 && m[1] == 0x5A && m[2] == 0x68) return CompressMethod::Bzip2;
  if (m[0] == 0xFD && m[1] == 0x37 && m[2] == 0x7A && m[3] == 0x58) return CompressMethod::Xz;
  if (m[0] == 0x5D && m[1] == 0x00 && m[2] == 0x00) return CompressMethod::LzmaLegacy;
  return CompressMethod::Raw;
}

std::vector<std::uint8_t> DecompressGzip(const std::vector<std::uint8_t>& in) {
  z_stream strm{};
  strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
  strm.avail_in = static_cast<uInt>(in.size());
  if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
    throw std::runtime_error("gzip init failed");
  }
  std::vector<std::uint8_t> out(8 * 1024 * 1024);
  std::size_t out_off = 0;
  int ret = Z_OK;
  while (ret != Z_STREAM_END) {
    if (out_off == out.size()) out.resize(out.size() * 2);
    strm.next_out = reinterpret_cast<Bytef*>(out.data() + static_cast<std::ptrdiff_t>(out_off));
    strm.avail_out = static_cast<uInt>(out.size() - out_off);
    ret = inflate(&strm, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) {
      inflateEnd(&strm);
      throw std::runtime_error("gzip inflate failed");
    }
    out_off = out.size() - strm.avail_out;
  }
  inflateEnd(&strm);
  out.resize(out_off);
  return out;
}

std::vector<std::uint8_t> CompressGzip(const std::vector<std::uint8_t>& in) {
  z_stream strm{};
  if (deflateInit2(&strm, 9, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    throw std::runtime_error("gzip deflate init failed");
  }
  const uLong bound = deflateBound(&strm, static_cast<uLong>(in.size()));
  std::vector<std::uint8_t> out(static_cast<std::size_t>(bound));
  strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
  strm.avail_in = static_cast<uInt>(in.size());
  strm.next_out = reinterpret_cast<Bytef*>(out.data());
  strm.avail_out = static_cast<uInt>(out.size());
  const int ret = deflate(&strm, Z_FINISH);
  if (ret != Z_STREAM_END) {
    deflateEnd(&strm);
    throw std::runtime_error("gzip deflate failed");
  }
  out.resize(out.size() - strm.avail_out);
  deflateEnd(&strm);
  return out;
}

std::vector<std::uint8_t> DecompressLz4Frame(const std::vector<std::uint8_t>& in) {
  LZ4F_decompressionContext_t dctx;
  if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION))) {
    throw std::runtime_error("lz4 frame dctx create failed");
  }
  std::vector<std::uint8_t> out;
  out.reserve(std::max<std::size_t>(in.size() * 2, 4 * 1024 * 1024));
  std::array<std::uint8_t, 1024 * 1024> chunk{};
  std::size_t src_off = 0;

  while (true) {
    std::size_t src_sz = in.size() - src_off;
    std::size_t dst_sz = chunk.size();
    const size_t ret = LZ4F_decompress(dctx,
                                       chunk.data(),
                                       &dst_sz,
                                       in.data() + static_cast<std::ptrdiff_t>(src_off),
                                       &src_sz,
                                       nullptr);
    if (LZ4F_isError(ret)) {
      LZ4F_freeDecompressionContext(dctx);
      throw std::runtime_error("lz4 frame decompress failed");
    }
    src_off += src_sz;
    if (dst_sz > 0) {
      out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(dst_sz));
    }
    if (ret == 0) {
      break;
    }
    if (src_off >= in.size() && ret > 0) {
      LZ4F_freeDecompressionContext(dctx);
      throw std::runtime_error("lz4 frame data truncated");
    }
    if (src_sz == 0 && dst_sz == 0) {
      LZ4F_freeDecompressionContext(dctx);
      throw std::runtime_error("lz4 frame decode stalled");
    }
  }

  LZ4F_freeDecompressionContext(dctx);
  return out;
}

std::vector<std::uint8_t> DecompressLz4Legacy(const std::vector<std::uint8_t>& in) {
  if (in.size() < 4) {
    throw std::runtime_error("lz4 legacy blob too small");
  }
  std::size_t p = 0;
  const std::uint32_t magic = static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
                              (static_cast<std::uint32_t>(in[2]) << 16) | (static_cast<std::uint32_t>(in[3]) << 24);
  if (magic != kLz4LegacyMagic) {
    throw std::runtime_error("lz4 legacy magic mismatch");
  }
  p += 4;

  std::vector<std::uint8_t> out;
  out.reserve(64 * 1024 * 1024);
  std::vector<char> block(kLz4BlockSize);

  while (p + 4 <= in.size()) {
    std::uint32_t block_size = static_cast<std::uint32_t>(in[p]) | (static_cast<std::uint32_t>(in[p + 1]) << 8) |
                               (static_cast<std::uint32_t>(in[p + 2]) << 16) | (static_cast<std::uint32_t>(in[p + 3]) << 24);
    p += 4;
    if (block_size == 0) break;
    if (p + block_size > in.size()) throw std::runtime_error("lz4 legacy truncated block");
    int decoded = LZ4_decompress_safe(reinterpret_cast<const char*>(in.data() + static_cast<std::ptrdiff_t>(p)),
                                      block.data(),
                                      static_cast<int>(block_size),
                                      static_cast<int>(block.size()));
    if (decoded < 0) {
      throw std::runtime_error("lz4 legacy decode failed");
    }
    out.insert(out.end(), reinterpret_cast<std::uint8_t*>(block.data()),
               reinterpret_cast<std::uint8_t*>(block.data()) + decoded);
    p += block_size;
  }
  return out;
}

std::vector<std::uint8_t> CompressLz4Legacy(const std::vector<std::uint8_t>& in) {
  std::vector<std::uint8_t> out;
  out.reserve(sizeof(std::uint32_t) + in.size() / 2);

  const std::uint32_t magic = kLz4LegacyMagic;
  out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&magic),
             reinterpret_cast<const std::uint8_t*>(&magic) + sizeof(magic));

  std::size_t in_off = 0;
  while (in_off < in.size()) {
    const std::size_t chunk = std::min<std::size_t>(kLz4BlockSize, in.size() - in_off);
    const int bound = LZ4_compressBound(static_cast<int>(chunk));
    std::vector<char> comp(static_cast<std::size_t>(bound));
    int csz = LZ4_compress_HC(reinterpret_cast<const char*>(in.data() + static_cast<std::ptrdiff_t>(in_off)),
                              comp.data(),
                              static_cast<int>(chunk),
                              bound,
                              kLz4HcLevel);
    if (csz <= 0) throw std::runtime_error("lz4 legacy compress failed");

    const std::uint32_t sz = static_cast<std::uint32_t>(csz);
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&sz),
               reinterpret_cast<const std::uint8_t*>(&sz) + sizeof(sz));
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(comp.data()),
               reinterpret_cast<const std::uint8_t*>(comp.data()) + csz);
    in_off += chunk;
  }
  return out;
}

std::vector<std::uint8_t> CompressLz4Frame(const std::vector<std::uint8_t>& in,
                                           const std::vector<std::uint8_t>& old_kernel_blob) {
  LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
  if (old_kernel_blob.size() >= 6) {
    const std::uint8_t flg = old_kernel_blob[4];
    const std::uint8_t bd = old_kernel_blob[5];
    prefs.frameInfo.blockMode = (flg & 0x20) ? LZ4F_blockIndependent : LZ4F_blockLinked;
    prefs.frameInfo.blockChecksumFlag = (flg & 0x10) ? LZ4F_blockChecksumEnabled : LZ4F_noBlockChecksum;
    prefs.frameInfo.contentChecksumFlag = (flg & 0x08) ? LZ4F_contentChecksumEnabled : LZ4F_noContentChecksum;
    const std::uint8_t block_size_id = (bd >> 4) & 0x07;
    switch (block_size_id) {
      case 4: prefs.frameInfo.blockSizeID = LZ4F_max64KB; break;
      case 5: prefs.frameInfo.blockSizeID = LZ4F_max256KB; break;
      case 6: prefs.frameInfo.blockSizeID = LZ4F_max1MB; break;
      case 7: prefs.frameInfo.blockSizeID = LZ4F_max4MB; break;
      default: break;
    }
  }
  const size_t bound = LZ4F_compressFrameBound(in.size(), &prefs);
  std::vector<std::uint8_t> out(bound);
  size_t written = LZ4F_compressFrame(out.data(), out.size(), in.data(), in.size(), &prefs);
  if (LZ4F_isError(written)) {
    throw std::runtime_error("lz4 frame compress failed");
  }
  out.resize(written);
  return out;
}

std::vector<std::uint8_t> DecompressBzip2(const std::vector<std::uint8_t>& in) {
  std::size_t cap = std::max<std::size_t>(in.size() * 4, 4 * 1024 * 1024);
  if (cap > std::numeric_limits<unsigned int>::max()) {
    cap = std::numeric_limits<unsigned int>::max();
  }
  for (;;) {
    std::vector<std::uint8_t> out(cap);
    unsigned int out_len = static_cast<unsigned int>(out.size());
    const int rc = BZ2_bzBuffToBuffDecompress(reinterpret_cast<char*>(out.data()),
                                              &out_len,
                                              const_cast<char*>(reinterpret_cast<const char*>(in.data())),
                                              static_cast<unsigned int>(in.size()),
                                              0,
                                              0);
    if (rc == BZ_OK) {
      out.resize(out_len);
      return out;
    }
    if (rc != BZ_OUTBUFF_FULL || cap == std::numeric_limits<unsigned int>::max()) {
      throw std::runtime_error("bzip2 decompress failed");
    }
    cap = std::min<std::size_t>(cap * 2, std::numeric_limits<unsigned int>::max());
  }
}

std::vector<std::uint8_t> CompressBzip2(const std::vector<std::uint8_t>& in) {
  unsigned int cap = static_cast<unsigned int>(in.size() * 1.01) + 600;
  std::vector<std::uint8_t> out(cap);
  unsigned int out_len = cap;
  int rc = BZ2_bzBuffToBuffCompress(reinterpret_cast<char*>(out.data()),
                                    &out_len,
                                    const_cast<char*>(reinterpret_cast<const char*>(in.data())),
                                    static_cast<unsigned int>(in.size()),
                                    9,
                                    0,
                                    30);
  if (rc != BZ_OK) {
    throw std::runtime_error("bzip2 compress failed");
  }
  out.resize(out_len);
  return out;
}

std::vector<std::uint8_t> DecompressXzLike(const std::vector<std::uint8_t>& in) {
  xz_crc32_init();
  xz_dec* dec = xz_dec_init(XZ_SINGLE, 0);
  if (!dec) throw std::runtime_error("xz init failed");

  std::vector<std::uint8_t> out(std::max<std::size_t>(in.size() * 4, 8 * 1024 * 1024));
  xz_buf b{};
  b.in = in.data();
  b.in_pos = 0;
  b.in_size = in.size();
  b.out = out.data();
  b.out_pos = 0;
  b.out_size = out.size();

  for (;;) {
    const std::size_t prev_in = b.in_pos;
    const std::size_t prev_out = b.out_pos;
    const xz_ret rc = xz_dec_run(dec, &b);
    if (rc == XZ_STREAM_END) {
      break;
    }
    if (rc == XZ_UNSUPPORTED_CHECK) {
      continue;
    }
    if (rc != XZ_OK) {
      xz_dec_end(dec);
      throw std::runtime_error("xz/lzma decompress failed");
    }
    if (b.out_pos == b.out_size) {
      const std::size_t old_size = out.size();
      if (old_size > std::numeric_limits<std::size_t>::max() / 2) {
        xz_dec_end(dec);
        throw std::runtime_error("xz/lzma output too large");
      }
      out.resize(old_size * 2);
      b.out = out.data();
      b.out_size = out.size();
      continue;
    }
    if (b.in_pos == b.in_size) {
      xz_dec_end(dec);
      throw std::runtime_error("xz/lzma data truncated");
    }
    if (b.in_pos == prev_in && b.out_pos == prev_out) {
      xz_dec_end(dec);
      throw std::runtime_error("xz/lzma decode stalled");
    }
  }
  xz_dec_end(dec);
  out.resize(b.out_pos);
  return out;
}

std::vector<std::uint8_t> DecompressZstd(const std::vector<std::uint8_t>& in) {
  if (in.empty()) {
    return {};
  }

  constexpr std::size_t kMaxOutput = 1024ull * 1024ull * 1024ull;
  const unsigned long long frame_size = ZSTD_getFrameContentSize(in.data(), in.size());
  if (frame_size == ZSTD_CONTENTSIZE_ERROR) {
    throw std::runtime_error("zstd frame invalid");
  }
  if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN && frame_size > kMaxOutput) {
    throw std::runtime_error("zstd output too large");
  }

  ZSTD_DCtx* dctx = ZSTD_createDCtx();
  if (dctx == nullptr) {
    throw std::runtime_error("zstd dctx create failed");
  }

  std::vector<std::uint8_t> out;
  if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN) {
    out.reserve(static_cast<std::size_t>(frame_size));
  } else {
    out.reserve(std::max<std::size_t>(in.size() * 3, 4 * 1024 * 1024));
  }

  std::vector<std::uint8_t> chunk(std::max<std::size_t>(ZSTD_DStreamOutSize(), 64 * 1024));
  ZSTD_inBuffer input{in.data(), in.size(), 0};
  size_t ret = 1;

  while (input.pos < input.size || ret != 0) {
    const std::size_t prev_pos = input.pos;
    ZSTD_outBuffer output{chunk.data(), chunk.size(), 0};
    ret = ZSTD_decompressStream(dctx, &output, &input);
    if (ZSTD_isError(ret)) {
      const char* msg = ZSTD_getErrorName(ret);
      ZSTD_freeDCtx(dctx);
      throw std::runtime_error(std::string("zstd decompress failed: ") + (msg ? msg : "unknown"));
    }
    if (output.pos > 0) {
      if (out.size() > kMaxOutput - output.pos) {
        ZSTD_freeDCtx(dctx);
        throw std::runtime_error("zstd output too large");
      }
      out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(output.pos));
    }
    if (input.pos == prev_pos && output.pos == 0 && ret > 0) {
      ZSTD_freeDCtx(dctx);
      throw std::runtime_error("zstd decode stalled");
    }
    if (input.pos == input.size && ret > 0) {
      ZSTD_freeDCtx(dctx);
      throw std::runtime_error("zstd data truncated");
    }
  }

  ZSTD_freeDCtx(dctx);
  return out;
}

std::vector<std::uint8_t> CompressZstd(const std::vector<std::uint8_t>& in) {
  constexpr int kLevel = 3;
  const std::size_t bound = ZSTD_compressBound(in.size());
  std::vector<std::uint8_t> out(bound);
  const std::size_t written = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), kLevel);
  if (ZSTD_isError(written)) {
    const char* msg = ZSTD_getErrorName(written);
    throw std::runtime_error(std::string("zstd compress failed: ") + (msg ? msg : "unknown"));
  }
  out.resize(written);
  return out;
}

std::vector<std::uint8_t> AutoDecompressKernel(const std::vector<std::uint8_t>& kernel_blob,
                                               CompressMethod method) {
  switch (method) {
    case CompressMethod::Gzip:
      return DecompressGzip(kernel_blob);
    case CompressMethod::Lz4Frame:
      return DecompressLz4Frame(kernel_blob);
    case CompressMethod::Lz4Legacy:
      return DecompressLz4Legacy(kernel_blob);
    case CompressMethod::Bzip2:
      return DecompressBzip2(kernel_blob);
    case CompressMethod::Xz:
    case CompressMethod::LzmaLegacy:
      return DecompressXzLike(kernel_blob);
    case CompressMethod::Zstd:
      return DecompressZstd(kernel_blob);
    default:
      return kernel_blob;
  }
}

std::vector<std::uint8_t> AutoCompressKernel(const std::vector<std::uint8_t>& kernel_raw,
                                             CompressMethod method,
                                             const std::vector<std::uint8_t>& old_kernel_blob) {
  switch (method) {
    case CompressMethod::Gzip:
      return CompressGzip(kernel_raw);
    case CompressMethod::Lz4Frame:
      return CompressLz4Frame(kernel_raw, old_kernel_blob);
    case CompressMethod::Lz4Legacy:
      return CompressLz4Legacy(kernel_raw);
    case CompressMethod::Bzip2:
      return CompressBzip2(kernel_raw);
    case CompressMethod::Xz:
    case CompressMethod::LzmaLegacy:
      // Keep compatibility with original kptools behavior: repack xz/lzma kernels as gzip.
      return CompressGzip(kernel_raw);
    case CompressMethod::Zstd:
      return CompressZstd(kernel_raw);
    default:
      return kernel_raw;
  }
}

int FindDtbOffset(const std::vector<std::uint8_t>& kernel_blob) {
  if (kernel_blob.size() < sizeof(FdtHeader)) return -1;
  for (std::size_t i = 0; i + sizeof(FdtHeader) <= kernel_blob.size(); ++i) {
    if (!(kernel_blob[i] == 0xd0 && kernel_blob[i + 1] == 0x0d && kernel_blob[i + 2] == 0xfe &&
          kernel_blob[i + 3] == 0xed)) {
      continue;
    }
    const auto* fdt = reinterpret_cast<const FdtHeader*>(kernel_blob.data() + static_cast<std::ptrdiff_t>(i));
    const std::uint32_t totalsize = Fdt32ToCpu(fdt->totalsize);
    const std::uint32_t off_dt_struct = Fdt32ToCpu(fdt->off_dt_struct);
    if (totalsize <= 0x48 || i + totalsize > kernel_blob.size()) continue;
    if (i + off_dt_struct + 4 > kernel_blob.size()) continue;
    const std::uint32_t tag = Fdt32ToCpu(*reinterpret_cast<const std::uint32_t*>(
        kernel_blob.data() + static_cast<std::ptrdiff_t>(i + off_dt_struct)));
    if (tag == 0x00000001) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int IsSha256(const std::uint32_t id[8]) {
  if ((id[0] | id[1] | id[2] | id[3] | id[4] | id[5]) == 0) {
    return 1;
  }
  if (id[6] != 0 || id[7] != 0) {
    return 2;
  }
  return 0;
}

BootImgHdr ParseHeader(const std::vector<std::uint8_t>& boot) {
  if (boot.size() < sizeof(BootImgHdr)) {
    throw std::runtime_error("boot image too small");
  }
  BootImgHdr hdr{};
  std::memcpy(&hdr, boot.data(), sizeof(hdr));
  if (std::memcmp(hdr.magic, kBootMagic, kBootMagicLen) != 0) {
    throw std::runtime_error("invalid boot magic");
  }
  return hdr;
}

std::size_t KernelOffset(const BootImgHdr& hdr) {
  std::uint32_t off = hdr.page_size;
  if (hdr.unused[0] >= 3) off = 4096;
  if (hdr.unused[0] > 10) off = hdr.page_size;
  return off;
}

void UpdateBootIdHash(BootImgHdr* hdr,
                      const std::vector<std::uint8_t>& final_kernel_with_dtb,
                      const std::vector<std::uint8_t>& rest_no_avb,
                      std::uint32_t fmt_size,
                      std::uint32_t header_ver,
                      std::uint32_t extracted_size,
                      std::size_t page_size) {
  const int use_sha256 = IsSha256(hdr->id);
  const std::uint32_t checksum_aligned_initial = static_cast<std::uint32_t>(Align(fmt_size, page_size));
  std::size_t checksum_aligned = checksum_aligned_initial;

  auto safe_slice = [&](std::size_t off, std::size_t len) -> std::pair<const std::uint8_t*, std::size_t> {
    if (off >= rest_no_avb.size()) return {nullptr, 0};
    const std::size_t take = std::min(len, rest_no_avb.size() - off);
    return {rest_no_avb.data() + static_cast<std::ptrdiff_t>(off), take};
  };

  if (use_sha256) {
    SHA256_CTX ctx;
    BYTE hash[SHA256_BLOCK_SIZE];
    sha256_init(&ctx);
    sha256_update(&ctx, final_kernel_with_dtb.data(), final_kernel_with_dtb.size());
    sha256_update(&ctx, reinterpret_cast<const BYTE*>(&hdr->kernel_size), 4);

    auto ramdisk = safe_slice(0, fmt_size);
    if (ramdisk.first) sha256_update(&ctx, ramdisk.first, ramdisk.second);
    sha256_update(&ctx, reinterpret_cast<const BYTE*>(&fmt_size), 4);

    auto second = safe_slice(checksum_aligned, hdr->second_size);
    if (second.first) sha256_update(&ctx, second.first, second.second);
    sha256_update(&ctx, reinterpret_cast<const BYTE*>(&hdr->second_size), 4);
    if (hdr->second_size > 0) checksum_aligned += Align(hdr->second_size, page_size);

    if (extracted_size) {
      auto extra = safe_slice(checksum_aligned, page_size);
      if (extra.first) sha256_update(&ctx, extra.first, extra.second);
      sha256_update(&ctx, reinterpret_cast<const BYTE*>(&extracted_size), 4);
      checksum_aligned += Align(extracted_size, page_size);
    }

    if (header_ver == 1 || header_ver == 2) {
      auto rec = safe_slice(checksum_aligned, hdr->recovery_dtbo_size);
      if (rec.first) sha256_update(&ctx, rec.first, rec.second);
      sha256_update(&ctx, reinterpret_cast<const BYTE*>(&hdr->recovery_dtbo_size), 4);
      checksum_aligned += Align(hdr->recovery_dtbo_size, page_size);
    }

    if (header_ver == 2) {
      auto dtb = safe_slice(checksum_aligned, hdr->dtb_size);
      if (dtb.first) sha256_update(&ctx, dtb.first, dtb.second);
      sha256_update(&ctx, reinterpret_cast<const BYTE*>(&hdr->dtb_size), 4);
    }

    sha256_final(&ctx, hash);
    std::memcpy(hdr->id, hash, 32);
  } else {
    SHA1_CTX ctx;
    std::uint8_t hash[SHA1_DIGEST_SIZE];
    sha1_init(&ctx);
    sha1_update(&ctx, final_kernel_with_dtb.data(), final_kernel_with_dtb.size());
    sha1_update(&ctx, reinterpret_cast<const std::uint8_t*>(&hdr->kernel_size), 4);

    auto ramdisk = safe_slice(0, fmt_size);
    if (ramdisk.first) sha1_update(&ctx, ramdisk.first, ramdisk.second);
    sha1_update(&ctx, reinterpret_cast<const std::uint8_t*>(&fmt_size), 4);

    auto second = safe_slice(checksum_aligned, hdr->second_size);
    if (second.first) sha1_update(&ctx, second.first, second.second);
    sha1_update(&ctx, reinterpret_cast<const std::uint8_t*>(&hdr->second_size), 4);
    if (hdr->second_size > 0) checksum_aligned += Align(hdr->second_size, page_size);

    if (extracted_size) {
      auto extra = safe_slice(checksum_aligned, page_size);
      if (extra.first) sha1_update(&ctx, extra.first, extra.second);
      sha1_update(&ctx, reinterpret_cast<const std::uint8_t*>(&extracted_size), 4);
      checksum_aligned += Align(extracted_size, page_size);
    }

    if (header_ver == 1 || header_ver == 2) {
      auto rec = safe_slice(checksum_aligned, hdr->recovery_dtbo_size);
      if (rec.first) sha1_update(&ctx, rec.first, rec.second);
      sha1_update(&ctx, reinterpret_cast<const std::uint8_t*>(&hdr->recovery_dtbo_size), 4);
      checksum_aligned += Align(hdr->recovery_dtbo_size, page_size);
    }

    if (header_ver == 2) {
      auto dtb = safe_slice(checksum_aligned, hdr->dtb_size);
      if (dtb.first) sha1_update(&ctx, dtb.first, dtb.second);
      sha1_update(&ctx, reinterpret_cast<const std::uint8_t*>(&hdr->dtb_size), 4);
    }

    sha1_final(&ctx, hash);
    std::memcpy(hdr->id, hash, SHA1_DIGEST_SIZE);
  }
}

std::size_t FindLastAvbSignature(const std::vector<std::uint8_t>& data) {
  const std::array<std::uint8_t, 19> sig_base = {
      0x41, 0x56, 0x42, 0x30, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  for (std::uint8_t v = 0; v <= 2; ++v) {
    auto sig = sig_base;
    sig[18] = v;
    auto it = std::search(data.begin(), data.end(), sig.begin(), sig.end());
    if (it == data.end()) {
      continue;
    }
    std::size_t found = static_cast<std::size_t>(it - data.begin());
    ++it;
    while (it != data.end()) {
      it = std::search(it, data.end(), sig.begin(), sig.end());
      if (it == data.end()) {
        break;
      }
      found = static_cast<std::size_t>(it - data.begin());
      ++it;
    }
    return found;
  }
  return std::string::npos;
}

std::size_t FindLastNonZeroOffset(const std::vector<std::uint8_t>& data) {
  for (std::size_t i = data.size(); i > 0; --i) {
    if (data[i - 1] != 0) {
      return i;
    }
  }
  return 0;
}

bool ReadAvbFooterIfPresent(const std::vector<std::uint8_t>& boot, AvbFooter* out_footer) {
  if (boot.size() < kAvbFooterSize || out_footer == nullptr) {
    return false;
  }
  const std::size_t footer_off = boot.size() - kAvbFooterSize;
  const std::uint8_t* footer_ptr = boot.data() + static_cast<std::ptrdiff_t>(footer_off);
  if (offsetof(AvbFooter, magic) + 4 > kAvbFooterSize) {
    return false;
  }
  if (std::memcmp(footer_ptr + offsetof(AvbFooter, magic), "AVBf", 4) != 0) {
    return false;
  }
  std::memcpy(out_footer, footer_ptr, sizeof(AvbFooter));
  return true;
}

}  // namespace

bool BootUnpackKernel(const std::string& boot_image_path, const std::string& out_kernel_path) {
  try {
    const auto boot = ReadFile(boot_image_path);
    const BootImgHdr hdr = ParseHeader(boot);
    const std::size_t kernel_off = KernelOffset(hdr);
    if (kernel_off + hdr.kernel_size > boot.size()) {
      throw std::runtime_error("kernel range out of bounds");
    }
    std::vector<std::uint8_t> kernel_blob(hdr.kernel_size);
    std::memcpy(kernel_blob.data(), boot.data() + static_cast<std::ptrdiff_t>(kernel_off), hdr.kernel_size);

    const CompressMethod method = DetectCompressMethod(kernel_blob);
    const auto raw = AutoDecompressKernel(kernel_blob, method);
    WriteFile(out_kernel_path, raw);

    std::fprintf(stderr,
                 "boot unpack done: header_ver=%u page=%u kernel_off=0x%zx kernel_size=0x%x method=%d\n",
                 hdr.unused[0],
                 hdr.page_size,
                 kernel_off,
                 hdr.kernel_size,
                 static_cast<int>(method));
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "boot unpack failed: %s\n", e.what());
    return false;
  }
}

bool BootRepackKernel(const std::string& orig_boot_path,
                      const std::string& new_kernel_path,
                      const std::string& out_boot_path) {
  try {
    const auto boot = ReadFile(orig_boot_path);
    auto hdr = ParseHeader(boot);

    const std::size_t total_size = boot.size();
    const std::uint32_t header_ver = hdr.unused[0] > 10 ? 0u : hdr.unused[0];
    std::uint32_t extracted_size = 0;
    if (hdr.unused[0] > 10) {
      extracted_size = hdr.unused[0];
    }

    const std::size_t page_size = header_ver >= 3 ? kPageDefault : hdr.page_size;
    const std::uint32_t fmt_size = header_ver >= 3 ? hdr.kernel_addr : hdr.ramdisk_size;

    const std::size_t kernel_off = page_size;
    if (kernel_off + hdr.kernel_size > boot.size()) {
      throw std::runtime_error("kernel range out of bounds");
    }

    std::vector<std::uint8_t> old_kernel_blob(hdr.kernel_size);
    std::memcpy(old_kernel_blob.data(), boot.data() + static_cast<std::ptrdiff_t>(kernel_off), hdr.kernel_size);
    CompressMethod method = DetectCompressMethod(old_kernel_blob);

    std::vector<std::uint8_t> extracted_dtb;
    if (header_ver < 3) {
      const int dtb_off = FindDtbOffset(old_kernel_blob);
      if (dtb_off > 0) {
        extracted_dtb.assign(old_kernel_blob.begin() + dtb_off, old_kernel_blob.end());
      }
    }

    const auto new_kernel_raw = ReadFile(new_kernel_path);
    std::vector<std::uint8_t> final_kernel_blob = AutoCompressKernel(new_kernel_raw, method, old_kernel_blob);

    const std::size_t old_kernel_aligned = Align(hdr.kernel_size, page_size);
    const std::size_t rest_off = page_size + old_kernel_aligned;

    AvbFooter avb{};
    const bool has_avb_footer = ReadAvbFooterIfPresent(boot, &avb);

    std::vector<std::uint8_t> rest_no_avb;
    if (rest_off < total_size) {
      const std::size_t end_no_avb = has_avb_footer ? (total_size - kAvbFooterSize) : total_size;
      if (rest_off < end_no_avb) {
        rest_no_avb.assign(boot.begin() + static_cast<std::ptrdiff_t>(rest_off),
                           boot.begin() + static_cast<std::ptrdiff_t>(end_no_avb));
      }
    }
    std::vector<std::uint8_t> rest_payload = rest_no_avb;
    if (!rest_payload.empty()) {
      const std::size_t used = FindLastNonZeroOffset(rest_payload);
      const std::size_t threshold = (rest_payload.size() / 3) * 2;
      if (used <= threshold) {
        rest_payload.resize(used);
      }
    }

    std::vector<std::uint8_t> final_kernel_with_dtb = final_kernel_blob;
    final_kernel_with_dtb.insert(final_kernel_with_dtb.end(), extracted_dtb.begin(), extracted_dtb.end());
    hdr.kernel_size = static_cast<std::uint32_t>(final_kernel_with_dtb.size());

    if (!(IsSha256(hdr.id) != 1 || header_ver <= 3)) {
      // keep as-is when not expected to update
    } else {
      UpdateBootIdHash(&hdr,
                       final_kernel_with_dtb,
                       rest_payload,
                       fmt_size,
                       header_ver,
                       extracted_size,
                       page_size);
    }

    // Update AVB data_size fields if AVB signature can be found in the tail payload.
    const std::size_t new_kernel_aligned = Align(final_kernel_with_dtb.size(), page_size);
    const std::size_t avb_sig_off = FindLastAvbSignature(rest_payload);
    if (has_avb_footer && avb_sig_off != std::string::npos) {
      const std::uint32_t avb_size = static_cast<std::uint32_t>(page_size + avb_sig_off + new_kernel_aligned);
      avb.data_size1 = Swap32(avb_size);
      avb.data_size2 = Swap32(avb_size);
    }

    std::vector<std::uint8_t> out;
    out.resize(page_size, 0);
    std::memcpy(out.data(), &hdr, sizeof(hdr));

    out.insert(out.end(), final_kernel_with_dtb.begin(), final_kernel_with_dtb.end());
    out.resize(page_size + new_kernel_aligned, 0);

    out.insert(out.end(), rest_payload.begin(), rest_payload.end());

    const std::size_t min_target_no_avb = has_avb_footer ? (total_size - kAvbFooterSize) : total_size;
    std::size_t target_no_avb = min_target_no_avb;
    if (out.size() > min_target_no_avb) {
      if (has_avb_footer) {
        const std::size_t expanded_total = Align(out.size() + kAvbFooterSize, page_size);
        target_no_avb = expanded_total - kAvbFooterSize;
      } else {
        target_no_avb = Align(out.size(), page_size);
      }
    }
    if (out.size() < target_no_avb) {
      out.resize(target_no_avb, 0);
    }

    if (has_avb_footer) {
      out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&avb),
                 reinterpret_cast<const std::uint8_t*>(&avb) + sizeof(avb));
    }

    WriteFile(out_boot_path, out);

    std::fprintf(stderr,
                 "boot repack done: header_ver=%u page=%zu old_kernel=0x%x new_kernel=0x%zx method=%d dtb=0x%zx\n",
                 header_ver,
                 page_size,
                 static_cast<unsigned>(old_kernel_blob.size()),
                 final_kernel_with_dtb.size(),
                 static_cast<int>(method),
                 extracted_dtb.size());
    return true;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "boot repack failed: %s\n", e.what());
    return false;
  }
}

}  // namespace apd
