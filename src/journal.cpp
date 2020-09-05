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

#include <cstring>

namespace dhara {

/************************************************************************
 * Metapage binary format
 */

using hdr_cbuf_t = std::span<const std::byte, DHARA_HEADER_SIZE>;
using hdr_buf_t = std::span<std::byte, DHARA_HEADER_SIZE>;

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

/* Clear user metadata */
void hdr_clear_user(std::span<std::byte> buf, uint8_t log2_page_size) {
  auto user = buf.subspan(DHARA_HEADER_SIZE + DHARA_COOKIE_SIZE);
  memset(user.data(), 0xff, user.size_bytes());
}

/* Obtain pointers to user data */
size_t hdr_user_offset(uint8_t which) {
  return DHARA_HEADER_SIZE + DHARA_COOKIE_SIZE + which * DHARA_META_SIZE;
}

/************************************************************************
 * Page geometry helpers
 */

/* Is this page index aligned to N bits? */
int is_aligned(page_t p, int n) { return !(p & ((1 << n) - 1)); }

/* Are these two pages from the same alignment group? */
int align_eq(page_t a, page_t b, int n) { return !((a ^ b) >> n); }

/* What is the successor of this block? */
static block_t next_block(const Nand &n, block_t blk) {
  blk++;
  if (blk >= n.num_blocks()) blk = 0;

  return blk;
}

page_t Journal::next_upage(page_t p) const noexcept {
  p++;
  if (is_aligned(p + 1, this->log2_ppc)) p++;

  if (p >= (this->nand.num_blocks() << this->nand.log2_ppb())) p = 0;

  return p;
}

/* Calculate a checkpoint period: the largest value of ppc such that
 * (2**ppc - 1) metadata blocks can fit on a page with one journal
 * header.
 */
constexpr int choose_ppc(int log2_page_size, int max) {
  const int max_meta = (1 << log2_page_size) - DHARA_HEADER_SIZE - DHARA_COOKIE_SIZE;
  int total_meta = DHARA_META_SIZE;
  int ppc = 1;

  while (ppc < max) {
    total_meta <<= 1;
    total_meta += DHARA_META_SIZE;

    if (total_meta > max_meta) break;

    ppc++;
  }

  return ppc;
}

/************************************************************************
 * Journal setup/resume
 */

/* Clear recovery status */
void Journal::clear_recovery() noexcept {
  this->recover_next = DHARA_PAGE_NONE;
  this->recover_root = DHARA_PAGE_NONE;
  this->recover_meta = DHARA_PAGE_NONE;
  this->flags &= ~(DHARA_JOURNAL_F_BAD_META | DHARA_JOURNAL_F_RECOVERY | DHARA_JOURNAL_F_ENUM_DONE);
}

/* Set up an empty journal */
void Journal::reset_journal() noexcept {
  /* We don't yet have a bad block estimate, so make a
   * conservative guess.
   */
  this->epoch = 0;
  this->bb_last = this->nand.num_blocks() >> 6;
  this->bb_current = 0;

  this->flags = 0;

  /* Empty journal */
  this->head = 0;
  this->tail = 0;
  this->tail_sync = 0;
  this->root_ = DHARA_PAGE_NONE;

  /* No recovery required */
  clear_recovery();

  /* Empty metadata buffer */
  memset(this->page_buf.data(), 0xff, page_buf.size_bytes());
}

void Journal::roll_stats() noexcept {
  this->bb_last = this->bb_current;
  this->bb_current = 0;
  this->epoch++;
}

Journal::Journal(Nand &n, page_buf_t page_buf) noexcept
    : nand(n), page_buf(page_buf), log2_ppc(choose_ppc(nand.log2_page_size(), nand.log2_ppb())) {
  reset_journal();
}

/* Find the first checkpoint-containing block. If a block contains any
 * checkpoints at all, then it must contain one in the first checkpoint
 * location -- otherwise, we would have considered the block eraseable.
 */
int Journal::find_checkblock(block_t blk, block_t *where, error_t *err) const noexcept {
  const std::size_t page_size = this->nand.page_size();
  for (int i = 0; (blk < this->nand.num_blocks()) && (i < DHARA_MAX_RETRIES); i++) {
    const page_t p = (blk << this->nand.log2_ppb()) | ((1 << this->log2_ppc) - 1);

    if (!(this->nand.is_bad(blk) || this->nand.read(p, 0, page_buf, err)) &&
        hdr_has_magic(this->page_buf)) {
      *where = blk;
      return 0;
    }

    blk++;
  }

  set_error(err, error_t::too_bad);
  return -1;
}

block_t Journal::find_last_checkblock(block_t first) const noexcept {
  block_t low = first;
  block_t high = this->nand.num_blocks() - 1;

  while (low <= high) {
    const block_t mid = (low + high) >> 1;
    block_t found;

    if ((find_checkblock(mid, &found, NULL) < 0) ||
        (hdr_get_epoch(this->page_buf) != this->epoch)) {
      if (!mid) return first;

      high = mid - 1;
    } else {
      block_t nf;

      if (((found + 1) >= this->nand.num_blocks()) || (find_checkblock(found + 1, &nf, NULL) < 0) ||
          (hdr_get_epoch(this->page_buf) != this->epoch))
        return found;

      low = nf;
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
int Journal::cp_free(page_t first_user) const noexcept {
  const int count = 1 << this->log2_ppc;
  int i;

  for (i = 0; i < count; i++)
    if (!this->nand.is_free(first_user + i)) return 0;

  return 1;
}

page_t Journal::find_last_group(block_t blk) const noexcept {
  const int num_groups = 1 << (this->nand.log2_ppb() - this->log2_ppc);
  int low = 0;
  int high = num_groups - 1;

  /* If a checkpoint group is completely unprogrammed, everything
   * following it will be completely unprogrammed also.
   *
   * Therefore, binary search checkpoint groups until we find the
   * last programmed one.
   */
  while (low <= high) {
    int mid = (low + high) >> 1;
    const page_t p = (mid << this->log2_ppc) | (blk << this->nand.log2_ppb());

    if (cp_free(p)) {
      high = mid - 1;
    } else if (((mid + 1) >= num_groups) || cp_free(p + (1 << this->log2_ppc))) {
      return p;
    } else {
      low = mid + 1;
    }
  }

  return blk << this->nand.log2_ppb();
}

int Journal::find_root(page_t start, error_t *err) noexcept {
  const std::uint8_t log2_ppb = this->nand.log2_ppb();
  const block_t blk = start >> log2_ppb;
  int i = (start & ((1 << log2_ppb) - 1)) >> this->log2_ppc;

  while (i >= 0) {
    const page_t p = (blk << log2_ppb) + ((i + 1) << this->log2_ppc) - 1;

    if (!this->nand.read(p, 0, page_buf, err) && (hdr_has_magic(this->page_buf)) &&
        (hdr_get_epoch(this->page_buf) == this->epoch)) {
      this->root_ = p - 1;
      return 0;
    }

    i--;
  }

  set_error(err, error_t::too_bad);
  return -1;
}

int Journal::find_head(page_t start, error_t *err) noexcept {
  this->head = start;

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
    this->head = next_upage(this->head);
    if (!this->head) roll_stats();

    /* If we hit the end of the block, we're done */
    if (is_aligned(this->head, this->nand.log2_ppb())) {
      /* Make sure we don't chase over the tail */
      if (align_eq(this->head, this->tail, this->nand.log2_ppb()))
        this->tail = next_block(this->nand, this->tail >> this->nand.log2_ppb())
                     << this->nand.log2_ppb();
      break;
    }
  } while (!cp_free(this->head));

  return 0;
}

int Journal::resume(error_t *err) noexcept {
  block_t first, last;
  page_t last_group;

  /* Find the first checkpoint-containing block */
  if (find_checkblock(0, &first, err) < 0) {
    reset_journal();
    return -1;
  }

  /* Find the last checkpoint-containing block in this epoch */
  this->epoch = hdr_get_epoch(this->page_buf);
  last = find_last_checkblock(first);

  /* Find the last programmed checkpoint group in the block */
  last_group = find_last_group(last);

  /* Perform a linear scan to find the last good checkpoint (and
   * therefore the root).
   */
  if (find_root(last_group, err) < 0) {
    reset_journal();
    return -1;
  }

  /* Restore settings from checkpoint */
  this->tail = hdr_get_tail(this->page_buf);
  this->bb_current = hdr_get_bb_current(this->page_buf);
  this->bb_last = hdr_get_bb_last(this->page_buf);
  hdr_clear_user(this->page_buf, this->nand.log2_page_size());

  /* Perform another linear scan to find the next free user page */
  if (find_head(last_group, err) < 0) {
    reset_journal();
    return -1;
  }

  this->flags = 0;
  this->tail_sync = this->tail;

  clear_recovery();
  return 0;
}

/**************************************************************************
 * Public interface
 */

page_t Journal::capacity() const noexcept {
  const block_t max_bad = this->bb_last > this->bb_current ? this->bb_last : this->bb_current;
  const block_t good_blocks = this->nand.num_blocks() - max_bad - 1;
  const int log2_cpb = this->nand.log2_ppb() - this->log2_ppc;
  const page_t good_cps = good_blocks << log2_cpb;

  /* Good checkpoints * (checkpoint period - 1) */
  return (good_cps << this->log2_ppc) - good_cps;
}

page_t Journal::size() const noexcept {
  /* Find the number of raw pages, and the number of checkpoints
   * between the head and the tail. The difference between the two
   * is the number of user pages (upper limit).
   */
  page_t num_pages = this->head;
  page_t num_cps = this->head >> this->log2_ppc;

  if (this->head < this->tail_sync) {
    const page_t total_pages = this->nand.num_blocks() << this->nand.log2_ppb();

    num_pages += total_pages;
    num_cps += total_pages >> this->log2_ppc;
  }

  num_pages -= this->tail_sync;
  num_cps -= this->tail_sync >> this->log2_ppc;

  return num_pages - num_cps;
}

int Journal::read_meta(page_t p, meta_buf_t buf, error_t *err) noexcept {
  /* Offset of metadata within the metadata page */
  const page_t ppc_mask = (1 << this->log2_ppc) - 1;
  const size_t offset = hdr_user_offset(p & ppc_mask);
  meta_cbuf_t meta_buf = page_buf.subspan(offset, meta_size);

  /* Special case: buffered metadata */
  if (align_eq(p, this->head, this->log2_ppc)) {
    memcpy(buf.data(), meta_buf.data(), meta_buf.size_bytes());
    return 0;
  }

  /* Special case: incomplete metadata dumped at start of
   * recovery.
   */
  if ((this->recover_meta != DHARA_PAGE_NONE) && align_eq(p, this->recover_root, this->log2_ppc))
    return this->nand.read(this->recover_meta, offset, buf, err);

  /* General case: fetch from metadata page for checkpoint group */
  return this->nand.read(p | ppc_mask, offset, buf, err);
}

page_t Journal::peek() noexcept {
  if (this->head == this->tail) return DHARA_PAGE_NONE;

  if (is_aligned(this->tail, this->nand.log2_ppb())) {
    block_t blk = this->tail >> this->nand.log2_ppb();
    int i;

    for (i = 0; i < DHARA_MAX_RETRIES; i++) {
      if ((blk == (this->head >> this->nand.log2_ppb())) || !this->nand.is_bad(blk)) {
        this->tail = blk << this->nand.log2_ppb();

        if (this->tail == this->head) this->root_ = DHARA_PAGE_NONE;

        return this->tail;
      }

      blk = next_block(this->nand, blk);
    }
  }

  return this->tail;
}

void Journal::dequeue() noexcept {
  if (this->head == this->tail) return;

  this->tail = next_upage(this->tail);

  /* If the journal is clean at the time of dequeue, then this
   * data was always obsolete, and can be reused immediately.
   */
  if (!(this->flags & (DHARA_JOURNAL_F_DIRTY | DHARA_JOURNAL_F_RECOVERY)))
    this->tail_sync = this->tail;

  if (this->head == this->tail) this->root_ = DHARA_PAGE_NONE;
}

void Journal::clear() noexcept {
  this->tail = this->head;
  this->root_ = DHARA_PAGE_NONE;
  this->flags |= DHARA_JOURNAL_F_DIRTY;

  hdr_clear_user(this->page_buf, this->nand.log2_page_size());
}

int Journal::skip_block(error_t *err) noexcept {
  const block_t next = next_block(this->nand, this->head >> this->nand.log2_ppb());

  /* We can't roll onto the same block as the tail */
  if ((this->tail_sync >> this->nand.log2_ppb()) == next) {
    set_error(err, error_t::journal_full);
    return -1;
  }

  this->head = next << this->nand.log2_ppb();
  if (!this->head) roll_stats();

  return 0;
}

/* Make sure the head pointer is on a ready-to-program page. */
int Journal::prepare_head(error_t *err) noexcept {
  const page_t next = next_upage(this->head);
  int i;

  /* We can't write if doing so would cause the head pointer to
   * roll onto the same block as the last-synced tail.
   */
  if (align_eq(next, this->tail_sync, this->nand.log2_ppb()) &&
      !align_eq(next, this->head, this->nand.log2_ppb())) {
    set_error(err, error_t::journal_full);
    return -1;
  }

  this->flags |= DHARA_JOURNAL_F_DIRTY;
  if (!is_aligned(this->head, this->nand.log2_ppb())) return 0;

  for (i = 0; i < DHARA_MAX_RETRIES; i++) {
    const block_t blk = this->head >> this->nand.log2_ppb();

    if (!this->nand.is_bad(blk)) return this->nand.erase(blk, err);

    this->bb_current++;
    if (skip_block(err) < 0) return -1;
  }

  set_error(err, error_t::too_bad);
  return -1;
}

void Journal::restart_recovery(page_t old_head) noexcept {
  /* Mark the current head bad immediately, unless we're also
   * using it to hold our dumped metadata (it will then be marked
   * bad at the end of recovery).
   */
  if ((this->recover_meta == DHARA_PAGE_NONE) ||
      !align_eq(this->recover_meta, old_head, this->nand.log2_ppb()))
    this->nand.mark_bad(old_head >> this->nand.log2_ppb());
  else
    this->flags |= DHARA_JOURNAL_F_BAD_META;

  /* Start recovery again. Reset the source enumeration to
   * the start of the original bad block, and reset the
   * destination enumeration to the newly found good
   * block.
   */
  this->flags &= ~DHARA_JOURNAL_F_ENUM_DONE;
  this->recover_next = this->recover_root & ~((1 << this->nand.log2_ppb()) - 1);

  this->root_ = this->recover_root;
}

int Journal::dump_meta(error_t *err) noexcept {
  const std::uint8_t log2_page_size = this->nand.log2_page_size();
  const std::uint8_t log2_ppb = this->nand.log2_ppb();
  /* We've just begun recovery on a new erasable block, but we
   * have buffered metadata from the failed block.
   */
  for (int i = 0; i < DHARA_MAX_RETRIES; i++) {
    error_t my_err;

    /* Try to dump metadata on this page */
    if (!(prepare_head(&my_err) || this->nand.prog(this->head, page_buf, &my_err))) {
      this->recover_meta = this->head;
      this->head = next_upage(this->head);
      if (!this->head) roll_stats();
      hdr_clear_user(this->page_buf, log2_page_size);
      return 0;
    }

    /* Report fatal errors */
    if (my_err != error_t::bad_block) {
      set_error(err, my_err);
      return -1;
    }

    this->bb_current++;
    this->nand.mark_bad(this->head >> log2_ppb);

    if (skip_block(err) < 0) return -1;
  }

  set_error(err, error_t::too_bad);
  return -1;
}

int Journal::recover_from(error_t write_err, error_t *err) noexcept {
  const page_t old_head = this->head;

  if (write_err != error_t::bad_block) {
    set_error(err, write_err);
    return -1;
  }

  /* Advance to the next free page */
  this->bb_current++;
  if (skip_block(err) < 0) return -1;

  /* Are we already in the middle of a recovery? */
  if (in_recovery()) {
    restart_recovery(old_head);
    set_error(err, error_t::recover);
    return -1;
  }

  /* Were we block aligned? No recovery required! */
  if (is_aligned(old_head, this->nand.log2_ppb())) {
    this->nand.mark_bad(old_head >> this->nand.log2_ppb());
    return 0;
  }

  this->recover_root = this->root_;
  this->recover_next = this->recover_root & ~((1 << this->nand.log2_ppb()) - 1);

  /* Are we holding buffered metadata? Dump it first. */
  if (!is_aligned(old_head, this->log2_ppc) && dump_meta(err) < 0) return -1;

  this->flags |= DHARA_JOURNAL_F_RECOVERY;
  set_error(err, error_t::recover);
  return -1;
}

void Journal::finish_recovery() noexcept {
  /* We just recovered the last page. Mark the recovered
   * block as bad.
   */
  this->nand.mark_bad(this->recover_root >> this->nand.log2_ppb());

  /* If we had to dump metadata, and the page on which we
   * did this also went bad, mark it bad too.
   */
  if (this->flags & DHARA_JOURNAL_F_BAD_META)
    this->nand.mark_bad(this->recover_meta >> this->nand.log2_ppb());

  /* Was the tail on this page? Skip it forward */
  clear_recovery();
}

int Journal::push_meta(const std::byte *meta, error_t *err) noexcept {
  const page_t old_head = this->head;
  error_t my_err;
  const size_t offset = hdr_user_offset(this->head & ((1 << this->log2_ppc) - 1));
  meta_buf_t meta_buf = page_buf.subspan(offset, meta_size);

  /* We've just written a user page. Add the metadata to the
   * buffer.
   */
  if (meta)
    memcpy(meta_buf.data(), meta, meta_buf.size_bytes());
  else
    memset(meta_buf.data(), 0xff, meta_buf.size_bytes());

  /* Unless we've filled the buffer, don't do any IO */
  if (!is_aligned(this->head + 2, this->log2_ppc)) {
    this->root_ = this->head;
    this->head++;
    return 0;
  }

  /* We don't need to check for immediate recover, because that'll
   * never happen -- we're not block-aligned.
   */
  hdr_put_magic(this->page_buf);
  hdr_set_epoch(this->page_buf, this->epoch);
  hdr_set_tail(this->page_buf, this->tail);
  hdr_set_bb_current(this->page_buf, this->bb_current);
  hdr_set_bb_last(this->page_buf, this->bb_last);

  if (this->nand.prog(this->head + 1, this->page_buf, &my_err) < 0)
    return recover_from(my_err, err);

  this->flags &= ~DHARA_JOURNAL_F_DIRTY;

  this->root_ = old_head;
  this->head = next_upage(this->head);

  if (!this->head) roll_stats();

  if (this->flags & DHARA_JOURNAL_F_ENUM_DONE) finish_recovery();

  if (!(this->flags & DHARA_JOURNAL_F_RECOVERY)) this->tail_sync = this->tail;

  return 0;
}

int Journal::enqueue(const std::byte *data, const std::byte *meta, error_t *err) noexcept {
  error_t my_err;
  int i;

  for (i = 0; i < DHARA_MAX_RETRIES; i++) {
    if (!(prepare_head(&my_err) ||
          (data && this->nand.prog(this->head, {data, this->nand.page_size()}, &my_err))))
      return push_meta(meta, err);

    if (recover_from(my_err, err) < 0) return -1;
  }

  set_error(err, error_t::too_bad);
  return -1;
}

int Journal::copy(page_t p, const std::byte *meta, error_t *err) noexcept {
  error_t my_err;
  int i;

  for (i = 0; i < DHARA_MAX_RETRIES; i++) {
    if (!(prepare_head(&my_err) || this->nand.copy(p, this->head, &my_err)))
      return push_meta(meta, err);

    if (recover_from(my_err, err) < 0) return -1;
  }

  set_error(err, error_t::too_bad);
  return -1;
}

page_t Journal::next_recoverable() noexcept {
  const page_t n = this->recover_next;

  if (!in_recovery()) return DHARA_PAGE_NONE;

  if (this->flags & DHARA_JOURNAL_F_ENUM_DONE) return DHARA_PAGE_NONE;

  if (this->recover_next == this->recover_root)
    this->flags |= DHARA_JOURNAL_F_ENUM_DONE;
  else
    this->recover_next = next_upage(this->recover_next);

  return n;
}

}  // namespace dhara