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
#include "util.hpp"

#include <dhara/journal.hpp>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace std;
using namespace dhara;
using namespace dhara_tests;

template <typename Context>
void suspend_resume(Context &ctxt, TestJournalBase &j) {
  const page_t old_root = j.root();
  const auto old_ends = j.end_pointers();
  error_t err;

  j.clear();
  assert(j.root() == TestJournalBase::page_none);

  DHARA_TRY_ABORT(ctxt.wait_for([&](auto &&op) { j.resume(std::move(op)); }));

  assert(old_root == j.root());
  assert(old_ends == j.end_pointers());
}

StaticSimNand sim_nand;

int main() {
  TestJournal journal(sim_nand);
  HostThreadCtxt ctxt;

  sim_nand.reset();
  sim_nand.inject_bad(20);

  printf("Journal init\n");
  (void)ctxt.wait_for([&](auto &&op) { journal.resume(std::move(op)); });
  journal.dump_info();
  printf("\n");

  printf("Enqueue/dequeue, 100 pages x20\n");
  for (int rep = 0; rep < 20; rep++) {
    int count;

    count = DHARA_TRY_ABORT(
        ctxt.wait_for<int>([&](auto &&op) { journal.enqueue_sequence(0, 100, std::move(op)); }));
    assert(count == 100);

    printf("    size     = %d -> ", journal.size());
    DHARA_TRY_ABORT(
        ctxt.wait_for<void>([&](auto &&op) { journal.dequeue_sequence(0, count, std::move(op)); }));
    printf("%d\n", journal.size());
  }
  printf("\n");

  printf("Journal stats:\n");
  journal.dump_info();
  printf("\n");

  printf("Enqueue/dequeue, ~100 pages x20 (resume)\n");
  for (int rep = 0; rep < 20; rep++) {
    auto cookie = journal.cookie();
    int count;

    cookie[0] = static_cast<byte>(rep);
    count = DHARA_TRY_ABORT(
        ctxt.wait_for<int>([&](auto &&op) { journal.enqueue_sequence(0, 100, std::move(op)); }));
    assert(count == 100);

    while (!journal.is_clean()) {
      const int c = DHARA_TRY_ABORT(ctxt.wait_for<int>(
          [&](auto &&op) { journal.enqueue_sequence(count++, 1, std::move(op)); }));
      assert(c == 1);
    }

    printf("    size     = %d -> ", journal.size());
    suspend_resume(ctxt, journal);
    DHARA_TRY_ABORT(
        ctxt.wait_for<void>([&](auto &&op) { journal.dequeue_sequence(0, count, std::move(op)); }));
    printf("%d\n", journal.size());

    assert(cookie[0] == static_cast<byte>(rep));
  }
  printf("\n");

  printf("Journal stats:\n");
  journal.dump_info();
  printf("\n");

  printf("Async context stats:\n");
  ctxt.dump_stats();
  printf("\n");

  sim_nand.dump();
  return 0;
}
