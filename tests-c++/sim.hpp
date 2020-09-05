/* Dhara - NAND flash management layer
 * Copyright (C) 2013 Daniel Beer <dlbeer@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTUOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef TESTS_SIM_H_
#define TESTS_SIM_H_

#include <dhara/nand.hpp>

#include <array>
#include <span>

namespace dhara_tests {

using namespace dhara;

/* Call counts */
struct sim_stats {
  int frozen;

  int is_bad;
  int mark_bad;

  int erase;
  int erase_fail;

  int is_erased;
  int prog;
  int prog_fail;

  int read;
  int read_bytes;
};

struct block_status_flags {
  bool bad_mark : 1;
  bool failed : 1;
};

struct block_status {
  block_status_flags flags;

  /* Index of the next unprogrammed page. 0 means a fully erased
   * block, and PAGES_PER_BLOCK is a fully programmed block.
   */
  int next_page;

  /* Timebomb counter: if non-zero, this is the number of
   * operations until permanent failure.
   */
  int timebomb;
};

/* Simulated NAND layer. This layer reads and writes to an in-memory
 * buffer.
 */
class SimNand : public NandBase {
 public:
  /* Is the given block bad? */
  virtual bool is_bad(block_t b) const noexcept override;

  /* Mark bad the given block (or attempt to). No return value is
   * required, because there's nothing that can be done in response.
   */
  virtual void mark_bad(block_t b) noexcept override;

  /* Erase the given block. This function should return 0 on success or -1
   * on failure.
   *
   * The status reported by the chip should be checked. If an erase
   * operation fails, return -1 and set err to E_BAD_BLOCK.
   */
  virtual Outcome<void> erase(block_t b) noexcept override;

  /* Program the given page. The data pointer is a pointer to an entire
   * page ((1 << log2_page_size) bytes). The operation status should be
   * checked. If the operation fails, return -1 and set err to
   * E_BAD_BLOCK.
   *
   * Pages will be programmed sequentially within a block, and will not be
   * reprogrammed.
   */
  virtual Outcome<void> prog(page_t p, std::span<const std::byte> data) noexcept override;

  /* Check that the given page is erased */
  virtual bool is_free(page_t p) const noexcept override;

  /* Read a portion of a page. ECC must be handled by the NAND
   * implementation. Returns 0 on sucess or -1 if an error occurs. If an
   * uncorrectable ECC error occurs, return -1 and set err to E_ECC.
   */
  virtual Outcome<void> read(page_t p, size_t offset,
                             std::span<std::byte> data) const noexcept override;

  /* Read a page from one location and reprogram it in another location.
   * This might be done using the chip's internal buffers, but it must use
   * ECC.
   */
  virtual Outcome<void> copy(page_t src, page_t dst) noexcept override;

 public:
  /* Reset to start-up defaults */
  void reset();

  /* Dump statistics and status */
  void dump() const;

  /* Halt/resume counting of statistics */
  void freeze();

  void thaw();

  /* Set faults on individual blocks */
  void set_failed(block_t blk);

  void set_timebomb(block_t blk, int ttl);

  /* Create some factory-marked bad blocks */
  void inject_bad(int count);

  /* Create some unmarked bad blocks */
  void inject_failed(int count);

  /* Create a timebomb on the given block */
  void inject_timebombs(int count, int max_ttl);

 private:
  void timebomb_tick(block_t blk);

 protected:
  [[nodiscard]] virtual std::span<block_status> blocks() = 0;

  [[nodiscard]] virtual std::span<const block_status> blocks() const = 0;

  [[nodiscard]] virtual std::span<std::byte> pages() = 0;

  [[nodiscard]] virtual std::span<const std::byte> pages() const = 0;

  [[nodiscard]] virtual std::span<std::byte> page_buf() = 0;

  [[nodiscard]] virtual std::span<const std::byte> page_buf() const = 0;

  [[nodiscard]] std::span<std::byte> block_data(block_t bidx) {
    return pages().subspan(std::size_t(bidx) << log2_block_size(), block_size());
  }
  [[nodiscard]] std::span<const std::byte> block_data(block_t bidx) const {
    return pages().subspan(std::size_t(bidx) << log2_block_size(), block_size());
  }
  [[nodiscard]] std::span<std::byte> page_data(page_t pidx) {
    return pages().subspan(std::size_t(pidx) << log2_page_size(), page_size());
  }
  [[nodiscard]] std::span<const std::byte> page_data(page_t pidx) const {
    return pages().subspan(std::size_t(pidx) << log2_page_size(), page_size());
  }

 private:
  mutable sim_stats stats;
};

template <std::uint8_t log2_page_size__ = 9u, std::uint8_t log2_ppb__ = 3u,
          std::size_t num_blocks__ = 113u>
class StaticSimNand final : public SimNand {
 public:
  static constexpr std::uint8_t log2_page_size_ = log2_page_size__;
  static constexpr std::uint8_t log2_ppb_ = log2_ppb__;
  static constexpr std::size_t num_blocks_ = num_blocks__;
  static constexpr std::uint8_t log2_block_size_ = log2_page_size_ + log2_ppb_;
  static constexpr std::size_t page_size_ = 1u << log2_page_size_;
  static constexpr std::size_t pages_per_block_ = 1u << log2_ppb_;
  static constexpr std::size_t block_size_ = 1u << log2_block_size_;
  static constexpr std::size_t mem_size_ = num_blocks_ * block_size_;

  /* Base-2 logarithm of the page size. If your device supports
   * partial programming, you may want to subdivide the actual
   * pages into separate ECC-correctable regions and present those
   * as pages.
   */
  [[nodiscard]] virtual constexpr std::uint8_t log2_page_size() const noexcept final {
    return log2_page_size_;
  }

  /* Base-2 logarithm of the number of pages within an eraseblock */
  [[nodiscard]] virtual constexpr std::uint8_t log2_ppb() const noexcept final { return log2_ppb_; }

  /* Total number of eraseblocks */
  [[nodiscard]] virtual constexpr std::size_t num_blocks() const noexcept final {
    return num_blocks_;
  }

 protected:
  [[nodiscard]] virtual std::span<block_status> blocks() final { return blocks_; };

  [[nodiscard]] virtual std::span<const block_status> blocks() const final { return blocks_; };

  [[nodiscard]] virtual std::span<std::byte> pages() final { return pages_; };

  [[nodiscard]] virtual std::span<const std::byte> pages() const final { return pages_; };

  [[nodiscard]] virtual std::span<std::byte> page_buf() final { return page_buf_; };

  [[nodiscard]] virtual std::span<const std::byte> page_buf() const final { return page_buf_; };

 private:
  std::array<block_status, num_blocks_> blocks_;
  std::array<std::byte, mem_size_> pages_;
  std::array<std::byte, page_size_> page_buf_;
};

}  // namespace dhara_tests

#endif
