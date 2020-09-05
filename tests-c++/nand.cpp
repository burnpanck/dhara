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

#include "sim.hpp"
#include "util.hpp"

#include <iostream>

using namespace std;
using namespace dhara;
using namespace dhara_tests;

StaticSimNand sim_nand;

int main() {
  sim_nand.reset();
  sim_nand.inject_bad(5);

  Outcome<void> res(error_t::none);

  for (int i = 0; i < (1 << sim_nand.log2_ppb()); i++) {
    for (int j = 0; j < sim_nand.num_blocks(); j++) {
      std::array<std::byte, sim_nand.page_size_> block;
      page_t p = (j << sim_nand.log2_ppb()) | i;

      if (sim_nand.is_bad(j)) continue;

      if (!i && (res = sim_nand.erase(j)).has_error()) dabort("erase", res.error());

      seq_gen(p, block);
      if ((res = sim_nand.prog(p, block)).has_error()) dabort("prog", res.error());
    }
  }

  for (int i = 0; i < (sim_nand.num_blocks() << sim_nand.log2_ppb()); i++) {
    std::array<std::byte, sim_nand.page_size_> block;

    if (sim_nand.is_bad(i >> sim_nand.log2_ppb())) continue;

    if ((res = sim_nand.read(i, 0, block)).has_error()) dabort("read", res.error());

    seq_assert(i, block);
  }

  sim_nand.dump();
  return 0;
}
