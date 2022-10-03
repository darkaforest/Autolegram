//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/DialogId.h"
#include "td/telegram/Dimensions.h"
#include "td/telegram/files/FileId.h"
#include "td/telegram/files/FileSourceId.h"
#include "td/telegram/FullMessageId.h"
#include "td/telegram/PhotoFormat.h"
#include "td/telegram/PhotoSize.h"
#include "td/telegram/SecretInputMedia.h"
#include "td/telegram/SpecialStickerSetType.h"
#include "td/telegram/StickerFormat.h"
#include "td/telegram/StickerSetId.h"
#include "td/telegram/StickerType.h"
#include "td/telegram/td_api.h"
#include "td/telegram/telegram_api.h"

#include "td/actor/actor.h"
#include "td/actor/MultiPromise.h"
#include "td/actor/Timeout.h"

#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/FlatHashMap.h"
#include "td/utils/FlatHashSet.h"
#include "td/utils/Hints.h"
#include "td/utils/Promise.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/WaitFreeHashMap.h"
#include "td/utils/WaitFreeHashSet.h"

#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace td {

class Td;

class StickersManager final : public Actor {
 public:
  static constexpr int64 GREAT_MINDS_SET_ID = 1842540969984001;

  static vector<StickerSetId> convert_sticker_set_ids(const vector<int64> &sticker_set_ids);
  static vector<int64> convert_sticker_set_ids(const vector<StickerSetId> &sticker_set_ids);

  StickersManager(Td *td, ActorShared<> parent);
  StickersManager(const StickersManager &) = delete;
  StickersManager &operator=(const StickersManager &) = delete;
  StickersManager(StickersManager &&) = delete;
  StickersManager &operator=(StickersManager &&) = delete;
  ~StickersManager() final;

  void init();

  StickerType get_sticker_type(FileId file_id) const;

  bool is_premium_custom_emoji(int64 custom_emoji_id, bool default_result) const;

  tl_object_ptr<td_api::sticker> get_sticker_object(FileId file_id, bool for_animated_emoji = false,
                                                    bool for_clicked_animated_emoji = false) const;

  tl_object_ptr<td_api::stickers> get_stickers_object(const vector<FileId> &sticker_ids) const;

  tl_object_ptr<td_api::DiceStickers> get_dice_stickers_object(const string &emoji, int32 value) const;

  int32 get_dice_success_animation_frame_number(const string &emoji, int32 value) const;

  tl_object_ptr<td_api::stickerSet> get_sticker_set_object(StickerSetId sticker_set_id) const;

  tl_object_ptr<td_api::stickerSets> get_sticker_sets_object(int32 total_count,
                                                             const vector<StickerSetId> &sticker_set_ids,
                                                             size_t covers_limit) const;

  td_api::object_ptr<td_api::sticker> get_premium_gift_sticker_object(int32 month_count);

  td_api::object_ptr<td_api::animatedEmoji> get_animated_emoji_object(const string &emoji, int64 custom_emoji_id);

  tl_object_ptr<telegram_api::InputStickerSet> get_input_sticker_set(StickerSetId sticker_set_id) const;

  void register_premium_gift(int32 months, FullMessageId full_message_id, const char *source);

  void unregister_premium_gift(int32 months, FullMessageId full_message_id, const char *source);

  void register_dice(const string &emoji, int32 value, FullMessageId full_message_id, const char *source);

  void unregister_dice(const string &emoji, int32 value, FullMessageId full_message_id, const char *source);

  void register_emoji(const string &emoji, int64 custom_emoji_id, FullMessageId full_message_id, const char *source);

  void unregister_emoji(const string &emoji, int64 custom_emoji_id, FullMessageId full_message_id, const char *source);

  void get_animated_emoji(string emoji, bool is_recursive,
                          Promise<td_api::object_ptr<td_api::animatedEmoji>> &&promise);

  void get_all_animated_emojis(bool is_recursive, Promise<td_api::object_ptr<td_api::emojis>> &&promise);

  void get_custom_emoji_reaction_generic_animations(bool is_recursive,
                                                    Promise<td_api::object_ptr<td_api::files>> &&promise);

  void get_default_emoji_statuses(bool is_recursive, Promise<td_api::object_ptr<td_api::emojiStatuses>> &&promise);

  bool is_default_emoji_status(int64 custom_emoji_id);

  void get_custom_emoji_stickers(vector<int64> &&document_ids, bool use_database,
                                 Promise<td_api::object_ptr<td_api::stickers>> &&promise);

  void get_premium_gift_option_sticker(int32 month_count, bool is_recursive,
                                       Promise<td_api::object_ptr<td_api::sticker>> &&promise);

  void get_animated_emoji_click_sticker(const string &message_text, FullMessageId full_message_id,
                                        Promise<td_api::object_ptr<td_api::sticker>> &&promise);

  void on_send_animated_emoji_clicks(DialogId dialog_id, const string &emoji);

  bool is_sent_animated_emoji_click(DialogId dialog_id, const string &emoji);

  Status on_animated_emoji_message_clicked(string &&emoji, FullMessageId full_message_id, string data);

  bool is_active_reaction(const string &reaction) const;

  void create_sticker(FileId file_id, FileId premium_animation_file_id, string minithumbnail, PhotoSize thumbnail,
                      Dimensions dimensions, tl_object_ptr<telegram_api::documentAttributeSticker> sticker,
                      tl_object_ptr<telegram_api::documentAttributeCustomEmoji> custom_emoji,
                      StickerFormat sticker_format, MultiPromiseActor *load_data_multipromise_ptr);

  bool has_input_media(FileId sticker_file_id, bool is_secret) const;

  tl_object_ptr<telegram_api::InputMedia> get_input_media(FileId file_id,
                                                          tl_object_ptr<telegram_api::InputFile> input_file,
                                                          tl_object_ptr<telegram_api::InputFile> input_thumbnail,
                                                          const string &emoji) const;

  SecretInputMedia get_secret_input_media(FileId sticker_file_id,
                                          tl_object_ptr<telegram_api::InputEncryptedFile> input_file,
                                          BufferSlice thumbnail, int32 layer) const;

