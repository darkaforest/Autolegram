//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/TdParameters.h"

#include "td/db/binlog/BinlogEvent.h"
#include "td/db/binlog/BinlogInterface.h"
#include "td/db/DbKey.h"
#include "td/db/KeyValueSyncInterface.h"

#include "td/utils/Promise.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"

#include <functional>
#include <memory>

namespace td {

class Binlog;
template <class BinlogT>
class BinlogKeyValue;
class ConcurrentBinlog;
class DialogDbSyncInterface;
class DialogDbSyncSafeInterface;
class DialogDbAsyncInterface;
class FileDbInterface;
class MessagesDbSyncInterface;
class MessagesDbSyncSafeInterface;
class MessagesDbAsyncInterface;
class SqliteConnectionSafe;
class SqliteKeyValueSafe;
class SqliteKeyValueAsyncInterface;
class SqliteKeyValue;

class TdDb {
 public:
  TdDb();
  TdDb(const TdDb &) = delete;
  TdDb &operator=(const TdDb &) = delete;
  TdDb(TdDb &&) = delete;
  TdDb &operator=(TdDb &&) = delete;
  ~TdDb();

  struct OpenedDatabase {
    string database_directory;
    string files_directory;

    unique_ptr<TdDb> database;

    vector<BinlogEvent> to_secret_chats_manager;
    vector<BinlogEvent> user_events;
    vector<BinlogEvent> chat_events;
    vector<BinlogEvent> channel_events;
    vector<BinlogEvent> secret_chat_events;
    vector<BinlogEvent> web_page_events;
    vector<BinlogEvent> save_app_log_events;
    vector<BinlogEvent> to_poll_manager;
    vector<BinlogEvent> to_messages_manager;
    vector<BinlogEvent> to_notification_manager;
    vector<BinlogEvent> to_notification_settings_manager;
  };
  static void open(int32 scheduler_id, TdParameters parameters, DbKey key, Promise<OpenedDatabase> &&promise);

  static Status destroy(const TdParameters &parameters);

  std::shared_ptr<FileDbInterface> get_file_db_shared();
  std::shared_ptr<SqliteConnectionSafe> &get_sqlite_connection_safe();
#define get_binlog() get_binlog_impl(__FILE__, __LINE__)
  BinlogInterface *get_binlog_impl(const char *file, int line);

  std::shared_ptr<KeyValueSyncInterface> get_binlog_pmc_shared();
  std::shared_ptr<KeyValueSyncInterface> get_config_pmc_shared();
  KeyValueSyncInterface *get_binlog_pmc();
  KeyValueSyncInterface *get_config_pmc();

  SqliteKeyValue *get_sqlite_sync_pmc();
  SqliteKeyValueAsyncInterface *get_sqlite_pmc();

  CSlice binlog_path() const;
  CSlice sqlite_path() const;

  void flush_all();

  void close_all(Promise<> on_finished);
  void close_and_destroy_all(Promise<> on_finished);

  MessagesDbSyncInterface *get_messages_db_sync();
  MessagesDbAsyncInterface *get_messages_db_async();

  DialogDbSyncInterface *get_dialog_db_sync();
  DialogDbAsyncInterface *get_dialog_db_async();

  void change_key(DbKey key, Promise<> promise);

  void with_db_path(const std::function<void(CSlice)> &callback);

  Result<string> get_stats();

 private:
  string sqlite_path_;
  std::shared_ptr<SqliteConnectionSafe> sql_connection_;

  std::shared_ptr<FileDbInterface> file_db_;

  std::shared_ptr<SqliteKeyValueSafe> common_kv_safe_;
  unique_ptr<SqliteKeyValueAsyncInterface> common_kv_async_;

  std::shared_ptr<MessagesDbSyncSafeInterface> messages_db_sync_safe_;
  std::shared_ptr<MessagesDbAsyncInterface> messages_db_async_;

  std::shared_ptr<DialogDbSyncSafeInterface> dialog_db_sync_safe_;
  std::shared_ptr<DialogDbAsyncInterface> dialog_db_async_;

  std::shared_ptr<BinlogKeyValue<ConcurrentBinlog>> binlog_pmc_;
  std::shared_ptr<BinlogKeyValue<ConcurrentBinlog>> config_pmc_;
  std::shared_ptr<ConcurrentBinlog> binlog_;

  static void open_impl(TdParameters parameters, DbKey key, Promise<OpenedDatabase> &&promise);

  static Status check_parameters(TdParameters &parameters);

  Status init_sqlite(const TdParameters &parameters, const DbKey &key, const DbKey &old_key,
                     BinlogKeyValue<Binlog> &binlog_pmc);

  void do_close(Promise<> on_finished, bool destroy_flag);
};

}  // namespace td
