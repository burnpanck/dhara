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

void TestJournal::check_upage(page_t p) const {
  const page_t mask = (1 << log2_ppc) - 1;

  assert((~p) & mask);
  assert(p < (nand.num_blocks() << nand.log2_ppb()));
}

void TestJournal::check() const {
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
    assert(root_ == DHARA_PAGE_NONE);
  }
}

void TestJournal::recover() {
  int retry_count = 0;

  printf("    recover: start\n");

  while (in_recovery()) {
    const page_t p = next_recoverable();
    error_t err;
    int ret;

    check();

    if (p == DHARA_PAGE_NONE) {
      ret = Journal::enqueue(nullptr, nullptr, &err);
    } else {
      std::byte meta[DHARA_META_SIZE];

      if (read_meta(p, meta, &err) < 0) dabort("read_meta", err);

      ret = copy(p, meta, &err);
    }

    check();

    if (ret < 0) {
      if (err == error_t::recover) {
        printf("    recover: restart\n");
        if (++retry_count >= DHARA_MAX_RETRIES) dabort("recover", error_t::too_bad);
        continue;
      }

      dabort("copy", err);
    }
  }

  check();
  printf("    recover: complete\n");
}

int TestJournal::enqueue(uint32_t id, error_t *err) {
  const std::size_t page_size = 1 << nand.log2_page_size();
  std::byte r[page_size];
  std::byte meta[DHARA_META_SIZE];
  error_t my_err;

  seq_gen(id, {r, page_size});
  dhara_w32(meta, id);

  for (int i = 0; i < DHARA_MAX_RETRIES; i++) {
    check();
    if (!Journal::enqueue(r, meta, &my_err)) return 0;

    if (my_err != error_t::recover) {
      set_error(err, my_err);
      return -1;
    }

    recover();
  }

  set_error(err, error_t::too_bad);
  return -1;
}

int TestJournal::enqueue_sequence(int start, int count) {
  if (count < 0) count = nand.num_blocks() << nand.log2_ppb();

  for (int i = 0; i < count; i++) {
    std::byte meta[DHARA_META_SIZE];
    page_t root;
    error_t err;

    if (enqueue(start + i, &err) < 0) {
      if (err == error_t::journal_full) return i;

      dabort("enqueue", err);
    }

    assert(size() >= i);
    root = this->root();

    if (read_meta(root, meta, &err) < 0) dabort("read_meta", err);
    auto t = dhara_r32(meta);
    assert(t == start + i);
    assert(dhara_r32(meta) == start + i);
  }

  return count;
}

void TestJournal::dequeue_sequence(int next, int count) {
  const int max_garbage = 1 << log2_ppc;
  int garbage_count = 0;

  while (count > 0) {
    std::byte meta[DHARA_META_SIZE];
    uint32_t id;
    page_t tail = peek();
    error_t err;

    assert(tail != DHARA_PAGE_NONE);

    check();
    if (read_meta(tail, meta, &err) < 0) dabort("read_meta", err);

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

      Outcome<void> res(error_t::none);
      if ((res = nand.read(tail, 0, {r, page_size})).has_error()) dabort("nand_read", res.error());

      seq_assert(id, {r, page_size});
    }
  }

  check();
}

void TestJournal::dump_info() const {
  printf("    log2_ppc   = %d\n", log2_ppc);
  printf("    size       = %d\n", size());
  printf("    capacity   = %d\n", capacity());
  printf("    bb_current = %d\n", bb_current);
  printf("    bb_last    = %d\n", bb_last);
}

}  // namespace dhara_tests