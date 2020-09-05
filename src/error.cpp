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

#include <dhara/error.hpp>

#include <array>
#include <cstddef>

namespace dhara {

const char *strerror(error_t err) {
  auto ecast = [](error_t e) { return static_cast<std::underlying_type_t<error_t>>(e); };

  static const char *const messages[] = {
      [static_cast<int>(error_t::none)] = "No error",
      [static_cast<int>(error_t::bad_block)] = "Bad page/eraseblock",
      [static_cast<int>(error_t::ecc)] = "ECC failure",
      [static_cast<int>(error_t::too_bad)] = "Too many bad blocks",
      [static_cast<int>(error_t::recover)] = "Journal recovery is required",
      [static_cast<int>(error_t::journal_full)] = "Journal is full",
      [static_cast<int>(error_t::not_found)] = "No such sector",
      [static_cast<int>(error_t::map_full)] = "Sector map is full",
      [static_cast<int>(error_t::corrupt_map)] = "Sector map is corrupted"};
  const char *msg = nullptr;

  if ((ecast(err) >= 0) && (ecast(err) < ecast(error_t::max))) msg = messages[ecast(err)];

  if (msg) return msg;

  return "Unknown error";
}

}  // namespace dhara