  vector<FileId> get_stickers(StickerType sticker_type, string emoji, int32 limit, DialogId dialog_id, bool force,
                              Promise<Unit> &&promise);

  void search_stickers(string emoji, int32 limit, Promise<td_api::object_ptr<td_api::stickers>> &&promise);

  void get_premium_stickers(int32 limit, Promise<td_api::object_ptr<td_api::stickers>> &&promise);

  vector<StickerSetId> get_installed_sticker_sets(StickerType sticker_type, Promise<Unit> &&promise);

  static bool has_webp_thumbnail(const vector<tl_object_ptr<telegram_api::PhotoSize>> &thumbnails);

  StickerSetId get_sticker_set_id(const tl_object_ptr<telegram_api::InputStickerSet> &set_ptr);

  StickerSetId add_sticker_set(tl_object_ptr<telegram_api::InputStickerSet> &&set_ptr);

  StickerSetId get_sticker_set(StickerSetId set_id, Promise<Unit> &&promise);

  StickerSetId search_sticker_set(const string &short_name_to_search, Promise<Unit> &&promise);

  std::pair<int32, vector<StickerSetId>> search_installed_sticker_sets(StickerType sticker_type, const string &query,
                                                                       int32 limit, Promise<Unit> &&promise);

  vector<StickerSetId> search_sticker_sets(const string &query, Promise<Unit> &&promise);

  void change_sticker_set(StickerSetId set_id, bool is_installed, bool is_archived, Promise<Unit> &&promise);

  void view_featured_sticker_sets(const vector<StickerSetId> &sticker_set_ids);

  td_api::object_ptr<td_api::emojiReaction> get_emoji_reaction_object(const string &emoji);

  vector<string> get_recent_reactions();

  vector<string> get_top_reactions();

  void add_recent_reaction(const string &reaction);

  void clear_recent_reactions(Promise<Unit> &&promise);

  void reload_reactions();

  void reload_recent_reactions();

  void reload_top_reactions();

  void reload_special_sticker_set_by_type(SpecialStickerSetType type, bool is_recursive = false);

  void on_get_available_reactions(tl_object_ptr<telegram_api::messages_AvailableReactions> &&available_reactions_ptr);

  void on_get_recent_reactions(tl_object_ptr<telegram_api::messages_Reactions> &&reactions_ptr);

  void on_get_top_reactions(tl_object_ptr<telegram_api::messages_Reactions> &&reactions_ptr);

  void on_get_installed_sticker_sets(StickerType sticker_type,
                                     tl_object_ptr<telegram_api::messages_AllStickers> &&stickers_ptr);

  void on_get_installed_sticker_sets_failed(StickerType sticker_type, Status error);

  StickerSetId on_get_messages_sticker_set(StickerSetId sticker_set_id,
                                           tl_object_ptr<telegram_api::messages_StickerSet> &&set_ptr, bool is_changed,
                                           const char *source);

  StickerSetId on_get_sticker_set(tl_object_ptr<telegram_api::stickerSet> &&set, bool is_changed, const char *source);

  StickerSetId on_get_sticker_set_covered(tl_object_ptr<telegram_api::StickerSetCovered> &&set_ptr, bool is_changed,
                                          const char *source);

  void on_get_special_sticker_set(const SpecialStickerSetType &type, StickerSetId sticker_set_id);

  void on_load_special_sticker_set(const SpecialStickerSetType &type, Status result);

  void on_load_sticker_set_fail(StickerSetId sticker_set_id, const Status &error);

  void on_install_sticker_set(StickerSetId set_id, bool is_archived,
                              tl_object_ptr<telegram_api::messages_StickerSetInstallResult> &&result);

  void on_uninstall_sticker_set(StickerSetId set_id);

  void on_update_animated_emoji_zoom();

  void on_update_disable_animated_emojis();

  void on_update_dice_emojis();

  void on_update_dice_success_values();

  void on_update_emoji_sounds();

  void on_update_sticker_sets(StickerType sticker_type);

  void on_update_sticker_sets_order(StickerType sticker_type, const vector<StickerSetId> &sticker_set_ids);

  void on_update_move_sticker_set_to_top(StickerType sticker_type, StickerSetId sticker_set_id);

  std::pair<int32, vector<StickerSetId>> get_archived_sticker_sets(StickerType sticker_type,
                                                                   StickerSetId offset_sticker_set_id, int32 limit,
                                                                   bool force, Promise<Unit> &&promise);

  void on_get_archived_sticker_sets(StickerType sticker_type, StickerSetId offset_sticker_set_id,
                                    vector<tl_object_ptr<telegram_api::StickerSetCovered>> &&sticker_sets,
                                    int32 total_count);

  td_api::object_ptr<td_api::trendingStickerSets> get_featured_sticker_sets(StickerType sticker_type, int32 offset,
                                                                            int32 limit, Promise<Unit> &&promise);

  void on_get_featured_sticker_sets(StickerType sticker_type, int32 offset, int32 limit, uint32 generation,
                                    tl_object_ptr<telegram_api::messages_FeaturedStickers> &&sticker_sets_ptr);

  void on_get_featured_sticker_sets_failed(StickerType sticker_type, int32 offset, int32 limit, uint32 generation,
                                           Status error);

  vector<StickerSetId> get_attached_sticker_sets(FileId file_id, Promise<Unit> &&promise);

  void on_get_attached_sticker_sets(FileId file_id,
                                    vector<tl_object_ptr<telegram_api::StickerSetCovered>> &&sticker_sets);

  void reorder_installed_sticker_sets(StickerType sticker_type, const vector<StickerSetId> &sticker_set_ids,
                                      Promise<Unit> &&promise);

  void move_sticker_set_to_top_by_sticker_id(FileId sticker_id);

  void move_sticker_set_to_top_by_custom_emoji_ids(const vector<int64> &custom_emoji_ids);

  FileId upload_sticker_file(UserId user_id, tl_object_ptr<td_api::inputSticker> &&sticker, Promise<Unit> &&promise);

  void get_suggested_sticker_set_name(string title, Promise<string> &&promise);

