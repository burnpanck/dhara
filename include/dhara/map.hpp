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

#ifndef DHARA_MAP_H_
#define DHARA_MAP_H_

#include <dhara/journal.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace dhara {

/* The map is a journal indexing format. It maps virtual sectors to
 * pages of data in flash memory.
 */
using sector_t = std::uint32_t;
using sector_count_t = std::uint32_t;

struct MapConfig {
  std::uint8_t gc_ratio;
};

class MapBase : public JournalSpec<132u, 4u> {
  using base_t = JournalSpec<132u, 4u>;

 protected:
  using meta_buf_t = std::array<std::byte, meta_size>;

 public:
  static constexpr sector_t sector_none = static_cast<sector_t>(-1);

  void init() noexcept { count = 0; }

  /* Recover stored state, if possible. If there is no valid stored state
   * on the chip, -1 is returned, and an empty map is initialized.
   */
  Outcome<void> resume() noexcept;

  /* Clear the map (delete all sectors). */
  void clear() noexcept;

  /* Obtain the maximum capacity of the map. */
  sector_t capacity() const noexcept;

  /* Obtain the current number of allocated sectors. */
  sector_t size() const noexcept { return count; }

  /* Find the physical page which holds the current data for this sector.
   * Returns 0 on success or -1 if an error occurs. If the sector doesn't
   * exist, the error is E_NOT_FOUND.
   */
  Outcome<page_t> find(sector_t s) noexcept;

  /* Copy any flash page to a logical sector. */
  Outcome<void> copy_page(page_t src, sector_t dst) noexcept;

  /* Copy one sector to another. If the source sector is unmapped, the
   * destination sector will be trimmed.
   */
  Outcome<void> copy_sector(sector_t src, sector_t dst) noexcept;

  /* Delete a logical sector. You don't necessarily need to do this, but
   * it's a useful hint if you no longer require the sector's data to be
   * kept.
   *
   * If order is non-zero, it specifies that all sectors in the
   * (2**order)-aligned group of s are to be deleted.
   */
  Outcome<void> trim(sector_t s) noexcept;

  /* Synchronize the map. Once this returns successfully, all changes to
   * date are persistent and durable. Conversely, there is no guarantee
   * that unsynchronized changes will be persistent.
   */
  Outcome<void> sync() noexcept;

  /* Perform one garbage collection step. You can do this whenever you
   * like, but it's not necessary -- garbage collection happens
   * automatically and is interleaved with other operations.
   */
  Outcome<void> gc() noexcept;

 protected:
  /* Initialize a map. You need to supply a buffer for page metadata, and
   * a garbage collection ratio. This is the ratio of garbage collection
   * operations to real writes when automatic collection is active.
   *
   * Smaller values lead to faster and more predictable IO, at the
   * expense of capacity. You should always initialize the same chip with
   * the same garbage collection ratio.
   */
  MapBase(const MapConfig &map_config, const JournalConfig &config, NandBase &n,
          byte_buf_t page_buf) noexcept;

  /* Write data to a logical sector. */
  Outcome<void> write(sector_t s, const std::byte *data) noexcept;

  /* Read from the given logical sector. If the sector is unmapped, a
   * blank page (0xff) will be returned.
   */
  Outcome<void> read(sector_t s, std::byte *data) noexcept;

 private:
  Outcome<page_t> trace_path(sector_t target, std::optional<meta_span_t> new_meta) noexcept;
  Outcome<void> raw_gc(page_t src) noexcept;
  Outcome<void> pad_queue() noexcept;
  Outcome<void> try_recover(error_t cause) noexcept;
  Outcome<void> auto_gc() noexcept;
  Outcome<void> prepare_write(sector_t dst, meta_span_t meta) noexcept;
  Outcome<void> try_delete(sector_t s) noexcept;

 protected:
  const MapConfig &map_config;

  sector_count_t count;
};

template <std::uint8_t log2_page_size_, std::uint8_t log2_ppb_, std::size_t gc_ratio = 4u, std::size_t max_retries_ = 8u,
          typename Base =
              Journal<log2_page_size_, log2_ppb_, MapBase::meta_size, MapBase::cookie_size, max_retries_, MapBase>>
class Map : public Base {
  using base_t = Base;
 public:
  static constexpr MapConfig map_config = {.gc_ratio = gc_ratio};

  template <typename NBase>
  explicit Map(Nand<log2_page_size_,log2_ppb_,NBase> &n) noexcept : base_t(n, map_config) {}
};

}  // namespace dhara

#endif
