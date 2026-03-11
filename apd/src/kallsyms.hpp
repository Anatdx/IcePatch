#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace apd {

struct KallsymsProbeResult {
  bool ok = false;
  bool has_symbol_offsets = false;
  int version_major = 0;
  int version_minor = 0;
  int version_patch = 0;
  std::size_t token_table_offset = 0;
  std::size_t token_index_offset = 0;
  bool little_endian_token_index = true;
  std::size_t markers_offset = 0;
  std::size_t names_offset = 0;
  std::size_t approx_addr_or_offset_start = 0;
  std::size_t approx_addr_or_offset_end = 0;
  int approx_num_syms = 0;
  int num_syms = 0;
  std::size_t offset_table_start = 0;
  int markers_elem_size = 0;
  int offset_table_elem_size = 0;
  bool has_relative_base = false;
  std::uint64_t kernel_base = 0;
  int kallsyms_lookup_name_index = -1;
  int kallsyms_lookup_name_offset = -1;
  int paging_init_offset = -1;
  int printk_offset = -1;
  int panic_offset = -1;
  int rest_init_offset = -1;
  int cgroup_init_offset = -1;
  int kernel_init_offset = -1;
  int copy_process_offset = -1;
  int cgroup_post_fork_offset = -1;
  int avc_denied_offset = -1;
  int slow_avc_audit_offset = -1;
  int input_handle_event_offset = -1;
  int report_cfi_failure_offset = -1;
  int cfi_slowpath_diag_offset = -1;
  int cfi_slowpath_offset = -1;
  int memblock_reserve_offset = -1;
  int memblock_free_offset = -1;
  int memblock_mark_nomap_offset = -1;
  int memblock_phys_alloc_try_nid_offset = -1;
  int memblock_virt_alloc_try_nid_offset = -1;
  int memblock_alloc_try_nid_offset = -1;
  int tcp_init_sock_offset = -1;
  std::string message;
};

KallsymsProbeResult ProbeKallsymsLayout(const std::vector<std::uint8_t>& image);
bool PrintKallsymsProbe(const std::string& image_path);
bool DumpKallsyms(const std::string& image_path);
bool DumpIkconfig(const std::string& image_path);

}  // namespace apd
