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

#include <dhara/journal.hpp>

#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <optional>
#include <thread>

using namespace std;
using namespace dhara;
using namespace dhara_tests;

void suspend_resume(TestJournalBase &j) {
  const page_t old_root = j.root();
  const auto old_ends = j.end_pointers();
  error_t err;

  j.clear();
  assert(j.root() == TestJournalBase::page_none);

  DHARA_TRY_ABORT(j.resume());

  assert(old_root == j.root());
  assert(old_ends == j.end_pointers());
}

StaticSimNand sim_nand;

template <typename Result>
class SetEventOpImpl : public AsyncOpImpl<Result> {
  using ret_t = std::optional<Outcome<Result>>;
 public:
  explicit SetEventOpImpl(ret_t &out) : out(out) {}

  virtual void done(AsyncOpCtxtBase &context, Outcome<Result> &&result) &&noexcept final {
    out = std::move(result);
    context.pop(this);
    context.get_current<SingleShotEventBase>().set();
  }
  virtual void done(AsyncOpCtxtBase &context, const Outcome<Result> &result) &&noexcept final {
    out = result;
    context.pop(this);
    context.get_current<SingleShotEventBase>().set();
  }

 private:
  ret_t &out;
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

template <std::size_t stack_size = 1024>
class HostThreadCtxt final : public AsyncOpCtxtBase {
 public:
  HostThreadCtxt() : AsyncOpCtxtBase(stack) {}

  template <typename Result = void, typename Call>
  Outcome<Result> wait_for(Call &&call) {
    assert_empty();
    auto *event_ptr = try_get_event();
    assert(event_ptr);
    auto &event = *event_ptr;
    std::optional<Outcome<Result>> ret;
    auto *event_op_ptr = try_emplace<SetEventOpImpl<Result>>(ret);
    assert(event_op_ptr);
    auto &event_op = *event_op_ptr;
    std::forward<Call>(call)(AsyncOp(*this, event_op));
    event.wait_and_destroy(*this);
    assert_empty();
    assert(ret);
    return *ret;
  }

  SingleShotEventBase *try_get_event() noexcept { return try_emplace<SingleShotEvent>(); }
  virtual void send_to_thread(AsyncCallBase &func) noexcept final {
    // this will wait for any previously active call to complete
    cur_call.reset();
    cur_call = std::async(std::launch::async, [&]() { func.run(); });
  }

  void assert_empty() const noexcept {
    assert(&get_current<std::byte>() == stack.data() + stack.size());
  }

 private:
  std::array<std::byte, stack_size> stack;
  std::optional<std::future<void>> cur_call;
};

int main() {
  TestJournal journal(sim_nand);
  HostThreadCtxt ctxt;

  sim_nand.reset();
  sim_nand.inject_bad(20);

  printf("Journal init\n");
  (void) ctxt.wait_for([&](auto &&op) { journal.resume(std::move(op)); });
  journal.dump_info();
  printf("\n");

  printf("Enqueue/dequeue, 100 pages x20\n");
  for (int rep = 0; rep < 20; rep++) {
    int count;

    count = DHARA_TRY_ABORT(ctxt.wait_for<int>([&](auto &&op) { journal.enqueue_sequence(0, 100, std::move(op)); }));
    assert(count == 100);

    printf("    size     = %d -> ", journal.size());
    journal.dequeue_sequence(0, count);
    printf("%d\n", journal.size());
  }
  printf("\n");

  printf("Journal stats:\n");
  journal.dump_info();
  printf("\n");

  printf("Enqueue/dequeue, ~100 pages x20 (resume)\n");
  for (int rep = 0; rep < 20; rep++) {
    auto cookie = journal.cookie();
    int count;

    cookie[0] = static_cast<byte>(rep);
    count = journal.enqueue_sequence(0, 100);
    assert(count == 100);

    while (!journal.is_clean()) {
      const int c = journal.enqueue_sequence(count++, 1);

      assert(c == 1);
    }

    printf("    size     = %d -> ", journal.size());
    suspend_resume(journal);
    journal.dequeue_sequence(0, count);
    printf("%d\n", journal.size());

    assert(cookie[0] == static_cast<byte>(rep));
  }
  printf("\n");

  printf("Journal stats:\n");
  journal.dump_info();
  printf("\n");

  sim_nand.dump();

  return 0;
}
