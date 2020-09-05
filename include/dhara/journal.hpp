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

#ifndef DHARA_JOURNAL_H_
#define DHARA_JOURNAL_H_

#include <dhara/nand.hpp>

#include <cstdint>
#include <span>

namespace dhara {

/* Number of bytes used by the journal checkpoint header. */
#define DHARA_HEADER_SIZE 16

/* Global metadata available for a higher layer. This metadata is
 * persistent once the journal reaches a checkpoint, and is restored on
 * startup.
 */
#define DHARA_COOKIE_SIZE 4

/* This is the size of the metadata slice which accompanies each written
 * page. This is independent of the underlying page/OOB size.
 */
#define DHARA_META_SIZE 132

/* When a block fails, or garbage is encountered, we try again on the
 * next block/checkpoint. We can do this up to the given number of
 * times.
 */
#define DHARA_MAX_RETRIES 8

/* This is a page number which can be used to represent "no such page".
 * It's guaranteed to never be a valid user page.
 */
#define DHARA_PAGE_NONE ((page_t)0xffffffff)

/* State flags */
#define DHARA_JOURNAL_F_DIRTY 0x01
#define DHARA_JOURNAL_F_BAD_META 0x02
#define DHARA_JOURNAL_F_RECOVERY 0x04
#define DHARA_JOURNAL_F_ENUM_DONE 0x08

/* The journal layer presents the NAND pages as a double-ended queue.
 * Pages, with associated metadata may be pushed onto the end of the
 * queue, and pages may be popped from the end.
 *
 * Block erase, metadata storage are handled automatically. Bad blocks
 * are handled by relocating data to the next available non-bad page in
 * the sequence.
 *
 * It's up to the user to ensure that the queue doesn't grow beyond the
 * capacity of the NAND chip, but helper functions are provided to
 * assist with this. If the head meets the tail, the journal will refuse
 * to enqueue more pages.
 */
class Journal {
 public:
  static constexpr std::size_t meta_size = DHARA_META_SIZE;
  static constexpr std::size_t cookie_size = DHARA_COOKIE_SIZE;

  using page_buf_t = std::span<std::byte>;
  using page_cbuf_t = std::span<const std::byte>;
  using meta_buf_t = std::span<std::byte, meta_size>;
  using meta_cbuf_t = std::span<const std::byte, meta_size>;
  using cookie_buf_t = std::span<std::byte, cookie_size>;
  using cookie_cbuf_t = std::span<const std::byte, cookie_size>;

  /* Initialize a journal. You must supply a pointer to a NAND chip
   * driver, and a single page buffer. This page buffer will be used
   * exclusively by the journal, but you are responsible for allocating
   * it, and freeing it (if necessary) at the end.
   *
   * No NAND operations are performed at this point.
   */
  Journal(Nand &n, page_buf_t page_buf) noexcept;

  /* Start up the journal -- search the NAND for the journal head, or
   * initialize a blank journal if one isn't found. Returns 0 on success
   * or -1 if a (fatal) error occurs.
   *
   * This operation is O(log N), where N is the number of pages in the
   * NAND chip. All other operations are O(1).
   *
   * If this operation fails, the journal will be reset to an empty state.
   */
  int resume(error_t *err) noexcept;

  /* Obtain an upper bound on the number of user pages storable in the
   * journal.
   */
  page_t capacity() const noexcept;

  /* Obtain an upper bound on the number of user pages consumed by the
   * journal.
   */
  page_t size() const noexcept;

  /* Obtain a pointer to the cookie data */
  cookie_cbuf_t cookie() const noexcept { return page_buf.subspan(DHARA_HEADER_SIZE, cookie_size); }
  /* Obtain a pointer to the cookie data */
  cookie_buf_t cookie() noexcept { return page_buf.subspan(DHARA_HEADER_SIZE, cookie_size); }

  /* Obtain the locations of the first and last pages in the journal.
   */
  page_t root() const noexcept { return root_; }

  /* Read metadata associated with a page. This assumes that the page
   * provided is a valid data page. The actual page data is read via the
   * normal NAND interface.
   */
  int read_meta(page_t p, meta_buf_t buf, error_t *err) noexcept;

  /* Advance the tail to the next non-bad block and return the page that's
   * ready to read. If no page is ready, return DHARA_PAGE_NONE.
   */
  page_t peek() noexcept;

  /* Remove the last page from the journal. This doesn't take permanent
   * effect until the next checkpoint.
   */
  void dequeue() noexcept;

  /* Remove all pages form the journal. This doesn't take permanent effect
   * until the next checkpoint.
   */
  void clear() noexcept;

