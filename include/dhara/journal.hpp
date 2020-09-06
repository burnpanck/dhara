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

#include <bitset>
#include <cstdint>
#include <cstring>
#include <span>

namespace dhara {

struct JournalConfig {
  NandConfig nand;

  /* Global metadata available for a higher layer. This metadata is
   * persistent once the journal reaches a checkpoint, and is restored on
   * startup.
   */
  std::size_t meta_size;

  /* This is the size of the metadata slice which accompanies each written
   * page. This is independent of the underlying page/OOB size.
   */
  std::size_t cookie_size;

  /* When a block fails, or garbage is encountered, we try again on the
   * next block/checkpoint. We can do this up to the given number of
   * times.
   */
  std::size_t max_retries;

  /* In the journal, user data is grouped into checkpoints of
   * 2**log2_ppc contiguous aligned pages.
   *
   * The last page of each checkpoint contains the journal header
   * and the metadata for the other pages in the period (the user
   * pages).
   */
  std::uint8_t log2_ppc;
};

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
class JournalBase {
 protected:
  static constexpr std::size_t header_size = 16u;

  using hdr_cbuf_t = std::span<const std::byte, header_size>;
  using hdr_buf_t = std::span<std::byte, header_size>;

  using byte_buf_t = std::span<std::byte>;
  using byte_cbuf_t = std::span<const std::byte>;

  enum class Flag { dirty = 0, bad_meta = 1, recovery = 2, enum_done = 3 };
  using flags_t = std::bitset<4>;

  [[nodiscard]] byte_buf_t page_buf() noexcept { return {page_buf_ptr, config.nand.page_size()}; }

  [[nodiscard]] byte_cbuf_t page_buf() const noexcept {
    return {page_buf_ptr, config.nand.page_size()};
  }

  /* Initialize a journal. You must supply a pointer to a NAND chip
   * driver, and a single page buffer. This page buffer will be used
   * exclusively by the journal, but you are responsible for allocating
   * it, and freeing it (if necessary) at the end.
   *
   * No NAND operations are performed at this point.
   */
  JournalBase(const JournalConfig &config, NandBase &n, byte_buf_t page_buf) noexcept;

 public:
  static constexpr page_t page_none = static_cast<page_t>(-1);

  /* Start up the journal -- search the NAND for the journal head, or
   * initialize a blank journal if one isn't found. Returns 0 on success
   * or -1 if a (fatal) error occurs.
   *
   * This operation is O(log N), where N is the number of pages in the
   * NAND chip. All other operations are O(1).
   *
   * If this operation fails, the journal will be reset to an empty state.
   */
  Outcome<void> resume() noexcept;

  /* Obtain an upper bound on the number of user pages storable in the
   * journal.
   */
  [[nodiscard]] std::size_t capacity() const noexcept;

  /* Obtain an upper bound on the number of user pages consumed by the
   * journal.
   */
  [[nodiscard]] std::size_t size() const noexcept;

  /* Obtain the locations of the first and last pages in the journal.
   */
  [[nodiscard]] page_t root() const noexcept { return root_; }

  /* Advance the tail to the next non-bad block and return the page that's
   * ready to read. If no page is ready, return `page_none`.
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

  /* Mark the journal dirty. */
  void mark_dirty() noexcept { flags[int(Flag::dirty)] = true; }

  /* Is the journal checkpointed? If true, then all pages enqueued are now
   * persistent.
   */
  [[nodiscard]] bool is_clean() const noexcept { return !(flags[int(Flag::dirty)]); }

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
  [[nodiscard]] bool in_recovery() const noexcept { return flags[int(Flag::recovery)]; }

  page_t next_recoverable() noexcept;

 protected:
  /* Calculate a checkpoint period: the largest value of ppc such that
 * (2**ppc - 1) metadata blocks can fit on a page with one journal
 * header.
 */
  static constexpr std::size_t choose_ppc(std::size_t cookie_size, std::size_t meta_size, std::size_t log2_page_size, std::size_t max) {
    const std::size_t max_meta = (1u << log2_page_size) - header_size - cookie_size;
    std::size_t total_meta = meta_size;
    std::size_t ppc = 1;

    while (ppc < max) {
      total_meta <<= 1u;
      total_meta += meta_size;

      if (total_meta > max_meta) break;

      ppc++;
    }

    return ppc;
  }

  Outcome<void> read_meta(page_t p, std::byte *buf) noexcept;
  Outcome<void> enqueue(const std::byte *data, const std::byte *meta) noexcept;
  Outcome<void> copy(page_t p, const std::byte *meta) noexcept;

 private:
  /* Clear user metadata */
  void hdr_clear_user(std::span<std::byte> buf, uint8_t log2_page_size) const noexcept {
    auto user = buf.subspan(header_size + config.cookie_size);
    std::memset(user.data(), 0xff, user.size_bytes());
  }

/* Obtain pointers to user data */
  [[nodiscard]] std::size_t hdr_user_offset(uint8_t which) const noexcept {
    return header_size + config.cookie_size + which * config.meta_size;
  }

