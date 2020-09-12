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

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <optional>
#include <thread>

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
  void enqueue_sequence(int start, int count, AsyncOp<int> &&op);

  /* Dequeue a sequence of seed/payload pages. Make sure there's not too
   * much garbage, and that we get the non-garbage pages in the expected
   * order.
   */
  void dequeue_sequence(int start, int count);
  void dequeue_sequence(int start, int count, AsyncOp<void> &&op);

  [[nodiscard]] std::pair<page_t, page_t> end_pointers() const { return {tail, head}; }

  void dump_info() const;

  void do_tail_sync() {
    // needed in jfill test; no clue what exactly it does, but it is supposed to free some space
    tail_sync = tail;
  }

 private:
  void check_upage(page_t p) const;
  void recover();
  void recover(AsyncOp<void> &&op);
  Outcome<void> enqueue(std::uint32_t id, std::byte *meta_buf);
  void enqueue(std::uint32_t id, std::byte *meta_buf, AsyncOp<void> &&op);
};

template <std::uint8_t log2_page_size_, std::uint8_t log2_ppb_, std::size_t meta_size_ = 132u,
          std::size_t cookie_size_ = 4u, std::size_t max_retries_ = 8u>
class TestJournal
    : public Journal<log2_page_size_, log2_ppb_, meta_size_, cookie_size_, max_retries_,
                     JournalSpec<meta_size_, cookie_size_, TestJournalBase>> {
  using base_t = Journal<log2_page_size_, log2_ppb_, meta_size_, cookie_size_, max_retries_,
                         JournalSpec<meta_size_, cookie_size_, TestJournalBase>>;

 public:
  using base_t::base_t;
};
template <std::uint8_t log2_page_size_, std::uint8_t log2_ppb_, typename NBase>
TestJournal(Nand<log2_page_size_, log2_ppb_, NBase> &nand)
    -> TestJournal<log2_page_size_, log2_ppb_>;

template <typename Result>
class SetEventOpImpl : public AsyncOpImpl<Result> {
  using ret_t = std::optional<Outcome<Result>>;

 public:
  explicit SetEventOpImpl(ret_t &out, SingleShotEventBase &event) : out(out), event(event) {}

  virtual void done(AsyncOpCtxtBase &context, Outcome<Result> &&result) &&noexcept final {
    out = std::move(result);
    auto &levent = event;
    context.pop(this);
    levent.set();
  }
  virtual void done(AsyncOpCtxtBase &context, const Outcome<Result> &result) &&noexcept final {
    out = result;
    auto &levent = event;
    context.pop(this);
    levent.set();
  }

 private:
  ret_t &out;
  SingleShotEventBase &event;
};

class SingleShotEvent final : public SingleShotEventBase {
 public:
  virtual void set() noexcept final {
    std::lock_guard lock(mut);
    is_set_ = true;
    cond.notify_all();
  }

  virtual bool is_set() const noexcept final {
    std::lock_guard lock(mut);
    return is_set_;
  }

  virtual void wait() noexcept final {
    std::unique_lock lock(mut);
    while (!is_set_) {
      cond.wait(lock);
    }
  }
  virtual void destroy(AsyncOpCtxtBase &context) noexcept final { context.pop(this); }

 private:
  mutable std::mutex mut;
  std::condition_variable cond;
  bool is_set_ = false;
};

class HostThreadCtxtBase : public AsyncOpCtxtBase {
 public:
  explicit HostThreadCtxtBase(const std::span<std::byte> &stack) : AsyncOpCtxtBase(stack) {}

  template <typename Result = void, typename Call>
  Outcome<Result> wait_for(Call &&call) {
    SingleShotEvent event;
    std::optional<Outcome<Result>> ret;
    auto *event_op_ptr = try_emplace<SetEventOpImpl<Result>>(ret, event);
    assert(event_op_ptr);
    auto &event_op = *event_op_ptr;
    std::forward<Call>(call)(AsyncOp(*this, event_op));
    event.wait();
    assert(ret);
    return *ret;
  }

  virtual void send_to_thread(AsyncCallBase &func) noexcept final;

  void dump_stats() const noexcept;

 protected:
  virtual void trace_construct(std::byte *frame, std::size_t sz, const char *type) noexcept final;
  virtual void trace_construct_fail(std::byte *frame, std::size_t sz,
                                    const char *type) noexcept final;
  virtual void trace_alloc(std::byte *frame, std::size_t sz) noexcept final;
  virtual void trace_alloc_fail(std::byte *frame, std::size_t sz) noexcept final;
  virtual void trace_destruct(std::byte *frame, std::size_t sz, const char *type) noexcept final;
  virtual void trace_dealloc(std::byte *frame, std::size_t sz) noexcept final;
  virtual void trace_get(std::byte *frame, const char *type) const noexcept final;

 private:
  [[nodiscard]] virtual std::size_t get_pos(std::byte *frame) const noexcept = 0;

  std::optional<std::future<void>> cur_call;
  std::mutex mut;
  bool active = false;
  AsyncCallBase *next_call = nullptr;

  bool do_print_trace = false;
  std::size_t depth = 0;

  // stats
  std::size_t max_depth = 0;
  std::size_t max_use = 0;
  std::size_t num_construct = 0;
  std::size_t num_alloc = 0;
};

template <std::size_t stack_size = 2048>
class HostThreadCtxt final : public HostThreadCtxtBase {
 public:
  HostThreadCtxt() : HostThreadCtxtBase(stack) {}

 private:
  [[nodiscard]] virtual std::size_t get_pos(std::byte *frame) const noexcept final {
    return stack.end() - frame;
  }

  std::array<std::byte, stack_size> stack;
};

}  // namespace dhara_tests

#endif