  enum class CheckStickerSetNameResult : uint8 { Ok, Invalid, Occupied };
  void check_sticker_set_name(const string &name, Promise<CheckStickerSetNameResult> &&promise);

  static td_api::object_ptr<td_api::CheckStickerSetNameResult> get_check_sticker_set_name_result_object(
      CheckStickerSetNameResult result);

  void create_new_sticker_set(UserId user_id, string title, string short_name, StickerType sticker_type,
                              vector<td_api::object_ptr<td_api::inputSticker>> &&stickers, string software,
                              Promise<td_api::object_ptr<td_api::stickerSet>> &&promise);

  void add_sticker_to_set(UserId user_id, string short_name, tl_object_ptr<td_api::inputSticker> &&sticker,
                          Promise<td_api::object_ptr<td_api::stickerSet>> &&promise);

  void set_sticker_set_thumbnail(UserId user_id, string short_name, tl_object_ptr<td_api::InputFile> &&thumbnail,
                                 Promise<td_api::object_ptr<td_api::stickerSet>> &&promise);

  void set_sticker_position_in_set(const tl_object_ptr<td_api::InputFile> &sticker, int32 position,
                                   Promise<Unit> &&promise);

  void remove_sticker_from_set(const tl_object_ptr<td_api::InputFile> &sticker, Promise<Unit> &&promise);

  vector<FileId> get_recent_stickers(bool is_attached, Promise<Unit> &&promise);

  void on_get_recent_stickers(bool is_repair, bool is_attached,
                              tl_object_ptr<telegram_api::messages_RecentStickers> &&stickers_ptr);

  void on_get_recent_stickers_failed(bool is_repair, bool is_attached, Status error);

  FileSourceId get_recent_stickers_file_source_id(int is_attached);

  void add_recent_sticker(bool is_attached, const tl_object_ptr<td_api::InputFile> &input_file,
                          Promise<Unit> &&promise);

  void add_recent_sticker_by_id(bool is_attached, FileId sticker_id);

  void remove_recent_sticker(bool is_attached, const tl_object_ptr<td_api::InputFile> &input_file,
                             Promise<Unit> &&promise);

  void send_save_recent_sticker_query(bool is_attached, FileId sticker_id, bool unsave, Promise<Unit> &&promise);

  void clear_recent_stickers(bool is_attached, Promise<Unit> &&promise);

  void on_update_recent_stickers_limit();

  void on_update_favorite_stickers_limit();

  void reload_favorite_stickers(bool force);

  void repair_favorite_stickers(Promise<Unit> &&promise);

  void on_get_favorite_stickers(bool is_repair,
                                tl_object_ptr<telegram_api::messages_FavedStickers> &&favorite_stickers_ptr);

  void on_get_favorite_stickers_failed(bool is_repair, Status error);

  FileSourceId get_app_config_file_source_id();

  FileSourceId get_favorite_stickers_file_source_id();

  vector<FileId> get_favorite_stickers(Promise<Unit> &&promise);

  void add_favorite_sticker(const tl_object_ptr<td_api::InputFile> &input_file, Promise<Unit> &&promise);

  void add_favorite_sticker_by_id(FileId sticker_id);

  void remove_favorite_sticker(const tl_object_ptr<td_api::InputFile> &input_file, Promise<Unit> &&promise);

  void send_fave_sticker_query(FileId sticker_id, bool unsave, Promise<Unit> &&promise);

  vector<FileId> get_attached_sticker_file_ids(const vector<int32> &int_file_ids);

  vector<string> get_sticker_emojis(const tl_object_ptr<td_api::InputFile> &input_file, Promise<Unit> &&promise);

  vector<string> search_emojis(const string &text, bool exact_match, const vector<string> &input_language_codes,
                               bool force, Promise<Unit> &&promise);

  int64 get_emoji_suggestions_url(const string &language_code, Promise<Unit> &&promise);

  td_api::object_ptr<td_api::httpUrl> get_emoji_suggestions_url_result(int64 random_id);

  void reload_sticker_set(StickerSetId sticker_set_id, int64 access_hash, Promise<Unit> &&promise);

  void reload_installed_sticker_sets(StickerType sticker_type, bool force);

  void reload_featured_sticker_sets(StickerType sticker_type, bool force);

  void reload_recent_stickers(bool is_attached, bool force);

  void repair_recent_stickers(bool is_attached, Promise<Unit> &&promise);

  FileId get_sticker_thumbnail_file_id(FileId file_id) const;

  vector<FileId> get_sticker_file_ids(FileId file_id) const;

  void delete_sticker_thumbnail(FileId file_id);

  FileId dup_sticker(FileId new_id, FileId old_id);

  void merge_stickers(FileId new_id, FileId old_id);

  template <class StorerT>
  void store_sticker(FileId file_id, bool in_sticker_set, StorerT &storer, const char *source) const;

  template <class ParserT>
  FileId parse_sticker(bool in_sticker_set, ParserT &parser);

  void on_uploaded_sticker_file(FileId file_id, tl_object_ptr<telegram_api::MessageMedia> media,
                                Promise<Unit> &&promise);

  void on_find_stickers_success(const string &emoji, tl_object_ptr<telegram_api::messages_Stickers> &&stickers);

  void on_find_stickers_fail(const string &emoji, Status &&error);

  void on_find_sticker_sets_success(const string &query,
                                    tl_object_ptr<telegram_api::messages_FoundStickerSets> &&sticker_sets);

  void on_find_sticker_sets_fail(const string &query, Status &&error);

  void send_get_attached_stickers_query(FileId file_id, Promise<Unit> &&promise);

  void get_current_state(vector<td_api::object_ptr<td_api::Update>> &updates) const;

  template <class StorerT>
  void store_sticker_set_id(StickerSetId sticker_set_id, StorerT &storer) const;

  template <class ParserT>
  void parse_sticker_set_id(StickerSetId &sticker_set_id, ParserT &parser);

 private:
  static constexpr int32 MAX_FEATURED_STICKER_SET_VIEW_DELAY = 5;
  static constexpr int32 OLD_FEATURED_STICKER_SET_SLICE_SIZE = 20;

