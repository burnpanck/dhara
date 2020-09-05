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

#include "util.hpp"

#include "dhara/bytes.hpp"
#include "dhara/error.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <span>

using namespace std;

namespace dhara_tests {

void seq_gen(unsigned int seed, std::span<byte> buf) {
  mt19937 engine(seed);
  std::uniform_int_distribution<> distrib(0, 255);
  for (size_t i = 0; i < buf.size(); i++) buf[i] = static_cast<byte>(distrib(engine));
}

void seq_assert(unsigned int seed, std::span<const byte> buf) {
  mt19937 engine(seed);
  std::uniform_int_distribution<> distrib(0, 255);
  for (size_t i = 0; i < buf.size(); i++) {
    const uint8_t expect = distrib(engine);
    const auto actual = static_cast<unsigned int>(buf[i]);
    if (actual != expect) {
      fprintf(stderr,
              "seq_assert: mismatch at %ld in "
              "sequence %d: 0x%02x (expected 0x%02x)\n",
              i, seed, static_cast<unsigned int>(actual), static_cast<unsigned int>(expect));
      abort();
    }
  }
}

[[noreturn]] void dabort(const char *message, error_t err) {
  fprintf(stderr, "%s: error_t => %s\n", message, strerror(err));
  abort();
}

}  // namespace dhara_tests