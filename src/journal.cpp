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

#include <dhara/bytes.hpp>
#include <dhara/journal.hpp>

#include <cassert>
#include <cstring>

namespace dhara {

/************************************************************************
 * Metapage binary format
 */

static constexpr std::size_t header_size = 16u;
using hdr_cbuf_t = std::span<const std::byte, header_size>;
using hdr_buf_t = std::span<std::byte, header_size>;

/* Does the page buffer contain a valid checkpoint page? */
int hdr_has_magic(hdr_cbuf_t buf) {
  return (static_cast<char>(buf[0]) == 'D') && (static_cast<char>(buf[1]) == 'h') &&
         (static_cast<char>(buf[2]) == 'a');
}

void hdr_put_magic(hdr_buf_t buf) {
  buf[0] = static_cast<std::byte>('D');
  buf[1] = static_cast<std::byte>('h');
  buf[2] = static_cast<std::byte>('a');
}

/* What epoch is this page? */
uint8_t hdr_get_epoch(hdr_cbuf_t buf) { return static_cast<std::uint8_t>(buf[3]); }

void hdr_set_epoch(hdr_buf_t buf, uint8_t e) { buf[3] = static_cast<std::byte>(e); }

page_t hdr_get_tail(hdr_cbuf_t buf) { return r32(buf.subspan<4, 4>()); }

void hdr_set_tail(hdr_buf_t buf, page_t tail) { w32(buf.subspan<4, 4>(), tail); }

page_t hdr_get_bb_current(hdr_cbuf_t buf) { return r32(buf.subspan<8, 4>()); }

void hdr_set_bb_current(hdr_buf_t buf, page_t count) { w32(buf.subspan<8, 4>(), count); }

page_t hdr_get_bb_last(hdr_cbuf_t buf) { return r32(buf.subspan<12, 4>()); }

void hdr_set_bb_last(hdr_buf_t buf, page_t count) { w32(buf.subspan<12, 4>(), count); }

/************************************************************************
 * Page geometry helpers
 */

/* Is this page index aligned to N bits? */
bool is_aligned(page_t p, unsigned int n) { return !(p & ((1u << n) - 1u)); }

/* Are these two pages from the same alignment group? */
bool align_eq(page_t a, page_t b, unsigned int n) { return !((a ^ b) >> n); }

/* What is the successor of this block? */
block_t next_block(const NandBase &n, block_t blk) {
  blk++;
  if (blk >= n.num_blocks()) blk = 0;

  return blk;
}

page_t JournalBase::next_upage(page_t p) const noexcept {
  p++;
  if (is_aligned(p + 1, config.log2_ppc)) p++;

  if (p >= (nand.num_blocks() << config.nand.log2_ppb)) p = 0;

  return p;
}

/************************************************************************
 * Journal setup/resume
 */

/* Clear recovery status */
void JournalBase::clear_recovery() noexcept {
  recover_next = page_none;
  recover_root = page_none;
  recover_meta = page_none;
  flags[int(Flag::bad_meta)] = false;
  flags[int(Flag::recovery)] = false;
  flags[int(Flag::enum_done)] = false;
}

/* Set up an empty journal */
void JournalBase::reset_journal() noexcept {
  /* We don't yet have a bad block estimate, so make a
   * conservative guess.
   */
  epoch = 0;
  bb_last = nand.num_blocks() >> 6u;
  bb_current = 0;

  flags = 0;

  /* Empty journal */
  head = 0;
  tail = 0;
  tail_sync = 0;
  root_ = page_none;

  /* No recovery required */
  clear_recovery();

  /* Empty metadata buffer */
  auto page_buf = this->page_buf();
  memset(page_buf.data(), 0xff, page_buf.size_bytes());
}

void JournalBase::roll_stats() noexcept {
  bb_last = bb_current;
  bb_current = 0;
  epoch++;
}

JournalBase::JournalBase(const JournalConfig &config, NandBase &n, byte_buf_t page_buf) noexcept
    : config(config), nand(n), page_buf_ptr(page_buf.data()) {
  assert(page_buf.size() == config.nand.page_size());
  reset_journal();
}

/* Find the first checkpoint-containing block. If a block contains any
 * checkpoints at all, then it must contain one in the first checkpoint
 * location -- otherwise, we would have considered the block eraseable.
 */
Outcome<block_t> JournalBase::find_checkblock(block_t blk) noexcept {
  const std::size_t page_size = nand.page_size();
  const auto page_buf = this->page_buf();

  for (int i = 0; (blk < nand.num_blocks()) && (i < config.max_retries); i++) {
    const page_t p = (blk << nand.log2_ppb()) | ((1u << config.log2_ppc) - 1u);

    if (!(nand.is_bad(blk) || nand.read(p, 0, page_buf).has_error()) &&
        hdr_has_magic(hdr_cbuf_t(page_buf))) {
      return blk;
    }

    blk++;
  }

  return error_t::too_bad;
}

block_t JournalBase::find_last_checkblock(block_t first) noexcept {
  block_t low = first;
  block_t high = nand.num_blocks() - 1;
  const auto page_buf = this->page_buf();

  while (low <= high) {
    const block_t mid = (low + high) >> 1u;
    auto res = find_checkblock(mid);

    if (res.has_error() || (hdr_get_epoch(hdr_cbuf_t(page_buf)) != epoch)) {
      if (!mid) return first;

      high = mid - 1;
    } else {
      block_t found = res.value();

      if ((found + 1) >= nand.num_blocks()) return found;
      auto res2 = find_checkblock(found + 1);
      if (res2.has_error() || (hdr_get_epoch(hdr_cbuf_t(page_buf)) != epoch)) return found;

      low = res2.value();
    }
  }

  return first;
}

/* Test whether a checkpoint group is in a state fit for reprogramming,
 * but allow for the fact that is_free() might not have any way of
 * distinguishing between an unprogrammed page, and a page programmed
 * with all-0xff bytes (but if so, it must be ok to reprogram such a
 * page).
 *
 * We used to test for an unprogrammed checkpoint group by checking to
 * see if the first user-page had been programmed since last erase (by
 * testing only the first page with is_free). This works if is_free is
 * precise, because the pages are written in order.
 *
 * If is_free is imprecise, we need to check all pages in the group.
 * That also works, because the final page in a checkpoint group is
 * guaranteed to contain non-0xff bytes. Therefore, we return 1 only if
 * the group is truly unprogrammed, or if it was partially programmed
 * with some all-0xff user pages (which changes nothing for us).
 */
bool JournalBase::cp_free(page_t first_user) const noexcept {
  const int count = 1u << config.log2_ppc;

  for (int i = 0; i < count; i++)
    if (!nand.is_free(first_user + i)) return false;

  return true;
}

page_t JournalBase::find_last_group(block_t blk) const noexcept {
  const int num_groups = 1u << (config.nand.log2_ppb - config.log2_ppc);
  int low = 0;
  int high = num_groups - 1;

  /* If a checkpoint group is completely unprogrammed, everything
   * following it will be completely unprogrammed also.
   *
   * Therefore, binary search checkpoint groups until we find the
   * last programmed one.
   */
  while (low <= high) {
    int mid = (low + high) >> 1u;
    const page_t p = (mid << config.log2_ppc) | (blk << config.nand.log2_ppb);

    if (cp_free(p)) {
      high = mid - 1;
    } else if (((mid + 1) >= num_groups) || cp_free(p + (1u << config.log2_ppc))) {
      return p;
    } else {
      low = mid + 1;
    }
  }

  return blk << config.nand.log2_ppb;
}

Outcome<void> JournalBase::find_root(page_t start) noexcept {
  const std::uint8_t log2_ppb = config.nand.log2_ppb;
  const block_t blk = start >> log2_ppb;
  const auto page_buf = this->page_buf();
  int i = (start & ((1u << log2_ppb) - 1u)) >> config.log2_ppc;

  while (i >= 0) {
    const page_t p = (blk << log2_ppb) + ((i + 1u) << config.log2_ppc) - 1u;

    if (nand.read(p, 0, page_buf).has_value() && (hdr_has_magic(hdr_cbuf_t(page_buf))) &&
        (hdr_get_epoch(hdr_cbuf_t(page_buf)) == epoch)) {
      root_ = p - 1;
      return error_t::none;
    }

    i--;
  }

  return error_t::too_bad;
}

Outcome<void> JournalBase::find_head(page_t start) noexcept {
  const std::uint8_t log2_ppb = config.nand.log2_ppb;

  head = start;

  /* Starting from the last good checkpoint, find either:
   *
   *   (a) the next free user-page in the same block
   *   (b) or, the first page of the next block
   *
   * The block we end up on might be bad, but that's ok -- we'll
   * skip it when we go to prepare the next write.
   */
  do {
    /* Skip to the next userpage */
    head = next_upage(head);
    if (!head) roll_stats();

    /* If we hit the end of the block, we're done */
    if (is_aligned(head, log2_ppb)) {
      /* Make sure we don't chase over the tail */
      if (align_eq(head, tail, log2_ppb)) tail = next_block(nand, tail >> log2_ppb) << log2_ppb;
      break;
    }
  } while (!cp_free(head));

  return error_t::none;
}

Outcome<void> JournalBase::resume() noexcept {
  const auto page_buf = this->page_buf();

  block_t first, last;
  page_t last_group;

  /* Find the first checkpoint-containing block */
  {
    auto res = find_checkblock(0);
    if (res.has_error()) {
      reset_journal();
      return res.error();
    }
    first = res.value();
  }

  /* Find the last checkpoint-containing block in this epoch */
  epoch = hdr_get_epoch(hdr_cbuf_t(page_buf));
  last = find_last_checkblock(first);

  /* Find the last programmed checkpoint group in the block */
  last_group = find_last_group(last);

  /* Perform a linear scan to find the last good checkpoint (and
   * therefore the root).
   */
  {
    auto res = find_root(last_group);
    if (res.has_error()) {
      reset_journal();
      return res.error();
    }
  }

  /* Restore settings from checkpoint */
  tail = hdr_get_tail(hdr_cbuf_t(page_buf));
  bb_current = hdr_get_bb_current(hdr_cbuf_t(page_buf));
  bb_last = hdr_get_bb_last(hdr_cbuf_t(page_buf));
  hdr_clear_user(page_buf, nand.log2_page_size());

  /* Perform another linear scan to find the next free user page */
  {
    auto res = find_head(last_group);
    if (res.has_error()) {
      reset_journal();
      return res.error();
    }
  }

  flags = 0;
  tail_sync = tail;

  clear_recovery();
  return error_t::none;
}

/**************************************************************************
 * Public interface
 */

std::size_t JournalBase::capacity() const noexcept {
  const block_t max_bad = bb_last > bb_current ? bb_last : bb_current;
  const block_t good_blocks = nand.num_blocks() - max_bad - 1;
  const int log2_cpb = nand.log2_ppb() - config.log2_ppc;
  const page_t good_cps = good_blocks << log2_cpb;

  /* Good checkpoints * (checkpoint period - 1) */
  return (good_cps << config.log2_ppc) - good_cps;
}

std::size_t JournalBase::size() const noexcept {
  /* Find the number of raw pages, and the number of checkpoints
   * between the head and the tail. The difference between the two
   * is the number of user pages (upper limit).
   */
  const std::uint8_t log2_ppc = config.log2_ppc;

  std::size_t num_pages = head;
  std::size_t num_cps = head >> log2_ppc;

  if (head < tail_sync) {
    const std::size_t total_pages = nand.num_blocks() << config.nand.log2_ppb;

    num_pages += total_pages;
    num_cps += total_pages >> log2_ppc;
  }

  num_pages -= tail_sync;
  num_cps -= tail_sync >> log2_ppc;

  return num_pages - num_cps;
}

Outcome<void> JournalBase::read_meta(page_t p, std::byte *buf) noexcept {
  /* Offset of metadata within the metadata page */
  const page_t ppc_mask = (1u << config.log2_ppc) - 1;
  const size_t offset = hdr_user_offset(p & ppc_mask);
  byte_buf_t out_buf(buf, config.meta_size);

  /* Special case: buffered metadata */
  if (align_eq(p, head, config.log2_ppc)) {
    memcpy(out_buf.data(), page_buf_ptr + offset, out_buf.size_bytes());
    return error_t::none;
  }

  /* Special case: incomplete metadata dumped at start of
   * recovery.
   */
  if ((recover_meta != page_none) && align_eq(p, recover_root, config.log2_ppc)) {
    return nand.read(recover_meta, offset, out_buf);
  }

  /* General case: fetch from metadata page for checkpoint group */
  return nand.read(p | ppc_mask, offset, out_buf);
}

page_t JournalBase::peek() noexcept {
  if (head == tail) return page_none;

  if (is_aligned(tail, nand.log2_ppb())) {
    block_t blk = tail >> nand.log2_ppb();
    int i;

    for (i = 0; i < config.max_retries; i++) {
      if ((blk == (head >> nand.log2_ppb())) || !nand.is_bad(blk)) {
        tail = blk << nand.log2_ppb();

        if (tail == head) root_ = page_none;

        return tail;
      }

      blk = next_block(nand, blk);
    }
  }

  return tail;
}

void JournalBase::dequeue() noexcept {
  if (head == tail) return;

  tail = next_upage(tail);

  /* If the journal is clean at the time of dequeue, then this
   * data was always obsolete, and can be reused immediately.
   */
  if (!(flags[int(Flag::dirty)] || flags[int(Flag::recovery)])) tail_sync = tail;

  if (head == tail) root_ = page_none;
}

void JournalBase::clear() noexcept {
  tail = head;
  root_ = page_none;
  flags[int(Flag::dirty)] = true;

  hdr_clear_user(page_buf(), nand.log2_page_size());
}

Outcome<void> JournalBase::skip_block() noexcept {
  const std::uint8_t log2_ppb = config.nand.log2_ppb;
  const block_t next = next_block(nand, head >> log2_ppb);

  /* We can't roll onto the same block as the tail */
  if ((tail_sync >> log2_ppb) == next) {
    return error_t::journal_full;
  }

  head = next << log2_ppb;
  if (!head) roll_stats();

  return error_t::none;
}

/* Make sure the head pointer is on a ready-to-program page. */
Outcome<void> JournalBase::prepare_head() noexcept {
  const std::uint8_t log2_ppb = config.nand.log2_ppb;
  const page_t next = next_upage(head);
  int i;

  /* We can't write if doing so would cause the head pointer to
   * roll onto the same block as the last-synced tail.
   */
  if (align_eq(next, tail_sync, log2_ppb) && !align_eq(next, head, log2_ppb)) {
    return error_t::journal_full;
  }

  flags[int(Flag::dirty)] = true;
  if (!is_aligned(head, log2_ppb)) return error_t::none;

  for (i = 0; i < config.max_retries; i++) {
    const block_t blk = head >> log2_ppb;

    if (!nand.is_bad(blk)) {
      return nand.erase(blk);
    }

    bb_current++;
    DHARA_TRY(skip_block());
  }

  return error_t::too_bad;
}

void JournalBase::restart_recovery(page_t old_head) noexcept {
  const std::uint8_t log2_ppb = config.nand.log2_ppb;
  /* Mark the current head bad immediately, unless we're also
   * using it to hold our dumped metadata (it will then be marked
   * bad at the end of recovery).
   */
  if ((recover_meta == page_none) || !align_eq(recover_meta, old_head, log2_ppb))
    nand.mark_bad(old_head >> log2_ppb);
  else
    flags[int(Flag::bad_meta)] = true;

  /* Start recovery again. Reset the source enumeration to
   * the start of the original bad block, and reset the
   * destination enumeration to the newly found good
   * block.
   */
  flags[int(Flag::enum_done)] = false;
  recover_next = recover_root & ~((1u << log2_ppb) - 1u);

  root_ = recover_root;
}

Outcome<void> JournalBase::dump_meta() noexcept {
  const std::uint8_t log2_page_size = config.nand.log2_page_size;
  const std::uint8_t log2_ppb = config.nand.log2_ppb;
  const auto page_buf = this->page_buf();
  /* We've just begun recovery on a new erasable block, but we
   * have buffered metadata from the failed block.
   */
  for (int i = 0; i < config.max_retries; i++) {
    auto res = [&]() -> Outcome<void> {
      /* Try to dump metadata on this page */
      DHARA_TRY(prepare_head());
      DHARA_TRY(nand.prog(head, page_buf_ptr));
      recover_meta = head;
      head = next_upage(head);
      if (!head) roll_stats();
      hdr_clear_user(page_buf, log2_page_size);
      return error_t::none;
    }();

    /* Report fatal errors */
    if (res.has_error() && res.error() != error_t::bad_block) {
      return res;
    }

    bb_current++;
    nand.mark_bad(head >> log2_ppb);

    DHARA_TRY(skip_block());
  }

  return error_t::too_bad;
}

Outcome<void> JournalBase::recover_from(error_t write_err) noexcept {
  const std::uint8_t log2_ppb = config.nand.log2_ppb;

  const page_t old_head = head;

  if (write_err != error_t::bad_block) {
    return write_err;
  }

  /* Advance to the next free page */
  bb_current++;
  DHARA_TRY(skip_block());

  /* Are we already in the middle of a recovery? */
  if (in_recovery()) {
    restart_recovery(old_head);
    return error_t::recover;
  }

  /* Were we block aligned? No recovery required! */
  if (is_aligned(old_head, log2_ppb)) {
    nand.mark_bad(old_head >> log2_ppb);
    return error_t::none;
  }

  recover_root = root_;
  recover_next = recover_root & ~((1u << log2_ppb) - 1u);

  /* Are we holding buffered metadata? Dump it first. */
  if (!is_aligned(old_head, config.log2_ppc)) {
    DHARA_TRY(dump_meta());
  }

  flags[int(Flag::recovery)] = true;
  return error_t::recover;
}

void JournalBase::finish_recovery() noexcept {
  /* We just recovered the last page. Mark the recovered
   * block as bad.
   */
  nand.mark_bad(recover_root >> config.nand.log2_ppb);

  /* If we had to dump metadata, and the page on which we
   * did this also went bad, mark it bad too.
   */
  if (flags[int(Flag::bad_meta)]) nand.mark_bad(recover_meta >> config.nand.log2_ppb);

  /* Was the tail on this page? Skip it forward */
  clear_recovery();
}

Outcome<void> JournalBase::push_meta(const std::byte *meta) noexcept {
  const page_t old_head = head;
  const size_t offset = hdr_user_offset(head & ((1u << config.log2_ppc) - 1u));
  const auto page_buf = this->page_buf();
  const byte_buf_t meta_buf(page_buf.subspan(offset, config.meta_size));

  /* We've just written a user page. Add the metadata to the
   * buffer.
   */
  if (meta)
    memcpy(meta_buf.data(), meta, meta_buf.size_bytes());
  else
    memset(meta_buf.data(), 0xff, meta_buf.size_bytes());

  /* Unless we've filled the buffer, don't do any IO */
  if (!is_aligned(head + 2, config.log2_ppc)) {
    root_ = head;
    head++;
    return error_t::none;
  }

  /* We don't need to check for immediate recover, because that'll
   * never happen -- we're not block-aligned.
   */
  hdr_buf_t hdr_buf(page_buf);
  hdr_put_magic(hdr_buf);
  hdr_set_epoch(hdr_buf, epoch);
  hdr_set_tail(hdr_buf, tail);
  hdr_set_bb_current(hdr_buf, bb_current);
  hdr_set_bb_last(hdr_buf, bb_last);

  {
    auto res = nand.prog(head + 1, page_buf.data());
    if (res.has_error()) return recover_from(res.error());
  }
  flags[int(Flag::dirty)] = false;

  root_ = old_head;
  head = next_upage(head);

  if (!head) roll_stats();

  if (flags[int(Flag::enum_done)]) finish_recovery();

  if (!flags[int(Flag::recovery)]) tail_sync = tail;

  return error_t::none;
}

Outcome<void> JournalBase::enqueue(const std::byte *data, const std::byte *meta) noexcept {
  for (int i = 0; i < config.max_retries; i++) {
    auto res = [&]() -> Outcome<void> {
      DHARA_TRY(prepare_head());
      if (data) {
        DHARA_TRY(nand.prog(head, data));
      }
      return error_t::none;
    }();
    if (res.has_value()) {
      return push_meta(meta);
    }

    DHARA_TRY(recover_from(res.error()));
  }

  return error_t::too_bad;
}

Outcome<void> JournalBase::copy(page_t p, const std::byte *meta) noexcept {
  for (int i = 0; i < config.max_retries; i++) {
    auto res = [&]() -> Outcome<void> {
      DHARA_TRY(prepare_head());
      DHARA_TRY(nand.copy(p, head));
      return error_t::none;
    }();
    if (res.has_value()) {
      return push_meta(meta);
    }

    DHARA_TRY(recover_from(res.error()));
  }

  return error_t::too_bad;
}

page_t JournalBase::next_recoverable() noexcept {
  const page_t n = recover_next;

  if (!in_recovery()) return page_none;

  if (flags[int(Flag::enum_done)]) return page_none;

  if (recover_next == recover_root)
    flags[int(Flag::enum_done)] = true;
  else
    recover_next = next_upage(recover_next);

  return n;
}

}  // namespace dhara