  static constexpr int32 MAX_FOUND_STICKERS = 100;                 // server side limit
  static constexpr size_t MAX_STICKER_SET_TITLE_LENGTH = 64;       // server side limit
  static constexpr size_t MAX_STICKER_SET_SHORT_NAME_LENGTH = 64;  // server side limit
  static constexpr size_t MAX_GET_CUSTOM_EMOJI_STICKERS = 200;     // server-side limit

  static constexpr int32 EMOJI_KEYWORDS_UPDATE_DELAY = 3600;
  static constexpr double MIN_ANIMATED_EMOJI_CLICK_DELAY = 0.2;

  static constexpr int32 MAX_RECENT_REACTIONS = 100;  // some reasonable value

  class Sticker {
   public:
    StickerSetId set_id_;
    string alt_;
    Dimensions dimensions_;
    string minithumbnail_;
    PhotoSize s_thumbnail_;
    PhotoSize m_thumbnail_;
    FileId premium_animation_file_id_;
    FileId file_id_;
    StickerFormat format_ = StickerFormat::Unknown;
    StickerType type_ = StickerType::Regular;
    bool is_premium_ = false;
    bool is_from_database_ = false;
    bool is_being_reloaded_ = false;
    int32 point_ = -1;
    double x_shift_ = 0;
    double y_shift_ = 0;
    double scale_ = 0;
    int32 emoji_receive_date_ = 0;
  };

  class StickerSet {
   public:
    bool is_inited_ = false;  // basic information about the set
    bool was_loaded_ = false;
    bool is_loaded_ = false;

    StickerSetId id_;
    int64 access_hash_ = 0;
    string title_;
    string short_name_;
    StickerFormat sticker_format_ = StickerFormat::Unknown;
    StickerType sticker_type_ = StickerType::Regular;
    int32 sticker_count_ = 0;
    int32 hash_ = 0;
    int32 expires_at_ = 0;

    string minithumbnail_;
    PhotoSize thumbnail_;
    int64 thumbnail_document_id_ = 0;

    vector<FileId> sticker_ids_;
    vector<int32> premium_sticker_positions_;
    FlatHashMap<string, vector<FileId>> emoji_stickers_map_;              // emoji -> stickers
    FlatHashMap<FileId, vector<string>, FileIdHash> sticker_emojis_map_;  // sticker -> emojis

    bool is_installed_ = false;
    bool is_archived_ = false;
    bool is_official_ = false;
    bool is_viewed_ = true;
    bool is_thumbnail_reloaded_ = false;
    bool are_legacy_sticker_thumbnails_reloaded_ = false;
    mutable bool was_update_sent_ = false;  // does the sticker set is known to the client
    bool is_changed_ = true;                // have new changes that need to be sent to the client and database
    bool need_save_to_database_ = true;     // have new changes that need only to be saved to the database

    vector<uint32> load_requests_;
    vector<uint32> load_without_stickers_requests_;
  };

  struct PendingNewStickerSet {
    MultiPromiseActor upload_files_multipromise_{"UploadNewStickerSetFilesMultiPromiseActor"};
    UserId user_id_;
    string title_;
    string short_name_;
    StickerType sticker_type_ = StickerType::Regular;
    StickerFormat sticker_format_ = StickerFormat::Unknown;
    vector<FileId> file_ids_;
    vector<tl_object_ptr<td_api::inputSticker>> stickers_;
    string software_;
    Promise<td_api::object_ptr<td_api::stickerSet>> promise_;
  };

  struct PendingAddStickerToSet {
    string short_name_;
    FileId file_id_;
    tl_object_ptr<td_api::inputSticker> sticker_;
    Promise<td_api::object_ptr<td_api::stickerSet>> promise_;
  };

  struct PendingSetStickerSetThumbnail {
    string short_name_;
    FileId file_id_;
    Promise<td_api::object_ptr<td_api::stickerSet>> promise_;
  };

  struct PendingGetAnimatedEmojiClickSticker {
    string message_text_;
    FullMessageId full_message_id_;
    double start_time_ = 0;
    Promise<td_api::object_ptr<td_api::sticker>> promise_;
  };

  struct PendingOnAnimatedEmojiClicked {
    string emoji_;
    FullMessageId full_message_id_;
    vector<std::pair<int, double>> clicks_;
  };

  struct SpecialStickerSet {
    StickerSetId id_;
    int64 access_hash_ = 0;
    string short_name_;
    SpecialStickerSetType type_;
    bool is_being_loaded_ = false;
    bool is_being_reloaded_ = false;
  };

  struct FoundStickers {
    vector<FileId> sticker_ids_;
    int32 cache_time_ = 300;
    double next_reload_time_ = 0;

    template <class StorerT>
    void store(StorerT &storer) const;

    template <class ParserT>
    void parse(ParserT &parser);
  };

  struct Reaction {
    string reaction_;
    string title_;
    bool is_active_ = false;
    bool is_premium_ = false;
    FileId static_icon_;
    FileId appear_animation_;
    FileId select_animation_;
    FileId activate_animation_;
    FileId effect_animation_;
    FileId around_animation_;
    FileId center_animation_;

    bool is_valid() const {
      return static_icon_.is_valid() && appear_animation_.is_valid() && select_animation_.is_valid() &&
             activate_animation_.is_valid() && effect_animation_.is_valid() && !reaction_.empty();
    }

    template <class StorerT>
    void store(StorerT &storer) const;

    template <class ParserT>
    void parse(ParserT &parser);
  };

  struct Reactions {
    int32 hash_ = 0;
    bool are_being_reloaded_ = false;
    vector<Reaction> reactions_;

    template <class StorerT>
    void store(StorerT &storer) const;

    template <class ParserT>
    void parse(ParserT &parser);
  };

  struct ReactionList {
    int64 hash_ = 0;
    bool is_being_reloaded_ = false;
    vector<string> reactions_;

    template <class StorerT>
    void store(StorerT &storer) const;

    template <class ParserT>
    void parse(ParserT &parser);
  };

