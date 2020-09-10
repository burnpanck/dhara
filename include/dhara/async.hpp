//
// Created by Yves Delley on 04.09.20.
//

#ifndef DHARA_ASYNC_H_
#define DHARA_ASYNC_H_

#include "dhara/error.hpp"

#include <cassert>
#include <cstdint>
#include <span>
#include <tuple>

namespace dhara {

template <typename Result>
class AsyncOpImpl;

template <typename OuterResult, typename InnerResult, typename Body>
class NestedAsyncOpImpl;

template <typename Result>
class AsyncOpWithStateImpl;

template <typename Result>
class AsyncOp;

template <typename Body>
class AsyncLoopImpl;

template <typename OuterResult, typename Body>
class NestedAsyncLoopImpl;

template <typename OuterResult, typename Body>
class NestedAsyncCall;

template <typename OuterImpl, typename InnerResult, typename... Args>
class LoopContinueAsyncOpImpl;

class AsyncOpCtxtBase;

class StackFrameBase {};

//! API for a threading primitive that intrinsically asynchroneous operations may use to create a
//! synchroneous interface
class SingleShotEventBase : public StackFrameBase {
 public:
  virtual void set() noexcept = 0;
  virtual bool is_set() const noexcept = 0;
  virtual void wait() noexcept = 0;
  virtual void destroy(AsyncOpCtxtBase &) noexcept = 0;

  void wait_and_destroy(AsyncOpCtxtBase &context) noexcept {
    wait();
    destroy(context);
  }
};

class AsyncCallBase : public StackFrameBase {
 public:
  virtual void run() noexcept = 0;
};

class AsyncOpCtxtBase {
  static constexpr std::size_t min_align = alignof(void *);
  static_assert(!(min_align & (min_align - 1u)), "Expected a power of two for min_align");

 public:
  explicit AsyncOpCtxtBase(const std::span<std::byte> &stack)
      : stack_top(stack.data()), cur_frame(stack.data() + stack.size()) {
    // check that we have received a properly aligned buffer;
    // TODO: instead we should just re-align whatever we receive
    assert(!(reinterpret_cast<std::uintptr_t>(stack_top) & (min_align - 1u)));
    assert(!(reinterpret_cast<std::uintptr_t>(cur_frame) & (min_align - 1u)));
  };

  template <typename T>
  std::remove_cvref_t<T> *try_push(T &&arg) noexcept {
    return try_emplace<std::remove_cvref_t<T>>(std::forward<T>(arg));
  }

  template <typename T, typename... Args>
  T *try_emplace(Args &&... args) noexcept {
    return try_enconstruct<T>([&]() { return T(std::forward<Args>(args)...); });
  }

  template <typename T = void, typename F>
  std::conditional_t<std::is_same_v<T, void>, std::invoke_result_t<F>, T> *try_enconstruct(
      F &&construct) noexcept {
    using Ret = std::conditional_t<std::is_same_v<T, void>, std::invoke_result_t<F>, T>;
    static_assert(alignof(Ret) <= min_align,
                  "Currently, we do not support types with large alignment");
    const std::size_t sz = storage_size<Ret>();
    if (cur_frame < stack_top + sz) return nullptr;
    cur_frame -= sz;
    auto ret = new (cur_frame) Ret(std::forward<F>(construct)());
    // ensure that we can reconstruct the reference from the frame pointer;
    assert(ret == reinterpret_cast<Ret *>(cur_frame));
    return ret;
  }

  //! either returns a buffer at least as large as requested, or an empty span
  std::span<std::byte> try_allocate(std::size_t size) noexcept {
    size = (size + (min_align - 1u)) & (min_align - 1u);
    if (cur_frame < stack_top + size) return {};
    cur_frame -= size;
    return {cur_frame, size};
  }

  void deallocate(std::size_t size) noexcept { cur_frame += size; }

  template <typename T>
  void pop(T *frame) noexcept {
    assert(cur_frame == reinterpret_cast<std::byte *>(frame));
    frame->~T();
    cur_frame += storage_size<T>();
  }

