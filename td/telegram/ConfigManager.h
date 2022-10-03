//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/net/DcId.h"
#include "td/telegram/net/DcOptions.h"
#include "td/telegram/net/NetQuery.h"
#include "td/telegram/SuggestedAction.h"
#include "td/telegram/td_api.h"
#include "td/telegram/telegram_api.h"

#include "td/actor/actor.h"

#include "td/utils/common.h"
#include "td/utils/FloodControlStrict.h"
#include "td/utils/logging.h"
#include "td/utils/Promise.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/Time.h"

#include <limits>
#include <map>

namespace td {

extern int VERBOSITY_NAME(config_recoverer);

using SimpleConfig = tl_object_ptr<telegram_api::help_configSimple>;
struct SimpleConfigResult {
  Result<SimpleConfig> r_config;
  Result<int32> r_http_date;
};

Result<SimpleConfig> decode_config(Slice input);

ActorOwn<> get_simple_config_azure(Promise<SimpleConfigResult> promise, bool prefer_ipv6, Slice domain_name,
                                   bool is_test, int32 scheduler_id);

ActorOwn<> get_simple_config_google_dns(Promise<SimpleConfigResult> promise, bool prefer_ipv6, Slice domain_name,
                                        bool is_test, int32 scheduler_id);

ActorOwn<> get_simple_config_mozilla_dns(Promise<SimpleConfigResult> promise, bool prefer_ipv6, Slice domain_name,
                                         bool is_test, int32 scheduler_id);

ActorOwn<> get_simple_config_firebase_remote_config(Promise<SimpleConfigResult> promise, bool prefer_ipv6,
                                                    Slice domain_name, bool is_test, int32 scheduler_id);

ActorOwn<> get_simple_config_firebase_realtime(Promise<SimpleConfigResult> promise, bool prefer_ipv6, Slice domain_name,
                                               bool is_test, int32 scheduler_id);

ActorOwn<> get_simple_config_firebase_firestore(Promise<SimpleConfigResult> promise, bool prefer_ipv6,
                                                Slice domain_name, bool is_test, int32 scheduler_id);

class HttpDate {
  static bool is_leap(int32 year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  }
  static int32 days_in_month(int32 year, int32 month) {
    static int cnt[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return cnt[month - 1] + (month == 2 && is_leap(year));
  }
  static int32 seconds_in_day() {
    return 24 * 60 * 60;
  }

 public:
  static Result<int32> to_unix_time(int32 year, int32 month, int32 day, int32 hour, int32 minute, int32 second);
  static Result<int32> parse_http_date(std::string slice);
};

class ConfigRecoverer;
class ConfigManager final : public NetQueryCallback {
 public:
  explicit ConfigManager(ActorShared<> parent);

  void request_config(bool reopen_sessions);

  void lazy_request_config();

  void get_app_config(Promise<td_api::object_ptr<td_api::JsonValue>> &&promise);

  void reget_app_config(Promise<Unit> &&promise);

  void get_content_settings(Promise<Unit> &&promise);

  void set_content_settings(bool ignore_sensitive_content_restrictions, Promise<Unit> &&promise);

  void get_global_privacy_settings(Promise<Unit> &&promise);

  void set_archive_and_mute(bool archive_and_mute, Promise<Unit> &&promise);

  void hide_suggested_action(SuggestedAction suggested_action);

  void dismiss_suggested_action(SuggestedAction suggested_action, Promise<Unit> &&promise);

  void on_dc_options_update(DcOptions dc_options);

  void get_current_state(vector<td_api::object_ptr<td_api::Update>> &updates) const;

 private:
  ActorShared<> parent_;
  int32 config_sent_cnt_{0};
  bool reopen_sessions_after_get_config_{false};
  ActorOwn<ConfigRecoverer> config_recoverer_;
  int ref_cnt_{1};
  Timestamp expire_time_;

  FloodControlStrict lazy_request_flood_control_;

  vector<Promise<td_api::object_ptr<td_api::JsonValue>>> get_app_config_queries_;
  vector<Promise<Unit>> reget_app_config_queries_;

  vector<Promise<Unit>> get_content_settings_queries_;
  vector<Promise<Unit>> set_content_settings_queries_[2];
  bool is_set_content_settings_request_sent_ = false;
  bool last_set_content_settings_ = false;

  vector<Promise<Unit>> get_global_privacy_settings_queries_;
  vector<Promise<Unit>> set_archive_and_mute_queries_[2];
  bool is_set_archive_and_mute_request_sent_ = false;
  bool last_set_archive_and_mute_ = false;

  vector<SuggestedAction> suggested_actions_;
  size_t dismiss_suggested_action_request_count_ = 0;
  std::map<int32, vector<Promise<Unit>>> dismiss_suggested_action_queries_;

  static constexpr uint64 REFCNT_TOKEN = std::numeric_limits<uint64>::max() - 2;

  void start_up() final;
  void hangup_shared() final;
  void hangup() final;
  void loop() final;
  void try_stop();

  void on_result(NetQueryPtr res) final;

  void request_config_from_dc_impl(DcId dc_id, bool reopen_sessions);
  void process_config(tl_object_ptr<telegram_api::config> config);

  void try_request_app_config();

  void process_app_config(tl_object_ptr<telegram_api::JSONValue> &config);

  void do_set_ignore_sensitive_content_restrictions(bool ignore_sensitive_content_restrictions);

  void do_set_archive_and_mute(bool archive_and_mute);

  static Timestamp load_config_expire_time();
  static void save_config_expire(Timestamp timestamp);
  static void save_dc_options_update(const DcOptions &dc_options);
  static DcOptions load_dc_options_update();

  ActorShared<> create_reference();
};

}  // namespace td
