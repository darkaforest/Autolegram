//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/BackgroundId.h"
#include "td/telegram/BackgroundType.h"
#include "td/telegram/td_api.h"
#include "td/telegram/telegram_api.h"

#include "td/actor/actor.h"

#include "td/utils/common.h"
#include "td/utils/Promise.h"
#include "td/utils/Status.h"

namespace td {

class Td;

class ThemeManager final : public Actor {
 public:
  ThemeManager(Td *td, ActorShared<> parent);

  void init();

  void on_update_theme(telegram_api::object_ptr<telegram_api::theme> &&theme, Promise<Unit> &&promise);

  static string get_theme_parameters_json_string(const td_api::object_ptr<td_api::themeParameters> &theme,
                                                 bool for_web_view);

  void get_current_state(vector<td_api::object_ptr<td_api::Update>> &updates) const;

 private:
  // apeend-only
  enum class BaseTheme : int32 { Classic, Day, Night, Tinted, Arctic };

  static constexpr int32 THEME_CACHE_TIME = 3600;

  struct ThemeSettings {
    int32 accent_color = 0;
    int32 message_accent_color = 0;
    BackgroundId background_id;
    BackgroundType background_type;
    BaseTheme base_theme = BaseTheme::Classic;
    vector<int32> message_colors;
    bool animate_message_colors = false;

    template <class StorerT>
    void store(StorerT &storer) const;

    template <class ParserT>
    void parse(ParserT &parser);
  };

  friend bool operator==(const ThemeSettings &lhs, const ThemeSettings &rhs);

  friend bool operator!=(const ThemeSettings &lhs, const ThemeSettings &rhs);

  struct ChatTheme {
    string emoji;
    int64 id = 0;
    ThemeSettings light_theme;
    ThemeSettings dark_theme;

    template <class StorerT>
    void store(StorerT &storer) const;

    template <class ParserT>
    void parse(ParserT &parser);
  };

  struct ChatThemes {
    int64 hash = 0;
    double next_reload_time = 0;
    vector<ChatTheme> themes;

    template <class StorerT>
    void store(StorerT &storer) const;

    template <class ParserT>
    void parse(ParserT &parser);
  };

  void start_up() final;

  void loop() final;

  void tear_down() final;

  static bool is_dark_base_theme(BaseTheme base_theme);

  void on_get_chat_themes(Result<telegram_api::object_ptr<telegram_api::account_Themes>> result);

  td_api::object_ptr<td_api::themeSettings> get_theme_settings_object(const ThemeSettings &settings) const;

  td_api::object_ptr<td_api::chatTheme> get_chat_theme_object(const ChatTheme &theme) const;

  td_api::object_ptr<td_api::updateChatThemes> get_update_chat_themes_object() const;

  static string get_chat_themes_database_key();

  void save_chat_themes();

  void send_update_chat_themes() const;

  static BaseTheme get_base_theme(const telegram_api::object_ptr<telegram_api::BaseTheme> &base_theme);

  ThemeSettings get_chat_theme_settings(telegram_api::object_ptr<telegram_api::themeSettings> settings);

  ChatThemes chat_themes_;

  Td *td_;
  ActorShared<> parent_;
};

}  // namespace td
