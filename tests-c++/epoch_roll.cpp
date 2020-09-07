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

#include "dhara/map.hpp"

#include <cassert>
#include <cstdio>

#define GC_RATIO 4

using namespace std;
using namespace dhara;
using namespace dhara_tests;

static StaticSimNand sim_nand;

int main(void) {
  TestMap<sim_nand.config.log2_page_size, sim_nand.config.log2_ppb> map(sim_nand);

  int write_seed = 0;
  int i;

  sim_nand.reset();
  map.init();
  map.resume();
  printf("resumed, head = %d\n", map.get_head());

  /* Write pages until we have just barely wrapped around, but not
   * yet hit a checkpoint.
   */
  for (i = 0; i < 200; i++) map.write(i, write_seed++);
  printf("written a little, head = %d\n", map.get_head());

  for (i = 0; i < 200; i++) map.write(i, write_seed++);
  printf("written a little, head = %d\n", map.get_head());
  for (i = 0; i < 200; i++) map.write(i, write_seed++);
  printf("written a little, head = %d\n", map.get_head());
  for (i = 0; i < 79; i++) map.write(i, write_seed++);
  printf("written a little, head = %d\n", map.get_head());
  assert(map.get_head() == 1); /* Required for this test */

  /* Now, see what happens on resume if we don't sync.
   *
   * Here's where a bug occured: the new epoch counter was not
   * incremented when finding the next free user page, if that
   * procedure required wrapping around the end of the chip from
   * the last checkblock. From this point on, new pages written
   * are potentially lost, because they will be wrongly identified
   * as older than the pages coming physically later in the chip.
   */
  printf("before resume: head = %d, tail = %d, epoch = %d\n", map.get_head(), map.get_tail(),
         map.get_epoch());
  map.resume();
  printf("resumed, head = %d, tail = %d, epoch = %d\n", map.get_head(), map.get_tail(),
         map.get_epoch());

  for (i = 0; i < 2; i++) map.write(i, i + 10000);
  printf("written new data, head = %d\n", map.get_head());
  map.sync();

  /* Try another resume */
  printf("--------------------------------------------------------\n");
  printf("before resume: head = %d, tail = %d, epoch = %d\n", map.get_head(), map.get_tail(),
         map.get_epoch());
  map.do_assert(0, 10000);
  map.do_assert(1, 10001);
  map.resume();
  printf("resumed, head = %d, tail = %d, epoch = %d\n", map.get_head(), map.get_tail(),
         map.get_epoch());
  map.do_assert(0, 10000);
  map.do_assert(1, 10001);

  return 0;
}
