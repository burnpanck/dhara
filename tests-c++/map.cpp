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

static int check_recurse(MapBase &m, page_t parent, page_t page, sector_t id_expect,
                         unsigned int depth) {
  uint8_t meta[m.config.meta_size];
  error_t err;
  const page_t h_offset = m.head - m.tail;
  const page_t p_offset = parent - m.tail;
  const page_t offset = page - m.tail;
  sector_t id;
  int count = 1;
  unsigned int i;

  if (page == MapBase::page_none) return 0;

  /* Make sure this is a valid journal user page, and one which is
   * older than the page pointing to it.
   */
  assert(offset < p_offset);
  assert(offset < h_offset);
  assert((~page) & ((1u << m.config.log2_ppc) - 1u));

  /* Fetch metadata */
  if (m.read_meta(page, meta, &err) < 0) dabort("mt_check", err);

  /* Check the first <depth> bits of the ID field */
  id = dhara_r32(meta);
  if (!depth) {
    id_expect = id;
  } else {
    assert(!((id ^ id_expect) >> (32u - depth)));
  }

  /* Check all alt-pointers */
  for (i = depth; i < 32u; i++) {
    page_t child = dhara_r32(meta + (i << 2u) + 4u);

    count += check_recurse(m, page, child, id ^ (1u << (31u - i)), i + 1u);
  }

  return count;
}

static void mt_check(struct dhara_map *m) {
  int count;

  sim_freeze();
  count = check_recurse(m, m->journal.head, dhara_journal_root(&m->journal), 0, 0);
  sim_thaw();

  assert(m->count == count);
}

static void mt_write(struct dhara_map *m, sector_t s, int seed) {
  const size_t page_size = 1 << m->journal.nand->log2_page_size();
  uint8_t buf[page_size];
  error_t err;

  seq_gen(seed, buf, sizeof(buf));
  if (dhara_map_write(m, s, buf, &err) < 0) dabort("map_write", err);
}

static void mt_assert(struct dhara_map *m, sector_t s, int seed) {
  const size_t page_size = 1 << m->journal.nand->log2_page_size();
  uint8_t buf[page_size];
  error_t err;

  if (dhara_map_read(m, s, buf, &err) < 0) dabort("map_read", err);

  seq_assert(seed, buf, sizeof(buf));
}

static void mt_trim(struct dhara_map *m, sector_t s) {
  error_t err;

  if (dhara_map_trim(m, s, &err) < 0) dabort("map_trim", err);
}

static void mt_assert_blank(struct dhara_map *m, sector_t s) {
  error_t err;
  page_t loc;
  int r;

  r = dhara_map_find(m, s, &loc, &err);
  assert(r < 0);
  assert(err == error_t::not_found);
}

int main(void) {
  const size_t page_size = 1 << sim_nand.log2_page_size();
  uint8_t page_buf[page_size];
  struct dhara_map map;
  int i;

  printf("%d\n", (int)sizeof(map));

  sim_reset();
  sim_inject_bad(10);
  sim_inject_timebombs(30, 20);

  printf("Map init\n");
  dhara_map_init(&map, &sim_nand, page_buf, GC_RATIO);
  dhara_map_resume(&map, NULL);
  printf("  capacity: %d\n", dhara_map_capacity(&map));
  printf("  sector count: %d\n", NUM_SECTORS);
  printf("\n");

  printf("Sync...\n");
  dhara_map_sync(&map, NULL);
  printf("Resume...\n");
  dhara_map_init(&map, &sim_nand, page_buf, GC_RATIO);
  dhara_map_resume(&map, NULL);

  printf("Writing sectors...\n");
  shuffle(0);
  for (i = 0; i < NUM_SECTORS; i++) {
    const sector_t s = sector_list[i];

    mt_write(&map, s, s);
    mt_check(&map);
  }

  printf("Sync...\n");
  dhara_map_sync(&map, NULL);
  printf("Resume...\n");
  dhara_map_init(&map, &sim_nand, page_buf, GC_RATIO);
  dhara_map_resume(&map, NULL);
  printf("  capacity: %d\n", dhara_map_capacity(&map));
  printf("  use count: %d\n", dhara_map_size(&map));
  printf("\n");

  printf("Read back...\n");
  shuffle(1);
  for (i = 0; i < NUM_SECTORS; i++) {
    const sector_t s = sector_list[i];

    mt_assert(&map, s, s);
  }

  printf("Rewrite/trim half...\n");
  shuffle(2);
  for (i = 0; i < NUM_SECTORS; i += 2) {
    const sector_t s0 = sector_list[i];
    const sector_t s1 = sector_list[i + 1];

    mt_write(&map, s0, ~s0);
    mt_check(&map);
    mt_trim(&map, s1);
    mt_check(&map);
  }

  printf("Sync...\n");
  dhara_map_sync(&map, NULL);
  printf("Resume...\n");
  dhara_map_init(&map, &sim_nand, page_buf, GC_RATIO);
  dhara_map_resume(&map, NULL);
  printf("  capacity: %d\n", dhara_map_capacity(&map));
  printf("  use count: %d\n", dhara_map_size(&map));
  printf("\n");

  printf("Read back...\n");
  for (i = 0; i < NUM_SECTORS; i += 2) {
    const sector_t s0 = sector_list[i];
    const sector_t s1 = sector_list[i + 1];

    mt_assert(&map, s0, ~s0);
    mt_assert_blank(&map, s1);
  }

  printf("\n");
  sim_dump();
  return 0;
}