  /* Append a page to the journal. Both raw page data and metadata must be
   * specified. The push operation is not persistent until a checkpoint is
   * reached.
   *
   * This operation may fail with the error code E_RECOVER. If this
   * occurs, the upper layer must complete the assisted recovery procedure
   * and then try again.
   *
   * This operation may be used as part of a recovery. If further errors
   * occur during recovery, E_RECOVER is returned, and the procedure must
   * be restarted.
   */
  int enqueue(const std::byte *data, const std::byte *meta, error_t *err) noexcept;

  /* Copy an existing page to the front of the journal. New metadata must
   * be specified. This operation is not persistent until a checkpoint is
   * reached.
   *
   * This operation may fail with the error code E_RECOVER. If this
   * occurs, the upper layer must complete the assisted recovery procedure
   * and then try again.
   *
   * This operation may be used as part of a recovery. If further errors
   * occur during recovery, E_RECOVER is returned, and the procedure must
   * be restarted.
   */
  int copy(page_t p, const std::byte *meta, error_t *err) noexcept;

  /* Mark the journal dirty. */
  void mark_dirty() noexcept { flags |= DHARA_JOURNAL_F_DIRTY; }

  /* Is the journal checkpointed? If true, then all pages enqueued are now
   * persistent.
   */
  int is_clean() const noexcept { return !(flags & DHARA_JOURNAL_F_DIRTY); }

  /* If an operation returns E_RECOVER, you must begin the recovery
   * procedure. You must then:
   *
   *    - call dhara_journal_next_recoverable() to obtain the next block
   *      to be recovered (if any). If there are no blocks remaining to be
   *      recovered, DHARA_JOURNAL_PAGE_NONE is returned.
   *
   *    - proceed to the next checkpoint. Once the journal is clean,
   *      recovery will finish automatically.
   *
   * If any operation during recovery fails due to a bad block, E_RECOVER
   * is returned again, and recovery restarts. Do not add new data to the
   * journal (rewrites of recovered data are fine) until recovery is
   * complete.
   */
  int in_recovery() const noexcept { return flags & DHARA_JOURNAL_F_RECOVERY; }

  page_t next_recoverable() noexcept;

 private:
  page_t next_upage(page_t p) const noexcept;
  void clear_recovery() noexcept;
  void reset_journal() noexcept;
  void roll_stats() noexcept;
  int find_checkblock(block_t blk, block_t *where, error_t *err) const noexcept;
  block_t find_last_checkblock(block_t first) const noexcept;
  int cp_free(page_t first_user) const noexcept;
  page_t find_last_group(block_t blk) const noexcept;
  int find_root(page_t start, error_t *err) noexcept;
  int find_head(page_t start, error_t *err) noexcept;
  int skip_block(error_t *err) noexcept;
  int prepare_head(error_t *err) noexcept;
  void restart_recovery(page_t old_head) noexcept;
  int dump_meta(error_t *err) noexcept;
  int recover_from(error_t write_err, error_t *err) noexcept;
  void finish_recovery() noexcept;
  int push_meta(const std::byte *meta, error_t *err) noexcept;

  // Member variables are protected not private to give direct access for testing
 protected:
  Nand &nand;
  const page_buf_t page_buf;

  /* In the journal, user data is grouped into checkpoints of
   * 2**log2_ppc contiguous aligned pages.
   *
   * The last page of each checkpoint contains the journal header
   * and the metadata for the other pages in the period (the user
   * pages).
   */
  const std::uint8_t log2_ppc;

  /* Epoch counter. This is incremented whenever the journal head
   * passes the end of the chip and wraps around.
   */
  std::uint8_t epoch;

  /* General purpose flags field */
  std::uint8_t flags;

  /* Bad-block counters. bb_last is our best estimate of the
   * number of bad blocks in the chip as a whole. bb_current is
   * the number of bad blocks in all blocks before the current
   * head.
   */
  block_t bb_current;
  block_t bb_last;

  /* Log head and tail. The tail pointer points to the last user
   * page in the log, and the head pointer points to the next free
   * raw page. The root points to the last written user page.
   */
  page_t tail_sync;
  page_t tail;
  page_t head;

  /* This points to the last written user page in the journal */
  page_t root_;

  /* Recovery mode: recover_root points to the last valid user
   * page in the block requiring recovery. recover_next points to
   * the next user page needing recovery.
   *
   * If we had buffered metadata before recovery started, it will
   * have been dumped to a free page, indicated by recover_meta.
   * If this block later goes bad, we will have to defer bad-block
   * marking until recovery is complete (F_BAD_META).
   */
  page_t recover_next;
  page_t recover_root;
  page_t recover_meta;
};

}  // namespace dhara

#endif