  class CustomEmojiLogEvent;
  class StickerListLogEvent;
  class StickerSetListLogEvent;

  class UploadStickerFileCallback;

  int64 get_custom_emoji_id(FileId sticker_id) const;

  static vector<td_api::object_ptr<td_api::closedVectorPath>> get_sticker_minithumbnail(CSlice path,
                                                                                        StickerSetId sticker_set_id,
                                                                                        int64 document_id, double zoom);

  static double get_sticker_set_minithumbnail_zoom(const StickerSet *sticker_set);

  static tl_object_ptr<td_api::MaskPoint> get_mask_point_object(int32 point);

  td_api::object_ptr<td_api::thumbnail> get_sticker_set_thumbnail_object(const StickerSet *sticker_set) const;

  tl_object_ptr<td_api::stickerSetInfo> get_sticker_set_info_object(StickerSetId sticker_set_id, size_t covers_limit,
                                                                    bool prefer_premium) const;

  Sticker *get_sticker(FileId file_id);
  const Sticker *get_sticker(FileId file_id) const;

  static string get_found_stickers_database_key(const string &emoji);

  void on_load_found_stickers_from_database(string emoji, string value);

  void on_search_stickers_finished(const string &emoji, const FoundStickers &found_stickers);

  static string get_custom_emoji_database_key(int64 custom_emoji_id);

  void load_custom_emoji_sticker_from_database_force(int64 custom_emoji_id);

  void load_custom_emoji_sticker_from_database(int64 custom_emoji_id, Promise<Unit> &&promise);

  void on_load_custom_emoji_from_database(int64 custom_emoji_id, string value);

  FileId on_get_sticker(unique_ptr<Sticker> new_sticker, bool replace);

  StickerSet *get_sticker_set(StickerSetId sticker_set_id);
  const StickerSet *get_sticker_set(StickerSetId sticker_set_id) const;

  StickerSet *add_sticker_set(StickerSetId sticker_set_id, int64 access_hash);

  static PhotoFormat get_sticker_set_thumbnail_format(StickerFormat sticker_format);

  std::pair<int64, FileId> on_get_sticker_document(tl_object_ptr<telegram_api::Document> &&document_ptr,
                                                   StickerFormat expected_format);

  static tl_object_ptr<telegram_api::InputStickerSet> get_input_sticker_set(const StickerSet *set);

  StickerSetId on_get_input_sticker_set(FileId sticker_file_id, tl_object_ptr<telegram_api::InputStickerSet> &&set_ptr,
                                        MultiPromiseActor *load_data_multipromise_ptr = nullptr);

  string get_sticker_set_short_name(FileId sticker_id) const;

  void on_resolve_sticker_set_short_name(FileId sticker_file_id, const string &short_name);

  int apply_installed_sticker_sets_order(StickerType sticker_type, const vector<StickerSetId> &sticker_set_ids);

  int move_installed_sticker_set_to_top(StickerType sticker_type, StickerSetId sticker_set_id);

  void on_update_sticker_set(StickerSet *sticker_set, bool is_installed, bool is_archived, bool is_changed,
                             bool from_database = false);

  static string get_sticker_set_database_key(StickerSetId set_id);

  static string get_full_sticker_set_database_key(StickerSetId set_id);

  string get_sticker_set_database_value(const StickerSet *s, bool with_stickers, const char *source);

  void update_sticker_set(StickerSet *sticker_set, const char *source);

  void load_sticker_sets(vector<StickerSetId> &&sticker_set_ids, Promise<Unit> &&promise);

  void load_sticker_sets_without_stickers(vector<StickerSetId> &&sticker_set_ids, Promise<Unit> &&promise);

  void on_load_sticker_set_from_database(StickerSetId sticker_set_id, bool with_stickers, string value);

  void update_load_requests(StickerSet *sticker_set, bool with_stickers, const Status &status);

  void update_load_request(uint32 load_request_id, const Status &status);

  void do_reload_sticker_set(StickerSetId sticker_set_id,
                             tl_object_ptr<telegram_api::InputStickerSet> &&input_sticker_set, int32 hash,
                             Promise<Unit> &&promise) const;

  void do_get_premium_stickers(int32 limit, Promise<td_api::object_ptr<td_api::stickers>> &&promise);

  static void read_featured_sticker_sets(void *td_void);

  int64 get_sticker_sets_hash(const vector<StickerSetId> &sticker_set_ids) const;

  int64 get_featured_sticker_sets_hash(StickerType sticker_type) const;

  int64 get_recent_stickers_hash(const vector<FileId> &sticker_ids) const;

  void load_installed_sticker_sets(StickerType sticker_type, Promise<Unit> &&promise);

  void load_featured_sticker_sets(StickerType sticker_type, Promise<Unit> &&promise);

  void load_old_featured_sticker_sets(StickerType sticker_type, Promise<Unit> &&promise);

  void load_recent_stickers(bool is_attached, Promise<Unit> &&promise);

  void on_load_installed_sticker_sets_from_database(StickerType sticker_type, string value);

  void on_load_installed_sticker_sets_finished(StickerType sticker_type,
                                               vector<StickerSetId> &&installed_sticker_set_ids,
                                               bool from_database = false);

  void on_load_featured_sticker_sets_from_database(StickerType sticker_type, string value);

  void on_load_featured_sticker_sets_finished(StickerType sticker_type, vector<StickerSetId> &&featured_sticker_set_ids,
                                              bool is_premium);

  void on_load_old_featured_sticker_sets_from_database(StickerType sticker_type, uint32 generation, string value);

  void on_load_old_featured_sticker_sets_finished(StickerType sticker_type, uint32 generation,
                                                  vector<StickerSetId> &&old_featured_sticker_set_ids);

  void on_load_recent_stickers_from_database(bool is_attached, string value);

  void on_load_recent_stickers_finished(bool is_attached, vector<FileId> &&recent_sticker_ids,
                                        bool from_database = false);

  td_api::object_ptr<td_api::updateInstalledStickerSets> get_update_installed_sticker_sets_object(
      StickerType sticker_type) const;

  void send_update_installed_sticker_sets(bool from_database = false);

