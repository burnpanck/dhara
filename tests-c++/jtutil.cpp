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
void TestJournalBase::recover(AsyncOp<void> &&op) {
  printf("    recover: start\n");
  std::move(op).loop(
      [this, retry_count = int(0), p = page_t(page_none)](auto &&looper, auto pos,
                                                          auto &&res) mutable {
        using Res = std::remove_cvref_t<decltype(res)>;
        if constexpr (pos == 0) {
          // first part of loop
          static_assert(std::is_same_v<Res, std::monostate>);

          if (!in_recovery()) {
            // loop exit
            check();
            printf("    recover: complete\n");
            return std::move(looper).finish_success();
          }
          p = next_recoverable();
          check();

          if (p == page_none)
            return std::move(looper).continue_after(
                [&](auto &&op) { return JournalBase::enqueue(nullptr, nullptr, std::move(op)); },
                loop_case<1>);
          return std::move(looper).continue_after(
              [&](auto &&op) {
                return std::move(op).allocate(config.meta_size, [&](auto &&op,
                                                                    std::span<std::byte> meta_buf) {
                  return std::move(op).nested_op(
                      [&](auto &&op) { return read_meta(p, meta_buf.data(), std::move(op)); },
                      [this, p, meta = meta_buf.data()](auto &&op, Outcome<void> res) {
                        if (!res.has_value()) {
                          dabort("read_meta(p, meta)", res.error());
                        }
                        return std::move(op).nested_op(
                            [&](auto &&op) { return copy(p, meta, std::move(op)); },
                            [](auto &&op, Outcome<void> res) { return std::move(op).done(res); });
                      });
                });
              },
              loop_case<1>);
        } else {
          static_assert(pos == 1);
          static_assert(std::is_same_v<Res, Outcome<void>>);

          check();

          if (res.has_error()) {
            if (res.error() == error_t::recover) {
              printf("    recover: restart\n");
              if (++retry_count >= config.max_retries) dabort("recover", error_t::too_bad);
              return std::move(looper).next(loop_case<0>, dummy_arg);
            }
            dabort("copy", res.error());
          }

          return std::move(looper).next(loop_case<0>, dummy_arg);
        }
      },
      loop_case<0>, dummy_arg);
}

Outcome<void> TestJournalBase::enqueue(uint32_t id, std::byte *meta_buf) {
  const std::size_t page_size = config.nand.page_size();
  std::byte r[page_size];

  seq_gen(id, {r, page_size});
  dhara_w32(meta_buf, id);

  for (int i = 0; i < config.max_retries; i++) {
    check();
    auto res = JournalBase::enqueue(r, meta_buf);
    if (res.has_value()) return res;

    if (res.error() != error_t::recover) {
      return res;
    }

    recover();
  }

  return error_t::too_bad;
}

void TestJournalBase::enqueue(std::uint32_t id, std::byte *meta_buf, AsyncOp<void> &&op) {
  return std::move(op).allocate(config.nand.page_size(), [&](auto &&op,
                                                             std::span<std::byte> page_buf) {
    seq_gen(id, page_buf);
    dhara_w32(meta_buf, id);

    return std::move(op).loop(
        [this, id, page_buf, meta_buf, i = int(0)](auto &&looper, auto pos, auto &&arg) mutable {
          using T = std::remove_cvref_t<decltype(arg)>;
          if constexpr (pos == 0) {
            // first part of loop body
            static_assert(std::is_same_v<T, std::monostate>);

            if (i++ >= config.max_retries)
              return std::move(looper).finish_failure(error_t::too_bad);

            check();

            return std::move(looper).continue_after(
                [&](auto &&op) {
                  return JournalBase::enqueue(page_buf.data(), meta_buf, std::move(op));
                },
                loop_case<1>);
          } else if constexpr (pos == 1) {
            // second part of loop body
            static_assert(std::is_same_v<T, Outcome<void>>);

            if (arg.has_value()) return std::move(looper).finish_success();
            error_t err = arg.error();
            if (err != error_t::recover) return std::move(looper).finish_failure(err);

            return std::move(looper).continue_after(
                [&](auto &&op) { return recover(std::move(op)); }, loop_case<2>);
          } else {
            // third part of loop body
            static_assert(pos == 2);
            static_assert(std::is_same_v<T, Outcome<void>>);

            if (arg.has_error()) return std::move(looper).finish_failure(arg.error());

            return std::move(looper).next(loop_case<0>, dummy_arg);
          }
        },
        loop_case<0>, dummy_arg);
  });
}

