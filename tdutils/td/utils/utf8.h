//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/utils/common.h"
#include "td/utils/Slice.h"

namespace td {

/// checks UTF-8 string for correctness
bool check_utf8(CSlice str);

/// checks if a code unit is a first code unit of a UTF-8 character
inline bool is_utf8_character_first_code_unit(unsigned char c) {
  return (c & 0xC0) != 0x80;
}

/// returns length of UTF-8 string in characters
inline size_t utf8_length(Slice str) {
  size_t result = 0;
  for (auto c : str) {
    result += is_utf8_character_first_code_unit(c);
  }
  return result;
}

/// returns length of UTF-8 string in UTF-16 code units
size_t utf8_utf16_length(Slice str);

/// appends a Unicode character using UTF-8 encoding
void append_utf8_character(string &str, uint32 ch);

/// moves pointer one UTF-8 character back
inline const unsigned char *prev_utf8_unsafe(const unsigned char *ptr) {
  while (!is_utf8_character_first_code_unit(*--ptr)) {
    // pass
  }
  return ptr;
}

/// moves pointer one UTF-8 character forward and saves code of the skipped character in *code
const unsigned char *next_utf8_unsafe(const unsigned char *ptr, uint32 *code);

/// truncates UTF-8 string to the given length in Unicode characters
template <class T>
T utf8_truncate(T str, size_t length) {
  if (str.size() > length) {
    for (size_t i = 0; i < str.size(); i++) {
      if (is_utf8_character_first_code_unit(static_cast<unsigned char>(str[i]))) {
        if (length == 0) {
          return str.substr(0, i);
        } else {
          length--;
        }
      }
    }
  }
  return str;
}

/// truncates UTF-8 string to the given length given in UTF-16 code units
Slice utf8_utf16_truncate(Slice str, size_t length);

template <class T>
T utf8_substr(T str, size_t offset) {
  if (offset == 0) {
    return str;
  }
  auto offset_pos = utf8_truncate(str, offset).size();
  return str.substr(offset_pos);
}

template <class T>
T utf8_substr(T str, size_t offset, size_t length) {
  return utf8_truncate(utf8_substr(str, offset), length);
}

Slice utf8_utf16_substr(Slice str, size_t offset);

Slice utf8_utf16_substr(Slice str, size_t offset, size_t length);

/// Returns UTF-8 string converted to lower case.
string utf8_to_lower(Slice str);

/// Returns valid UTF-8 representation of the string.
string utf8_encode(CSlice data);

}  // namespace td
