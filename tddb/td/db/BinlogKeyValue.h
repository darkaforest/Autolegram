//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/db/binlog/Binlog.h"
#include "td/db/binlog/BinlogEvent.h"
#include "td/db/DbKey.h"
#include "td/db/KeyValueSyncInterface.h"

#include "td/utils/algorithm.h"
#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/RwMutex.h"
#include "td/utils/Promise.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/StorerBase.h"
#include "td/utils/tl_parsers.h"
#include "td/utils/tl_storers.h"

#include <memory>
#include <unordered_map>
#include <utility>

namespace td {

template <class BinlogT>
class BinlogKeyValue final : public KeyValueSyncInterface {
 public:
  static constexpr int32 MAGIC = 0x2a280000;

  struct Event final : public Storer {
    Event() = default;
    Event(Slice key, Slice value) : key(key), value(value) {
    }
    Slice key;
    Slice value;
    template <class StorerT>
    void store(StorerT &&storer) const {
      storer.store_string(key);
      storer.store_string(value);
    }

    template <class ParserT>
    void parse(ParserT &&parser) {
      key = parser.template fetch_string<Slice>();
      value = parser.template fetch_string<Slice>();
    }

    size_t size() const final {
      TlStorerCalcLength storer;
      store(storer);
      return storer.get_length();
    }
    size_t store(uint8 *ptr) const final {
      TlStorerUnsafe storer(ptr);
      store(storer);
      return static_cast<size_t>(storer.get_buf() - ptr);
    }
  };

  int32 get_magic() const {
    return magic_;
  }

  Status init(string name, DbKey db_key = DbKey::empty(), int scheduler_id = -1,
              int32 override_magic = 0) TD_WARN_UNUSED_RESULT {
    close();
    if (override_magic) {
      magic_ = override_magic;
    }

    binlog_ = std::make_shared<BinlogT>();
    TRY_STATUS(binlog_->init(
        name,
        [&](const BinlogEvent &binlog_event) {
          Event event;
          event.parse(TlParser(binlog_event.data_));
          map_.emplace(event.key.str(), std::make_pair(event.value.str(), binlog_event.id_));
        },
        std::move(db_key), DbKey::empty(), scheduler_id));
    return Status::OK();
  }

  void external_init_begin(int32 override_magic = 0) {
    close();
    if (override_magic) {
      magic_ = override_magic;
    }
  }

  template <class OtherBinlogT>
  void external_init_handle(BinlogKeyValue<OtherBinlogT> &&other) {
    map_ = std::move(other.map_);
  }

  void external_init_handle(const BinlogEvent &binlog_event) {
    Event event;
    event.parse(TlParser(binlog_event.data_));
    map_.emplace(event.key.str(), std::make_pair(event.value.str(), binlog_event.id_));
  }

  void external_init_finish(std::shared_ptr<BinlogT> binlog) {
    binlog_ = std::move(binlog);
  }

  void close() {
    *this = BinlogKeyValue();
  }
  void close(Promise<> promise) final {
    binlog_->close(std::move(promise));
  }

  SeqNo set(string key, string value) final {
    auto lock = rw_mutex_.lock_write().move_as_ok();
    uint64 old_id = 0;
    auto it_ok = map_.emplace(key, std::make_pair(value, 0));
    if (!it_ok.second) {
      if (it_ok.first->second.first == value) {
        return 0;
      }
      VLOG(binlog) << "Change value of key " << key << " from " << hex_encode(it_ok.first->second.first) << " to "
                   << hex_encode(value);
      old_id = it_ok.first->second.second;
      it_ok.first->second.first = value;
    } else {
      VLOG(binlog) << "Set value of key " << key << " to " << hex_encode(value);
    }
    bool rewrite = false;
    uint64 id;
    auto seq_no = binlog_->next_id();
    if (old_id != 0) {
      rewrite = true;
      id = old_id;
    } else {
      id = seq_no;
      it_ok.first->second.second = id;
    }

    lock.reset();
    add_event(seq_no,
              BinlogEvent::create_raw(id, magic_, rewrite ? BinlogEvent::Flags::Rewrite : 0, Event{key, value}));
    return seq_no;
  }

