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

#include <dhara/bytes.hpp>

#include <cassert>
#include <cstdio>
#include <cstdlib>

using namespace std;

namespace dhara_tests {

void TestJournalBase::check_upage(page_t p) const {
  const page_t mask = (1 << config.log2_ppc) - 1;

  assert((~p) & mask);
  assert(p < (nand.num_blocks() << nand.log2_ppb()));
}

void TestJournalBase::check() const {
  /* Head and tail pointers always point to a valid user-page
   * index (never a meta-page, and never out-of-bounds).
   */
  check_upage(head);
  check_upage(tail);
  check_upage(tail_sync);

  /* The head never advances forward onto the same block as the
   * tail.
   */
  if (!((head ^ tail_sync) >> nand.log2_ppb())) {
    assert(head >= tail_sync);
  }

  /* The current tail is always between the head and the
   * synchronized tail.
   */
  assert((head - tail_sync) >= (tail - tail_sync));

  /* The root always points to a valid user page in a non-empty
   * journal.
   */
  if (head != tail) {
    const page_t raw_size = head - tail;
    const page_t root_offset = root_ - tail;

    check_upage(root_);
    assert(root_offset < raw_size);
  } else {
    assert(root_ == page_none);
  }
}

void TestJournalBase::recover() {
  int retry_count = 0;

  printf("    recover: start\n");

  while (in_recovery()) {
    const page_t p = next_recoverable();
    check();

    auto res = [&]() {
      if (p == page_none) {
        return JournalBase::enqueue(nullptr, nullptr);
      } else {
        std::byte meta[config.meta_size];

        DHARA_TRY_ABORT(read_meta(p, meta));

        return copy(p, meta);
      }
    }();

    check();

    if (res.has_error()) {
      if (res.error() == error_t::recover) {
        printf("    recover: restart\n");
        if (++retry_count >= config.max_retries) dabort("recover", error_t::too_bad);
        continue;
      }

      dabort("copy", res.error());
    }
  }

  check();
  printf("    recover: complete\n");
}

Outcome<void> TestJournalBase::enqueue(uint32_t id) {
  const std::size_t page_size = 1u << config.nand.log2_page_size;
  std::byte r[page_size];
  std::byte meta[config.meta_size];
  error_t my_err;

  seq_gen(id, {r, page_size});
  dhara_w32(meta, id);

  for (int i = 0; i < config.max_retries; i++) {
    check();
    auto res = JournalBase::enqueue(r, meta);
      if(res.has_value())  return res;

    if (res.error() != error_t::recover) {
      return res;
    }

    recover();
  }

  return error_t::too_bad;
}

int TestJournalBase::enqueue_sequence(int start, int count) {
  if (count < 0) count = nand.num_blocks() << nand.log2_ppb();

  for (int i = 0; i < count; i++) {
    std::byte meta[config.meta_size];

    {
      auto res = enqueue(start + i);
      if (res.has_error()) {
        if (res.error() == error_t::journal_full) return i;

        dabort("enqueue", res.error());
      }
    }

    assert(size() >= i);

    DHARA_TRY_ABORT(read_meta(this->root(), meta));
    auto t = dhara_r32(meta);
    assert(t == start + i);
    assert(dhara_r32(meta) == start + i);
  }

  return count;
}

void TestJournalBase::dequeue_sequence(int next, int count) {
  const int max_garbage = 1u << config.log2_ppc;
  int garbage_count = 0;

  while (count > 0) {
    std::byte meta[config.meta_size];
    uint32_t id;
    page_t tail = peek();

    assert(tail != page_none);

    check();
    DHARA_TRY_ABORT(read_meta(tail, meta));

    check();
    dequeue();
    id = dhara_r32(meta);

    if (id == 0xffffffff) {
      garbage_count++;
      assert(garbage_count < max_garbage);
    } else {
      const std::size_t page_size = nand.page_size();
      std::byte r[page_size];

      assert(id == next);
      garbage_count = 0;
      next++;
      count--;

      DHARA_TRY_ABORT(nand.read(tail, 0, {r, page_size}));

      seq_assert(id, {r, page_size});
    }
  }

  check();
}

void TestJournalBase::dump_info() const {
  printf("    log2_ppc   = %d\n", config.log2_ppc);
  printf("    size       = %lu\n", size());
  printf("    capacity   = %lu\n", capacity());
  printf("    bb_current = %d\n", bb_current);
  printf("    bb_last    = %d\n", bb_last);
}

}  // namespace dhara_tests