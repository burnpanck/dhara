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

#ifndef TESTS_UTIL_H_
#define TESTS_UTIL_H_

#include <dhara/error.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace dhara_tests {

using namespace dhara;

/* Abort, displaying an error */
[[noreturn]] void dabort(const char *message, error_t err);

/* Generate a pseudo-random sequence of data */
void seq_gen(unsigned int seed, std::span<std::byte> buf);

/* Check a pseudo-random sequence */
void seq_assert(unsigned int seed, std::span<const std::byte> buf);

#define DHARA_TRY_ABORT(x)                                          \
  do {                                                                 \
    auto __dhara_try_abort_res = (x);                               \
    if (__builtin_expect(__dhara_try_abort_res.has_error(), false)) \
      dabort(#x, __dhara_try_abort_res.error());                    \
  } while(false)

}  // namespace dhara_tests

#endif