  SeqNo erase(const string &key) final {
    auto lock = rw_mutex_.lock_write().move_as_ok();
    auto it = map_.find(key);
    if (it == map_.end()) {
      return 0;
    }
    VLOG(binlog) << "Remove value of key " << key << ", which is " << hex_encode(it->second.first);
    uint64 id = it->second.second;
    map_.erase(it);
    auto seq_no = binlog_->next_id();
    lock.reset();
    add_event(seq_no, BinlogEvent::create_raw(id, BinlogEvent::ServiceTypes::Empty, BinlogEvent::Flags::Rewrite,
                                              EmptyStorer()));
    return seq_no;
  }

  void add_event(uint64 seq_no, BufferSlice &&event) {
    binlog_->add_raw_event(BinlogDebugInfo{__FILE__, __LINE__}, seq_no, std::move(event));
  }

  bool isset(const string &key) final {
    auto lock = rw_mutex_.lock_read().move_as_ok();
    return map_.count(key) > 0;
  }

  string get(const string &key) final {
    auto lock = rw_mutex_.lock_read().move_as_ok();
    auto it = map_.find(key);
    if (it == map_.end()) {
      return string();
    }
    VLOG(binlog) << "Get value of key " << key << ", which is " << hex_encode(it->second.first);
    return it->second.first;
  }

  void force_sync(Promise<> &&promise) final {
    binlog_->force_sync(std::move(promise));
  }

  void lazy_sync(Promise<> &&promise) {
    binlog_->lazy_sync(std::move(promise));
  }

  std::unordered_map<string, string> prefix_get(Slice prefix) final {
    auto lock = rw_mutex_.lock_write().move_as_ok();
    std::unordered_map<string, string> res;
    for (const auto &kv : map_) {
      if (begins_with(kv.first, prefix)) {
        res.emplace(kv.first.substr(prefix.size()), kv.second.first);
      }
    }
    return res;
  }

  std::unordered_map<string, string> get_all() final {
    auto lock = rw_mutex_.lock_write().move_as_ok();
    std::unordered_map<string, string> res;
    for (const auto &kv : map_) {
      res.emplace(kv.first, kv.second.first);
    }
    return res;
  }

  void erase_by_prefix(Slice prefix) final {
    auto lock = rw_mutex_.lock_write().move_as_ok();
    vector<uint64> ids;
    table_remove_if(map_, [&](const auto &it) {
      if (begins_with(it.first, prefix)) {
        ids.push_back(it.second.second);
        return true;
      }
      return false;
    });
    auto seq_no = binlog_->next_id(narrow_cast<int32>(ids.size()));
    lock.reset();
    for (auto id : ids) {
      add_event(seq_no, BinlogEvent::create_raw(id, BinlogEvent::ServiceTypes::Empty, BinlogEvent::Flags::Rewrite,
                                                EmptyStorer()));
      seq_no++;
    }
  }
  template <class T>
  friend class BinlogKeyValue;

  static Status destroy(Slice name) {
    return Binlog::destroy(name);
  }

 private:
  std::unordered_map<string, std::pair<string, uint64>> map_;
  std::shared_ptr<BinlogT> binlog_;
  RwMutex rw_mutex_;
  int32 magic_ = MAGIC;
};

template <>
inline void BinlogKeyValue<Binlog>::add_event(uint64 seq_no, BufferSlice &&event) {
  binlog_->add_raw_event(std::move(event), BinlogDebugInfo{__FILE__, __LINE__});
}

template <>
inline void BinlogKeyValue<Binlog>::force_sync(Promise<> &&promise) {
  binlog_->sync();
  promise.set_value(Unit());
}

template <>
inline void BinlogKeyValue<Binlog>::lazy_sync(Promise<> &&promise) {
  force_sync(std::move(promise));
}

}  // namespace td
