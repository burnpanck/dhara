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

#ifndef DHARA_ERROR_H_
#define DHARA_ERROR_H_

#include <variant>

namespace dhara {

enum class error_t {
  none = 0,
  bad_block,
  ecc,
  too_bad,
  recover,
  journal_full,
  not_found,
  map_full,
  corrupt_map,
  max
};

template <typename T>
class [[nodiscard]] Outcome {
 public:
 public:
  Outcome() = delete;
  constexpr Outcome(error_t err) noexcept : content(err) {}
  template <typename U> requires (!std::is_convertible_v<U &&,error_t> && std::is_convertible_v<U &&,T>)
  constexpr Outcome(U &&v) : content(std::forward<U>(v)) {}

  [[nodiscard]] constexpr bool has_value() const noexcept { return content.index(); }
  [[nodiscard]] constexpr bool has_error() const noexcept { return !content.index(); }
  constexpr decltype(auto) value() & noexcept {
    return std::get<1>(content);
  }
  constexpr decltype(auto) value() && noexcept {
    return std::get<1>(content);
  }
  constexpr decltype(auto) value() const & noexcept {
    return std::get<1>(content);
  }
  [[nodiscard]] constexpr error_t error() const noexcept { return std::get<0>(content); }

  constexpr explicit operator bool() const noexcept { return has_value(); }

 private:
  std::variant<error_t, T> content;
};

template <>
class [[nodiscard]] Outcome<void> {
 public:
  Outcome() = delete;
  constexpr Outcome(error_t err) noexcept : err(err) {}

  [[nodiscard]] constexpr bool has_value() const noexcept { return err == error_t::none; }
  [[nodiscard]] constexpr bool has_error() const noexcept { return err != error_t::none; }
  constexpr void value() const noexcept {}
  [[nodiscard]] constexpr error_t error() const noexcept { return err; }

  constexpr explicit operator bool() const noexcept { return has_value(); }

  // only for Outcome<void>
  [[deprecated("this is a compatibility shim for C-style error handling")]] constexpr int
  handle_legacy_err(error_t *err) const noexcept {
    if (!has_error()) return 0;
    if(err) *err = this->err;
    return -1;
  }

 private:
  error_t err;
};

#define DHARA_TRY(x)                                                                          \
  ({                                                                                           \
    auto __dhara_try_res = (x);                                                               \
    if (__builtin_expect(__dhara_try_res.has_error(), false)) return __dhara_try_res.error(); \
     __dhara_try_res.value();                                                                                     \
  })

/* Produce a human-readable error message. This function is kept in a
 * separate compilation unit and can be omitted to reduce binary size.
 */
const char *strerror(error_t err);

/* Save an error */
[[deprecated]] inline void set_error(error_t *err, error_t v) {
  if (err) *err = v;
}

}  // namespace dhara

#endif