  void reload_old_featured_sticker_sets(StickerType sticker_type, uint32 generation = 0);

  void on_old_featured_sticker_sets_invalidated(StickerType sticker_type);

  void invalidate_old_featured_sticker_sets(StickerType sticker_type);

  void set_old_featured_sticker_set_count(StickerType sticker_type, int32 count);

  // must be called after every call to set_old_featured_sticker_set_count or
  // any change of old_featured_sticker_set_ids_ size
  void fix_old_featured_sticker_set_count(StickerType sticker_type);

  static size_t get_max_featured_sticker_count(StickerType sticker_type);

  static Slice get_featured_sticker_suffix(StickerType sticker_type);

  td_api::object_ptr<td_api::trendingStickerSets> get_trending_sticker_sets_object(
      StickerType sticker_type, const vector<StickerSetId> &sticker_set_ids) const;

  td_api::object_ptr<td_api::updateTrendingStickerSets> get_update_trending_sticker_sets_object(
      StickerType sticker_type) const;

  void send_update_featured_sticker_sets(StickerType sticker_type);

  td_api::object_ptr<td_api::updateRecentStickers> get_update_recent_stickers_object(int is_attached) const;

  void send_update_recent_stickers(bool is_attached, bool from_database = false);

  void save_recent_stickers_to_database(bool is_attached);

  void add_recent_sticker_impl(bool is_attached, FileId sticker_id, bool add_on_server, Promise<Unit> &&promise);

  int64 get_favorite_stickers_hash() const;

  void add_favorite_sticker_impl(FileId sticker_id, bool add_on_server, Promise<Unit> &&promise);

  void load_favorite_stickers(Promise<Unit> &&promise);

  void on_load_favorite_stickers_from_database(const string &value);

  void on_load_favorite_stickers_finished(vector<FileId> &&favorite_sticker_ids, bool from_database = false);

  td_api::object_ptr<td_api::updateFavoriteStickers> get_update_favorite_stickers_object() const;

  void send_update_favorite_stickers(bool from_database = false);

  void save_favorite_stickers_to_database();

  template <class StorerT>
  void store_sticker_set(const StickerSet *sticker_set, bool with_stickers, StorerT &storer, const char *source) const;

  template <class ParserT>
  void parse_sticker_set(StickerSet *sticker_set, ParserT &parser);

  std::pair<vector<FileId>, vector<FileId>> split_stickers_by_premium(const vector<FileId> &sticker_ids) const;

  std::pair<vector<FileId>, vector<FileId>> split_stickers_by_premium(const StickerSet *sticker_set) const;

  Result<std::tuple<FileId, bool, bool, StickerFormat>> prepare_input_file(
      const tl_object_ptr<td_api::InputFile> &input_file, StickerFormat format, StickerType type, bool for_thumbnail);

  Result<std::tuple<FileId, bool, bool, StickerFormat>> prepare_input_sticker(td_api::inputSticker *sticker,
                                                                              StickerType sticker_type);

  tl_object_ptr<telegram_api::inputStickerSetItem> get_input_sticker(const td_api::inputSticker *sticker,
                                                                     FileId file_id) const;

  void upload_sticker_file(UserId user_id, FileId file_id, Promise<Unit> &&promise);

  void on_upload_sticker_file(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file);

  void on_upload_sticker_file_error(FileId file_id, Status status);

  void do_upload_sticker_file(UserId user_id, FileId file_id, tl_object_ptr<telegram_api::InputFile> &&input_file,
                              Promise<Unit> &&promise);

  void on_new_stickers_uploaded(int64 random_id, Result<Unit> result);

  void on_added_sticker_uploaded(int64 random_id, Result<Unit> result);

  void do_add_sticker_to_set(UserId user_id, string short_name, tl_object_ptr<td_api::inputSticker> &&sticker,
                             Promise<td_api::object_ptr<td_api::stickerSet>> &&promise);

  void on_sticker_set_thumbnail_uploaded(int64 random_id, Result<Unit> result);

  void do_set_sticker_set_thumbnail(UserId user_id, string short_name, tl_object_ptr<td_api::InputFile> &&thumbnail,
                                    Promise<td_api::object_ptr<td_api::stickerSet>> &&promise);

  bool update_sticker_set_cache(const StickerSet *sticker_set, Promise<Unit> &promise);

  const StickerSet *get_premium_gift_sticker_set();

  static FileId get_premium_gift_option_sticker_id(const StickerSet *sticker_set, int32 month_count);

  FileId get_premium_gift_option_sticker_id(int32 month_count);

  void try_update_premium_gift_messages();

  const StickerSet *get_animated_emoji_sticker_set();

  static std::pair<FileId, int> get_animated_emoji_sticker(const StickerSet *sticker_set, const string &emoji);

  std::pair<FileId, int> get_animated_emoji_sticker(const string &emoji);

  FileId get_animated_emoji_sound_file_id(const string &emoji) const;

  FileId get_custom_animated_emoji_sticker_id(int64 custom_emoji_id) const;

  td_api::object_ptr<td_api::animatedEmoji> get_animated_emoji_object(std::pair<FileId, int> animated_sticker,
                                                                      FileId sound_file_id) const;

  void try_update_animated_emoji_messages();

  void try_update_custom_emoji_messages(int64 custom_emoji_id);

  static int get_emoji_number(Slice emoji);

  vector<FileId> get_animated_emoji_click_stickers(const StickerSet *sticker_set, Slice emoji) const;

  void choose_animated_emoji_click_sticker(const StickerSet *sticker_set, string message_text,
                                           FullMessageId full_message_id, double start_time,
                                           Promise<td_api::object_ptr<td_api::sticker>> &&promise);

  void send_click_animated_emoji_message_response(FileId sticker_id,
                                                  Promise<td_api::object_ptr<td_api::sticker>> &&promise);

  void flush_sent_animated_emoji_clicks();

  void flush_pending_animated_emoji_clicks();

  void schedule_update_animated_emoji_clicked(const StickerSet *sticker_set, Slice emoji, FullMessageId full_message_id,
                                              vector<std::pair<int, double>> clicks);

