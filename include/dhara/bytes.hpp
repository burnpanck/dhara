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

#ifndef DHARA_BYTES_H_
#define DHARA_BYTES_H_

#include <cstdint>
#include <span>

namespace dhara {

inline std::uint16_t r16(std::span<const std::byte, 2> data) {
  return ((std::uint16_t)data[0]) | (((std::uint16_t)data[1]) << 8);
}

inline void w16(std::span<std::byte, 2> data, std::uint16_t v) {
  data[0] = static_cast<std::byte>(v);
  data[1] = static_cast<std::byte>(v >> 8);
}

inline std::uint32_t r32(std::span<const std::byte, 4> data) {
  return ((std::uint32_t)data[0]) | (((std::uint32_t)data[1]) << 8) |
         (((std::uint32_t)data[2]) << 16) | (((std::uint32_t)data[3]) << 24);
}

inline void w32(std::span<std::byte, 4> data, std::uint32_t v) {
  data[0] = static_cast<std::byte>(v);
  data[1] = static_cast<std::byte>(v >> 8);
  data[2] = static_cast<std::byte>(v >> 16);
  data[3] = static_cast<std::byte>(v >> 24);
}

[[deprecated]] inline void dhara_w32(std::byte *data, std::uint32_t v) {
  w32(std::span<std::byte, 4>(data, 4), v);
}
[[deprecated]] inline std::uint32_t dhara_r32(const std::byte *data) {
  return r32(std::span<const std::byte, 4>(data, 4));
}
[[deprecated]] inline void dhara_w16(std::byte *data, std::uint32_t v) {
  w16(std::span<std::byte, 2>(data, 2), v);
}
[[deprecated]] inline std::uint16_t dhara_r16(const std::byte *data) {
  return r16(std::span<const std::byte, 2>(data, 2));
}

}  // namespace dhara

#endif
