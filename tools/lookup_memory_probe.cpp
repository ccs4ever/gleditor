#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/manifold_lookup.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory_resource>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

class CountingResource final : public std::pmr::memory_resource {
public:
  [[nodiscard]] std::size_t currentBytes() const noexcept { return current_; }
  [[nodiscard]] std::size_t peakBytes() const noexcept { return peak_; }
  void resetPeak() noexcept { peak_ = current_; }

private:
  void *do_allocate(std::size_t bytes, std::size_t alignment) override {
    void *const pointer =
        std::pmr::new_delete_resource()->allocate(bytes, alignment);
    bool inserted = false;
    try {
      inserted = allocations_.emplace(pointer, bytes).second;
    } catch (...) {
      std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
      throw;
    }
    if (!inserted) {
      std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
      throw std::bad_alloc{};
    }
    current_ += bytes;
    peak_ = std::max(peak_, current_);
    return pointer;
  }
  void do_deallocate(void *pointer, std::size_t bytes,
                     std::size_t alignment) override {
    const auto it = allocations_.find(pointer);
    if (it != allocations_.end()) {
      bytes = it->second;
      current_ -= it->second;
      allocations_.erase(it);
    }
    std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
  }
  bool
  do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    return this == &other;
  }

  // The ledger intentionally uses the default allocator, outside the measured
  // PMR resource, so observing allocations does not recursively observe itself.
  std::map<void *, std::size_t> allocations_;
  std::size_t current_{0};
  std::size_t peak_{0};
};

std::vector<std::uint32_t> keysFor(const std::string_view pattern,
                                   const std::size_t requested,
                                   std::uint32_t &hint) {
  std::vector<std::uint32_t> keys;
  const auto count = pattern == "sparse-in-dense"
                         ? std::min<std::size_t>(requested, 4096U)
                     : pattern.starts_with("sparse-branch-")
                         ? std::min<std::size_t>(requested, 3U)
                         : requested;
  keys.reserve(count);
  if (pattern == "sequential") {
    hint = static_cast<std::uint32_t>(count);
    for (std::size_t i = 1; i <= count; ++i)
      keys.push_back(static_cast<std::uint32_t>(i));
  } else if (pattern == "clustered") {
    for (std::size_t i = 0; i < count; ++i) {
      keys.push_back(
          static_cast<std::uint32_t>(1U + (i / 16U) * 1024U + (i % 16U)));
    }
    hint = static_cast<std::uint32_t>(count);
  } else if (pattern == "sparse-in-dense") {
    hint = 4096U;
    for (std::size_t i = 0; i < count; ++i) {
      keys.push_back(
          static_cast<std::uint32_t>(1U + ((i + 1U) * 8190U) / (count + 1U)));
    }
  } else if (pattern == "sparse-high") {
    hint = 4096U;
    for (std::size_t i = 0; i < count; ++i)
      keys.push_back(0x80000000U + static_cast<std::uint32_t>(2U * i));
  } else if (pattern.starts_with("sparse-branch-")) {
    hint = pattern == "sparse-branch-global-hint-10k" ? 10'000U : 148U;
    constexpr std::uint32_t branchKeys[]{1U, 5'000U, 9'999U};
    for (std::size_t i = 0; i < count; ++i) keys.push_back(branchKeys[i]);
  }
  return keys;
}

void measureByRef(const std::string_view pattern, const std::size_t requested) {
  std::uint32_t hint = 0;
  const auto keys    = keysFor(pattern, requested, hint);
  CountingResource denseResource;
  zigzag::detail::ManifoldOpIndexMap dense(&denseResource);
  dense.setOpCountHint(hint);
  for (const auto key : keys) {
    auto token = dense.prepareInsert(key);
    if (!token || !dense.commitInsert(std::move(*token), key, key ^ 0x55U))
      std::abort();
  }
  std::cout << "byRef," << pattern << ',' << keys.size() << ',' << hint << ','
            << dense.size() << ',' << dense.denseValueCapacity() << ','
            << dense.densePresenceBytes() << ','
            << dense.overflowBucketCapacity() << ','
            << denseResource.currentBytes() << ',' << denseResource.peakBytes()
            << ',';

  CountingResource unorderedResource;
  std::pmr::unordered_map<std::uint32_t, std::uint32_t> unordered{
      &unorderedResource};
  for (const auto key : keys) unordered.emplace(key, key ^ 0x55U);
  std::cout << unordered.bucket_count() << ','
            << unorderedResource.currentBytes() << ','
            << unorderedResource.peakBytes() << ',';
  CountingResource reservedResource;
  std::pmr::unordered_map<std::uint32_t, std::uint32_t> reserved{
      &reservedResource};
  reserved.reserve(keys.size());
  for (const auto key : keys) reserved.emplace(key, key ^ 0x55U);
  std::cout << reserved.bucket_count() << ',' << reservedResource.currentBytes()
            << ',' << reservedResource.peakBytes() << '\n';
}