  void send_update_animated_emoji_clicked(FullMessageId full_message_id, FileId sticker_id);

  td_api::object_ptr<td_api::updateDiceEmojis> get_update_dice_emojis_object() const;

  void start_up() final;

  void timeout_expired() final;

  void tear_down() final;

  void save_active_reactions();

  void save_reactions();

  void save_recent_reactions();

  void save_top_reactions();

  void load_active_reactions();

  void load_reactions();

  void load_recent_reactions();

  void load_top_reactions();

  void update_active_reactions();

  td_api::object_ptr<td_api::updateActiveEmojiReactions> get_update_active_emoji_reactions_object() const;

  SpecialStickerSet &add_special_sticker_set(const SpecialStickerSetType &type);

  static void init_special_sticker_set(SpecialStickerSet &sticker_set, int64 sticker_set_id, int64 access_hash,
                                       string name);

  void load_special_sticker_set_info_from_binlog(SpecialStickerSet &sticker_set);

  void load_special_sticker_set_by_type(SpecialStickerSetType type);

  void load_special_sticker_set(SpecialStickerSet &sticker_set);

  void reload_special_sticker_set(SpecialStickerSet &sticker_set, int32 hash);

  static void add_sticker_thumbnail(Sticker *s, PhotoSize thumbnail);

  td_api::object_ptr<td_api::stickers> get_custom_emoji_stickers_object(const vector<int64> &document_ids);

  void on_get_custom_emoji_documents(Result<vector<telegram_api::object_ptr<telegram_api::Document>>> &&r_documents,
                                     vector<int64> &&document_ids,
                                     Promise<td_api::object_ptr<td_api::stickers>> &&promise);

  static string get_emoji_language_code_version_database_key(const string &language_code);

  static string get_emoji_language_code_last_difference_time_database_key(const string &language_code);

  static string get_language_emojis_database_key(const string &language_code, const string &text);

  static string get_emoji_language_codes_database_key(const vector<string> &language_codes);

  int32 get_emoji_language_code_version(const string &language_code);

  double get_emoji_language_code_last_difference_time(const string &language_code);

  vector<string> get_emoji_language_codes(const vector<string> &input_language_codes, Slice text,
                                          Promise<Unit> &promise);

  void load_language_codes(vector<string> language_codes, string key, Promise<Unit> &&promise);

  void on_get_language_codes(const string &key, Result<vector<string>> &&result);

  static vector<string> search_language_emojis(const string &language_code, const string &text, bool exact_match);

  void load_emoji_keywords(const string &language_code, Promise<Unit> &&promise);

  void on_get_emoji_keywords(const string &language_code,
                             Result<telegram_api::object_ptr<telegram_api::emojiKeywordsDifference>> &&result);

  void load_emoji_keywords_difference(const string &language_code);

  void on_get_emoji_keywords_difference(
      const string &language_code, int32 from_version,
      Result<telegram_api::object_ptr<telegram_api::emojiKeywordsDifference>> &&result);

  void finish_get_emoji_keywords_difference(string language_code, int32 version);

  void on_get_emoji_suggestions_url(int64 random_id, Promise<Unit> &&promise,
                                    Result<telegram_api::object_ptr<telegram_api::emojiURL>> &&r_emoji_url);

  Td *td_;
  ActorShared<> parent_;

  bool is_inited_ = false;

  WaitFreeHashMap<FileId, unique_ptr<Sticker>, FileIdHash> stickers_;                     // file_id -> Sticker
  WaitFreeHashMap<StickerSetId, unique_ptr<StickerSet>, StickerSetIdHash> sticker_sets_;  // id -> StickerSet
  WaitFreeHashMap<string, StickerSetId> short_name_to_sticker_set_id_;

  vector<StickerSetId> installed_sticker_set_ids_[MAX_STICKER_TYPE];
  vector<StickerSetId> featured_sticker_set_ids_[MAX_STICKER_TYPE];
  vector<StickerSetId> old_featured_sticker_set_ids_[MAX_STICKER_TYPE];
  vector<FileId> recent_sticker_ids_[2];
  vector<FileId> favorite_sticker_ids_;

  double next_installed_sticker_sets_load_time_[MAX_STICKER_TYPE] = {0, 0, 0};
  double next_featured_sticker_sets_load_time_[MAX_STICKER_TYPE] = {0, 0, 0};
  double next_recent_stickers_load_time_[2] = {0, 0};
  double next_favorite_stickers_load_time_ = 0;

  int64 installed_sticker_sets_hash_[MAX_STICKER_TYPE] = {0, 0, 0};
  int64 featured_sticker_sets_hash_[MAX_STICKER_TYPE] = {0, 0, 0};
  int64 recent_stickers_hash_[2] = {0, 0};

  int32 old_featured_sticker_set_count_[MAX_STICKER_TYPE] = {-1, 0, 0};
  uint32 old_featured_sticker_set_generation_[MAX_STICKER_TYPE] = {1, 0, 0};

  bool need_update_installed_sticker_sets_[MAX_STICKER_TYPE] = {false, false, false};
  bool need_update_featured_sticker_sets_[MAX_STICKER_TYPE] = {false, false, false};

  bool are_installed_sticker_sets_loaded_[MAX_STICKER_TYPE] = {false, false, false};
  bool are_featured_sticker_sets_loaded_[MAX_STICKER_TYPE] = {false, true, false};
  bool are_recent_stickers_loaded_[2] = {false, false};
  bool are_favorite_stickers_loaded_ = false;

  bool are_featured_sticker_sets_premium_[MAX_STICKER_TYPE] = {false, false, false};
  bool are_old_featured_sticker_sets_invalidated_[MAX_STICKER_TYPE] = {false, false, false};

  vector<Promise<Unit>> load_installed_sticker_sets_queries_[MAX_STICKER_TYPE];
  vector<Promise<Unit>> load_featured_sticker_sets_queries_[MAX_STICKER_TYPE];
  vector<Promise<Unit>> load_old_featured_sticker_sets_queries_;
  vector<Promise<Unit>> load_recent_stickers_queries_[2];
  vector<Promise<Unit>> repair_recent_stickers_queries_[2];
  vector<Promise<Unit>> load_favorite_stickers_queries_;
  vector<Promise<Unit>> repair_favorite_stickers_queries_;