int TestJournalBase::enqueue_sequence(int start, int count) {
  if (count < 0) count = nand.num_blocks() << config.nand.log2_ppb;

  for (int i = 0; i < count; i++) {
    std::byte meta[config.meta_size];

    {
      auto res = enqueue(start + i, meta);
      if (res.has_error()) {
        if (res.error() == error_t::journal_full) return i;

        dabort("enqueue", res.error());
      }
    }

    assert(size() >= i);

    DHARA_TRY_ABORT(read_meta(this->root(), meta));
    assert(dhara_r32(meta) == start + i);
  }

  return count;
}

void TestJournalBase::enqueue_sequence(int start, int count, AsyncOp<int> &&op) {
  if (count < 0) count = nand.num_blocks() << config.nand.log2_ppb;

  return std::move(op).allocate(config.meta_size, [&](auto &&op, std::span<std::byte> meta) {
    return std::move(op).loop(
        [this, start, count, meta, i = int(0)](auto &&looper, auto pos, auto &&arg) mutable {
          using T = std::remove_cvref_t<decltype(arg)>;
          if constexpr (pos == 0) {
            // first part of loop body
            static_assert(std::is_same_v<T, std::monostate>);

            if (i >= count) {
              return std::move(looper).finish_success(count);
            }
            return std::move(looper).continue_after(
                [&](auto &&op) { return enqueue(start + i, meta.data(), std::move(op)); },
                loop_case<1>);
          } else if constexpr (pos == 1) {
            // second part of loop body
            static_assert(std::is_same_v<T, Outcome<void>>);

            if (arg.has_error()) {
              if (arg.error() == error_t::journal_full) return std::move(looper).finish_success(i);
              return std::move(looper).finish_failure(arg.error());
            }
            assert(size() >= i);

            return std::move(looper).continue_after(
                [&](auto &&op) { return read_meta(this->root(), meta.data(), std::move(op)); },
                loop_case<2>);
          } else {
            // third part of loop body
            static_assert(pos == 2);
            static_assert(std::is_same_v<T, Outcome<void>>);

            assert(arg.has_value());

            assert(dhara_r32(meta.data()) == start + i);
            ++i;
            return std::move(looper).next(loop_case<0>, dummy_arg);
          }
        },
        loop_case<0>, dummy_arg);
  });
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
      const std::size_t page_size = config.nand.page_size();
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
void TestJournalBase::dequeue_sequence(int next, int count, AsyncOp<void> &&op) {
  return std::move(op).allocate(config.meta_size, [&](auto &&op, std::span<std::byte> meta) {
    return std::move(op).loop(
        [this, next, count, meta, tail = page_t(page_none), garbage_count = int(0)](
            auto &&looper, auto pos, auto &&res) mutable {
          using Res = std::remove_cvref_t<decltype(res)>;
          if constexpr (pos == 0) {
            // first part of loop body
            static_assert(std::is_same_v<Res, std::monostate>);

            if (count <= 0) {
              // loop exit
              check();
              return std::move(looper).finish_success();
            }
            return std::move(looper).template continue_after<page_t>(
                [&](auto &&op) { return peek(std::move(op)); }, loop_case<1>);
          } else if constexpr (pos == 1) {
            // second part of loop body
            static_assert(std::is_same_v<Res, Outcome<page_t>>);
            assert(res.has_value());
            tail = res.value();
            assert(tail != page_none);

            check();

            return std::move(looper).continue_after(
                [&](auto &&op) { return read_meta(tail, meta.data(), std::move(op)); },
                loop_case<2>);
          } else if constexpr (pos == 2) {
            // third part of loop body
            static_assert(std::is_same_v<Res, Outcome<void>>);

            assert(res.has_value());

            check();
            dequeue();

            auto id = dhara_r32(meta.data());

            if (id == 0xffffffff) {
              garbage_count++;
              const int max_garbage = 1u << config.log2_ppc;
              assert(garbage_count < max_garbage);
              return std::move(looper).next(loop_case<0>, dummy_arg);
            }

            return std::move(looper).continue_after(
                [&](auto &&op) {
                  return std::move(op).allocate(
                      config.nand.page_size(), [&](auto &&op, std::span<std::byte> page_buf) {
                        assert(id == next);
                        garbage_count = 0;
                        next++;
                        count--;

                        return std::move(op).nested_op(
                            [&](auto &&op) { return nand.read(tail, 0, page_buf, std::move(op)); },
                            [this, id, page_buf](auto &&op, auto &&r) {
                              assert(r.has_value());
                              seq_assert(id, page_buf);
                              return std::move(op).success();
                            });
                      });
                },
                loop_case<3>);
          } else {
            static_assert(pos == 3);
            static_assert(std::is_same_v<Res, Outcome<void>>);

            assert(res.has_value());
            return std::move(looper).next(loop_case<0>, dummy_arg);
          }
        },
        loop_case<0>, dummy_arg);
  });
}

void TestJournalBase::dump_info() const {
  printf("    log2_ppc   = %d\n", config.log2_ppc);
  printf("    size       = %u\n", size());
  printf("    capacity   = %u\n", capacity());
  printf("    bb_current = %d\n", bb_current);
  printf("    bb_last    = %d\n", bb_last);
}

void HostThreadCtxtBase::send_to_thread(AsyncCallBase &func) noexcept {
  bool launch;
  {
    std::lock_guard lock(mut);
    assert(!next_call);
    next_call = &func;
    launch = !active;
    active = true;
  }
  if (launch) {
    cur_call = std::async(std::launch::async, [&]() {
      while (true) {
        AsyncCallBase *cur_call;
        {
          std::lock_guard lock(mut);
          if (!next_call) {
            active = false;
            break;
          }
          cur_call = next_call;
          next_call = nullptr;
        };
        if (do_print_trace)
          printf("%04lx        %2ld: : %s ...\n", get_pos(&get_current<std::byte>()), depth,
                 std::string(depth, '-').c_str());
        cur_call->run();
      }
    });
  }
}

void HostThreadCtxtBase::dump_stats() const noexcept {
  printf("    num_construct: %ld\n", num_construct);
  printf("    num_alloc:     %ld\n", num_alloc);
  printf("    max_depth:     %ld\n", max_depth);
  printf("    max_use:       %ld\n", max_use);
}

void HostThreadCtxtBase::trace_construct(std::byte *frame, std::size_t sz,
                                         const char *type) noexcept {
  if (do_print_trace)
    printf("%04lx (%04lx) %2ld: > %s %s\n", get_pos(frame), sz, depth,
           std::string(depth, '-').c_str(), type);
  ++depth;
  ++num_construct;
  max_depth = std::max(depth, max_depth);
  max_use = std::max(get_pos(frame), max_use);
}
void HostThreadCtxtBase::trace_construct_fail(std::byte *frame, std::size_t sz,
                                              const char *type) noexcept {
  if (do_print_trace)
    printf("%04lx (%04lx) %2ld: ! %s %s\n", get_pos(frame), sz, depth,
           std::string(depth, '-').c_str(), type);
}
void HostThreadCtxtBase::trace_alloc(std::byte *frame, std::size_t sz) noexcept {
  if (do_print_trace)
    printf("%04lx [%04lx] %2ld: > %s\n", get_pos(frame), sz, depth,
           std::string(depth, '-').c_str());
  ++depth;
  ++num_alloc;
  max_depth = std::max(depth, max_depth);
  max_use = std::max(get_pos(frame), max_use);
}
void HostThreadCtxtBase::trace_alloc_fail(std::byte *frame, std::size_t sz) noexcept {
  if (do_print_trace)
    printf("%04lx [%04lx] %2ld: ! %s \n", get_pos(frame), sz, depth,
           std::string(depth, '-').c_str());
}
void HostThreadCtxtBase::trace_destruct(std::byte *frame, std::size_t sz,
                                        const char *type) noexcept {
  --depth;
  if (do_print_trace)
    printf("%04lx (%04lx) %2ld: < %s %s\n", get_pos(frame), sz, depth,
           std::string(depth, '-').c_str(), type);
}
void HostThreadCtxtBase::trace_dealloc(std::byte *frame, std::size_t sz) noexcept {
  --depth;
  if (do_print_trace)
    printf("%04lx [%04lx] %2ld: < %s \n", get_pos(frame), sz, depth,
           std::string(depth, '-').c_str());
}
void HostThreadCtxtBase::trace_get(std::byte *frame, const char *type) const noexcept {
  if (do_print_trace)
    printf("%04lx        %2ld: * %s %s\n", get_pos(frame), depth, std::string(depth, '-').c_str(),
           type);
}

}  // namespace dhara_tests