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

#include <dhara/bytes.hpp>
#include <dhara/map.hpp>

#include <cstring>

namespace dhara {

static constexpr std::size_t radix_depth = sizeof(sector_t) << 3u;

sector_t d_bit(int depth) { return ((sector_t)1) << (radix_depth - depth - 1); }

/************************************************************************
 * Metadata/cookie layout
 */

void ck_set_count(std::span<std::byte,4> cookie, sector_t count) { w32(cookie, count); }

sector_t ck_get_count(std::span<const std::byte,4> cookie) { return r32(cookie); }

void meta_clear(std::span<std::byte,132> meta) { memset(meta.data(), 0xff, meta.size()); }

sector_t meta_get_id(std::span<const std::byte,132> meta) { return r32(meta.first<4>()); }

void meta_set_id(std::span<std::byte,132> meta, sector_t id) { w32(meta.first<4>(), id); }

page_t meta_get_alt(std::span<const std::byte,132> meta, unsigned int level) {
  return r32(std::span<const std::byte, 4>(meta.subspan(4u + (level << 2u),4)));
}

void meta_set_alt(std::span<std::byte,132> meta, unsigned int level, page_t alt) {
  w32(std::span<std::byte, 4>(meta.subspan(4u + (level << 2u),4)), alt);
}

/************************************************************************
 * Public interface
 */

MapBase::MapBase(const JournalConfig &config, const MapConfig &map_config, NandBase &n,
                 byte_buf_t page_buf) noexcept
    : base_t(config, n, page_buf), map_config(map_config) {}

Outcome<void> MapBase::resume() noexcept {
  {
    auto res = base_t::resume();
    if (res.has_error()) {
      count = 0;
      return res.error();
    }
  }

  count = ck_get_count(cookie());
  return error_t::none;
}

void MapBase::clear() noexcept {
  if (count) {
    count = 0;
    base_t::clear();
  }
}

sector_t MapBase::capacity() const noexcept {
  const sector_t cap = base_t::capacity();
  const sector_t reserve = cap / (map_config.gc_ratio + 1);
  const sector_t safety_margin = config.max_retries << config.nand.log2_ppb;

  if (reserve + safety_margin >= cap) return 0;

  return cap - reserve - safety_margin;
}

/* Trace the path from the root to the given sector, emitting
 * alt-pointers and alt-full bits in the given metadata buffer. This
 * also returns the physical page containing the given sector, if it
 * exists.
 *
 * If the page can't be found, a suitable path will be constructed
 * (containing PAGE_NONE alt-pointers), and DHARA_E_NOT_FOUND will be
 * returned.
 */
Outcome<page_t> MapBase::trace_path(sector_t target, std::optional<meta_span_t> new_meta) noexcept {
  meta_buf_t meta;
  int depth = 0;
  page_t p = root();

  if (new_meta) meta_set_id(*new_meta, target);

  if (p != page_none) {
    DHARA_TRY(read_meta(p, meta));

    bool done = true;
    while (depth < radix_depth) {
      const sector_t id = meta_get_id(meta);

      if (id == sector_none) {
        done = false;
        break;
      }

      if ((target ^ id) & d_bit(depth)) {
        if (new_meta) meta_set_alt(*new_meta, depth, p);

        p = meta_get_alt(meta, depth);
        if (p == page_none) {
          depth++;
          done = false;
          break;
        }

        DHARA_TRY(read_meta(p, meta));
      } else {
        if (new_meta) meta_set_alt(*new_meta, depth, meta_get_alt(meta, depth));
      }

      depth++;
    }
    if (done) return p;
  }

  if (new_meta) {
    while (depth < radix_depth) meta_set_alt(*new_meta, depth++, sector_none);
  }

  return error_t::not_found;
}

Outcome<page_t> MapBase::find(sector_t target) noexcept { return trace_path(target, {}); }

Outcome<void> MapBase::read(sector_t s, std::byte *data) noexcept {
  const std::size_t page_size = config.nand.page_size();
  auto res = find(s);
  if (res.has_error()) {
    if (res.error() == error_t::not_found) {
      memset(data, 0xff, page_size);
      return error_t::none;
    }
    return res.error();
  }

  return nand.read(res.value(), 0, {data, page_size});
}

/* Check the given page. If it's garbage, do nothing. Otherwise, rewrite
 * it at the front of the map. Return raw errors from the journal (do
 * not perform recovery).
 */
Outcome<void> MapBase::raw_gc(page_t src) noexcept {
  sector_t target;
  page_t current;
  meta_buf_t meta;

  DHARA_TRY(read_meta(src, meta));

  /* Is the page just filler/garbage? */
  target = meta_get_id(meta);
  if (target == sector_none) return error_t::none;

  /* Find out where the sector once represented by this page
   * currently resides (if anywhere).
   */
  {
    auto res = trace_path(target, meta);
    if (res.has_error()) {
      if (res.error() == error_t::not_found) return error_t::none;
      return res.error();
    }
    current = res.value();
  }

  /* Is this page still the most current representative? If not,
   * do nothing.
   */
  if (current != src) return error_t::none;

  /* Rewrite it at the front of the journal with updated metadata */
  ck_set_count(cookie(), this->count);
  return copy(src, meta);
}

Outcome<void> MapBase::pad_queue() noexcept {
  page_t p = root();
  meta_buf_t root_meta;

  ck_set_count(cookie(), count);

  if (p == page_none) return enqueue();

  DHARA_TRY(read_meta(p, root_meta));

  return copy(p, root_meta);
}

/* Attempt to recover the journal */
Outcome<void> MapBase::try_recover(error_t cause) noexcept {
  int restart_count = 0;

  if (cause != error_t::recover) {
    return cause;
  }

  while (in_recovery()) {
    page_t p = next_recoverable();

    auto res = p == page_none ? pad_queue() : raw_gc(p);

    if (res.has_error()) {
      if (res.error() != error_t::recover) {
        return res.error();
      }

      if (restart_count >= config.max_retries) {
        return error_t::too_bad;
      }

      restart_count++;
    }
  }

  return error_t::none;
}

Outcome<void> MapBase::auto_gc() noexcept {
  if (base_t::size() < capacity()) return error_t::none;

  for (int i = 0; i < map_config.gc_ratio; i++) {
    DHARA_TRY(gc());
  }

  return error_t::none;
}

Outcome<void> MapBase::prepare_write(sector_t dst, meta_span_t meta) noexcept {
  DHARA_TRY(auto_gc());

  auto res = trace_path(dst, meta);
  if (res.has_error()) {
    if (res.error() != error_t::not_found) {
      return res.error();
    }

    if (count >= capacity()) {
      return error_t::map_full;
    }

    count++;
  }

  ck_set_count(cookie(), count);
  return error_t::none;
}

Outcome<void> MapBase::write(sector_t dst, const std::byte *data) noexcept {
  meta_buf_t meta;
  for (;;) {
    const sector_t old_count = count;

    DHARA_TRY(prepare_write(dst, meta));

    auto res = enqueue(data, meta);
    if (res.has_value()) break;

    count = old_count;

    DHARA_TRY(try_recover(res.error()));
  }

  return error_t::none;
}

Outcome<void> MapBase::copy_page(page_t src, sector_t dst) noexcept {
  meta_buf_t meta;
  for (;;) {
    const sector_t old_count = count;

    DHARA_TRY(prepare_write(dst, meta));

    auto res = copy(src, meta);
    if (res.has_value()) break;

    count = old_count;

    DHARA_TRY(try_recover(res.error()));
  }

  return error_t::none;
}

Outcome<void> MapBase::copy_sector(sector_t src, sector_t dst) noexcept {
  auto res = find(src);
  if (res.has_error()) {
    if (res.error() == error_t::not_found) return trim(dst);

    return res.error();
  }

  return copy_page(res.value(), dst);
}

Outcome<void> MapBase::try_delete(sector_t s) noexcept {
  error_t my_err;
  meta_buf_t meta;
  page_t alt_page;
  meta_buf_t alt_meta;
  int level = radix_depth - 1;
  int i;

  {
    auto res = trace_path(s, meta);
    if (res.has_error()) {
      if (res.error() == error_t::not_found) return error_t::none;
      return res.error();
    }
  }

  /* Select any of the closest cousins of this node which are
   * subtrees of at least the requested order.
   */
  while (level >= 0) {
    alt_page = meta_get_alt(meta, level);
    if (alt_page != page_none) break;
    level--;
  }

  /* Special case: deletion of last sector */
  if (level < 0) {
    clear();
    return error_t::none;
  }

  /* Rewrite the cousin with an up-to-date path which doesn't
   * point to the original node.
   */
  DHARA_TRY(read_meta(alt_page, alt_meta));

  meta_set_id(meta, meta_get_id(alt_meta));

  meta_set_alt(meta, level, page_none);
  for (i = level + 1; i < radix_depth; i++) meta_set_alt(meta, i, meta_get_alt(alt_meta, i));

  meta_set_alt(meta, level, page_none);

  ck_set_count(cookie(), count - 1);
  DHARA_TRY(copy(alt_page, meta));

  count--;
  return error_t::none;
}

Outcome<void> MapBase::trim(sector_t s) noexcept {
  for (;;) {
    DHARA_TRY(auto_gc());

    auto res = try_delete(s);
    if (res.has_value()) break;

    DHARA_TRY(try_recover(res.error()));
  }

  return error_t::none;
}

Outcome<void> MapBase::sync() noexcept {
  while (!is_clean()) {
    page_t p = peek();
    Outcome<void> res(error_t::none);

    if (p == page_none) {
      res = pad_queue();
    } else {
      res = raw_gc(p);
      dequeue();
    }

    if (res.has_error()) {
      DHARA_TRY(try_recover(res.error()));
    }
  }

  return error_t::none;
}

Outcome<void> MapBase::gc() noexcept {
  if (!count) return error_t::none;

  for (;;) {
    page_t tail = peek();

    if (tail == page_none) break;

    auto res = raw_gc(tail);
    if (res.has_value()) {
      dequeue();
      break;
    }

    DHARA_TRY(try_recover(res.error()));
  }

  return error_t::none;
}

}  // namespace dhara