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

#ifndef TESTS_JTUTIL_H_
#define TESTS_JTUTIL_H_

#include <dhara/journal.hpp>

namespace dhara_tests {

using namespace dhara;

class TestJournal : public Journal {
 public:
  using Journal::Journal;

  /* Check the journal's invariants */
  void check() const;

  /* Try to enqueue a sequence of seed/payload pages, and return the
   * number successfully enqueued. Recovery is handled automatically, and
   * all other errors except E_JOURNAL_FULL are fatal.
   */
  int enqueue_sequence(int start, int count);

  /* Dequeue a sequence of seed/payload pages. Make sure there's not too
   * much garbage, and that we get the non-garbage pages in the expected
   * order.
   */
  void dequeue_sequence(int start, int count);

  [[nodiscard]] std::pair<page_t, page_t> end_pointers() const { return {tail, head}; }

  void dump_info() const;

 private:
  void check_upage(page_t p) const;
  void recover();
  int enqueue(uint32_t id, error_t *err);
};

}  // namespace dhara_tests

#endif