  template <typename T>
  T &get_current() const noexcept {
    return *reinterpret_cast<T *>(cur_frame);
  }

  //  virtual SingleShotEventBase *try_get_event() noexcept = 0;
  virtual void send_to_thread(AsyncCallBase &func) noexcept = 0;

 private:
  template <typename T>
  static std::size_t storage_size() {
    return (sizeof(T) + (min_align - 1u)) & (min_align - 1u);
  }

  std::byte *const stack_top;
  std::byte *cur_frame;
};

template <int i>
static constexpr auto loop_case = std::integral_constant<int,i>{};
static constexpr std::monostate dummy_arg = {};

template <typename Impl>
class AsyncLoop {
 public:
  AsyncLoop(AsyncOpCtxtBase &context, Impl &impl) : context(context), impl(impl) {}

  static AsyncLoop get_current(AsyncOpCtxtBase &context) {
    return AsyncLoop(context, context.get_current<Impl>());
  }

  template <typename... Args>
  void start(Args &&... args) &&noexcept {
    std::move(*this).next(std::forward<Args>(args)...);
  }

  //! Try to create an `AsyncOp<R>`, if successful, pass it to `call`. Completing the provided op
  //! will continue the loop with the outcome as last argument, appended to all arguments given
  //! here. If creating the op fails, immediately fail the loop.
  template <typename R = void, typename Call, typename... Args>
  void continue_after(Call &&call, Args &&... args) &&noexcept {
    auto ret = context.try_emplace<LoopContinueAsyncOpImpl<Impl, R, std::remove_cvref_t<Args>...>>(
        std::forward<Args>(args)...);
    if (ret) return std::forward<Call>(call)(AsyncOp<R>(context, *ret));
    std::move(*this).finish_failure(error_t::async_stack_ovfl);
  }

  //! Immediately restart the loop body with the arguments given.
  template <typename... Args>
  void next(Args &&... args) &&noexcept {
    impl.next(context, std::forward<Args>(args)...);
  }

  //  void done() &&noexcept { std::move(impl).done(context); }

  template <typename Then>
  void finish(Then &&then) &&noexcept {
    std::move(impl).finish(context, std::forward<Then>(then));
  }

  template <typename T>
  void finish_success(T &&res) &&noexcept {
    std::move(*this).finish([res = std::forward<T>(res)](auto &&op) mutable {
      return std::move(op).success(std::move(res));
    });
  }
  void finish_success() &&noexcept {
    std::move(*this).finish([](auto &&op) mutable { return std::move(op).success(); });
  }
  void finish_failure(error_t err) &&noexcept {
    std::move(*this).finish([err](auto &&op) mutable { return std::move(op).failure(err); });
  }
  template <typename T>
  void finish_with(T &&res) &&noexcept {
    std::move(*this).finish([res = std::forward<T>(res)](auto &&op) mutable {
      return std::move(op).done(std::move(res));
    });
  }

 private:
  AsyncOpCtxtBase &context;
  Impl &impl;
};

template <typename Result>
class AsyncOp {
 public:
  AsyncOp(AsyncOpCtxtBase &context, AsyncOpImpl<Result> &impl) : context(context), impl(impl) {}

  static AsyncOp get_current(AsyncOpCtxtBase &context) {
    return AsyncOp(context, context.get_current<AsyncOpImpl<Result>>());
  }

  //! Signal conclusion of the requested operation. Calling this may immediately destroy all
  //! operation state, including the AsyncOp that is called itself.
  template <typename T>
  requires(std::is_convertible_v<T, Outcome<Result>>) void done(T &&result) &&noexcept {
    std::move(impl).done(context, std::forward<T>(result));
  }
  //! Signal success of the requested operation. Calling this may immediately destroy all operation
  //! state, including the AsyncOp that is called itself.
  template <typename T>
  requires(std::is_convertible_v<T, Result> &&
           !std::is_void_v<Result>) void success(T &&result) &&noexcept {
    std::move(impl).done(context, std::forward<T>(result));
  }
  template <typename Dummy = void>
  requires(std::is_void_v<Result>) void success() &&noexcept {
    std::move(impl).done(context, error_t::none);
  }

