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

class TestJournalBase : public JournalBase {
 public:
  using JournalBase::JournalBase;

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
  Outcome<void> enqueue(uint32_t id) ;
};

template <std::uint8_t log2_page_size_, std::uint8_t log2_ppb_, std::size_t meta_size_ = 132u,
    std::size_t cookie_size_ = 4u, std::size_t max_retries_ = 8u>
class TestJournal : public Journal<log2_page_size_,log2_ppb_,meta_size_,cookie_size_,max_retries_,TestJournalBase> {
  using base_t = Journal<log2_page_size_,log2_ppb_,meta_size_,cookie_size_,max_retries_,TestJournalBase>;
 public:
  using base_t::base_t;
};
template <std::uint8_t log2_page_size_, std::uint8_t log2_ppb_, typename NBase, std::size_t page_buf_size>
TestJournal(Nand<log2_page_size_,log2_ppb_,NBase> &nand, std::span<std::byte,page_buf_size> page_buf)
-> TestJournal<log2_page_size_, log2_ppb_>;


}  // namespace dhara_tests

#endif
