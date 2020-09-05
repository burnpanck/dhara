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
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef DHARA_NAND_H_
#define DHARA_NAND_H_

#include <dhara/error.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace dhara {

/* Each page in a NAND device is indexed, starting at 0. It's required
 * that there be a power-of-two number of pages in a eraseblock, so you can
 * view a page number is being a concatenation (in binary) of a block
 * number and the number of a page within a block.
 */
typedef uint32_t page_t;

/* Blocks are also indexed, starting at 0. */
typedef uint32_t block_t;

/* Each NAND chip must be represented by one of these structures. It's
 * intended that this structure be embedded in a larger structure for
 * context.
 *
 * The functions declared below are not implemented -- they must be
 * provided and satisfy the documented conditions.
 */
class NandBase {
 public:
  /* Is the given block bad? */
  virtual bool is_bad(block_t b) const noexcept = 0;

  /* Mark bad the given block (or attempt to). No return value is
   * required, because there's nothing that can be done in response.
   */
  virtual void mark_bad(block_t b) noexcept = 0;

  /* Erase the given block. This function should return 0 on success or -1
   * on failure.
   *
   * The status reported by the chip should be checked. If an erase
   * operation fails, return -1 and set err to E_BAD_BLOCK.
   */
  virtual Outcome<void> erase(block_t b) noexcept = 0;

  /* Program the given page. The data pointer is a pointer to an entire
   * page ((1 << log2_page_size) bytes). The operation status should be
   * checked. If the operation fails, return -1 and set err to
   * E_BAD_BLOCK.
   *
   * Pages will be programmed sequentially within a block, and will not be
   * reprogrammed.
   */
  virtual Outcome<void> prog(page_t p, std::span<const std::byte> data) noexcept = 0;

  /* Check that the given page is erased */
  virtual bool is_free(page_t p) const noexcept = 0;

  /* Read a portion of a page. ECC must be handled by the NAND
   * implementation. Returns 0 on sucess or -1 if an error occurs. If an
   * uncorrectable ECC error occurs, return -1 and set err to E_ECC.
   */
  virtual Outcome<void> read(page_t p, std::size_t offset, std::span<std::byte> data) const noexcept = 0;

  /* Read a page from one location and reprogram it in another location.
   * This might be done using the chip's internal buffers, but it must use
   * ECC.
   */
  virtual Outcome<void> copy(page_t src, page_t dst) noexcept = 0;

  /* Base-2 logarithm of the page size. If your device supports
   * partial programming, you may want to subdivide the actual
   * pages into separate ECC-correctable regions and present those
   * as pages.
   */
  [[nodiscard]] virtual std::uint8_t log2_page_size() const noexcept = 0;

  /* Base-2 logarithm of the number of pages within an eraseblock */
  [[nodiscard]] virtual std::uint8_t log2_ppb() const noexcept = 0;

  /* Total number of eraseblocks */
  [[nodiscard]] virtual std::size_t num_blocks() const noexcept = 0;

  [[nodiscard]] virtual constexpr std::uint8_t log2_block_size() const {
    return log2_page_size() + log2_ppb();
  };
  [[nodiscard]] virtual constexpr std::size_t page_size() const { return 1u << log2_page_size(); }
  [[nodiscard]] virtual constexpr std::size_t pages_per_block() const { return 1u << log2_ppb(); };
  [[nodiscard]] virtual constexpr std::size_t block_size() const { return 1u << log2_block_size(); }
  [[nodiscard]] virtual constexpr std::size_t mem_size() const {
    return num_blocks() << log2_block_size();
  }
};

struct NandConfig {
  std::uint8_t log2_page_size;
  std::uint8_t log2_ppb;

  [[nodiscard]] constexpr std::uint8_t log2_block_size() const {
    return log2_page_size + log2_ppb;
  };
  [[nodiscard]] constexpr std::size_t page_size() const { return 1u << log2_page_size; }
  [[nodiscard]] constexpr std::size_t pages_per_block() const { return 1u << log2_ppb; };
  [[nodiscard]] constexpr std::size_t block_size() const { return 1u << log2_block_size(); }
};

template <std::uint8_t log2_page_size_, std::uint8_t log2_ppb_>
class Nand : public NandBase {
 public:
  static constexpr NandConfig config = {log2_page_size_, log2_ppb_};

  /* Base-2 logarithm of the page size. If your device supports
 * partial programming, you may want to subdivide the actual
 * pages into separate ECC-correctable regions and present those
 * as pages.
 */
  [[nodiscard]] virtual constexpr std::uint8_t log2_page_size() const noexcept final {
      return config.log2_page_size;
  };

  /* Base-2 logarithm of the number of pages within an eraseblock */
  [[nodiscard]] virtual constexpr std::uint8_t log2_ppb() const noexcept final {
    return config.log2_ppb;
  }

  [[nodiscard]] virtual constexpr std::uint8_t log2_block_size() const {
    return config.log2_block_size();
  };
  [[nodiscard]] virtual constexpr std::size_t page_size() const { return config.page_size(); }
  [[nodiscard]] virtual constexpr std::size_t pages_per_block() const { return config.pages_per_block(); };
  [[nodiscard]] virtual constexpr std::size_t block_size() const { return config.block_size(); }
  [[nodiscard]] virtual constexpr std::size_t mem_size() const {
    return num_blocks() << config.log2_block_size();
  }
};

}  // namespace dhara

#endif
