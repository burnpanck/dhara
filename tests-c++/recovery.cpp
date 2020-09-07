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

#include "sim.hpp"
#include <cassert>
#include <cstdio>

using namespace std;
using namespace dhara;
using namespace dhara_tests;

StaticSimNand sim_nand;

static void run(const char *name, void (*scen)(void)) {
  TestJournal journal(sim_nand);

  printf(
      "========================================"
      "================================\n"
      "%s\n"
      "========================================"
      "================================\n\n",
      name);

  sim_nand.reset();

  /* All tests are tuned for this value */
  assert(journal.config.log2_ppc == 2);

  scen();

  journal.enqueue_sequence(0, 30);
  journal.dequeue_sequence(0, 30);

  sim_nand.dump();
  printf("\n");
}

static void scen_control(void) {}

static void scen_instant_fail(void) { sim_nand.set_failed(0); }

static void scen_after_check(void) { sim_nand.set_timebomb(0, 6); }

static void scen_mid_check(void) { sim_nand.set_timebomb(0, 3); }

static void scen_meta_check(void) { sim_nand.set_timebomb(0, 5); }

static void scen_after_cascade(void) {
  sim_nand.set_timebomb(0, 6);
  sim_nand.set_timebomb(1, 3);
  sim_nand.set_timebomb(2, 3);
}

static void scen_mid_cascade(void) {
  sim_nand.set_timebomb(0, 3);
  sim_nand.set_timebomb(1, 3);
}

static void scen_meta_fail(void) {
  sim_nand.set_timebomb(0, 3);
  sim_nand.set_failed(1);
}

static void scen_bad_day(void) {
  int i;

  sim_nand.set_timebomb(0, 7);
  for (i = 1; i < 5; i++) sim_nand.set_timebomb(i, 3);
}

int main(void) {
  run("Control", scen_control);
  run("Instant fail", scen_instant_fail);

  run("Fail after checkpoint", scen_after_check);
  run("Fail mid-checkpoint", scen_mid_check);
  run("Fail on meta", scen_meta_check);

  run("Cascade fail after checkpoint", scen_after_cascade);
  run("Cascade fail mid-checkpoint", scen_mid_cascade);

  run("Metadata dump failure", scen_meta_fail);

  run("Bad day", scen_bad_day);

  return 0;
}