  //! Signal failure of the requested operation. Calling this may immediately destroy all operation
  //! state, including the AsyncOp that is called itself.
  void failure(error_t err) &&noexcept { std::move(impl).done(context, err); }

  //! Try to copy the supplied callback function onto the operation stack of the current async
  //! operation. Prepare a nested operation request, that, once completed, will call the callback.
  template <typename R = void, typename Call, typename Continuation>
  void nested_op(Call &&call, Continuation &&cont) &&noexcept {
    auto ret =
        context.try_emplace<NestedAsyncOpImpl<Result, R, std::remove_cvref_t<Continuation>>>(cont);
    if (ret) return std::forward<Call>(call)(AsyncOp<R>(context, *ret));
    std::move(*this).failure(error_t::async_stack_ovfl);
  }

  template <typename Body, typename... Args>
  void loop(Body &&body, Args &&... args) &&noexcept {
    using loop_t = NestedAsyncLoopImpl<Result, std::remove_cvref_t<Body>>;
    auto ret = context.try_emplace<loop_t>(std::forward<Body>(body));
    if (ret) return AsyncLoop<loop_t>(context, *ret).start(std::forward<Args>(args)...);
    std::move(*this).failure(error_t::async_stack_ovfl);
  }

  template <typename Call>
  void allocate(std::size_t size, Call &&call) &&noexcept {
    auto buf = context.try_allocate(size);
    if (buf.empty()) return std::move(*this).failure(error_t::async_stack_ovfl);
    using alloc_t = AsyncOpWithStateImpl<Result>;
    auto ret = context.try_emplace<alloc_t>(size);
    if (!ret) {
      context.deallocate(buf.size());
      return std::move(*this).failure(error_t::async_stack_ovfl);
    }
    return std::forward<Call>(call)(AsyncOp<Result>(context, *ret), buf);
  }

  template <typename Func>
  void run_in_thread(Func &&func) &&noexcept {
    using call_t = NestedAsyncCall<Result, std::remove_cvref_t<Func>>;
    auto ret = context.try_emplace<call_t>(context, std::forward<Func>(func));
    if (ret) return context.send_to_thread(*ret);
    std::move(*this).failure(error_t::async_stack_ovfl);
  }

 private:
  AsyncOpCtxtBase &context;
  AsyncOpImpl<Result> &impl;
};

template <typename Result = void>
class AsyncOpImpl : public StackFrameBase {
 public:
  virtual void done(AsyncOpCtxtBase &context, Outcome<Result> &&result) &&noexcept = 0;
  virtual void done(AsyncOpCtxtBase &context, const Outcome<Result> &result) &&noexcept = 0;
};

template <typename OuterResult, typename InnerResult, typename Body>
class NestedAsyncOpImpl final : public AsyncOpImpl<InnerResult> {
 public:
  explicit NestedAsyncOpImpl(Body &&body) : body(std::move(body)) {}
  explicit NestedAsyncOpImpl(const Body &body) : body(body) {}

  virtual void done(AsyncOpCtxtBase &context, Outcome<InnerResult> &&result) &&noexcept final {
    Body lbody = std::move(body);
    context.pop(this);
    lbody(AsyncOp<OuterResult>::get_current(context), std::move(result));
  }
  virtual void done(AsyncOpCtxtBase &context, const Outcome<InnerResult> &result) &&noexcept final {
    Body lbody = std::move(body);
    context.pop(this);
    lbody(AsyncOp<OuterResult>::get_current(context), result);
  }

 private:
  [[no_unique_address]] Body body;
};

template <typename Result>
class AsyncOpWithStateImpl final : public AsyncOpImpl<Result> {
 public:
  explicit AsyncOpWithStateImpl(std::size_t size) : size(size) {}