  vector<FileId> recent_sticker_file_ids_[2];
  FileSourceId recent_stickers_file_source_id_[2];
  vector<FileId> favorite_sticker_file_ids_;
  FileSourceId favorite_stickers_file_source_id_;

  FileSourceId app_config_file_source_id_;

  vector<StickerSetId> archived_sticker_set_ids_[MAX_STICKER_TYPE];
  int32 total_archived_sticker_set_count_[MAX_STICKER_TYPE] = {-1, -1, -1};

  FlatHashMap<FileId, vector<StickerSetId>, FileIdHash> attached_sticker_sets_;

  Hints installed_sticker_sets_hints_[MAX_STICKER_TYPE];  // search installed sticker sets by their title and name

  FlatHashMap<string, FoundStickers> found_stickers_;
  FlatHashMap<string, vector<std::pair<int32, Promise<td_api::object_ptr<td_api::stickers>>>>> search_stickers_queries_;

  std::unordered_map<string, vector<StickerSetId>> found_sticker_sets_;
  std::unordered_map<string, vector<Promise<Unit>>> search_sticker_sets_queries_;

  FlatHashSet<StickerSetId, StickerSetIdHash> pending_viewed_featured_sticker_set_ids_;
  Timeout pending_featured_sticker_set_views_timeout_;

  int32 recent_stickers_limit_ = 200;
  int32 favorite_stickers_limit_ = 5;

  FlatHashMap<SpecialStickerSetType, unique_ptr<SpecialStickerSet>, SpecialStickerSetTypeHash> special_sticker_sets_;

  struct StickerSetLoadRequest {
    Promise<Unit> promise_;
    Status error_;
    size_t left_queries_ = 0;
  };

  FlatHashMap<uint32, StickerSetLoadRequest> sticker_set_load_requests_;
  uint32 current_sticker_set_load_request_ = 0;

  FlatHashMap<int64, vector<Promise<Unit>>> custom_emoji_load_queries_;

  FlatHashMap<int64, unique_ptr<PendingNewStickerSet>> pending_new_sticker_sets_;

  FlatHashMap<int64, unique_ptr<PendingAddStickerToSet>> pending_add_sticker_to_sets_;

  FlatHashMap<int64, unique_ptr<PendingSetStickerSetThumbnail>> pending_set_sticker_set_thumbnails_;

  vector<Promise<Unit>> pending_get_animated_emoji_queries_;
  vector<Promise<Unit>> pending_get_premium_gift_option_sticker_queries_;
  vector<Promise<Unit>> pending_get_generic_animations_queries_;
  vector<Promise<Unit>> pending_get_default_statuses_queries_;

  double next_click_animated_emoji_message_time_ = 0;
  double next_update_animated_emoji_clicked_time_ = 0;
  vector<PendingGetAnimatedEmojiClickSticker> pending_get_animated_emoji_click_stickers_;
  vector<PendingOnAnimatedEmojiClicked> pending_on_animated_emoji_message_clicked_;

  string last_clicked_animated_emoji_;
  FullMessageId last_clicked_animated_emoji_full_message_id_;
  std::vector<std::pair<int, double>> pending_animated_emoji_clicks_;

  struct SentAnimatedEmojiClicks {
    double send_time_ = 0.0;
    DialogId dialog_id_;
    string emoji_;
  };
  std::vector<SentAnimatedEmojiClicks> sent_animated_emoji_clicks_;

  std::shared_ptr<UploadStickerFileCallback> upload_sticker_file_callback_;

  FlatHashMap<FileId, std::pair<UserId, Promise<Unit>>, FileIdHash> being_uploaded_files_;

  Reactions reactions_;
  vector<string> active_reactions_;

  ReactionList recent_reactions_;
  ReactionList top_reactions_;

  bool are_reactions_loaded_from_database_ = false;
  bool are_recent_reactions_loaded_from_database_ = false;
  bool are_top_reactions_loaded_from_database_ = false;

  FlatHashMap<string, vector<string>> emoji_language_codes_;
  FlatHashMap<string, int32> emoji_language_code_versions_;
  FlatHashMap<string, double> emoji_language_code_last_difference_times_;
  FlatHashSet<string> reloaded_emoji_keywords_;
  FlatHashMap<string, vector<Promise<Unit>>> load_emoji_keywords_queries_;
  FlatHashMap<string, vector<Promise<Unit>>> load_language_codes_queries_;
  FlatHashMap<int64, string> emoji_suggestions_urls_;

  struct GiftPremiumMessages {
    FlatHashSet<FullMessageId, FullMessageIdHash> full_message_ids_;
    FileId sticker_id_;
  };
  FlatHashMap<int32, unique_ptr<GiftPremiumMessages>> premium_gift_messages_;

  FlatHashMap<string, WaitFreeHashSet<FullMessageId, FullMessageIdHash>> dice_messages_;

  struct EmojiMessages {
    WaitFreeHashSet<FullMessageId, FullMessageIdHash> full_message_ids_;
    std::pair<FileId, int> animated_emoji_sticker_;
    FileId sound_file_id_;
  };
  FlatHashMap<string, unique_ptr<EmojiMessages>> emoji_messages_;

  struct CustomEmojiMessages {
    WaitFreeHashSet<FullMessageId, FullMessageIdHash> full_message_ids_;
    FileId sticker_id_;
  };
  FlatHashMap<int64, unique_ptr<CustomEmojiMessages>> custom_emoji_messages_;

  string dice_emojis_str_;
  vector<string> dice_emojis_;

  string dice_success_values_str_;
  vector<std::pair<int32, int32>> dice_success_values_;

  string emoji_sounds_str_;
  FlatHashMap<string, FileId> emoji_sounds_;

  WaitFreeHashMap<int64, FileId> custom_emoji_to_sticker_id_;

  double animated_emoji_zoom_ = 0.0;

  bool disable_animated_emojis_ = false;
};

}  // namespace td
