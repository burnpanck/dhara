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
#include <dhara/map.hpp>

#include <cstring>

#define DHARA_RADIX_DEPTH (sizeof(dhara_sector_t) << 3u)

namespace dhara {

dhara_sector_t d_bit(int depth) { return ((dhara_sector_t)1) << (DHARA_RADIX_DEPTH - depth - 1); }

/************************************************************************
 * Metadata/cookie layout
 */

void ck_set_count(std::byte *cookie, dhara_sector_t count) { dhara_w32(cookie, count); }

dhara_sector_t ck_get_count(const std::byte *cookie) { return dhara_r32(cookie); }

void meta_clear(std::byte *meta) { memset(meta, 0xff, DHARA_META_SIZE); }

dhara_sector_t meta_get_id(const std::byte *meta) { return dhara_r32(meta); }

void meta_set_id(std::byte *meta, dhara_sector_t id) { dhara_w32(meta, id); }

page_t meta_get_alt(const std::byte *meta, int level) { return dhara_r32(meta + 4 + (level << 2)); }

void meta_set_alt(std::byte *meta, int level, page_t alt) {
  dhara_w32(meta + 4 + (level << 2), alt);
}

/************************************************************************
 * Public interface
 */

void dhara_map_init(struct dhara_map *m, Nand *n, std::byte *page_buf, uint8_t gc_ratio) {
  if (!gc_ratio) gc_ratio = 1;

  dhara_journal_init(&m->journal, n, page_buf);
  m->gc_ratio = gc_ratio;
}

int dhara_map_resume(struct dhara_map *m, error_t *err) {
  if (dhara_journal_resume(&m->journal, err) < 0) {
    m->count = 0;
    return -1;
  }

  m->count = ck_get_count(dhara_journal_cookie(&m->journal));
  return 0;
}

void dhara_map_clear(struct dhara_map *m) {
  if (m->count) {
    m->count = 0;
    dhara_journal_clear(&m->journal);
  }
}

dhara_sector_t dhara_map_capacity(const struct dhara_map *m) {
  const dhara_sector_t cap = dhara_journal_capacity(&m->journal);
  const dhara_sector_t reserve = cap / (m->gc_ratio + 1);
  const dhara_sector_t safety_margin = DHARA_MAX_RETRIES << m->journal.nand->log2_ppb();

  if (reserve + safety_margin >= cap) return 0;

  return cap - reserve - safety_margin;
}

/* Trace the path from the root to the given sector, emitting
 * alt-pointers and alt-full bits in the given metadata buffer. This
 * also returns the physical page containing the given sector, if it
 * exists.
 *
 * If the page can't be found, a suitable path will be constructed
 * (containing PAGE_NONE alt-pointers), and DHARA_E_NOT_FOUND will be
 * returned.
 */
static int trace_path(struct dhara_map *m, dhara_sector_t target, page_t *loc, std::byte *new_meta,
                      error_t *err) {
  std::byte meta[DHARA_META_SIZE];
  int depth = 0;
  page_t p = dhara_journal_root(&m->journal);

  if (new_meta) meta_set_id(new_meta, target);

  if (p == DHARA_PAGE_NONE) goto not_found;

  if (dhara_journal_read_meta(&m->journal, p, meta, err) < 0) return -1;

  while (depth < DHARA_RADIX_DEPTH) {
    const dhara_sector_t id = meta_get_id(meta);

    if (id == DHARA_SECTOR_NONE) goto not_found;

    if ((target ^ id) & d_bit(depth)) {
      if (new_meta) meta_set_alt(new_meta, depth, p);

      p = meta_get_alt(meta, depth);
      if (p == DHARA_PAGE_NONE) {
        depth++;
        goto not_found;
      }

      if (dhara_journal_read_meta(&m->journal, p, meta, err) < 0) return -1;
    } else {
      if (new_meta) meta_set_alt(new_meta, depth, meta_get_alt(meta, depth));
    }

    depth++;
  }

  if (loc) *loc = p;

  return 0;

not_found:
  if (new_meta) {
    while (depth < DHARA_RADIX_DEPTH) meta_set_alt(new_meta, depth++, DHARA_SECTOR_NONE);
  }

  set_error(err, error_t::not_found);
  return -1;
}

int dhara_map_find(struct dhara_map *m, dhara_sector_t target, page_t *loc, error_t *err) {
  return trace_path(m, target, loc, NULL, err);
}

int dhara_map_read(struct dhara_map *m, dhara_sector_t s, std::byte *data, error_t *err) {
  const Nand *n = m->journal.nand;
  error_t my_err;
  page_t p;
  const std::size_t page_size = n->page_size();
  if (dhara_map_find(m, s, &p, &my_err) < 0) {
    if (my_err == error_t::not_found) {
      memset(data, 0xff, page_size);
      return 0;
    }

    set_error(err, my_err);
    return -1;
  }

  return n->read(p, 0, {data, page_size}, err);
}

/* Check the given page. If it's garbage, do nothing. Otherwise, rewrite
 * it at the front of the map. Return raw errors from the journal (do
 * not perform recovery).
 */
static int raw_gc(struct dhara_map *m, page_t src, error_t *err) {
  dhara_sector_t target;
  page_t current;
  error_t my_err;
  std::byte meta[DHARA_META_SIZE];

  if (dhara_journal_read_meta(&m->journal, src, meta, err) < 0) return -1;

  /* Is the page just filler/garbage? */
  target = meta_get_id(meta);
  if (target == DHARA_SECTOR_NONE) return 0;

  /* Find out where the sector once represented by this page
   * currently resides (if anywhere).
   */
  if (trace_path(m, target, &current, meta, &my_err) < 0) {
    if (my_err == error_t::not_found) return 0;

    set_error(err, my_err);
    return -1;
  }

  /* Is this page still the most current representative? If not,
   * do nothing.
   */
  if (current != src) return 0;

  /* Rewrite it at the front of the journal with updated metadata */
  ck_set_count(dhara_journal_cookie(&m->journal), m->count);
  if (dhara_journal_copy(&m->journal, src, meta, err) < 0) return -1;

  return 0;
}

static int pad_queue(struct dhara_map *m, error_t *err) {
  page_t p = dhara_journal_root(&m->journal);
  std::byte root_meta[DHARA_META_SIZE];

  ck_set_count(dhara_journal_cookie(&m->journal), m->count);

  if (p == DHARA_PAGE_NONE) return dhara_journal_enqueue(&m->journal, NULL, NULL, err);

  if (dhara_journal_read_meta(&m->journal, p, root_meta, err) < 0) return -1;

  return dhara_journal_copy(&m->journal, p, root_meta, err);
}

/* Attempt to recover the journal */
static int try_recover(struct dhara_map *m, error_t cause, error_t *err) {
  int restart_count = 0;

  if (cause != error_t::recover) {
    set_error(err, cause);
    return -1;
  }

  while (dhara_journal_in_recovery(&m->journal)) {
    page_t p = dhara_journal_next_recoverable(&m->journal);
    error_t my_err;
    int ret;

    if (p == DHARA_PAGE_NONE)
      ret = pad_queue(m, &my_err);
    else
      ret = raw_gc(m, p, &my_err);

    if (ret < 0) {
      if (my_err != error_t::recover) {
        set_error(err, my_err);
        return -1;
      }

      if (restart_count >= DHARA_MAX_RETRIES) {
        set_error(err, error_t::too_bad);
        return -1;
      }

      restart_count++;
    }
  }

  return 0;
}

static int auto_gc(struct dhara_map *m, error_t *err) {
  int i;

  if (dhara_journal_size(&m->journal) < dhara_map_capacity(m)) return 0;

  for (i = 0; i < m->gc_ratio; i++)
    if (dhara_map_gc(m, err) < 0) return -1;

  return 0;
}

static int prepare_write(struct dhara_map *m, dhara_sector_t dst, std::byte *meta, error_t *err) {
  error_t my_err;

  if (auto_gc(m, err) < 0) return -1;

  if (trace_path(m, dst, NULL, meta, &my_err) < 0) {
    if (my_err != error_t::not_found) {
      set_error(err, my_err);
      return -1;
    }

    if (m->count >= dhara_map_capacity(m)) {
      set_error(err, error_t::map_full);
      return -1;
    }

    m->count++;
  }

  ck_set_count(dhara_journal_cookie(&m->journal), m->count);
  return 0;
}

int dhara_map_write(struct dhara_map *m, dhara_sector_t dst, const std::byte *data, error_t *err) {
  for (;;) {
    std::byte meta[DHARA_META_SIZE];
    error_t my_err;
    const dhara_sector_t old_count = m->count;

    if (prepare_write(m, dst, meta, err) < 0) return -1;

    if (!dhara_journal_enqueue(&m->journal, data, meta, &my_err)) break;

    m->count = old_count;

    if (try_recover(m, my_err, err) < 0) return -1;
  }

  return 0;
}

int dhara_map_copy_page(struct dhara_map *m, page_t src, dhara_sector_t dst, error_t *err) {
  for (;;) {
    std::byte meta[DHARA_META_SIZE];
    error_t my_err;
    const dhara_sector_t old_count = m->count;

    if (prepare_write(m, dst, meta, err) < 0) return -1;

    if (!dhara_journal_copy(&m->journal, src, meta, &my_err)) break;

    m->count = old_count;

    if (try_recover(m, my_err, err) < 0) return -1;
  }

  return 0;
}

int dhara_map_copy_sector(struct dhara_map *m, dhara_sector_t src, dhara_sector_t dst,
                          error_t *err) {
  error_t my_err;
  page_t p;

  if (dhara_map_find(m, src, &p, &my_err) < 0) {
    if (my_err == error_t::not_found) return dhara_map_trim(m, dst, err);

    set_error(err, my_err);
    return -1;
  }

  return dhara_map_copy_page(m, p, dst, err);
}

static int try_delete(struct dhara_map *m, dhara_sector_t s, error_t *err) {
  error_t my_err;
  std::byte meta[DHARA_META_SIZE];
  page_t alt_page;
  std::byte alt_meta[DHARA_META_SIZE];
  int level = DHARA_RADIX_DEPTH - 1;
  int i;

  if (trace_path(m, s, NULL, meta, &my_err) < 0) {
    if (my_err == error_t::not_found) return 0;

    set_error(err, my_err);
    return -1;
  }

  /* Select any of the closest cousins of this node which are
   * subtrees of at least the requested order.
   */
  while (level >= 0) {
    alt_page = meta_get_alt(meta, level);
    if (alt_page != DHARA_PAGE_NONE) break;
    level--;
  }

  /* Special case: deletion of last sector */
  if (level < 0) {
    m->count = 0;
    dhara_journal_clear(&m->journal);
    return 0;
  }

  /* Rewrite the cousin with an up-to-date path which doesn't
   * point to the original node.
   */
  if (dhara_journal_read_meta(&m->journal, alt_page, alt_meta, err) < 0) return -1;

  meta_set_id(meta, meta_get_id(alt_meta));

  meta_set_alt(meta, level, DHARA_PAGE_NONE);
  for (i = level + 1; i < DHARA_RADIX_DEPTH; i++) meta_set_alt(meta, i, meta_get_alt(alt_meta, i));

  meta_set_alt(meta, level, DHARA_PAGE_NONE);

  ck_set_count(dhara_journal_cookie(&m->journal), m->count - 1);
  if (dhara_journal_copy(&m->journal, alt_page, meta, err) < 0) return -1;

  m->count--;
  return 0;
}

int dhara_map_trim(struct dhara_map *m, dhara_sector_t s, error_t *err) {
  for (;;) {
    error_t my_err;

    if (auto_gc(m, err) < 0) return -1;

    if (!try_delete(m, s, &my_err)) break;

    if (try_recover(m, my_err, err) < 0) return -1;
  }

  return 0;
}

int dhara_map_sync(struct dhara_map *m, error_t *err) {
  while (!dhara_journal_is_clean(&m->journal)) {
    page_t p = dhara_journal_peek(&m->journal);
    error_t my_err;
    int ret;

    if (p == DHARA_PAGE_NONE) {
      ret = pad_queue(m, &my_err);
    } else {
      ret = raw_gc(m, p, &my_err);
      dhara_journal_dequeue(&m->journal);
    }

    if ((ret < 0) && (try_recover(m, my_err, err) < 0)) return -1;
  }

  return 0;
}

int dhara_map_gc(struct dhara_map *m, error_t *err) {
  if (!m->count) return 0;

  for (;;) {
    page_t tail = dhara_journal_peek(&m->journal);
    error_t my_err;

    if (tail == DHARA_PAGE_NONE) break;

    if (!raw_gc(m, tail, &my_err)) {
      dhara_journal_dequeue(&m->journal);
      break;
    }

    if (try_recover(m, my_err, err) < 0) return -1;
  }

  return 0;
}

}  // namespace dhara