  virtual void done(AsyncOpCtxtBase &context, Outcome<Result> &&result) &&noexcept final {
    std::size_t lsize = size;
    context.pop(this);
    context.deallocate(lsize);
    AsyncOp<Result>::get_current(context).done(std::move(result));
  }
  virtual void done(AsyncOpCtxtBase &context, const Outcome<Result> &result) &&noexcept final {
    std::size_t lsize = size;
    context.pop(this);
    context.deallocate(lsize);
    AsyncOp<Result>::get_current(context).done(result);
  }

 private:
  std::size_t size;
};

template <typename Body>
class AsyncLoopImpl : public StackFrameBase {
 public:
  explicit AsyncLoopImpl(Body &&body) : body(std::move(body)) {}
  explicit AsyncLoopImpl(const Body &body) : body(body) {}

 protected:
  [[no_unique_address]] Body body;
};

template <typename OuterResult, typename Body>
class NestedAsyncLoopImpl final : public AsyncLoopImpl<Body> {
  using base_t = AsyncLoopImpl<Body>;

 public:
  template <typename... Args>
  void next(AsyncOpCtxtBase &context, Args &&... args) noexcept {
    base_t::body(AsyncLoop<NestedAsyncLoopImpl>::get_current(context), std::forward<Args>(args)...);
  }

  template <typename Then>
  void finish(AsyncOpCtxtBase &context, Then &&then) &&noexcept {
    Body lbody = std::move(base_t::body);
    context.pop(this);
    std::forward<Then>(then)(AsyncOp<OuterResult>::get_current(context));
  }

  using base_t::base_t;
};

template <typename OuterImpl, typename InnerResult, typename... Args>
class LoopContinueAsyncOpImpl final : public AsyncOpImpl<InnerResult> {
 public:
  template <typename... A>
  explicit LoopContinueAsyncOpImpl(A &&... args) : args(std::forward<A>(args)...) {}

  virtual void done(AsyncOpCtxtBase &context, Outcome<InnerResult> &&result) &&noexcept final {
    auto largs = std::move(args);
    context.pop(this);
    call_next(context, std::move(result), std::move(largs),
              std::make_index_sequence<sizeof...(Args)>{});
  }
  virtual void done(AsyncOpCtxtBase &context, const Outcome<InnerResult> &result) &&noexcept final {
    auto largs = std::move(args);
    context.pop(this);
    call_next(context, result, std::move(largs), std::make_index_sequence<sizeof...(Args)>{});
  }

 private:
  //! Helper function to let us take apart the extra arguments tuple
  template <typename T, std::size_t... I>
  static void call_next(AsyncOpCtxtBase &context, T &&result, std::tuple<Args...> &&args,
                        std::integer_sequence<std::size_t, I...>) {
    AsyncLoop<OuterImpl>::get_current(context).next(std::get<I>(std::move(args))..., std::forward<T>(result));
  }

  [[no_unique_address]] std::tuple<Args...> args;
};

template <typename OuterResult, typename Body>
class NestedAsyncCall final : public AsyncCallBase {
 public:
  explicit NestedAsyncCall(AsyncOpCtxtBase &context, Body &&body)
      : context(context), body(std::move(body)) {}
  explicit NestedAsyncCall(AsyncOpCtxtBase &context, const Body &body)
      : context(context), body(body) {}

  virtual void run() noexcept final {
    Body lbody = std::move(body);
    context.pop(this);
    if constexpr (std::is_same_v<OuterResult, void> && std::is_void_v<std::invoke_result_t<Body>>) {
      // void return value for an operation with void result
      lbody();
      AsyncOp<OuterResult>::get_current(context).success();
    } else {
      AsyncOp<OuterResult>::get_current(context).done(lbody());
    }
  }

 private:
  AsyncOpCtxtBase &context;
  [[no_unique_address]] Body body;
};

}  // namespace dhara

#endif  // DHARA_ASYNC_H_
