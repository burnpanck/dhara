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

#include "sim.hpp"
#include "util.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace std;

namespace dhara_tests {

void SimNand::reset() {
  auto blocks = this->blocks();

  memset(&stats, 0, sizeof(stats));
  memset(blocks.data(), 0, blocks.size_bytes());
  memset(pages().data(), 0x55, pages().size_bytes());

  for (int i = 0; i < blocks.size(); i++) blocks[i].next_page = pages_per_block();
}

void SimNand::timebomb_tick(block_t blk) {
  auto blocks = this->blocks();

  block_status &b = blocks[blk];

  if (b.timebomb) {
    b.timebomb--;
    if (!b.timebomb) b.flags.failed = true;
  }
}

/* Is the given block bad? */
bool SimNand::is_bad(block_t bno) const noexcept {
  auto blocks = this->blocks();

  if (bno >= blocks.size()) {
    fprintf(stderr,
            "sim: NAND_is_bad called on "
            "invalid block: %d\n",
            bno);
    abort();
  }

  if (!stats.frozen) stats.is_bad++;
  return blocks[bno].flags.bad_mark;
}

/* Mark bad the given block (or attempt to). No return value is
 * required, because there's nothing that can be done in response.
 */
void SimNand::mark_bad(block_t bno) noexcept {
  auto blocks = this->blocks();

  if (bno >= blocks.size()) {
    fprintf(stderr,
            "sim: NAND_mark_bad called on "
            "invalid block: %d\n",
            bno);
    abort();
  }

  if (!stats.frozen) stats.mark_bad++;
  blocks[bno].flags.bad_mark = true;
}

/* Erase the given block. This function should return 0 on success or -1
 * on failure.
 *
 * The status reported by the chip should be checked. If an erase
 * operation fails, return -1 and set err to E_BAD_BLOCK.
 */
Outcome<void> SimNand::erase(block_t bno) noexcept {
  auto blocks = this->blocks();

  if (bno >= blocks.size()) {
    fprintf(stderr,
            "sim: NAND_erase called on "
            "invalid block: %d\n",
            bno);
    abort();
  }

  if (blocks[bno].flags.bad_mark) {
    fprintf(stderr,
            "sim: NAND_erase called on "
            "block which is marked bad: %d\n",
            bno);
    abort();
  }

  if (!stats.frozen) stats.erase++;
  blocks[bno].next_page = 0;

  timebomb_tick(bno);

  auto blk = block_data(bno);

  if (blocks[bno].flags.failed) {
    if (!stats.frozen) stats.erase_fail++;
    seq_gen(bno * 57 + 29, blk);
    return error_t::bad_block;
  }

  memset(blk.data(), 0xff, blk.size_bytes());
  return error_t::none;
}

/* Program the given page. The data pointer is a pointer to an entire
 * page ((1 << log2_page_size) bytes). The operation status should be
 * checked. If the operation fails, return -1 and set err to
 * E_BAD_BLOCK.
 *
 * Pages will be programmed sequentially within a block, and will not be
 * reprogrammed.
 */
Outcome<void> SimNand::prog(page_t p, std::span<const std::byte> data) noexcept {
  const int bno = p >> log2_ppb();
  const int pno = p & ((1u << log2_ppb()) - 1u);

  if ((bno < 0) || (bno >= num_blocks())) {
    fprintf(stderr,
            "sim: NAND_prog called on "
            "invalid block: %d\n",
            bno);
    abort();
  }

  auto blocks = this->blocks();

  if (blocks[bno].flags.bad_mark) {
    fprintf(stderr,
            "sim: NAND_prog called on "
            "block which is marked bad: %d\n",
            bno);
    abort();
  }

  if (pno < blocks[bno].next_page) {
    fprintf(stderr,
            "sim: NAND_prog: out-of-order "
            "page programming. Block %d, page %d "
            "(expected %d)\n",
            bno, pno, blocks[bno].next_page);
    abort();
  }

  auto page = page_data(p);

  if (!stats.frozen) stats.prog++;
  blocks[bno].next_page = pno + 1;

  timebomb_tick(bno);

  if (blocks[bno].flags.failed) {
    if (!stats.frozen) stats.prog_fail++;
    seq_gen(p * 57 + 29, page);
    return error_t::bad_block;
  }

  memcpy(page.data(), data.data(), std::min(page.size_bytes(), data.size_bytes()));
  return error_t::none;
}

/* Check that the given page is erased */
bool SimNand::is_free(page_t p) const noexcept {
  const int bno = p >> log2_ppb();
  const int pno = p & ((1u << log2_ppb()) - 1u);

  if ((bno < 0) || (bno >= num_blocks())) {
    fprintf(stderr,
            "sim: NAND_is_free called on "
            "invalid block: %d\n",
            bno);
    abort();
  }

  auto blocks = this->blocks();

  if (!stats.frozen) stats.is_erased++;
  return blocks[bno].next_page <= pno;
}

/* Read a portion of a page. ECC must be handled by the NAND
 * implementation. Returns 0 on sucess or -1 if an error occurs. If an
 * uncorrectable ECC error occurs, return -1 and set err to E_ECC.
 */
Outcome<void> SimNand::read(page_t p, size_t offset, std::span<std::byte> data) const noexcept {
  const int bno = p >> log2_ppb();

  if ((bno < 0) || (bno >= num_blocks())) {
    fprintf(stderr,
            "sim: NAND_read called on "
            "invalid block: %d\n",
            bno);
    abort();
  }

  auto page = page_data(p).subspan(offset);

  if (data.size() > page.size()) {
    fprintf(stderr,
            "sim: NAND_read called on "
            "invalid range: offset = %ld, length = %ld\n",
            offset, data.size());
    abort();
  }

  if (!stats.frozen) {
    stats.read++;
    stats.read_bytes += data.size();
  }

  memcpy(data.data(), page.data(), data.size_bytes());
  return error_t::none;
}

/* Read a page from one location and reprogram it in another location.
 * This might be done using the chip's internal buffers, but it must use
 * ECC.
 */
Outcome<void> SimNand::copy(page_t src, page_t dst) noexcept {
  auto buf = page_buf();

  DHARA_TRY(read(src, 0, buf));
  DHARA_TRY(prog(dst, buf));

  return error_t::none;
}

static char rep_status(const struct block_status *b) {
  if (b->flags.failed) {
    return b->flags.bad_mark ? 'B' : 'b';
  }
  if (b->flags.bad_mark) return '?';

  if (b->next_page) return ':';

  return '.';
}

void SimNand::set_failed(block_t bno) { blocks()[bno].flags.failed = true; }

void SimNand::set_timebomb(block_t bno, int ttl) { blocks()[bno].timebomb = ttl; }

void SimNand::inject_bad(int count) {
  int i;

  for (i = 0; i < count; i++) {
    const int bno = rand() % num_blocks();
    auto &b = blocks()[bno];
    b.flags.bad_mark = true;
    b.flags.failed = true;
  }
}

void SimNand::inject_failed(int count) {
  int i;

  for (i = 0; i < count; i++) set_failed(rand() % num_blocks());
}

void SimNand::inject_timebombs(int count, int max_ttl) {
  int i;

  for (i = 0; i < count; i++) set_timebomb(rand() % num_blocks(), rand() % max_ttl + 1);
}

void SimNand::freeze() { stats.frozen++; }

void SimNand::thaw() { stats.frozen--; }

void SimNand::dump() const {
  int i;

  printf("NAND operation counts:\n");
  printf("    is_bad:         %d\n", stats.is_bad);
  printf("    mark_bad        %d\n", stats.mark_bad);
  printf("    erase:          %d\n", stats.erase);
  printf("    erase failures: %d\n", stats.erase_fail);
  printf("    is_erased:      %d\n", stats.is_erased);
  printf("    prog:           %d\n", stats.prog);
  printf("    prog failures:  %d\n", stats.prog_fail);
  printf("    read:           %d\n", stats.read);
  printf("    read (bytes):   %d\n", stats.read_bytes);
  printf("\n");

  printf("Block status:\n");
  i = 0;
  while (i < num_blocks()) {
    int j = num_blocks() - i;
    int k;

    if (j > 64) j = 64;

    printf("    ");
    for (k = 0; k < j; k++) fputc(rep_status(&blocks()[i + k]), stdout);
    fputc('\n', stdout);

    i += j;
  }
}

}  // namespace dhara_tests