void measureBirths(const std::string_view pattern,
                   const std::size_t requested) {
  std::uint32_t ignoredHint = 0;
  const auto keys           = keysFor(pattern, requested, ignoredHint);
  CountingResource preparedResource;
  zigzag::detail::PreparedU32UnorderedMap<zigzag::StructureBirthInfo> prepared{
      &preparedResource};
  for (const auto key : keys) {
    auto token = prepared.prepareInsert(key);
    const zigzag::StructureBirthInfo value{
        .opIndex     = key,
        .kind        = xanadu::StructureKind::Cell,
        .containerOp = 0,
        .nameSpan    = {},
    };
    if (!token || !prepared.commitInsert(std::move(*token), value))
      std::abort();
  }
  std::cout << "births-prepared," << pattern << ',' << keys.size() << ','
            << (keys.empty() ? 0U : keys.back()) << ',' << prepared.size()
            << ',' << prepared.capacity() << ",0,"
            << sizeof(zigzag::StructureBirthInfo) << ','
            << preparedResource.currentBytes() << ','
            << preparedResource.peakBytes() << ',';

  CountingResource unorderedResource;
  std::pmr::unordered_map<std::uint32_t, zigzag::StructureBirthInfo> unordered{
      &unorderedResource};
  for (const auto key : keys) {
    unordered.emplace(key, zigzag::StructureBirthInfo{
                               .opIndex     = key,
                               .kind        = xanadu::StructureKind::Cell,
                               .containerOp = 0,
                               .nameSpan    = {},
                           });
  }
  std::cout << unordered.bucket_count() << ','
            << unorderedResource.currentBytes() << ','
            << unorderedResource.peakBytes() << ',';
  CountingResource reservedResource;
  std::pmr::unordered_map<std::uint32_t, zigzag::StructureBirthInfo> reserved{
      &reservedResource};
  reserved.reserve(keys.size());
  for (const auto key : keys) {
    reserved.emplace(key, zigzag::StructureBirthInfo{
                              .opIndex     = key,
                              .kind        = xanadu::StructureKind::Cell,
                              .containerOp = 0,
                              .nameSpan    = {},
                          });
  }
  std::cout << reserved.bucket_count() << ',' << reservedResource.currentBytes()
            << ',' << reservedResource.peakBytes() << '\n';
}

} // namespace

int main(int argc, char **argv) {
  std::size_t count = 100'000U;
  if (argc > 2) {
    std::cerr << "usage: lookup_memory_probe [count<=1000000]\n";
    return 2;
  }
  if (argc == 2) {
    const std::string_view arg{argv[1]};
    const auto [end, error] =
        std::from_chars(arg.data(), arg.data() + arg.size(), count);
    if (error != std::errc{} || end != arg.data() + arg.size() || count == 0 ||
        count > 1'000'000U) {
      std::cerr << "count must be in [1, 1000000]\n";
      return 2;
    }
  }
  std::cout
      << "kind,pattern,requested_or_live,hint_or_max_key,live,primary_capacity,"
         "secondary_bytes_or_staging_buckets,overflow_or_mapped_value_bytes,"
         "current_"
         "bytes,peak_bytes,unordered_buckets,unordered_current,unordered_peak,"
         "reserved_buckets,reserved_current,reserved_peak\n";
  for (const auto pattern :
       {"sequential", "clustered", "sparse-in-dense", "sparse-high",
        "sparse-branch-local-hint", "sparse-branch-global-hint-10k"}) {
    measureByRef(pattern, count);
    measureBirths(pattern, count);
  }
}
