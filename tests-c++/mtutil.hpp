//
// Created by Yves Delley on 07.09.20.
//

#ifndef DHARA_NAND_FTL_MTUTIL_HPP
#define DHARA_NAND_FTL_MTUTIL_HPP

#include "util.hpp"

#include <dhara/bytes.hpp>
#include <dhara/map.hpp>

#include <cassert>

namespace dhara_tests {

using namespace dhara;

class TestMapBase : public MapBase {
  using base_t = MapBase;

 public:
  using base_t::base_t;

  int check_recurse(page_t parent, page_t page, sector_t id_expect, unsigned int depth) {
    meta_buf_t meta;
    const page_t h_offset = head - tail;
    const page_t p_offset = parent - tail;
    const page_t offset = page - tail;
    sector_t id;
    int count = 1;
    unsigned int i;

    if (page == MapBase::page_none) return 0;

    /* Make sure this is a valid journal user page, and one which is
     * older than the page pointing to it.
     */
    assert(offset < p_offset);
    assert(offset < h_offset);
    assert((~page) & ((1u << config.log2_ppc) - 1u));

    /* Fetch metadata */
    DHARA_TRY_ABORT(read_meta(page, meta));

    /* Check the first <depth> bits of the ID field */
    id = r32(std::span(meta).first<4>());
    if (!depth) {
      id_expect = id;
    } else {
      assert(!((id ^ id_expect) >> (32u - depth)));
    }

    /* Check all alt-pointers */
    for (i = depth; i < 32u; i++) {
      page_t child = r32(std::span<const std::byte, 4>(std::span(meta).subspan(4u + (i << 2u), 4)));

      count += check_recurse(page, child, id ^ (1u << (31u - i)), i + 1u);
    }

    return count;
  }

  void check() {
    auto &sim_nand = static_cast<SimNand&>(nand);

    sim_nand.freeze();
    const int count = check_recurse(head, root(), 0, 0);
    sim_nand.thaw();

    assert(this->count == count);
  }

  void write(sector_t s, int seed) {
    const size_t page_size = config.nand.page_size();
    std::byte raw_buf[page_size];
    std::span<std::byte> buf(raw_buf, page_size);

    seq_gen(seed, buf);
    DHARA_TRY_ABORT(base_t::write(s, buf.data()));
  }

  void do_assert(sector_t s, int seed) {
    const size_t page_size = config.nand.page_size();
    std::byte raw_buf[page_size];
    std::span<std::byte> buf(raw_buf, page_size);

    DHARA_TRY_ABORT(base_t::read(s, raw_buf));

    seq_assert(seed, buf);
  }

  void trim(sector_t s) { DHARA_TRY_ABORT(base_t::trim(s)); }

  void assert_blank(sector_t s) {
    auto res = base_t::find(s);
    assert(res.has_error());
    assert(res.error() == error_t::not_found);
  }

  [[nodiscard]] auto get_head() const {
    return head;
  }
  [[nodiscard]] auto get_tail() const {
    return tail;
  }
  [[nodiscard]] auto get_epoch() const {
    return epoch;
  }
};

template <std::uint8_t log2_page_size_, std::uint8_t log2_ppb_, std::size_t gc_ratio = 4u,
          std::size_t max_retries_ = 8u>
class TestMap : public Map<log2_page_size_, log2_ppb_, gc_ratio, max_retries_,
                           Journal<log2_page_size_, log2_ppb_, MapBase::meta_size,
                                   MapBase::cookie_size, max_retries_, TestMapBase>> {
  using base_t = Map<log2_page_size_, log2_ppb_, gc_ratio, max_retries_,
                     Journal<log2_page_size_, log2_ppb_, MapBase::meta_size, MapBase::cookie_size,
                             max_retries_, TestMapBase>>;

 public:
  using base_t::base_t;
};

}  // namespace dhara_tests

#endif  // DHARA_NAND_FTL_MTUTIL_HPP
