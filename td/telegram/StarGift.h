//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2024
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/files/FileId.h"
#include "td/telegram/td_api.h"
#include "td/telegram/telegram_api.h"

#include "td/utils/common.h"
#include "td/utils/StringBuilder.h"

namespace td {

class Td;

class StarGift {
  int64 id_ = 0;
  FileId sticker_file_id_;
  int64 star_count_ = 0;
  int64 default_sell_star_count_ = 0;
  int32 availability_remains_ = 0;
  int32 availability_total_ = 0;
  int32 first_sale_date_ = 0;
  int32 last_sale_date_ = 0;
  bool is_for_birthday_ = false;

  friend bool operator==(const StarGift &lhs, const StarGift &rhs);

  friend StringBuilder &operator<<(StringBuilder &string_builder, const StarGift &star_gift);

 public:
  StarGift() = default;

  StarGift(Td *td, telegram_api::object_ptr<telegram_api::starGift> &&star_gift);

  bool is_valid() const {
    return id_ != 0 && sticker_file_id_.is_valid();
  }

  int64 get_id() const {
    return id_;
  }

  int64 get_star_count() const {
    return star_count_;
  }

  td_api::object_ptr<td_api::gift> get_gift_object(const Td *td) const;

  template <class StorerT>
  void store(StorerT &storer) const;

  template <class ParserT>
  void parse(ParserT &parser);
};

bool operator==(const StarGift &lhs, const StarGift &rhs);

inline bool operator!=(const StarGift &lhs, const StarGift &rhs) {
  return !(lhs == rhs);
}

StringBuilder &operator<<(StringBuilder &string_builder, const StarGift &star_gift);

}  // namespace td
