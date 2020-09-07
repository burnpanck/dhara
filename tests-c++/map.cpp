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

#include "sim.hpp"
#include "util.hpp"

#include <dhara/bytes.hpp>
#include <dhara/map.hpp>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_SECTORS 200
#define GC_RATIO 4

using namespace std;
using namespace dhara;
using namespace dhara_tests;

static StaticSimNand sim_nand;
static sector_t sector_list[NUM_SECTORS];

static void shuffle(int seed) {
  int i;

  srand(seed);
  for (i = 0; i < NUM_SECTORS; i++) sector_list[i] = i;

  for (i = NUM_SECTORS - 1; i > 0; i--) {
    const int j = rand() % i;
    const int tmp = sector_list[i];

    sector_list[i] = sector_list[j];
    sector_list[j] = tmp;
  }
}

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
    int count;

    sim_nand.freeze();
    count = check_recurse(head, root(), 0, 0);
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

int main(void) {
  TestMap<sim_nand.config.log2_page_size, sim_nand.config.log2_ppb> map(sim_nand);
  int i;

  printf("%d\n", (int)sizeof(map));

  sim_nand.reset();
  sim_nand.inject_bad(10);
  sim_nand.inject_timebombs(30, 20);

  printf("Map init\n");
  map.init();
  (void)map.resume();
  printf("  capacity: %d\n", map.capacity());
  printf("  sector count: %d\n", NUM_SECTORS);
  printf("\n");

  printf("Sync...\n");
  (void)map.sync();
  printf("Resume...\n");
  map.init();
  (void)map.resume();

  printf("Writing sectors...\n");
  shuffle(0);
  for (i = 0; i < NUM_SECTORS; i++) {
    const sector_t s = sector_list[i];

    map.write(s, s);
    map.check();
  }

  printf("Sync...\n");
  (void)map.sync();
  printf("Resume...\n");
  map.init();
  (void)map.resume();
  printf("  capacity: %d\n", map.capacity());
  printf("  use count: %d\n", map.size());
  printf("\n");

  printf("Read back...\n");
  shuffle(1);
  for (i = 0; i < NUM_SECTORS; i++) {
    const sector_t s = sector_list[i];

    map.do_assert(s, s);
  }

  printf("Rewrite/trim half...\n");
  shuffle(2);
  for (i = 0; i < NUM_SECTORS; i += 2) {
    const sector_t s0 = sector_list[i];
    const sector_t s1 = sector_list[i + 1];

    map.write(s0, ~s0);
    map.check();
    map.trim(s1);
    map.check();
  }

  printf("Sync...\n");
  (void)map.sync();
  printf("Resume...\n");
  map.init();
  (void)map.resume();
  printf("  capacity: %d\n", map.capacity());
  printf("  use count: %d\n", map.size());
  printf("\n");

  printf("Read back...\n");
  for (i = 0; i < NUM_SECTORS; i += 2) {
    const sector_t s0 = sector_list[i];
    const sector_t s1 = sector_list[i + 1];

    map.do_assert(s0, ~s0);
    map.assert_blank(s1);
  }

  printf("\n");
  sim_nand.dump();
  return 0;
}
