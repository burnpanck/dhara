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
#include "mtutil.hpp"

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
