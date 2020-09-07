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

#include "jtutil.hpp"

#include "dhara/bytes.hpp"
#include "dhara/journal.hpp"
#include "sim.hpp"
#include "util.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace std;
using namespace dhara;
using namespace dhara_tests;

StaticSimNand sim_nand;

int main(void) {
  TestJournal journal(sim_nand);

  int rep;

  sim_nand.reset();
  sim_nand.inject_bad(10);
  sim_nand.inject_failed(10);

  printf("Journal init\n");
  printf("    capacity: %d\n", journal.capacity());
  printf("\n");

  for (rep = 0; rep < 5; rep++) {
    int count;

    printf("Rep: %d\n", rep);

    printf("    enqueue until error...\n");
    count = journal.enqueue_sequence(0, -1);
    printf("    enqueue count: %d\n", count);
    printf("    size: %d\n", journal.size());

    printf("    dequeue...\n");
    journal.dequeue_sequence(0, count);
    printf("    size: %d\n", journal.size());

    /* Only way to recover space here... */
    journal.do_tail_sync();
  }

  printf("\n");
  sim_nand.dump();
  return 0;
}