  [[nodiscard]] page_t next_upage(page_t p) const noexcept;
  void clear_recovery() noexcept;
  void reset_journal() noexcept;
  void roll_stats() noexcept;
  Outcome<block_t> find_checkblock(block_t blk) noexcept;
  block_t find_last_checkblock(block_t first) noexcept;
  [[nodiscard]] bool cp_free(page_t first_user) const noexcept;
  [[nodiscard]] page_t find_last_group(block_t blk) const noexcept;
  Outcome<void> find_root(page_t start) noexcept;
  Outcome<void> find_head(page_t start) noexcept;
  Outcome<void> skip_block() noexcept;
  Outcome<void> prepare_head() noexcept;
  void restart_recovery(page_t old_head) noexcept;
  Outcome<void> dump_meta() noexcept;
  Outcome<void> recover_from(error_t write_err) noexcept;
  void finish_recovery() noexcept;
  Outcome<void> push_meta(const std::byte *meta) noexcept;

  // Member variables are protected not private to give direct access for testing
 protected:
  const JournalConfig &config;

  NandBase &nand;

  std::byte *const page_buf_ptr;

  /* Epoch counter. This is incremented whenever the journal head
   * passes the end of the chip and wraps around.
   */
  std::uint8_t epoch;

  /* General purpose flags field */
  flags_t flags;

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

template <std::uint8_t log2_page_size_, std::uint8_t log2_ppb_, std::size_t meta_size_ = 132u,
          std::size_t cookie_size_ = 4u, std::size_t max_retries_ = 8u, typename Base = JournalBase>
class Journal : public Base {
  using base_t = Base;
  static constexpr JournalConfig make_config() {
    return {
        .nand = {.log2_page_size = log2_page_size_, .log2_ppb = log2_ppb_},
        .meta_size = meta_size_,
        .cookie_size = cookie_size_,
        .max_retries = max_retries_,
        .log2_ppc = base_t::choose_ppc(cookie_size_, meta_size_, log2_page_size_, 6u)
    };
  }
 public:
  static constexpr JournalConfig config = {
      .nand = {.log2_page_size = log2_page_size_, .log2_ppb = log2_ppb_},
      .meta_size = meta_size_,
      .cookie_size = cookie_size_,
      .max_retries = max_retries_,
      .log2_ppc = base_t::choose_ppc(cookie_size_, meta_size_, log2_page_size_, 6u)
  };

  using page_buf_t = std::span<std::byte, make_config().nand.page_size()>;
  using page_cbuf_t = std::span<const std::byte, make_config().nand.page_size()>;
  using meta_buf_t = std::span<std::byte, config.meta_size>;
  using meta_cbuf_t = std::span<const std::byte, config.meta_size>;
  using cookie_buf_t = std::span<std::byte, config.cookie_size>;
  using cookie_cbuf_t = std::span<const std::byte, config.cookie_size>;

  template <typename NBase>
  Journal(Nand<log2_page_size_,log2_ppb_,NBase> &nand, page_buf_t page_buf)
       : base_t(config, nand, page_buf) {}

 protected:
  [[nodiscard]] page_buf_t get_page_buf() noexcept {
    return page_buf_t(base_t::page_buf_ptr, config.nand.page_size());
  }
  [[nodiscard]] page_cbuf_t get_page_buf() const noexcept {
    return {base_t::page_buf_ptr, config.nand.page_size()};
  }
  template <std::size_t offset, std::size_t extent = std::dynamic_extent>
  [[nodiscard]] auto get_page_buf_subspan() noexcept {
    return get_page_buf().template subspan<offset, extent>();
  }
  template <std::size_t offset, std::size_t extent = std::dynamic_extent>
  [[nodiscard]] auto get_page_buf_subspan() const noexcept {
    return get_page_buf().template subspan<offset, extent>();
  }

 public:
  /* Obtain a pointer to the cookie data */
  [[nodiscard]] cookie_cbuf_t cookie() const noexcept {
    return get_page_buf_subspan<base_t::header_size, config.cookie_size>();
  }
  /* Obtain a pointer to the cookie data */
  [[nodiscard]] cookie_buf_t cookie() noexcept {
    return get_page_buf_subspan<base_t::header_size, config.cookie_size>();
  }

  /* Read metadata associated with a page. This assumes that the page
   * provided is a valid data page. The actual page data is read via the
   * normal NAND interface.
   */
  Outcome<void> read_meta(page_t p, meta_buf_t buf) noexcept {
    return base_t::read_meta(p,buf.data());
  }

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
  Outcome<void> enqueue(page_cbuf_t data, meta_cbuf_t meta) noexcept {
    return base_t::enqueue(data.data(),meta.data());
  }

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
  Outcome<void> copy(page_t p, meta_cbuf_t meta) noexcept {
    return base_t::copy(p, meta.data());
  }
};

template <std::uint8_t log2_page_size_, std::uint8_t log2_ppb_, typename NBase, std::size_t page_buf_size>
Journal(Nand<log2_page_size_,log2_ppb_,NBase> &nand, std::span<std::byte,page_buf_size> page_buf)
 -> Journal<log2_page_size_, log2_ppb_>;

}  // namespace dhara

#endif
