//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/AccessRights.h"
#include "td/telegram/AffectedHistory.h"
#include "td/telegram/ChannelId.h"
#include "td/telegram/ChatReactions.h"
#include "td/telegram/DialogAction.h"
#include "td/telegram/DialogDate.h"
#include "td/telegram/DialogDb.h"
#include "td/telegram/DialogFilterId.h"
#include "td/telegram/DialogId.h"
#include "td/telegram/DialogListId.h"
#include "td/telegram/DialogLocation.h"
#include "td/telegram/DialogParticipant.h"
#include "td/telegram/DialogSource.h"
#include "td/telegram/EncryptedFile.h"
#include "td/telegram/files/FileId.h"
#include "td/telegram/files/FileSourceId.h"
#include "td/telegram/FolderId.h"
#include "td/telegram/FullMessageId.h"
#include "td/telegram/Global.h"
#include "td/telegram/InputDialogId.h"
#include "td/telegram/InputGroupCallId.h"
#include "td/telegram/logevent/LogEventHelper.h"
#include "td/telegram/MessageContentType.h"
#include "td/telegram/MessageCopyOptions.h"
#include "td/telegram/MessageId.h"
#include "td/telegram/MessageLinkInfo.h"
#include "td/telegram/MessageReplyInfo.h"
#include "td/telegram/MessagesDb.h"
#include "td/telegram/MessageSearchFilter.h"
#include "td/telegram/MessageThreadInfo.h"
#include "td/telegram/MessageTtl.h"
#include "td/telegram/net/DcId.h"
#include "td/telegram/net/NetQuery.h"
#include "td/telegram/Notification.h"
#include "td/telegram/NotificationGroupId.h"
#include "td/telegram/NotificationGroupKey.h"
#include "td/telegram/NotificationGroupType.h"
#include "td/telegram/NotificationId.h"
#include "td/telegram/NotificationSettings.h"
#include "td/telegram/Photo.h"
#include "td/telegram/RecentDialogList.h"
#include "td/telegram/ReplyMarkup.h"
#include "td/telegram/ReportReason.h"
#include "td/telegram/RestrictionReason.h"
#include "td/telegram/ScheduledServerMessageId.h"
#include "td/telegram/secret_api.h"
#include "td/telegram/SecretChatId.h"
#include "td/telegram/SecretInputMedia.h"
#include "td/telegram/ServerMessageId.h"
#include "td/telegram/td_api.h"
#include "td/telegram/telegram_api.h"
#include "td/telegram/UserId.h"

#include "td/actor/actor.h"
#include "td/actor/MultiPromise.h"
#include "td/actor/MultiTimeout.h"
#include "td/actor/SignalSlot.h"
#include "td/actor/Timeout.h"

#include "td/utils/buffer.h"
#include "td/utils/ChangesProcessor.h"
#include "td/utils/common.h"
#include "td/utils/FlatHashMap.h"
#include "td/utils/FlatHashSet.h"
#include "td/utils/Heap.h"
#include "td/utils/Hints.h"
#include "td/utils/logging.h"
#include "td/utils/Promise.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/WaitFreeHashMap.h"
#include "td/utils/WaitFreeHashSet.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace td {

struct BinlogEvent;
class Dependencies;
class DialogActionBar;
class DialogFilter;
class DraftMessage;
struct InputMessageContent;
class MessageContent;
struct MessageReactions;
class Td;

class MessagesManager final : public Actor {
 public:
  //  static constexpr int32 MESSAGE_FLAG_IS_UNREAD = 1 << 0;
  static constexpr int32 MESSAGE_FLAG_IS_OUT = 1 << 1;
  static constexpr int32 MESSAGE_FLAG_IS_FORWARDED = 1 << 2;
  static constexpr int32 MESSAGE_FLAG_IS_REPLY = 1 << 3;
  static constexpr int32 MESSAGE_FLAG_HAS_MENTION = 1 << 4;
  static constexpr int32 MESSAGE_FLAG_HAS_UNREAD_CONTENT = 1 << 5;
  static constexpr int32 MESSAGE_FLAG_HAS_REPLY_MARKUP = 1 << 6;
  static constexpr int32 MESSAGE_FLAG_HAS_ENTITIES = 1 << 7;
  static constexpr int32 MESSAGE_FLAG_HAS_FROM_ID = 1 << 8;
  static constexpr int32 MESSAGE_FLAG_HAS_MEDIA = 1 << 9;
  static constexpr int32 MESSAGE_FLAG_HAS_INTERACTION_INFO = 1 << 10;
  static constexpr int32 MESSAGE_FLAG_IS_SENT_VIA_BOT = 1 << 11;
  static constexpr int32 MESSAGE_FLAG_IS_SILENT = 1 << 13;
  static constexpr int32 MESSAGE_FLAG_IS_POST = 1 << 14;
  static constexpr int32 MESSAGE_FLAG_HAS_EDIT_DATE = 1 << 15;
  static constexpr int32 MESSAGE_FLAG_HAS_AUTHOR_SIGNATURE = 1 << 16;
  static constexpr int32 MESSAGE_FLAG_HAS_MEDIA_ALBUM_ID = 1 << 17;
  static constexpr int32 MESSAGE_FLAG_IS_FROM_SCHEDULED = 1 << 18;
  static constexpr int32 MESSAGE_FLAG_IS_LEGACY = 1 << 19;
  static constexpr int32 MESSAGE_FLAG_HAS_REACTIONS = 1 << 20;
  static constexpr int32 MESSAGE_FLAG_HIDE_EDIT_DATE = 1 << 21;
  static constexpr int32 MESSAGE_FLAG_IS_RESTRICTED = 1 << 22;
  static constexpr int32 MESSAGE_FLAG_HAS_REPLY_INFO = 1 << 23;
  static constexpr int32 MESSAGE_FLAG_IS_PINNED = 1 << 24;
  static constexpr int32 MESSAGE_FLAG_HAS_TTL_PERIOD = 1 << 25;
  static constexpr int32 MESSAGE_FLAG_NOFORWARDS = 1 << 26;

  static constexpr int32 SEND_MESSAGE_FLAG_IS_REPLY = 1 << 0;
  static constexpr int32 SEND_MESSAGE_FLAG_DISABLE_WEB_PAGE_PREVIEW = 1 << 1;
  static constexpr int32 SEND_MESSAGE_FLAG_HAS_REPLY_MARKUP = 1 << 2;
  static constexpr int32 SEND_MESSAGE_FLAG_HAS_ENTITIES = 1 << 3;
  //  static constexpr int32 SEND_MESSAGE_FLAG_IS_POST = 1 << 4;
  static constexpr int32 SEND_MESSAGE_FLAG_DISABLE_NOTIFICATION = 1 << 5;
  static constexpr int32 SEND_MESSAGE_FLAG_FROM_BACKGROUND = 1 << 6;
  static constexpr int32 SEND_MESSAGE_FLAG_CLEAR_DRAFT = 1 << 7;
  static constexpr int32 SEND_MESSAGE_FLAG_WITH_MY_SCORE = 1 << 8;
  static constexpr int32 SEND_MESSAGE_FLAG_GROUP_MEDIA = 1 << 9;
  static constexpr int32 SEND_MESSAGE_FLAG_HAS_SCHEDULE_DATE = 1 << 10;
  static constexpr int32 SEND_MESSAGE_FLAG_HAS_MESSAGE = 1 << 11;
  static constexpr int32 SEND_MESSAGE_FLAG_HAS_SEND_AS = 1 << 13;
  static constexpr int32 SEND_MESSAGE_FLAG_NOFORWARDS = 1 << 14;
  static constexpr int32 SEND_MESSAGE_FLAG_UPDATE_STICKER_SETS_ORDER = 1 << 15;

  static constexpr int32 ONLINE_MEMBER_COUNT_CACHE_EXPIRE_TIME = 30 * 60;

  MessagesManager(Td *td, ActorShared<> parent);
  MessagesManager(const MessagesManager &) = delete;
  MessagesManager &operator=(const MessagesManager &) = delete;
  MessagesManager(MessagesManager &&) = delete;
  MessagesManager &operator=(MessagesManager &&) = delete;
  ~MessagesManager() final;

  static vector<MessageId> get_message_ids(const vector<int64> &input_message_ids);

  static vector<int32> get_server_message_ids(const vector<MessageId> &message_ids);

  static vector<int32> get_scheduled_server_message_ids(const vector<MessageId> &message_ids);

  static MessageId get_message_id(const tl_object_ptr<telegram_api::Message> &message_ptr, bool is_scheduled);

  static DialogId get_message_dialog_id(const tl_object_ptr<telegram_api::Message> &message_ptr);

  static FullMessageId get_full_message_id(const tl_object_ptr<telegram_api::Message> &message_ptr, bool is_scheduled);

  tl_object_ptr<telegram_api::InputPeer> get_input_peer(DialogId dialog_id, AccessRights access_rights) const;

  static tl_object_ptr<telegram_api::InputPeer> get_input_peer_force(DialogId dialog_id);

  vector<tl_object_ptr<telegram_api::InputPeer>> get_input_peers(const vector<DialogId> &dialog_ids,
                                                                 AccessRights access_rights) const;

  tl_object_ptr<telegram_api::InputDialogPeer> get_input_dialog_peer(DialogId dialog_id,
                                                                     AccessRights access_rights) const;

  vector<tl_object_ptr<telegram_api::InputDialogPeer>> get_input_dialog_peers(const vector<DialogId> &dialog_ids,
                                                                              AccessRights access_rights) const;

  tl_object_ptr<telegram_api::inputEncryptedChat> get_input_encrypted_chat(DialogId dialog_id,
                                                                           AccessRights access_rights) const;

  bool have_input_peer(DialogId dialog_id, AccessRights access_rights) const;

  void on_get_empty_messages(DialogId dialog_id, const vector<MessageId> &empty_message_ids);

  struct MessagesInfo {
    vector<tl_object_ptr<telegram_api::Message>> messages;
    int32 total_count = 0;
    bool is_channel_messages = false;
  };
  MessagesInfo get_messages_info(tl_object_ptr<telegram_api::messages_Messages> &&messages_ptr, const char *source);

  void get_channel_difference_if_needed(DialogId dialog_id, MessagesInfo &&messages_info,
                                        Promise<MessagesInfo> &&promise);

  void get_channel_differences_if_needed(MessagesInfo &&messages_info, Promise<MessagesInfo> &&promise);

  void on_get_messages(vector<tl_object_ptr<telegram_api::Message>> &&messages, bool is_channel_message,
                       bool is_scheduled, Promise<Unit> &&promise, const char *source);

  void on_get_history(DialogId dialog_id, MessageId from_message_id, MessageId old_last_new_message_id, int32 offset,
                      int32 limit, bool from_the_end, vector<tl_object_ptr<telegram_api::Message>> &&messages,
                      Promise<Unit> &&promise);

  void on_get_public_dialogs_search_result(const string &query, vector<tl_object_ptr<telegram_api::Peer>> &&my_peers,
                                           vector<tl_object_ptr<telegram_api::Peer>> &&peers);
  void on_failed_public_dialogs_search(const string &query, Status &&error);

  void on_get_message_search_result_calendar(DialogId dialog_id, MessageId from_message_id, MessageSearchFilter filter,
                                             int64 random_id, int32 total_count,
                                             vector<tl_object_ptr<telegram_api::Message>> &&messages,
                                             vector<tl_object_ptr<telegram_api::searchResultsCalendarPeriod>> &&periods,
                                             Promise<Unit> &&promise);
  void on_failed_get_message_search_result_calendar(DialogId dialog_id, int64 random_id);

  void on_get_dialog_messages_search_result(DialogId dialog_id, const string &query, DialogId sender_dialog_id,
                                            MessageId from_message_id, int32 offset, int32 limit,
                                            MessageSearchFilter filter, MessageId top_thread_message_id,
                                            int64 random_id, int32 total_count,
                                            vector<tl_object_ptr<telegram_api::Message>> &&messages,
                                            Promise<Unit> &&promise);
  void on_failed_dialog_messages_search(DialogId dialog_id, int64 random_id);

  void on_get_dialog_message_count(DialogId dialog_id, MessageSearchFilter filter, int32 total_count,
                                   Promise<int32> &&promise);

  void on_get_messages_search_result(const string &query, int32 offset_date, DialogId offset_dialog_id,
                                     MessageId offset_message_id, int32 limit, MessageSearchFilter filter,
                                     int32 min_date, int32 max_date, int64 random_id, int32 total_count,
                                     vector<tl_object_ptr<telegram_api::Message>> &&messages, Promise<Unit> &&promise);
  void on_failed_messages_search(int64 random_id);

  void on_get_outgoing_document_messages(vector<tl_object_ptr<telegram_api::Message>> &&messages,
                                         Promise<td_api::object_ptr<td_api::foundMessages>> &&promise);

  void on_get_scheduled_server_messages(DialogId dialog_id, uint32 generation,
                                        vector<tl_object_ptr<telegram_api::Message>> &&messages, bool is_not_modified);

  void on_get_recent_locations(DialogId dialog_id, int32 limit, int32 total_count,
                               vector<tl_object_ptr<telegram_api::Message>> &&messages,
                               Promise<td_api::object_ptr<td_api::messages>> &&promise);

  void on_get_message_public_forwards(int32 total_count, vector<tl_object_ptr<telegram_api::Message>> &&messages,
                                      Promise<td_api::object_ptr<td_api::foundMessages>> &&promise);

  // if message is from_update, flags have_previous and have_next are ignored and must be both true
  FullMessageId on_get_message(tl_object_ptr<telegram_api::Message> message_ptr, bool from_update,
                               bool is_channel_message, bool is_scheduled, bool have_previous, bool have_next,
                               const char *source);

  void open_secret_message(SecretChatId secret_chat_id, int64 random_id, Promise<>);

  void on_send_secret_message_success(int64 random_id, MessageId message_id, int32 date, unique_ptr<EncryptedFile> file,
                                      Promise<> promise);
  void on_send_secret_message_error(int64 random_id, Status error, Promise<> promise);

  void delete_secret_messages(SecretChatId secret_chat_id, std::vector<int64> random_ids, Promise<> promise);

  void delete_secret_chat_history(SecretChatId secret_chat_id, bool remove_from_dialog_list, MessageId last_message_id,
                                  Promise<> promise);

  void read_secret_chat_outbox(SecretChatId secret_chat_id, int32 up_to_date, int32 read_date);

  void on_update_secret_chat_state(SecretChatId secret_chat_id, SecretChatState state);

  void on_get_secret_message(SecretChatId secret_chat_id, UserId user_id, MessageId message_id, int32 date,
                             unique_ptr<EncryptedFile> file, tl_object_ptr<secret_api::decryptedMessage> message,
                             Promise<> promise);

  void on_secret_chat_screenshot_taken(SecretChatId secret_chat_id, UserId user_id, MessageId message_id, int32 date,
                                       int64 random_id, Promise<> promise);

  void on_secret_chat_ttl_changed(SecretChatId secret_chat_id, UserId user_id, MessageId message_id, int32 date,
                                  int32 ttl, int64 random_id, Promise<> promise);

  void on_update_sent_text_message(int64 random_id, tl_object_ptr<telegram_api::MessageMedia> message_media,
                                   vector<tl_object_ptr<telegram_api::MessageEntity>> &&entities);

  void delete_pending_message_web_page(FullMessageId full_message_id);

  void on_get_dialogs(FolderId folder_id, vector<tl_object_ptr<telegram_api::Dialog>> &&dialog_folders,
                      int32 total_count, vector<tl_object_ptr<telegram_api::Message>> &&messages,
                      Promise<Unit> &&promise);

  void on_get_common_dialogs(UserId user_id, int64 offset_chat_id, vector<tl_object_ptr<telegram_api::Chat>> &&chats,
                             int32 total_count);

  bool on_update_message_id(int64 random_id, MessageId new_message_id, const string &source);

  void on_update_dialog_draft_message(DialogId dialog_id, tl_object_ptr<telegram_api::DraftMessage> &&draft_message);

  void on_update_dialog_is_pinned(FolderId folder_id, DialogId dialog_id, bool is_pinned);

  void on_update_pinned_dialogs(FolderId folder_id);

  void on_update_dialog_is_marked_as_unread(DialogId dialog_id, bool is_marked_as_unread);

  void on_update_dialog_is_blocked(DialogId dialog_id, bool is_blocked);

  void on_update_dialog_last_pinned_message_id(DialogId dialog_id, MessageId last_pinned_message_id);

  void on_update_dialog_theme_name(DialogId dialog_id, string theme_name);

  void on_update_dialog_pending_join_requests(DialogId dialog_id, int32 pending_join_request_count,
                                              vector<int64> pending_requesters);

  void on_update_dialog_has_scheduled_server_messages(DialogId dialog_id, bool has_scheduled_server_messages);

  void on_update_dialog_folder_id(DialogId dialog_id, FolderId folder_id);

  void on_update_dialog_group_call(DialogId dialog_id, bool has_active_group_call, bool is_group_call_empty,
                                   const char *source, bool force = false);

  void on_update_dialog_group_call_id(DialogId dialog_id, InputGroupCallId input_group_call_id);

  void on_update_dialog_default_join_group_call_as_dialog_id(DialogId dialog_id, DialogId default_join_as_dialog_id,
                                                             bool force);

  void on_update_dialog_default_send_message_as_dialog_id(DialogId dialog_id, DialogId default_send_as_dialog_id,
                                                          bool force);

  void on_update_dialog_message_ttl(DialogId dialog_id, MessageTtl message_ttl);

  void on_update_dialog_filters();

  void on_update_service_notification(tl_object_ptr<telegram_api::updateServiceNotification> &&update,
                                      bool skip_new_entities, Promise<Unit> &&promise);

  void on_update_read_channel_inbox(tl_object_ptr<telegram_api::updateReadChannelInbox> &&update);

  void on_update_read_channel_outbox(tl_object_ptr<telegram_api::updateReadChannelOutbox> &&update);

  void on_update_read_channel_messages_contents(
      tl_object_ptr<telegram_api::updateChannelReadMessagesContents> &&update);

  void on_update_read_message_comments(DialogId dialog_id, MessageId message_id, MessageId max_message_id,
                                       MessageId last_read_inbox_message_id, MessageId last_read_outbox_message_id);

  void on_update_channel_too_long(tl_object_ptr<telegram_api::updateChannelTooLong> &&update, bool force_apply);

  void on_update_message_view_count(FullMessageId full_message_id, int32 view_count);

  void on_update_message_forward_count(FullMessageId full_message_id, int32 forward_count);

  void on_update_message_reactions(FullMessageId full_message_id,
                                   tl_object_ptr<telegram_api::messageReactions> &&reactions, Promise<Unit> &&promise);

  void update_message_reactions(FullMessageId full_message_id, unique_ptr<MessageReactions> &&reactions);

  void try_reload_message_reactions(DialogId dialog_id, bool is_finished);

  void on_get_message_reaction_list(FullMessageId full_message_id, const string &reaction,
                                    FlatHashMap<string, vector<DialogId>> reactions, int32 total_count);

  void on_update_message_interaction_info(FullMessageId full_message_id, int32 view_count, int32 forward_count,
                                          bool has_reply_info,
                                          tl_object_ptr<telegram_api::messageReplies> &&reply_info);

  void on_update_live_location_viewed(FullMessageId full_message_id);

  void on_update_some_live_location_viewed(Promise<Unit> &&promise);

  void on_external_update_message_content(FullMessageId full_message_id);

  void on_update_message_content(FullMessageId full_message_id);

  void on_read_channel_inbox(ChannelId channel_id, MessageId max_message_id, int32 server_unread_count, int32 pts,
                             const char *source);

  void on_read_channel_outbox(ChannelId channel_id, MessageId max_message_id);

  void on_update_channel_max_unavailable_message_id(ChannelId channel_id, MessageId max_unavailable_message_id);

  void on_update_dialog_online_member_count(DialogId dialog_id, int32 online_member_count, bool is_from_server);

  void on_update_delete_scheduled_messages(DialogId dialog_id, vector<ScheduledServerMessageId> &&server_message_ids);

  void on_update_created_public_broadcasts(vector<ChannelId> channel_ids);

  void on_dialog_action(DialogId dialog_id, MessageId top_thread_message_id, DialogId typing_dialog_id,
                        DialogAction action, int32 date,
                        MessageContentType message_content_type = MessageContentType::None);

  void read_history_inbox(DialogId dialog_id, MessageId max_message_id, int32 unread_count, const char *source);

  void delete_messages(DialogId dialog_id, const vector<MessageId> &message_ids, bool revoke, Promise<Unit> &&promise);

  void on_failed_message_deletion(DialogId dialog_id, const vector<int32> &server_message_ids);

  void on_failed_scheduled_message_deletion(DialogId dialog_id, const vector<MessageId> &message_ids);

  void delete_dialog_history(DialogId dialog_id, bool remove_from_dialog_list, bool revoke, Promise<Unit> &&promise);

  void delete_all_call_messages(bool revoke, Promise<Unit> &&promise);

  void delete_dialog_messages_by_sender(DialogId dialog_id, DialogId sender_dialog_id, Promise<Unit> &&promise);

  void delete_dialog_messages_by_date(DialogId dialog_id, int32 min_date, int32 max_date, bool revoke,
                                      Promise<Unit> &&promise);

  void on_dialog_deleted(DialogId dialog_id, Promise<Unit> &&promise);

  void on_update_dialog_group_call_rights(DialogId dialog_id);

  void read_all_dialog_mentions(DialogId dialog_id, Promise<Unit> &&promise);

  void read_all_dialog_reactions(DialogId dialog_id, Promise<Unit> &&promise);

  Status add_recently_found_dialog(DialogId dialog_id) TD_WARN_UNUSED_RESULT;

  Status remove_recently_found_dialog(DialogId dialog_id) TD_WARN_UNUSED_RESULT;

  void clear_recently_found_dialogs();

  std::pair<int32, vector<DialogId>> get_recently_opened_dialogs(int32 limit, Promise<Unit> &&promise);

  DialogId resolve_dialog_username(const string &username) const;

  DialogId search_public_dialog(const string &username_to_search, bool force, Promise<Unit> &&promise);

  void reload_voice_chat_on_search(const string &username);

  void get_dialog_send_message_as_dialog_ids(DialogId dialog_id,
                                             Promise<td_api::object_ptr<td_api::messageSenders>> &&promise,
                                             bool is_recursive = false);

  void set_dialog_default_send_message_as_dialog_id(DialogId dialog_id, DialogId message_sender_dialog_id,
                                                    Promise<Unit> &&promise);

  bool get_dialog_silent_send_message(DialogId dialog_id) const;

  DialogId get_dialog_default_send_message_as_dialog_id(DialogId dialog_id) const;

  Result<td_api::object_ptr<td_api::message>> send_message(
      DialogId dialog_id, MessageId top_thread_message_id, MessageId reply_to_message_id,
      tl_object_ptr<td_api::messageSendOptions> &&options, tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
      tl_object_ptr<td_api::InputMessageContent> &&input_message_content) TD_WARN_UNUSED_RESULT;

  Result<td_api::object_ptr<td_api::messages>> send_message_group(
      DialogId dialog_id, MessageId top_thread_message_id, MessageId reply_to_message_id,
      tl_object_ptr<td_api::messageSendOptions> &&options,
      vector<tl_object_ptr<td_api::InputMessageContent>> &&input_message_contents,
      bool only_preview) TD_WARN_UNUSED_RESULT;

  Result<MessageId> send_bot_start_message(UserId bot_user_id, DialogId dialog_id,
                                           const string &parameter) TD_WARN_UNUSED_RESULT;

  Result<MessageId> send_inline_query_result_message(DialogId dialog_id, MessageId top_thread_message_id,
                                                     MessageId reply_to_message_id,
                                                     tl_object_ptr<td_api::messageSendOptions> &&options,
                                                     int64 query_id, const string &result_id,
                                                     bool hide_via_bot) TD_WARN_UNUSED_RESULT;

  Result<td_api::object_ptr<td_api::messages>> forward_messages(DialogId to_dialog_id, DialogId from_dialog_id,
                                                                vector<MessageId> message_ids,
                                                                tl_object_ptr<td_api::messageSendOptions> &&options,
                                                                bool in_game_share,
                                                                vector<MessageCopyOptions> &&copy_options,
                                                                bool only_preview) TD_WARN_UNUSED_RESULT;

  Result<vector<MessageId>> resend_messages(DialogId dialog_id, vector<MessageId> message_ids) TD_WARN_UNUSED_RESULT;

  void set_dialog_message_ttl(DialogId dialog_id, int32 ttl, Promise<Unit> &&promise);

  Status send_screenshot_taken_notification_message(DialogId dialog_id);

  Result<MessageId> add_local_message(DialogId dialog_id, td_api::object_ptr<td_api::MessageSender> &&sender,
                                      MessageId reply_to_message_id, bool disable_notification,
                                      tl_object_ptr<td_api::InputMessageContent> &&input_message_content)
      TD_WARN_UNUSED_RESULT;

  void get_message_file_type(const string &message_file_head,
                             Promise<td_api::object_ptr<td_api::MessageFileType>> &&promise);

  void get_message_import_confirmation_text(DialogId dialog_id, Promise<string> &&promise);

  void import_messages(DialogId dialog_id, const td_api::object_ptr<td_api::InputFile> &message_file,
                       const vector<td_api::object_ptr<td_api::InputFile>> &attached_files, Promise<Unit> &&promise);

  void start_import_messages(DialogId dialog_id, int64 import_id, vector<FileId> &&attached_file_ids,
                             Promise<Unit> &&promise);

  void edit_message_text(FullMessageId full_message_id, tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                         tl_object_ptr<td_api::InputMessageContent> &&input_message_content, Promise<Unit> &&promise);

  void edit_message_live_location(FullMessageId full_message_id, tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                  tl_object_ptr<td_api::location> &&input_location, int32 heading,
                                  int32 proximity_alert_radius, Promise<Unit> &&promise);

  void edit_message_media(FullMessageId full_message_id, tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                          tl_object_ptr<td_api::InputMessageContent> &&input_message_content, Promise<Unit> &&promise);

  void edit_message_caption(FullMessageId full_message_id, tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                            tl_object_ptr<td_api::formattedText> &&input_caption, Promise<Unit> &&promise);

  void edit_message_reply_markup(FullMessageId full_message_id, tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                 Promise<Unit> &&promise);

  void edit_inline_message_text(const string &inline_message_id, tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                tl_object_ptr<td_api::InputMessageContent> &&input_message_content,
                                Promise<Unit> &&promise);

  void edit_inline_message_live_location(const string &inline_message_id,
                                         tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                         tl_object_ptr<td_api::location> &&input_location, int32 heading,
                                         int32 proximity_alert_radius, Promise<Unit> &&promise);

  void edit_inline_message_media(const string &inline_message_id, tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                 tl_object_ptr<td_api::InputMessageContent> &&input_message_content,
                                 Promise<Unit> &&promise);

  void edit_inline_message_caption(const string &inline_message_id, tl_object_ptr<td_api::ReplyMarkup> &&reply_markup,
                                   tl_object_ptr<td_api::formattedText> &&input_caption, Promise<Unit> &&promise);

  void edit_inline_message_reply_markup(const string &inline_message_id,
                                        tl_object_ptr<td_api::ReplyMarkup> &&reply_markup, Promise<Unit> &&promise);

  void edit_message_scheduling_state(FullMessageId full_message_id,
                                     td_api::object_ptr<td_api::MessageSchedulingState> &&scheduling_state,
                                     Promise<Unit> &&promise);

  void send_dialog_action(DialogId dialog_id, MessageId top_thread_message_id, DialogAction action,
                          Promise<Unit> &&promise);

  void after_set_typing_query(DialogId dialog_id, int32 generation);

  vector<DialogListId> get_dialog_lists_to_add_dialog(DialogId dialog_id);

  void add_dialog_to_list(DialogId dialog_id, DialogListId dialog_list_id, Promise<Unit> &&promise);

  void set_dialog_photo(DialogId dialog_id, const tl_object_ptr<td_api::InputChatPhoto> &input_photo,
                        Promise<Unit> &&promise);

  void set_dialog_title(DialogId dialog_id, const string &title, Promise<Unit> &&promise);

  void set_dialog_description(DialogId dialog_id, const string &description, Promise<Unit> &&promise);

  void set_active_reactions(vector<string> active_reactions);

  void set_dialog_available_reactions(DialogId dialog_id,
                                      td_api::object_ptr<td_api::ChatAvailableReactions> &&available_reactions_ptr,
                                      Promise<Unit> &&promise);

  void set_dialog_permissions(DialogId dialog_id, const td_api::object_ptr<td_api::chatPermissions> &permissions,
                              Promise<Unit> &&promise);

  void toggle_dialog_has_protected_content(DialogId dialog_id, bool has_protected_content, Promise<Unit> &&promise);

  void set_dialog_theme(DialogId dialog_id, const string &theme_name, Promise<Unit> &&promise);

  void pin_dialog_message(DialogId dialog_id, MessageId message_id, bool disable_notification, bool only_for_self,
                          bool is_unpin, Promise<Unit> &&promise);

  void unpin_all_dialog_messages(DialogId dialog_id, Promise<Unit> &&promise);

  void get_dialog_info_full(DialogId dialog_id, Promise<Unit> &&promise, const char *source);

  string get_dialog_title(DialogId dialog_id) const;

  bool have_dialog(DialogId dialog_id) const;
  bool have_dialog_force(DialogId dialog_id, const char *source);

  bool have_dialog_info(DialogId dialog_id) const;
  bool have_dialog_info_force(DialogId dialog_id) const;

  void reload_dialog_info_full(DialogId dialog_id, const char *source);

  void on_dialog_info_full_invalidated(DialogId dialog_id);

  bool load_dialog(DialogId dialog_id, int left_tries, Promise<Unit> &&promise);

  void load_dialogs(vector<DialogId> dialog_ids, Promise<vector<DialogId>> &&promise);

  void load_dialog_filter(DialogFilterId dialog_filter_id, bool force, Promise<Unit> &&promise);

  void get_recommended_dialog_filters(Promise<td_api::object_ptr<td_api::recommendedChatFilters>> &&promise);

  Result<DialogDate> get_dialog_list_last_date(DialogListId dialog_list_id);

  vector<DialogId> get_dialogs(DialogListId dialog_list_id, DialogDate offset, int32 limit, bool exact_limit,
                               bool force, Promise<Unit> &&promise);

  void get_dialogs_from_list(DialogListId dialog_list_id, int32 limit,
                             Promise<td_api::object_ptr<td_api::chats>> &&promise);

  vector<DialogId> search_public_dialogs(const string &query, Promise<Unit> &&promise);

  std::pair<int32, vector<DialogId>> search_dialogs(const string &query, int32 limit, Promise<Unit> &&promise);

  vector<DialogId> search_dialogs_on_server(const string &query, int32 limit, Promise<Unit> &&promise);

  void drop_common_dialogs_cache(UserId user_id);

  std::pair<int32, vector<DialogId>> get_common_dialogs(UserId user_id, DialogId offset_dialog_id, int32 limit,
                                                        bool force, Promise<Unit> &&promise);

  void block_message_sender_from_replies(MessageId message_id, bool need_delete_message, bool need_delete_all_messages,
                                         bool report_spam, Promise<Unit> &&promise);

  void get_blocked_dialogs(int32 offset, int32 limit, Promise<td_api::object_ptr<td_api::messageSenders>> &&promise);

  void on_get_blocked_dialogs(int32 offset, int32 limit, int32 total_count,
                              vector<tl_object_ptr<telegram_api::peerBlocked>> &&blocked_peers,
                              Promise<td_api::object_ptr<td_api::messageSenders>> &&promise);

  bool can_get_message_statistics(FullMessageId full_message_id);

  DialogId get_dialog_message_sender(FullMessageId full_message_id);

  bool have_message_force(FullMessageId full_message_id, const char *source);

  void get_message(FullMessageId full_message_id, Promise<Unit> &&promise);

  FullMessageId get_replied_message(DialogId dialog_id, MessageId message_id, bool force, Promise<Unit> &&promise);

  MessageId get_dialog_pinned_message(DialogId dialog_id, Promise<Unit> &&promise);

  void get_callback_query_message(DialogId dialog_id, MessageId message_id, int64 callback_query_id,
                                  Promise<Unit> &&promise);

  bool get_messages(DialogId dialog_id, const vector<MessageId> &message_ids, Promise<Unit> &&promise);

  void get_message_from_server(FullMessageId full_message_id, Promise<Unit> &&promise, const char *source,
                               tl_object_ptr<telegram_api::InputMessage> input_message = nullptr);

  void get_messages_from_server(vector<FullMessageId> &&message_ids, Promise<Unit> &&promise, const char *source,
                                tl_object_ptr<telegram_api::InputMessage> input_message = nullptr);

  void get_message_thread(DialogId dialog_id, MessageId message_id, Promise<MessageThreadInfo> &&promise);

  td_api::object_ptr<td_api::messageThreadInfo> get_message_thread_info_object(const MessageThreadInfo &info);

  void process_discussion_message(telegram_api::object_ptr<telegram_api::messages_discussionMessage> &&result,
                                  DialogId dialog_id, MessageId message_id, DialogId expected_dialog_id,
                                  MessageId expected_message_id, Promise<MessageThreadInfo> promise);

  void get_message_viewers(FullMessageId full_message_id, Promise<td_api::object_ptr<td_api::users>> &&promise);

  void translate_text(const string &text, const string &from_language_code, const string &to_language_code,
                      Promise<td_api::object_ptr<td_api::text>> &&promise);

  bool is_message_edited_recently(FullMessageId full_message_id, int32 seconds);

  bool is_deleted_secret_chat(DialogId dialog_id) const;

  Result<std::pair<string, bool>> get_message_link(FullMessageId full_message_id, int32 media_timestamp, bool for_group,
                                                   bool for_comment);

  string get_message_embedding_code(FullMessageId full_message_id, bool for_group, Promise<Unit> &&promise);

  void on_get_public_message_link(FullMessageId full_message_id, bool for_group, string url, string html);

  void get_message_link_info(Slice url, Promise<MessageLinkInfo> &&promise);

  td_api::object_ptr<td_api::messageLinkInfo> get_message_link_info_object(const MessageLinkInfo &info) const;

  void create_dialog_filter(td_api::object_ptr<td_api::chatFilter> filter,
                            Promise<td_api::object_ptr<td_api::chatFilterInfo>> &&promise);

  void edit_dialog_filter(DialogFilterId dialog_filter_id, td_api::object_ptr<td_api::chatFilter> filter,
                          Promise<td_api::object_ptr<td_api::chatFilterInfo>> &&promise);

  void delete_dialog_filter(DialogFilterId dialog_filter_id, Promise<Unit> &&promise);

  void reorder_dialog_filters(vector<DialogFilterId> dialog_filter_ids, int32 main_dialog_list_position,
                              Promise<Unit> &&promise);

  Status delete_dialog_reply_markup(DialogId dialog_id, MessageId message_id) TD_WARN_UNUSED_RESULT;

  Status set_dialog_draft_message(DialogId dialog_id, MessageId top_thread_message_id,
                                  tl_object_ptr<td_api::draftMessage> &&draft_message) TD_WARN_UNUSED_RESULT;

  void clear_all_draft_messages(bool exclude_secret_chats, Promise<Unit> &&promise);

  Status toggle_dialog_is_pinned(DialogListId dialog_list_id, DialogId dialog_id, bool is_pinned) TD_WARN_UNUSED_RESULT;

  Status toggle_dialog_is_marked_as_unread(DialogId dialog_id, bool is_marked_as_unread) TD_WARN_UNUSED_RESULT;

  Status toggle_message_sender_is_blocked(const td_api::object_ptr<td_api::MessageSender> &sender,
                                          bool is_blocked) TD_WARN_UNUSED_RESULT;

  Status toggle_dialog_silent_send_message(DialogId dialog_id, bool silent_send_message) TD_WARN_UNUSED_RESULT;

  Status set_pinned_dialogs(DialogListId dialog_list_id, vector<DialogId> dialog_ids) TD_WARN_UNUSED_RESULT;

  Status set_dialog_client_data(DialogId dialog_id, string &&client_data) TD_WARN_UNUSED_RESULT;

  void create_dialog(DialogId dialog_id, bool force, Promise<Unit> &&promise);

  DialogId create_new_group_chat(const vector<UserId> &user_ids, const string &title, int64 &random_id,
                                 Promise<Unit> &&promise);

  DialogId create_new_channel_chat(const string &title, bool is_megagroup, const string &description,
                                   const DialogLocation &location, bool for_import, int64 &random_id,
                                   Promise<Unit> &&promise);

  void create_new_secret_chat(UserId user_id, Promise<SecretChatId> &&promise);

  DialogId migrate_dialog_to_megagroup(DialogId dialog_id, Promise<Unit> &&promise);

  bool is_dialog_opened(DialogId dialog_id) const;

  Status open_dialog(DialogId dialog_id) TD_WARN_UNUSED_RESULT;

  Status close_dialog(DialogId dialog_id) TD_WARN_UNUSED_RESULT;

  Status view_messages(DialogId dialog_id, MessageId top_thread_message_id, const vector<MessageId> &message_ids,
                       bool force_read) TD_WARN_UNUSED_RESULT;

  void finish_get_message_views(DialogId dialog_id, const vector<MessageId> &message_ids);

  Status open_message_content(FullMessageId full_message_id) TD_WARN_UNUSED_RESULT;

  void click_animated_emoji_message(FullMessageId full_message_id,
                                    Promise<td_api::object_ptr<td_api::sticker>> &&promise);

  vector<DialogId> get_dialog_notification_settings_exceptions(NotificationSettingsScope scope, bool filter_scope,
                                                               bool compare_sound, bool force, Promise<Unit> &&promise);

  Status set_dialog_notification_settings(DialogId dialog_id,
                                          tl_object_ptr<td_api::chatNotificationSettings> &&notification_settings)
      TD_WARN_UNUSED_RESULT;

  void reset_all_notification_settings();

  tl_object_ptr<td_api::chat> get_chat_object(DialogId dialog_id) const;

  static tl_object_ptr<td_api::chats> get_chats_object(int32 total_count, const vector<DialogId> &dialog_ids);

  static tl_object_ptr<td_api::chats> get_chats_object(const std::pair<int32, vector<DialogId>> &dialog_ids);

  tl_object_ptr<td_api::chatFilter> get_chat_filter_object(DialogFilterId dialog_filter_id) const;

  tl_object_ptr<td_api::messages> get_dialog_history(DialogId dialog_id, MessageId from_message_id, int32 offset,
                                                     int32 limit, int left_tries, bool only_local,
                                                     Promise<Unit> &&promise);

  std::pair<DialogId, vector<MessageId>> get_message_thread_history(DialogId dialog_id, MessageId message_id,
                                                                    MessageId from_message_id, int32 offset,
                                                                    int32 limit, int64 &random_id,
                                                                    Promise<Unit> &&promise);

  td_api::object_ptr<td_api::messageCalendar> get_dialog_message_calendar(DialogId dialog_id, MessageId from_message_id,
                                                                          MessageSearchFilter filter, int64 &random_id,
                                                                          bool use_db, Promise<Unit> &&promise);

  std::pair<int32, vector<MessageId>> search_dialog_messages(DialogId dialog_id, const string &query,
                                                             const td_api::object_ptr<td_api::MessageSender> &sender,
                                                             MessageId from_message_id, int32 offset, int32 limit,
                                                             MessageSearchFilter filter,
                                                             MessageId top_thread_message_id, int64 &random_id,
                                                             bool use_db, Promise<Unit> &&promise);

  struct FoundMessages {
    vector<FullMessageId> full_message_ids;
    string next_offset;
    int32 total_count = 0;
  };

  td_api::object_ptr<td_api::foundMessages> get_found_messages_object(const FoundMessages &found_messages,
                                                                      const char *source);

  FoundMessages offline_search_messages(DialogId dialog_id, const string &query, string offset, int32 limit,
                                        MessageSearchFilter filter, int64 &random_id, Promise<> &&promise);

  std::pair<int32, vector<FullMessageId>> search_messages(FolderId folder_id, bool ignore_folder_id,
                                                          const string &query, int32 offset_date,
                                                          DialogId offset_dialog_id, MessageId offset_message_id,
                                                          int32 limit, MessageSearchFilter filter, int32 min_date,
                                                          int32 max_date, int64 &random_id, Promise<Unit> &&promise);

  std::pair<int32, vector<FullMessageId>> search_call_messages(MessageId from_message_id, int32 limit, bool only_missed,
                                                               int64 &random_id, bool use_db, Promise<Unit> &&promise);

  void search_outgoing_document_messages(const string &query, int32 limit,
                                         Promise<td_api::object_ptr<td_api::foundMessages>> &&promise);

  void search_dialog_recent_location_messages(DialogId dialog_id, int32 limit,
                                              Promise<td_api::object_ptr<td_api::messages>> &&promise);

  vector<FullMessageId> get_active_live_location_messages(Promise<Unit> &&promise);

  int64 get_dialog_message_by_date(DialogId dialog_id, int32 date, Promise<Unit> &&promise);

  void on_get_dialog_message_by_date_success(DialogId dialog_id, int32 date, int64 random_id,
                                             vector<tl_object_ptr<telegram_api::Message>> &&messages,
                                             Promise<Unit> &&promise);

  void on_get_dialog_message_by_date_fail(int64 random_id);

  void get_dialog_sparse_message_positions(DialogId dialog_id, MessageSearchFilter filter, MessageId from_message_id,
                                           int32 limit,
                                           Promise<td_api::object_ptr<td_api::messagePositions>> &&promise);

  void on_get_dialog_sparse_message_positions(
      DialogId dialog_id, MessageSearchFilter filter,
      telegram_api::object_ptr<telegram_api::messages_searchResultsPositions> positions,
      Promise<td_api::object_ptr<td_api::messagePositions>> &&promise);

  void get_dialog_message_count(DialogId dialog_id, MessageSearchFilter filter, bool return_local,
                                Promise<int32> &&promise);

  vector<MessageId> get_dialog_scheduled_messages(DialogId dialog_id, bool force, bool ignore_result,
                                                  Promise<Unit> &&promise);

  Result<td_api::object_ptr<td_api::availableReactions>> get_message_available_reactions(FullMessageId full_message_id,
                                                                                         int32 row_size);

  void add_message_reaction(FullMessageId full_message_id, string reaction, bool is_big, bool add_to_recent,
                            Promise<Unit> &&promise);

  void remove_message_reaction(FullMessageId full_message_id, string reaction, Promise<Unit> &&promise);

  void get_message_public_forwards(FullMessageId full_message_id, string offset, int32 limit,
                                   Promise<td_api::object_ptr<td_api::foundMessages>> &&promise);

  tl_object_ptr<td_api::message> get_dialog_message_by_date_object(int64 random_id);

  td_api::object_ptr<td_api::message> get_dialog_event_log_message_object(
      DialogId dialog_id, tl_object_ptr<telegram_api::Message> &&message, DialogId &sender_dialog_id);

  tl_object_ptr<td_api::message> get_message_object(FullMessageId full_message_id, const char *source);

  tl_object_ptr<td_api::messages> get_messages_object(int32 total_count, DialogId dialog_id,
                                                      const vector<MessageId> &message_ids, bool skip_not_found,
                                                      const char *source);

  tl_object_ptr<td_api::messages> get_messages_object(int32 total_count, const vector<FullMessageId> &full_message_ids,
                                                      bool skip_not_found, const char *source);

  void process_pts_update(tl_object_ptr<telegram_api::Update> &&update_ptr);

  void skip_old_pending_pts_update(tl_object_ptr<telegram_api::Update> &&update, int32 new_pts, int32 old_pts,
                                   int32 pts_count, const char *source);

  void add_pending_channel_update(DialogId dialog_id, tl_object_ptr<telegram_api::Update> &&update, int32 new_pts,
                                  int32 pts_count, Promise<Unit> &&promise, const char *source,
                                  bool is_postponed_update = false);

  bool is_old_channel_update(DialogId dialog_id, int32 new_pts);

  bool is_update_about_username_change_received(DialogId dialog_id) const;

  void on_dialog_bots_updated(DialogId dialog_id, vector<UserId> bot_user_ids, bool from_database);

  void on_dialog_photo_updated(DialogId dialog_id);
  void on_dialog_title_updated(DialogId dialog_id);
  void on_dialog_username_updated(DialogId dialog_id, const string &old_username, const string &new_username);
  void on_dialog_default_permissions_updated(DialogId dialog_id);
  void on_dialog_has_protected_content_updated(DialogId dialog_id);

  void on_dialog_user_is_contact_updated(DialogId dialog_id, bool is_contact);
  void on_dialog_user_is_deleted_updated(DialogId dialog_id, bool is_deleted);

  void on_dialog_linked_channel_updated(DialogId dialog_id, ChannelId old_linked_channel_id,
                                        ChannelId new_linked_channel_id) const;

  void drop_dialog_pending_join_requests(DialogId dialog_id);

  void on_resolved_username(const string &username, DialogId dialog_id);
  void drop_username(const string &username);

  void on_update_notification_scope_is_muted(NotificationSettingsScope scope, bool is_muted);

  void on_update_dialog_notify_settings(DialogId dialog_id,
                                        tl_object_ptr<telegram_api::peerNotifySettings> &&peer_notify_settings,
                                        const char *source);

  void on_update_dialog_available_reactions(
      DialogId dialog_id, telegram_api::object_ptr<telegram_api::ChatReactions> &&available_reactions);

  void hide_dialog_action_bar(DialogId dialog_id);

  void remove_dialog_action_bar(DialogId dialog_id, Promise<Unit> &&promise);

  void reget_dialog_action_bar(DialogId dialog_id, const char *source, bool is_repair = true);

  void report_dialog(DialogId dialog_id, const vector<MessageId> &message_ids, ReportReason &&reason,
                     Promise<Unit> &&promise);

  void report_dialog_photo(DialogId dialog_id, FileId file_id, ReportReason &&reason, Promise<Unit> &&promise);

  void on_get_peer_settings(DialogId dialog_id, tl_object_ptr<telegram_api::peerSettings> &&peer_settings,
                            bool ignore_privacy_exception = false);

  void on_authorization_success();

  void before_get_difference();

  void after_get_difference();

  bool on_get_dialog_error(DialogId dialog_id, const Status &status, const string &source);

  void on_send_message_get_quick_ack(int64 random_id);

  void check_send_message_result(int64 random_id, DialogId dialog_id, const telegram_api::Updates *updates_ptr,
                                 const char *source);

  FullMessageId on_send_message_success(int64 random_id, MessageId new_message_id, int32 date, int32 ttl_period,
                                        FileId new_file_id, const char *source);

  void on_send_message_file_part_missing(int64 random_id, int bad_part);

  void on_send_message_file_reference_error(int64 random_id);

  void on_send_media_group_file_reference_error(DialogId dialog_id, vector<int64> random_ids);

  void on_send_message_fail(int64 random_id, Status error);

  void on_upload_message_media_success(DialogId dialog_id, MessageId message_id,
                                       tl_object_ptr<telegram_api::MessageMedia> &&media);

  void on_upload_message_media_file_part_missing(DialogId dialog_id, MessageId message_id, int bad_part);

  void on_upload_message_media_fail(DialogId dialog_id, MessageId message_id, Status error);

  void on_create_new_dialog_success(int64 random_id, tl_object_ptr<telegram_api::Updates> &&updates,
                                    DialogType expected_type, Promise<Unit> &&promise);

  void on_create_new_dialog_fail(int64 random_id, Status error, Promise<Unit> &&promise);

  void on_get_channel_difference(DialogId dialog_id, int32 request_pts, int32 request_limit,
                                 tl_object_ptr<telegram_api::updates_ChannelDifference> &&difference_ptr);

  void force_create_dialog(DialogId dialog_id, const char *source, bool expect_no_access = false,
                           bool force_update_dialog_pos = false);

  void on_get_dialog_query_finished(DialogId dialog_id, Status &&status);

  void remove_sponsored_dialog();

  void on_get_sponsored_dialog(tl_object_ptr<telegram_api::Peer> peer, DialogSource source,
                               vector<tl_object_ptr<telegram_api::User>> users,
                               vector<tl_object_ptr<telegram_api::Chat>> chats);

  FileSourceId get_message_file_source_id(FullMessageId full_message_id, bool force = false);

  struct MessagePushNotificationInfo {
    NotificationGroupId group_id;
    NotificationGroupType group_type = NotificationGroupType::Calls;
    DialogId settings_dialog_id;
  };
  Result<MessagePushNotificationInfo> get_message_push_notification_info(DialogId dialog_id, MessageId message_id,
                                                                         int64 random_id, UserId sender_user_id,
                                                                         DialogId sender_dialog_id, int32 date,
                                                                         bool is_from_scheduled, bool contains_mention,
                                                                         bool is_pinned, bool is_from_binlog);

  struct MessageNotificationGroup {
    DialogId dialog_id;
    NotificationGroupType type = NotificationGroupType::Calls;
    int32 total_count = 0;
    vector<Notification> notifications;
  };
  MessageNotificationGroup get_message_notification_group_force(NotificationGroupId group_id);

  vector<NotificationGroupKey> get_message_notification_group_keys_from_database(NotificationGroupKey from_group_key,
                                                                                 int32 limit);

  void get_message_notifications_from_database(DialogId dialog_id, NotificationGroupId group_id,
                                               NotificationId from_notification_id, MessageId from_message_id,
                                               int32 limit, Promise<vector<Notification>> promise);

  void remove_message_notification(DialogId dialog_id, NotificationGroupId group_id, NotificationId notification_id);

  void remove_message_notifications_by_message_ids(DialogId dialog_id, const vector<MessageId> &message_ids);

  void remove_message_notifications(DialogId dialog_id, NotificationGroupId group_id,
                                    NotificationId max_notification_id, MessageId max_message_id);

  void remove_scope_pinned_message_notifications(NotificationSettingsScope scope);

  void on_update_scope_mention_notifications(NotificationSettingsScope scope, bool disable_mention_notifications);

  void upload_dialog_photo(DialogId dialog_id, FileId file_id, bool is_animation, double main_frame_timestamp,
                           bool is_reupload, Promise<Unit> &&promise, vector<int> bad_parts = {});

  void on_binlog_events(vector<BinlogEvent> &&events);

  void set_poll_answer(FullMessageId full_message_id, vector<int32> &&option_ids, Promise<Unit> &&promise);

  void get_poll_voters(FullMessageId full_message_id, int32 option_id, int32 offset, int32 limit,
                       Promise<std::pair<int32, vector<UserId>>> &&promise);

  void stop_poll(FullMessageId full_message_id, td_api::object_ptr<td_api::ReplyMarkup> &&reply_markup,
                 Promise<Unit> &&promise);

  Result<string> get_login_button_url(FullMessageId full_message_id, int64 button_id);

  Result<ServerMessageId> get_invoice_message_id(FullMessageId full_message_id);

  Result<ServerMessageId> get_payment_successful_message_id(FullMessageId full_message_id);

  bool can_set_game_score(FullMessageId full_message_id) const;

  void get_current_state(vector<td_api::object_ptr<td_api::Update>> &updates) const;

  void add_message_file_to_downloads(FullMessageId full_message_id, FileId file_id, int32 priority,
                                     Promise<td_api::object_ptr<td_api::file>> promise);

  void get_message_file_search_text(FullMessageId full_message_id, string unique_file_id, Promise<string> promise);

 private:
  class PendingPtsUpdate {
   public:
    tl_object_ptr<telegram_api::Update> update;
    int32 pts;
    int32 pts_count;
    Promise<Unit> promise;

    PendingPtsUpdate(tl_object_ptr<telegram_api::Update> &&update, int32 pts, int32 pts_count, Promise<Unit> &&promise)
        : update(std::move(update)), pts(pts), pts_count(pts_count), promise(std::move(promise)) {
    }
  };

  struct MessageInfo {
    DialogId dialog_id;
    MessageId message_id;
    UserId sender_user_id;
    DialogId sender_dialog_id;
    int32 date = 0;
    int32 ttl_period = 0;
    int32 ttl = 0;
    bool disable_web_page_preview = false;
    int64 random_id = 0;
    tl_object_ptr<telegram_api::messageFwdHeader> forward_header;
    MessageId reply_to_message_id;
    tl_object_ptr<telegram_api::messageReplyHeader> reply_header;
    UserId via_bot_user_id;
    int32 view_count = 0;
    int32 forward_count = 0;
    tl_object_ptr<telegram_api::messageReplies> reply_info;
    tl_object_ptr<telegram_api::messageReactions> reactions;
    int32 flags = 0;
    int32 edit_date = 0;
    vector<RestrictionReason> restriction_reasons;
    string author_signature;
    int64 media_album_id = 0;

    unique_ptr<MessageContent> content;
    tl_object_ptr<telegram_api::ReplyMarkup> reply_markup;
  };

  struct MessageForwardInfo {
    UserId sender_user_id;
    int32 date = 0;
    DialogId sender_dialog_id;
    MessageId message_id;
    string author_signature;
    string sender_name;
    DialogId from_dialog_id;
    MessageId from_message_id;
    string psa_type;
    bool is_imported = false;

    MessageForwardInfo() = default;

    MessageForwardInfo(UserId sender_user_id, int32 date, DialogId sender_dialog_id, MessageId message_id,
                       string author_signature, string sender_name, DialogId from_dialog_id, MessageId from_message_id,
                       string psa_type, bool is_imported)
        : sender_user_id(sender_user_id)
        , date(date)
        , sender_dialog_id(sender_dialog_id)
        , message_id(message_id)
        , author_signature(std::move(author_signature))
        , sender_name(std::move(sender_name))
        , from_dialog_id(from_dialog_id)
        , from_message_id(from_message_id)
        , psa_type(std::move(psa_type))
        , is_imported(is_imported) {
    }

    bool operator==(const MessageForwardInfo &rhs) const {
      return sender_user_id == rhs.sender_user_id && date == rhs.date && sender_dialog_id == rhs.sender_dialog_id &&
             message_id == rhs.message_id && author_signature == rhs.author_signature &&
             sender_name == rhs.sender_name && from_dialog_id == rhs.from_dialog_id &&
             from_message_id == rhs.from_message_id && psa_type == rhs.psa_type && is_imported == rhs.is_imported;
    }

    bool operator!=(const MessageForwardInfo &rhs) const {
      return !(*this == rhs);
    }

    friend StringBuilder &operator<<(StringBuilder &string_builder, const MessageForwardInfo &forward_info) {
      string_builder << "MessageForwardInfo[" << (forward_info.is_imported ? "imported " : "") << "sender "
                     << forward_info.sender_user_id;
      if (!forward_info.author_signature.empty() || !forward_info.sender_name.empty()) {
        string_builder << '(' << forward_info.author_signature << '/' << forward_info.sender_name << ')';
      }
      if (!forward_info.psa_type.empty()) {
        string_builder << ", psa_type " << forward_info.psa_type;
      }
      if (forward_info.sender_dialog_id.is_valid()) {
        string_builder << ", source ";
        if (forward_info.message_id.is_valid()) {
          string_builder << FullMessageId(forward_info.sender_dialog_id, forward_info.message_id);
        } else {
          string_builder << forward_info.sender_dialog_id;
        }
      }
      if (forward_info.from_dialog_id.is_valid() || forward_info.from_message_id.is_valid()) {
        string_builder << ", from " << FullMessageId(forward_info.from_dialog_id, forward_info.from_message_id);
      }
      return string_builder << " at " << forward_info.date << ']';
    }
  };

  // Do not forget to update MessagesManager::update_message and all make_unique<Message> when this class is changed
  struct Message {
    int32 random_y = 0;

    MessageId message_id;
    UserId sender_user_id;
    DialogId sender_dialog_id;
    int32 date = 0;
    int32 edit_date = 0;
    int32 send_date = 0;

    int64 random_id = 0;

    unique_ptr<MessageForwardInfo> forward_info;

    MessageId reply_to_message_id;
    int64 reply_to_random_id = 0;  // for send_message
    DialogId reply_in_dialog_id;
    MessageId top_thread_message_id;
    MessageId linked_top_thread_message_id;
    vector<MessageId> local_thread_message_ids;

    UserId via_bot_user_id;

    vector<RestrictionReason> restriction_reasons;

    string author_signature;

    bool is_channel_post = false;
    bool is_outgoing = false;
    bool is_failed_to_send = false;
    bool disable_notification = false;
    bool contains_mention = false;
    bool contains_unread_mention = false;
    bool hide_edit_date = false;
    bool had_reply_markup = false;  // had non-inline reply markup?
    bool had_forward_info = false;
    bool is_content_secret = false;  // must be shown only while tapped
    bool is_mention_notification_disabled = false;
    bool is_from_scheduled = false;
    bool is_pinned = false;
    bool are_media_timestamp_entities_found = false;
    bool noforwards = false;

    bool has_explicit_sender = false;       // for send_message
    bool is_copy = false;                   // for send_message
    bool from_background = false;           // for send_message
    bool update_stickersets_order = false;  // for send_message
    bool disable_web_page_preview = false;  // for send_message
    bool clear_draft = false;               // for send_message
    bool in_game_share = false;             // for send_message
    bool hide_via_bot = false;              // for resend_message
    bool is_bot_start_message = false;      // for resend_message

    bool have_previous = false;
    bool have_next = false;
    bool from_database = false;

    bool has_get_message_views_query = false;
    bool need_view_counter_increment = false;

    DialogId real_forward_from_dialog_id;    // for resend_message
    MessageId real_forward_from_message_id;  // for resend_message

    string send_emoji;  // for send_message

    NotificationId notification_id;
    NotificationId removed_notification_id;

    int32 max_reply_media_timestamp = -1;
    int32 max_own_media_timestamp = -2;  // to update replied messages on the first load

    int32 view_count = 0;
    int32 forward_count = 0;
    MessageReplyInfo reply_info;
    unique_ptr<MessageReactions> reactions;
    unique_ptr<DraftMessage> thread_draft_message;
    uint32 available_reactions_generation = 0;
    int32 interaction_info_update_date = 0;

    int32 legacy_layer = 0;

    int32 send_error_code = 0;
    string send_error_message;
    double try_resend_at = 0;

    int32 ttl_period = 0;       // counted from message send date
    int32 ttl = 0;              // counted from message content view date
    double ttl_expires_at = 0;  // only for TTL

    int64 media_album_id = 0;

    unique_ptr<MessageContent> content;

    unique_ptr<ReplyMarkup> reply_markup;

    int32 edited_schedule_date = 0;
    unique_ptr<MessageContent> edited_content;
    unique_ptr<ReplyMarkup> edited_reply_markup;
    uint64 edit_generation = 0;
    Promise<Unit> edit_promise;

    int32 last_edit_pts = 0;

    const char *debug_source = "null";

    unique_ptr<Message> left;
    unique_ptr<Message> right;

    mutable int32 last_access_date = 0;
    mutable bool is_update_sent = false;  // whether the message is known to the app

    mutable uint64 send_message_log_event_id = 0;

    mutable NetQueryRef send_query_ref;

    template <class StorerT>
    void store(StorerT &storer) const;

    template <class ParserT>
    void parse(ParserT &parser);
  };

  struct NotificationGroupInfo {
    NotificationGroupId group_id;
    int32 last_notification_date = 0;            // date of last notification in the group
    NotificationId last_notification_id;         // identifier of last notification in the group
    NotificationId max_removed_notification_id;  // notification identifier, up to which all notifications are removed
    MessageId max_removed_message_id;            // message identifier, up to which all notifications are removed
    bool is_changed = false;                     // true, if the group needs to be saved to database
    bool try_reuse = false;  // true, if the group needs to be deleted from database and tried to be reused

    template <class StorerT>
    void store(StorerT &storer) const;

    template <class ParserT>
    void parse(ParserT &parser);
  };

  struct Dialog {
    DialogId dialog_id;
    MessageId last_new_message_id;  // identifier of the last known server message received from update, there should be
                                    // no server messages after it
    MessageId last_message_id;      // identifier of the message after which currently there are no messages, i.e. a
                                    // message without a gap after it, memory only
    MessageId first_database_message_id;  // identifier of the first message in the database, needed
                                          // until there are no gaps in the database
    MessageId last_database_message_id;   // identifier of the last local or server message, if last_database_message_id
                                          // is known and last_message_id is known, then last_database_message_id <=
                                          // last_message_id

    std::array<MessageId, message_search_filter_count()> first_database_message_id_by_index;
    // use struct Count?
    std::array<int32, message_search_filter_count()> message_count_by_index{{0}};

    int32 server_unread_count = 0;
    int32 local_unread_count = 0;
    int32 unread_mention_count = 0;
    int32 unread_reaction_count = 0;
    int32 last_read_inbox_message_date = 0;  // secret chats only
    MessageId last_read_inbox_message_id;
    MessageId last_read_outbox_message_id;
    MessageId last_pinned_message_id;
    MessageId reply_markup_message_id;
    DialogNotificationSettings notification_settings;
    ChatReactions available_reactions;
    uint32 available_reactions_generation = 0;
    MessageTtl message_ttl;
    unique_ptr<DraftMessage> draft_message;
    unique_ptr<DialogActionBar> action_bar;
    LogEventIdWithGeneration save_draft_message_log_event_id;
    LogEventIdWithGeneration save_notification_settings_log_event_id;
    std::unordered_map<int64, LogEventIdWithGeneration> read_history_log_event_ids;
    std::unordered_set<MessageId, MessageIdHash> updated_read_history_message_ids;
    LogEventIdWithGeneration set_folder_id_log_event_id;
    InputGroupCallId active_group_call_id;
    InputGroupCallId expected_active_group_call_id;
    DialogId default_join_group_call_as_dialog_id;
    DialogId default_send_message_as_dialog_id;
    string theme_name;
    int32 pending_join_request_count = 0;
    vector<UserId> pending_join_request_user_ids;
    int32 have_full_history_source = 0;
    int32 unload_dialog_delay_seed = 0;
    int64 last_media_album_id = 0;

    FolderId folder_id;
    vector<DialogListId> dialog_list_ids;  // TODO replace with mask

    MessageId
        last_read_all_mentions_message_id;  // all mentions with a message identifier not greater than it are implicitly read
    MessageId
        max_unavailable_message_id;  // maximum unavailable message identifier for dialogs with cleared/unavailable history

    int32 last_clear_history_date = 0;
    MessageId last_clear_history_message_id;
    int64 order = DEFAULT_ORDER;
    MessageId deleted_last_message_id;
    int32 delete_last_message_date = 0;
    int32 pending_last_message_date = 0;
    MessageId pending_last_message_id;
    MessageId max_notification_message_id;
    MessageId last_edited_message_id;
    uint32 scheduled_messages_sync_generation = 0;
    uint32 last_repair_scheduled_messages_generation = 0;

    MessageId max_added_message_id;
    MessageId being_added_message_id;
    MessageId being_updated_last_new_message_id;
    MessageId being_updated_last_database_message_id;
    MessageId being_deleted_message_id;

    NotificationGroupInfo message_notification_group;
    NotificationGroupInfo mention_notification_group;
    NotificationId new_secret_chat_notification_id;  // secret chats only
    MessageId pinned_message_notification_message_id;

    bool has_contact_registered_message = false;

    bool is_last_message_deleted_locally = false;

    bool need_repair_action_bar = false;
    bool know_action_bar = false;
    bool has_outgoing_messages = false;

    bool is_opened = false;
    bool was_opened = false;

    bool need_restore_reply_markup = true;
    bool need_drop_default_send_message_as_dialog_id = false;

    bool have_full_history = false;
    bool is_empty = false;

    bool is_last_read_inbox_message_id_inited = false;
    bool is_last_read_outbox_message_id_inited = false;
    bool is_last_pinned_message_id_inited = false;
    bool is_folder_id_inited = false;
    bool need_repair_server_unread_count = false;
    bool need_repair_channel_server_unread_count = false;
    bool is_marked_as_unread = false;
    bool is_blocked = false;
    bool is_is_blocked_inited = false;
    bool last_sent_has_scheduled_messages = false;
    bool has_scheduled_server_messages = false;
    bool has_scheduled_database_messages = false;
    bool is_has_scheduled_database_messages_checked = false;
    bool has_loaded_scheduled_messages_from_database = false;
    bool sent_scheduled_messages = false;
    bool had_last_yet_unsent_message = false;  // whether the dialog was stored to database without last message
    bool has_active_group_call = false;
    bool is_group_call_empty = false;
    bool is_message_ttl_inited = false;
    bool has_expected_active_group_call_id = false;
    bool has_bots = false;
    bool is_has_bots_inited = false;
    bool is_theme_name_inited = false;
    bool is_available_reactions_inited = false;
    bool had_yet_unsent_message_id_overflow = false;

    bool increment_view_counter = false;

    bool is_update_new_chat_sent = false;
    bool is_update_new_chat_being_sent = false;
    bool has_unload_timeout = false;
    bool is_channel_difference_finished = false;

    bool suffix_load_done_ = false;
    bool suffix_load_has_query_ = false;

    int32 pts = 0;                                                 // for channels only
    int32 pending_read_channel_inbox_pts = 0;                      // for channels only
    int32 pending_read_channel_inbox_server_unread_count = 0;      // for channels only
    MessageId pending_read_channel_inbox_max_message_id;           // for channels only
    std::unordered_map<int64, MessageId> random_id_to_message_id;  // for secret chats and yet unsent messages only

    MessageId last_assigned_message_id;  // identifier of the last local or yet unsent message, assigned after
                                         // application start, used to guarantee that all assigned message identifiers
                                         // are different

    FlatHashMap<MessageId, std::set<MessageId>, MessageIdHash>
        yet_unsent_thread_message_ids;  // top_thread_message_id -> yet unsent message IDs

    FlatHashMap<ScheduledServerMessageId, int32, ScheduledServerMessageIdHash> scheduled_message_date;

    FlatHashMap<MessageId, MessageId, MessageIdHash> yet_unsent_message_id_to_persistent_message_id;

    FlatHashMap<int32, MessageId> last_assigned_scheduled_message_id;  // date -> message_id

    WaitFreeHashSet<MessageId, MessageIdHash> deleted_message_ids;
    FlatHashSet<ScheduledServerMessageId, ScheduledServerMessageIdHash> deleted_scheduled_server_message_ids;

    vector<std::pair<DialogId, MessageId>> pending_new_message_notifications;
    vector<std::pair<DialogId, MessageId>> pending_new_mention_notifications;

    FlatHashMap<NotificationId, MessageId, NotificationIdHash> notification_id_to_message_id;

    string client_data;

    // Load from newest to oldest message
    MessageId suffix_load_first_message_id_;  // identifier of some message such all suffix messages in range
                                              // [suffix_load_first_message_id_, last_message_id] are loaded
    MessageId suffix_load_query_message_id_;
    std::vector<std::pair<Promise<>, std::function<bool(const Message *)>>> suffix_load_queries_;

    FlatHashMap<MessageId, int64, MessageIdHash> pending_viewed_live_locations;  // message_id -> task_id
    FlatHashSet<MessageId, MessageIdHash> pending_viewed_message_ids;

    unique_ptr<Message> messages;
    unique_ptr<Message> scheduled_messages;

    struct MessageOp {
      enum : int8 { Add, SetPts, Delete, DeleteAll } type;
      bool from_update = false;
      bool have_previous = false;
      bool have_next = false;
      MessageContentType content_type = MessageContentType::None;
      int32 pts = 0;
      MessageId message_id;
      const char *source = nullptr;
      double date = 0;

      MessageOp(decltype(type) type, MessageId message_id, MessageContentType content_type, bool from_update,
                bool have_previous, bool have_next, const char *source)
          : type(type)
          , from_update(from_update)
          , have_previous(have_previous)
          , have_next(have_next)
          , content_type(content_type)
          , message_id(message_id)
          , source(source)
          , date(G()->server_time()) {
      }

      MessageOp(decltype(type) type, int32 pts, const char *source)
          : type(type), pts(pts), source(source), date(G()->server_time()) {
      }
    };

    const char *debug_set_dialog_last_database_message_id = "Unknown";  // to be removed soon
    vector<MessageOp> debug_message_op;

    // message identifiers loaded from database
    MessageId debug_last_new_message_id;
    MessageId debug_first_database_message_id;
    MessageId debug_last_database_message_id;

    Dialog() = default;
    Dialog(const Dialog &) = delete;
    Dialog &operator=(const Dialog &) = delete;
    Dialog(Dialog &&other) = delete;
    Dialog &operator=(Dialog &&other) = delete;
    ~Dialog() = default;

    template <class StorerT>
    void store(StorerT &storer) const;

    template <class ParserT>
    void parse(ParserT &parser);
  };

  struct RecommendedDialogFilter {
    unique_ptr<DialogFilter> dialog_filter;
    string description;
  };

  struct DialogList {
    DialogListId dialog_list_id;
    bool is_message_unread_count_inited_ = false;
    bool is_dialog_unread_count_inited_ = false;
    bool need_unread_count_recalc_ = true;
    int32 unread_message_total_count_ = 0;
    int32 unread_message_muted_count_ = 0;
    int32 unread_dialog_total_count_ = 0;
    int32 unread_dialog_muted_count_ = 0;
    int32 unread_dialog_marked_count_ = 0;
    int32 unread_dialog_muted_marked_count_ = 0;
    int32 in_memory_dialog_total_count_ = 0;
    int32 server_dialog_total_count_ = -1;
    int32 secret_chat_total_count_ = -1;

    vector<Promise<Unit>> load_list_queries_;

    FlatHashMap<DialogId, int64, DialogIdHash> pinned_dialog_id_orders_;
    vector<DialogDate> pinned_dialogs_;
    bool are_pinned_dialogs_inited_ = false;

    DialogDate last_pinned_dialog_date_ = MIN_DIALOG_DATE;  // in memory

    // date of the last loaded dialog
    // min(folder1_last_dialog_date_, folder2_last_dialog_date, last_pinned_dialog_date_)
    DialogDate list_last_dialog_date_ = MIN_DIALOG_DATE;  // in memory
  };

  struct DialogFolder {
    FolderId folder_id;
    // date of the last loaded dialog in the folder
    DialogDate folder_last_dialog_date_{MAX_ORDINARY_DIALOG_ORDER, DialogId()};  // in memory

    std::set<DialogDate> ordered_dialogs_;  // all known dialogs, including with default order

    // date of last known user/group/channel dialog in the right order
    DialogDate last_server_dialog_date_{MAX_ORDINARY_DIALOG_ORDER, DialogId()};
    DialogDate last_loaded_database_dialog_date_{MAX_ORDINARY_DIALOG_ORDER, DialogId()};
    DialogDate last_database_server_dialog_date_{MAX_ORDINARY_DIALOG_ORDER, DialogId()};

    MultiPromiseActor load_folder_dialog_list_multipromise_{
        "LoadDialogListMultiPromiseActor"};  // must be defined before pending_on_get_dialogs_
    int32 load_dialog_list_limit_max_ = 0;
  };

  class DialogListViewIterator {
    MessagesManager *messages_manager_;
    const DialogListId *dialog_list_id_;

   public:
    DialogListViewIterator(MessagesManager *messages_manager, const DialogListId *dialog_list_id)
        : messages_manager_(messages_manager), dialog_list_id_(dialog_list_id) {
    }

    DialogList &operator*() const {
      auto dialog_list_ptr = messages_manager_->get_dialog_list(*dialog_list_id_);
      CHECK(dialog_list_ptr != nullptr);
      return *dialog_list_ptr;
    }

    bool operator!=(const DialogListViewIterator &other) const {
      return dialog_list_id_ != other.dialog_list_id_;
    }

    void operator++() {
      dialog_list_id_++;
    }
  };

  class DialogListView {
    MessagesManager *messages_manager_;
    // TODO can be optimized to store only mask of dialog lists
    vector<DialogListId> dialog_list_ids_;

   public:
    DialogListView(MessagesManager *messages_manager, vector<DialogListId> dialog_list_ids)
        : messages_manager_(messages_manager), dialog_list_ids_(std::move(dialog_list_ids)) {
    }

    DialogListViewIterator begin() {
      return DialogListViewIterator(messages_manager_, dialog_list_ids_.empty() ? nullptr : &dialog_list_ids_[0]);
    }

    DialogListViewIterator end() {
      return DialogListViewIterator(
          messages_manager_, dialog_list_ids_.empty() ? nullptr : &dialog_list_ids_[0] + dialog_list_ids_.size());
    }
  };

  struct DialogPositionInList {
    int64 order = DEFAULT_ORDER;
    int64 private_order = 0;
    int64 public_order = 0;
    bool is_pinned = false;
    bool is_sponsored = false;

    int32 total_dialog_count = 0;

    friend StringBuilder &operator<<(StringBuilder &string_builder, const DialogPositionInList &order) {
      return string_builder << "order = " << order.order << ", private_order = " << order.private_order
                            << ", public_order = " << order.public_order << ", is_pinned = " << order.is_pinned
                            << ", is_sponsored = " << order.is_sponsored
                            << ", total_dialog_count = " << order.total_dialog_count;
    }
  };

  class MessagesIteratorBase {
    vector<const Message *> stack_;

   protected:
    MessagesIteratorBase() = default;

    // points iterator to message with greatest id which is less or equal than message_id
    MessagesIteratorBase(const Message *root, MessageId message_id) {
      size_t last_right_pos = 0;
      while (root != nullptr) {
        //        LOG(DEBUG) << "Have root->message_id = " << root->message_id;
        stack_.push_back(root);
        if (root->message_id <= message_id) {
          //          LOG(DEBUG) << "Go right";
          last_right_pos = stack_.size();
          root = root->right.get();
        } else {
          //          LOG(DEBUG) << "Go left";
          root = root->left.get();
        }
      }
      stack_.resize(last_right_pos);
    }

    const Message *operator*() const {
      return stack_.empty() ? nullptr : stack_.back();
    }

    ~MessagesIteratorBase() = default;

   public:
    MessagesIteratorBase(const MessagesIteratorBase &) = delete;
    MessagesIteratorBase &operator=(const MessagesIteratorBase &) = delete;
    MessagesIteratorBase(MessagesIteratorBase &&other) = default;
    MessagesIteratorBase &operator=(MessagesIteratorBase &&other) = default;

    void operator++() {
      if (stack_.empty()) {
        return;
      }

      const Message *cur = stack_.back();
      if (!cur->have_next) {
        stack_.clear();
        return;
      }
      if (cur->right == nullptr) {
        while (true) {
          stack_.pop_back();
          if (stack_.empty()) {
            return;
          }
          const Message *new_cur = stack_.back();
          if (new_cur->left.get() == cur) {
            return;
          }
          cur = new_cur;
        }
      }

      cur = cur->right.get();
      while (cur != nullptr) {
        stack_.push_back(cur);
        cur = cur->left.get();
      }
    }

    void operator--() {
      if (stack_.empty()) {
        return;
      }

      const Message *cur = stack_.back();
      if (!cur->have_previous) {
        stack_.clear();
        return;
      }
      if (cur->left == nullptr) {
        while (true) {
          stack_.pop_back();
          if (stack_.empty()) {
            return;
          }
          const Message *new_cur = stack_.back();
          if (new_cur->right.get() == cur) {
            return;
          }
          cur = new_cur;
        }
      }

      cur = cur->left.get();
      while (cur != nullptr) {
        stack_.push_back(cur);
        cur = cur->right.get();
      }
    }
  };

  class MessagesIterator final : public MessagesIteratorBase {
   public:
    MessagesIterator() = default;

    MessagesIterator(Dialog *d, MessageId message_id)
        : MessagesIteratorBase(message_id.is_scheduled() ? d->scheduled_messages.get() : d->messages.get(),
                               message_id) {
    }

    Message *operator*() const {
      return const_cast<Message *>(MessagesIteratorBase::operator*());
    }
  };

  class MessagesConstIterator final : public MessagesIteratorBase {
   public:
    MessagesConstIterator() = default;

    MessagesConstIterator(const Dialog *d, MessageId message_id)
        : MessagesIteratorBase(message_id.is_scheduled() ? d->scheduled_messages.get() : d->messages.get(),
                               message_id) {
    }

    const Message *operator*() const {
      return MessagesIteratorBase::operator*();
    }
  };

  struct PendingSecretMessage {
    enum class Type : int32 { NewMessage, DeleteMessages, DeleteHistory };
    Type type = Type::NewMessage;

    // for NewMessage
    MessageInfo message_info;
    MultiPromiseActor load_data_multipromise{"LoadPendingSecretMessageDataMultiPromiseActor"};

    // for DeleteMessages/DeleteHistory
    DialogId dialog_id;
    vector<int64> random_ids;
    MessageId last_message_id;
    bool remove_from_dialog_list = false;

    Promise<> success_promise;
  };

  struct MessageSendOptions {
    bool disable_notification = false;
    bool from_background = false;
    bool update_stickersets_order = false;
    bool protect_content = false;
    int32 schedule_date = 0;

    MessageSendOptions() = default;
    MessageSendOptions(bool disable_notification, bool from_background, bool update_stickersets_order,
                       bool protect_content, int32 schedule_date)
        : disable_notification(disable_notification)
        , from_background(from_background)
        , update_stickersets_order(update_stickersets_order)
        , protect_content(protect_content)
        , schedule_date(schedule_date) {
    }
  };

  class BlockMessageSenderFromRepliesOnServerLogEvent;
  class DeleteAllCallMessagesOnServerLogEvent;
  class DeleteAllChannelMessagesFromSenderOnServerLogEvent;
  class DeleteDialogHistoryOnServerLogEvent;
  class DeleteDialogMessagesByDateOnServerLogEvent;
  class DeleteMessageLogEvent;
  class DeleteMessagesOnServerLogEvent;
  class DeleteScheduledMessagesOnServerLogEvent;
  class ForwardMessagesLogEvent;
  class GetChannelDifferenceLogEvent;
  class ReadAllDialogMentionsOnServerLogEvent;
  class ReadAllDialogReactionsOnServerLogEvent;
  class ReadHistoryInSecretChatLogEvent;
  class ReadHistoryOnServerLogEvent;
  class ReadMessageContentsOnServerLogEvent;
  class ReadMessageThreadHistoryOnServerLogEvent;
  class RegetDialogLogEvent;
  class ReorderPinnedDialogsOnServerLogEvent;
  class ResetAllNotificationSettingsOnServerLogEvent;
  class SaveDialogDraftMessageOnServerLogEvent;
  class SendBotStartMessageLogEvent;
  class SendInlineQueryResultMessageLogEvent;
  class SendMessageLogEvent;
  class SendScreenshotTakenNotificationMessageLogEvent;
  class SetDialogFolderIdOnServerLogEvent;
  class ToggleDialogIsBlockedOnServerLogEvent;
  class ToggleDialogIsMarkedAsUnreadOnServerLogEvent;
  class ToggleDialogIsPinnedOnServerLogEvent;
  class ToggleDialogReportSpamStateOnServerLogEvent;
  class UnpinAllDialogMessagesOnServerLogEvent;
  class UpdateDialogNotificationSettingsOnServerLogEvent;

  class DialogFiltersLogEvent;

  static constexpr size_t MAX_GROUPED_MESSAGES = 10;               // server side limit
  static constexpr int32 MAX_GET_DIALOGS = 100;                    // server side limit
  static constexpr int32 MAX_GET_HISTORY = 100;                    // server side limit
  static constexpr int32 MAX_SEARCH_MESSAGES = 100;                // server side limit
  static constexpr int32 MIN_SEARCH_PUBLIC_DIALOG_PREFIX_LEN = 4;  // server side limit
  static constexpr int32 MIN_CHANNEL_DIFFERENCE = 1;
  static constexpr int32 MAX_CHANNEL_DIFFERENCE = 100;
  static constexpr int32 MAX_BOT_CHANNEL_DIFFERENCE = 100000;   // server side limit
  static constexpr int32 MAX_RECENT_DIALOGS = 50;               // some reasonable value
  static constexpr size_t MAX_TITLE_LENGTH = 128;               // server side limit for chat title
  static constexpr size_t MAX_DESCRIPTION_LENGTH = 255;         // server side limit for chat description
  static constexpr size_t MAX_DIALOG_FILTER_TITLE_LENGTH = 12;  // server side limit for dialog filter title
  static constexpr int32 MAX_PRIVATE_MESSAGE_TTL = 60;          // server side limit
  static constexpr int32 DIALOG_FILTERS_CACHE_TIME = 86400;

  static constexpr int64 SPONSORED_DIALOG_ORDER = static_cast<int64>(2147483647) << 32;
  static constexpr int32 MIN_PINNED_DIALOG_DATE = 2147000000;  // some big date
  static constexpr int64 MAX_ORDINARY_DIALOG_ORDER =
      9221294780217032704;  // == get_dialog_order(MessageId(), MIN_PINNED_DIALOG_DATE - 1)

  static constexpr int32 UPDATE_CHANNEL_TO_LONG_FLAG_HAS_PTS = 1 << 0;

  static constexpr int32 CHANNEL_DIFFERENCE_FLAG_IS_FINAL = 1 << 0;
  static constexpr int32 CHANNEL_DIFFERENCE_FLAG_HAS_TIMEOUT = 1 << 1;

  static constexpr int32 DIALOG_FLAG_HAS_PTS = 1 << 0;
  static constexpr int32 DIALOG_FLAG_HAS_DRAFT = 1 << 1;
  static constexpr int32 DIALOG_FLAG_IS_PINNED = 1 << 2;
  static constexpr int32 DIALOG_FLAG_HAS_FOLDER_ID = 1 << 4;

  static constexpr int32 MAX_MESSAGE_VIEW_DELAY = 1;  // seconds
  static constexpr int32 MIN_SAVE_DRAFT_DELAY = 1;    // seconds
  static constexpr int32 MIN_READ_HISTORY_DELAY = 3;  // seconds
  static constexpr int32 MAX_SAVE_DIALOG_DELAY = 0;   // seconds

  static constexpr int32 LIVE_LOCATION_VIEW_PERIOD = 60;      // seconds, server-side limit
  static constexpr int32 UPDATE_VIEWED_MESSAGES_PERIOD = 15;  // seconds

  static constexpr int32 USERNAME_CACHE_EXPIRE_TIME = 3 * 86400;
  static constexpr int32 USERNAME_CACHE_EXPIRE_TIME_SHORT = 900;
  static constexpr int32 AUTH_NOTIFICATION_ID_CACHE_TIME = 7 * 86400;
  static constexpr size_t MAX_SAVED_AUTH_NOTIFICATION_IDS = 100;

  static constexpr int32 ONLINE_MEMBER_COUNT_UPDATE_TIME = 5 * 60;

  static constexpr int32 MAX_RESEND_DELAY = 86400;  // seconds, some resonable limit

  static constexpr int32 SCHEDULE_WHEN_ONLINE_DATE = 2147483646;

  static constexpr double DIALOG_ACTION_TIMEOUT = 5.5;

  static constexpr const char *DELETE_MESSAGE_USER_REQUEST_SOURCE = "user request";

  static constexpr bool DROP_SEND_MESSAGE_UPDATES = false;

  static int32 get_message_date(const tl_object_ptr<telegram_api::Message> &message_ptr);

  static bool is_dialog_inited(const Dialog *d);

  int32 get_dialog_mute_until(const Dialog *d) const;

  bool is_dialog_muted(const Dialog *d) const;

  bool is_dialog_pinned_message_notifications_disabled(const Dialog *d) const;

  bool is_dialog_mention_notifications_disabled(const Dialog *d) const;

  bool is_dialog_pinned(DialogListId dialog_list_id, DialogId dialog_id) const;

  int64 get_dialog_pinned_order(DialogListId dialog_list_id, DialogId dialog_id) const;

  static int64 get_dialog_pinned_order(const DialogList *list, DialogId dialog_id);

  void open_dialog(Dialog *d);

  void close_dialog(Dialog *d);

  DialogId get_my_dialog_id() const;

  void on_resolve_secret_chat_message_via_bot_username(const string &via_bot_username, MessageInfo *message_info_ptr,
                                                       Promise<Unit> &&promise);

  void add_secret_message(unique_ptr<PendingSecretMessage> pending_secret_message, Promise<Unit> lock_promise = {});

  void on_add_secret_message_ready(int64 token);

  void finish_add_secret_message(unique_ptr<PendingSecretMessage> pending_secret_message);

  void finish_delete_secret_messages(DialogId dialog_id, std::vector<int64> random_ids, Promise<> promise);

  void finish_delete_secret_chat_history(DialogId dialog_id, bool remove_from_dialog_list, MessageId last_message_id,
                                         Promise<> promise);

  MessageInfo parse_telegram_api_message(tl_object_ptr<telegram_api::Message> message_ptr, bool is_scheduled,
                                         const char *source) const;

  std::pair<DialogId, unique_ptr<Message>> create_message(MessageInfo &&message_info, bool is_channel_message);

  MessageId find_old_message_id(DialogId dialog_id, MessageId message_id) const;

  void delete_update_message_id(DialogId dialog_id, MessageId message_id);

  void get_dialog_message_count_from_server(DialogId dialog_id, MessageSearchFilter filter, Promise<int32> &&promise);

  FullMessageId on_get_message(MessageInfo &&message_info, bool from_update, bool is_channel_message,
                               bool have_previous, bool have_next, const char *source);

  Result<InputMessageContent> process_input_message_content(
      DialogId dialog_id, tl_object_ptr<td_api::InputMessageContent> &&input_message_content);

  Result<MessageCopyOptions> process_message_copy_options(DialogId dialog_id,
                                                          tl_object_ptr<td_api::messageCopyOptions> &&options) const;

  Result<MessageSendOptions> process_message_send_options(DialogId dialog_id,
                                                          tl_object_ptr<td_api::messageSendOptions> &&options,
                                                          bool allow_update_stickersets_order) const;

  static Status can_use_message_send_options(const MessageSendOptions &options,
                                             const unique_ptr<MessageContent> &content, int32 ttl);
  static Status can_use_message_send_options(const MessageSendOptions &options, const InputMessageContent &content);

  Status can_use_top_thread_message_id(Dialog *d, MessageId top_thread_message_id, MessageId reply_to_message_id);

  bool is_anonymous_administrator(DialogId dialog_id, string *author_signature) const;

  int64 generate_new_random_id(const Dialog *d);

  unique_ptr<Message> create_message_to_send(Dialog *d, MessageId top_thread_message_id, MessageId reply_to_message_id,
                                             const MessageSendOptions &options, unique_ptr<MessageContent> &&content,
                                             bool suppress_reply_info, unique_ptr<MessageForwardInfo> forward_info,
                                             bool is_copy, DialogId send_as_dialog_id) const;

  Message *get_message_to_send(Dialog *d, MessageId top_thread_message_id, MessageId reply_to_message_id,
                               const MessageSendOptions &options, unique_ptr<MessageContent> &&content,
                               bool *need_update_dialog_pos, bool suppress_reply_info = false,
                               unique_ptr<MessageForwardInfo> forward_info = nullptr, bool is_copy = false,
                               DialogId sender_dialog_id = DialogId());

  int64 begin_send_message(DialogId dialog_id, const Message *m);

  Status can_send_message(DialogId dialog_id) const TD_WARN_UNUSED_RESULT;

  bool can_resend_message(const Message *m) const;

  bool can_edit_message(DialogId dialog_id, const Message *m, bool is_editing, bool only_reply_markup = false) const;

  bool has_qts_messages(DialogId dialog_id) const;

  bool can_report_dialog(DialogId dialog_id) const;

  Status can_pin_messages(DialogId dialog_id) const;

  static Status can_get_media_timestamp_link(DialogId dialog_id, const Message *m);

  bool can_report_message_reactions(DialogId dialog_id, const Message *m) const;

  Status can_get_message_viewers(FullMessageId full_message_id) TD_WARN_UNUSED_RESULT;

  Status can_get_message_viewers(DialogId dialog_id, const Message *m) const TD_WARN_UNUSED_RESULT;

  void cancel_edit_message_media(DialogId dialog_id, Message *m, Slice error_message);

  void on_message_media_edited(DialogId dialog_id, MessageId message_id, FileId file_id, FileId thumbnail_file_id,
                               bool was_uploaded, bool was_thumbnail_uploaded, string file_reference,
                               int32 schedule_date, uint64 generation, Result<int32> &&result);

  static MessageId get_persistent_message_id(const Dialog *d, MessageId message_id);

  static FullMessageId get_replied_message_id(DialogId dialog_id, const Message *m);

  MessageId get_reply_to_message_id(Dialog *d, MessageId top_thread_message_id, MessageId message_id, bool for_draft);

  void fix_server_reply_to_message_id(DialogId dialog_id, MessageId message_id, DialogId reply_in_dialog_id,
                                      MessageId &reply_to_message_id) const;

  bool can_set_game_score(DialogId dialog_id, const Message *m) const;

  void add_postponed_channel_update(DialogId dialog_id, tl_object_ptr<telegram_api::Update> &&update, int32 new_pts,
                                    int32 pts_count, Promise<Unit> &&promise);

  void process_channel_update(tl_object_ptr<telegram_api::Update> &&update_ptr);

  void on_message_edited(FullMessageId full_message_id, int32 pts, bool had_message);

  void delete_messages_from_updates(const vector<MessageId> &message_ids);

  void delete_dialog_messages(DialogId dialog_id, const vector<MessageId> &message_ids, bool from_updates,
                              bool skip_update_for_not_found_messages, const char *source);

  void update_dialog_pinned_messages_from_updates(DialogId dialog_id, const vector<MessageId> &message_ids,
                                                  bool is_pin);

  bool update_message_is_pinned(Dialog *d, Message *m, bool is_pin, const char *source);

  void do_forward_messages(DialogId to_dialog_id, DialogId from_dialog_id, const vector<Message *> &messages,
                           const vector<MessageId> &message_ids, bool drop_author, bool drop_media_captions,
                           uint64 log_event_id);

  void send_forward_message_query(int32 flags, DialogId to_dialog_id, DialogId from_dialog_id,
                                  tl_object_ptr<telegram_api::InputPeer> as_input_peer, vector<MessageId> message_ids,
                                  vector<int64> random_ids, int32 schedule_date, Promise<Unit> promise);

  Result<td_api::object_ptr<td_api::message>> forward_message(DialogId to_dialog_id, DialogId from_dialog_id,
                                                              MessageId message_id,
                                                              tl_object_ptr<td_api::messageSendOptions> &&options,
                                                              bool in_game_share,
                                                              MessageCopyOptions &&copy_options) TD_WARN_UNUSED_RESULT;

  unique_ptr<MessageForwardInfo> create_message_forward_info(DialogId from_dialog_id, DialogId to_dialog_id,
                                                             const Message *forwarded_message) const;

  void fix_forwarded_message(Message *m, DialogId to_dialog_id, const Message *forwarded_message, int64 media_album_id,
                             bool drop_author) const;

  struct ForwardedMessages {
    struct CopiedMessage {
      unique_ptr<MessageContent> content;
      MessageId top_thread_message_id;
      MessageId reply_to_message_id;
      MessageId original_message_id;
      MessageId original_reply_to_message_id;
      unique_ptr<ReplyMarkup> reply_markup;
      int64 media_album_id;
      bool disable_web_page_preview;
      size_t index;
    };
    vector<CopiedMessage> copied_messages;

    struct ForwardedMessageContent {
      unique_ptr<MessageContent> content;
      int64 media_album_id;
      size_t index;
    };
    vector<ForwardedMessageContent> forwarded_message_contents;
    bool drop_author = false;
    bool drop_media_captions = false;

    Dialog *from_dialog;
    Dialog *to_dialog;
    MessageSendOptions message_send_options;
  };

  Result<ForwardedMessages> get_forwarded_messages(DialogId to_dialog_id, DialogId from_dialog_id,
                                                   const vector<MessageId> &message_ids,
                                                   tl_object_ptr<td_api::messageSendOptions> &&options,
                                                   bool in_game_share, vector<MessageCopyOptions> &&copy_options);

  void do_send_media(DialogId dialog_id, Message *m, FileId file_id, FileId thumbnail_file_id,
                     tl_object_ptr<telegram_api::InputFile> input_file,
                     tl_object_ptr<telegram_api::InputFile> input_thumbnail);

  void do_send_secret_media(DialogId dialog_id, Message *m, FileId file_id, FileId thumbnail_file_id,
                            tl_object_ptr<telegram_api::InputEncryptedFile> input_encrypted_file,
                            BufferSlice thumbnail);

  void do_send_message(DialogId dialog_id, const Message *m, vector<int> bad_parts = {});

  void on_message_media_uploaded(DialogId dialog_id, const Message *m,
                                 tl_object_ptr<telegram_api::InputMedia> &&input_media, FileId file_id,
                                 FileId thumbnail_file_id);

  void on_secret_message_media_uploaded(DialogId dialog_id, const Message *m, SecretInputMedia &&secret_input_media,
                                        FileId file_id, FileId thumbnail_file_id);

  void on_upload_message_media_finished(int64 media_album_id, DialogId dialog_id, MessageId message_id, Status result);

  void do_send_message_group(int64 media_album_id);

  void on_text_message_ready_to_send(DialogId dialog_id, MessageId message_id);

  void on_media_message_ready_to_send(DialogId dialog_id, MessageId message_id, Promise<Message *> &&promise);

  void send_secret_message(DialogId dialog_id, const Message *m, SecretInputMedia media);

  void on_yet_unsent_media_queue_updated(DialogId dialog_id);

  static void save_send_bot_start_message_log_event(UserId bot_user_id, DialogId dialog_id, const string &parameter,
                                                    const Message *m);

  void do_send_bot_start_message(UserId bot_user_id, DialogId dialog_id, MessageId message_id, const string &parameter);

  static void save_send_inline_query_result_message_log_event(DialogId dialog_id, const Message *m, int64 query_id,
                                                              const string &result_id);

  void do_send_inline_query_result_message(DialogId dialog_id, MessageId message_id, int64 query_id,
                                           const string &result_id);

  static uint64 save_send_screenshot_taken_notification_message_log_event(DialogId dialog_id, const Message *m);

  void do_send_screenshot_taken_notification_message(DialogId dialog_id, const Message *m, uint64 log_event_id);

  void restore_message_reply_to_message_id(Dialog *d, Message *m);

  Message *continue_send_message(DialogId dialog_id, unique_ptr<Message> &&m, uint64 log_event_id);

  bool is_message_unload_enabled() const;

  int64 generate_new_media_album_id();

  static bool can_forward_message(DialogId from_dialog_id, const Message *m);

  bool can_save_message(DialogId dialog_id, const Message *m) const;

  bool can_get_message_statistics(DialogId dialog_id, const Message *m) const;

  struct CanDeleteDialog {
    bool for_self_;
    bool for_all_users_;

    CanDeleteDialog(bool for_self, bool for_all_users) : for_self_(for_self), for_all_users_(for_all_users) {
    }
  };
  CanDeleteDialog can_delete_dialog(const Dialog *d) const;

  static bool can_delete_channel_message(const DialogParticipantStatus &status, const Message *m, bool is_bot);

  bool can_delete_message(DialogId dialog_id, const Message *m) const;

  bool can_revoke_message(DialogId dialog_id, const Message *m) const;

  bool can_unload_message(const Dialog *d, const Message *m) const;

  void unload_message(Dialog *d, MessageId message_id);

  unique_ptr<Message> delete_message(Dialog *d, MessageId message_id, bool is_permanently_deleted,
                                     bool *need_update_dialog_pos, const char *source);

  unique_ptr<Message> do_delete_message(Dialog *d, MessageId message_id, bool is_permanently_deleted,
                                        bool only_from_memory, bool *need_update_dialog_pos, const char *source);

  unique_ptr<Message> do_delete_scheduled_message(Dialog *d, MessageId message_id, bool is_permanently_deleted,
                                                  const char *source);

  void on_message_deleted(Dialog *d, Message *m, bool is_permanently_deleted, const char *source);

  static bool is_deleted_message(const Dialog *d, MessageId message_id);

  int32 get_unload_dialog_delay() const;

  double get_next_unload_dialog_delay(Dialog *d) const;

  void unload_dialog(DialogId dialog_id);

  void delete_all_dialog_messages(Dialog *d, bool remove_from_dialog_list, bool is_permanently_deleted);

  void do_delete_all_dialog_messages(Dialog *d, unique_ptr<Message> &message, bool is_permanently_deleted,
                                     vector<int64> &deleted_message_ids);

  void erase_delete_messages_log_event(uint64 log_event_id);

  void delete_sent_message_on_server(DialogId dialog_id, MessageId message_id, MessageId old_message_id);

  void delete_messages_on_server(DialogId dialog_id, vector<MessageId> message_ids, bool revoke, uint64 log_event_id,
                                 Promise<Unit> &&promise);

  void delete_scheduled_messages_on_server(DialogId dialog_id, vector<MessageId> message_ids, uint64 log_event_id,
                                           Promise<Unit> &&promise);

  void delete_dialog_history_on_server(DialogId dialog_id, MessageId max_message_id, bool remove_from_dialog_list,
                                       bool revoke, bool allow_error, uint64 log_event_id, Promise<Unit> &&promise);

  void delete_all_call_messages_on_server(bool revoke, uint64 log_event_id, Promise<Unit> &&promise);

  void block_message_sender_from_replies_on_server(MessageId message_id, bool need_delete_message,
                                                   bool need_delete_all_messages, bool report_spam, uint64 log_event_id,
                                                   Promise<Unit> &&promise);

  void delete_all_channel_messages_by_sender_on_server(ChannelId channel_id, DialogId sender_dialog_id,
                                                       uint64 log_event_id, Promise<Unit> &&promise);

  void delete_dialog_messages_by_date_on_server(DialogId dialog_id, int32 min_date, int32 max_date, bool revoke,
                                                uint64 log_event_id, Promise<Unit> &&promise);

  void read_all_dialog_mentions_on_server(DialogId dialog_id, uint64 log_event_id, Promise<Unit> &&promise);

  void read_all_dialog_reactions_on_server(DialogId dialog_id, uint64 log_event_id, Promise<Unit> &&promise);

  void unpin_all_dialog_messages_on_server(DialogId dialog_id, uint64 log_event_id, Promise<Unit> &&promise);

  using AffectedHistoryQuery = std::function<void(DialogId, Promise<AffectedHistory>)>;

  void run_affected_history_query_until_complete(DialogId dialog_id, AffectedHistoryQuery query,
                                                 bool get_affected_messages, Promise<Unit> &&promise);

  void on_get_affected_history(DialogId dialog_id, AffectedHistoryQuery query, bool get_affected_messages,
                               AffectedHistory affected_history, Promise<Unit> &&promise);

  static MessageId find_message_by_date(const Message *m, int32 date);

  static void find_messages_by_date(const Message *m, int32 min_date, int32 max_date, vector<MessageId> &message_ids);

  static void find_messages(const Message *m, vector<MessageId> &message_ids,
                            const std::function<bool(const Message *)> &condition);

  static void find_old_messages(const Message *m, MessageId max_message_id, vector<MessageId> &message_ids);

  static void find_newer_messages(const Message *m, MessageId min_message_id, vector<MessageId> &message_ids);

  void find_unloadable_messages(const Dialog *d, int32 unload_before_date, const Message *m,
                                vector<MessageId> &message_ids, bool &has_left_to_unload_messages) const;

  void on_pending_message_views_timeout(DialogId dialog_id);

  void update_message_interaction_info(FullMessageId full_message_id, int32 view_count, int32 forward_count,
                                       bool has_reply_info, tl_object_ptr<telegram_api::messageReplies> &&reply_info,
                                       bool has_reactions, unique_ptr<MessageReactions> &&reactions);

  bool is_active_message_reply_info(DialogId dialog_id, const MessageReplyInfo &info) const;

  bool is_visible_message_reply_info(DialogId dialog_id, const Message *m) const;

  bool is_visible_message_reactions(DialogId dialog_id, const Message *m) const;

  bool has_unread_message_reactions(DialogId dialog_id, const Message *m) const;

  void on_message_reply_info_changed(DialogId dialog_id, const Message *m) const;

  Result<FullMessageId> get_top_thread_full_message_id(DialogId dialog_id, const Message *m) const;

  td_api::object_ptr<td_api::messageInteractionInfo> get_message_interaction_info_object(DialogId dialog_id,
                                                                                         const Message *m) const;

  vector<td_api::object_ptr<td_api::unreadReaction>> get_unread_reactions_object(DialogId dialog_id,
                                                                                 const Message *m) const;

  bool update_message_interaction_info(Dialog *d, Message *m, int32 view_count, int32 forward_count,
                                       bool has_reply_info, MessageReplyInfo &&reply_info, bool has_reactions,
                                       unique_ptr<MessageReactions> &&reactions, const char *source);

  bool update_message_contains_unread_mention(Dialog *d, Message *m, bool contains_unread_mention, const char *source);

  bool remove_message_unread_reactions(Dialog *d, Message *m, const char *source);

  void read_message_content_from_updates(MessageId message_id);

  void read_channel_message_content_from_updates(Dialog *d, MessageId message_id);

  bool read_message_content(Dialog *d, Message *m, bool is_local_read, const char *source);

  void read_message_contents_on_server(DialogId dialog_id, vector<MessageId> message_ids, uint64 log_event_id,
                                       Promise<Unit> &&promise, bool skip_log_event = false);

  bool has_incoming_notification(DialogId dialog_id, const Message *m) const;

  int32 calc_new_unread_count_from_last_unread(Dialog *d, MessageId max_message_id, MessageType type) const;

  int32 calc_new_unread_count_from_the_end(Dialog *d, MessageId max_message_id, MessageType type,
                                           int32 hint_unread_count) const;

  int32 calc_new_unread_count(Dialog *d, MessageId max_message_id, MessageType type, int32 hint_unread_count) const;

  void repair_server_unread_count(DialogId dialog_id, int32 unread_count, const char *source);

  void repair_channel_server_unread_count(Dialog *d);

  void read_history_outbox(DialogId dialog_id, MessageId max_message_id, int32 read_date = -1);

  void read_history_on_server(Dialog *d, MessageId max_message_id);

  void do_read_history_on_server(DialogId dialog_id);

  void read_history_on_server_impl(Dialog *d, MessageId max_message_id);

  void read_message_thread_history_on_server_impl(Dialog *d, MessageId top_thread_message_id, MessageId max_message_id);

  void on_read_history_finished(DialogId dialog_id, MessageId top_thread_message_id, uint64 generation);

  void read_message_thread_history_on_server(Dialog *d, MessageId top_thread_message_id, MessageId max_message_id,
                                             MessageId last_message_id);

  void read_secret_chat_outbox_inner(DialogId dialog_id, int32 up_to_date, int32 read_date);

  void set_dialog_max_unavailable_message_id(DialogId dialog_id, MessageId max_unavailable_message_id, bool from_update,
                                             const char *source);

  void set_dialog_online_member_count(DialogId dialog_id, int32 online_member_count, bool is_from_server,
                                      const char *source);

  void on_update_dialog_online_member_count_timeout(DialogId dialog_id);

  void on_update_viewed_messages_timeout(DialogId dialog_id);

  bool delete_newer_server_messages_at_the_end(Dialog *d, MessageId max_message_id);

  template <class T, class It>
  vector<MessageId> get_message_history_slice(const T &begin, It it, const T &end, MessageId from_message_id,
                                              int32 offset, int32 limit);

  void preload_newer_messages(const Dialog *d, MessageId max_message_id);

  void preload_older_messages(const Dialog *d, MessageId min_message_id);

  void on_get_history_from_database(DialogId dialog_id, MessageId from_message_id,
                                    MessageId old_last_database_message_id, int32 offset, int32 limit,
                                    bool from_the_end, bool only_local, vector<MessagesDbDialogMessage> &&messages,
                                    Promise<Unit> &&promise);

  void get_history_from_the_end(DialogId dialog_id, bool from_database, bool only_local, Promise<Unit> &&promise);

  void get_history_from_the_end_impl(const Dialog *d, bool from_database, bool only_local, Promise<Unit> &&promise,
                                     const char *source);

  void get_history(DialogId dialog_id, MessageId from_message_id, int32 offset, int32 limit, bool from_database,
                   bool only_local, Promise<Unit> &&promise);

  void get_history_impl(const Dialog *d, MessageId from_message_id, int32 offset, int32 limit, bool from_database,
                        bool only_local, Promise<Unit> &&promise);

  void load_messages(DialogId dialog_id, MessageId from_message_id, int32 offset, int32 limit, int left_tries,
                     bool only_local, Promise<Unit> &&promise);

  void load_messages_impl(const Dialog *d, MessageId from_message_id, int32 offset, int32 limit, int left_tries,
                          bool only_local, Promise<Unit> &&promise);

  void load_dialog_scheduled_messages(DialogId dialog_id, bool from_database, int64 hash, Promise<Unit> &&promise);

  void on_get_scheduled_messages_from_database(DialogId dialog_id, vector<MessagesDbDialogMessage> &&messages);

  static int32 get_random_y(MessageId message_id);

  static void set_message_id(unique_ptr<Message> &message, MessageId message_id);

  static bool is_allowed_useless_update(const tl_object_ptr<telegram_api::Update> &update);

  bool is_message_auto_read(DialogId dialog_id, bool is_outgoing) const;

  void fail_send_message(FullMessageId full_message_id, int error_code, const string &error_message);

  void fail_send_message(FullMessageId full_message_id, Status error);

  void fail_edit_message_media(FullMessageId full_message_id, Status &&error);

  void on_dialog_updated(DialogId dialog_id, const char *source);

  static BufferSlice get_dialog_database_value(const Dialog *d);

  void save_dialog_to_database(DialogId dialog_id);

  void on_save_dialog_to_database(DialogId dialog_id, bool can_reuse_notification_group, bool success);

  void try_reuse_notification_group(NotificationGroupInfo &group_info);

  void load_dialog_list(DialogList &list, int32 limit, Promise<Unit> &&promise);

  void load_folder_dialog_list(FolderId folder_id, int32 limit, bool only_local);

  void on_load_folder_dialog_list(FolderId folder_id, Result<Unit> &&result);

  void load_folder_dialog_list_from_database(FolderId folder_id, int32 limit, Promise<Unit> &&promise);

  void preload_folder_dialog_list(FolderId folder_id);

  void get_dialogs_from_list_impl(int64 task_id);

  void on_get_dialogs_from_list(int64 task_id, Result<Unit> &&result);

  static void invalidate_message_indexes(Dialog *d);

  void update_message_count_by_index(Dialog *d, int diff, const Message *m);

  void update_message_count_by_index(Dialog *d, int diff, int32 index_mask);

  int32 get_message_index_mask(DialogId dialog_id, const Message *m) const;

  void update_reply_count_by_message(Dialog *d, int diff, const Message *m);

  void update_message_reply_count(Dialog *d, MessageId message_id, DialogId replier_dialog_id,
                                  MessageId reply_message_id, int32 update_date, int diff, bool is_recursive = false);

  Message *add_message_to_dialog(DialogId dialog_id, unique_ptr<Message> message, bool from_update, bool *need_update,
                                 bool *need_update_dialog_pos, const char *source);

  Message *add_message_to_dialog(Dialog *d, unique_ptr<Message> message, bool from_update, bool *need_update,
                                 bool *need_update_dialog_pos, const char *source);

  Message *add_scheduled_message_to_dialog(Dialog *d, unique_ptr<Message> message, bool from_update, bool *need_update,
                                           const char *source);

  void register_new_local_message_id(Dialog *d, const Message *m);

  void on_message_changed(const Dialog *d, const Message *m, bool need_send_update, const char *source);

  void on_message_notification_changed(Dialog *d, const Message *m, const char *source);

  bool need_delete_file(FullMessageId full_message_id, FileId file_id) const;

  bool need_delete_message_files(DialogId dialog_id, const Message *m) const;

  void add_message_to_database(const Dialog *d, const Message *m, const char *source);

  void delete_all_dialog_messages_from_database(Dialog *d, MessageId max_message_id, const char *source);

  void delete_message_from_database(Dialog *d, MessageId message_id, const Message *m, bool is_permanently_deleted);

  void update_reply_to_message_id(DialogId dialog_id, MessageId old_message_id, MessageId new_message_id,
                                  bool have_new_message, const char *source);

  void delete_message_files(DialogId dialog_id, const Message *m) const;

  static void add_random_id_to_message_id_correspondence(Dialog *d, int64 random_id, MessageId message_id);

  static void delete_random_id_to_message_id_correspondence(Dialog *d, int64 random_id, MessageId message_id);

  static void add_notification_id_to_message_id_correspondence(Dialog *d, NotificationId notification_id,
                                                               MessageId message_id);

  static void delete_notification_id_to_message_id_correspondence(Dialog *d, NotificationId notification_id,
                                                                  MessageId message_id);

  void remove_message_notification_id(Dialog *d, Message *m, bool is_permanent, bool force_update,
                                      bool ignore_pinned_message_notification_removal = false);

  void remove_new_secret_chat_notification(Dialog *d, bool is_permanent);

  void fix_dialog_last_notification_id(Dialog *d, bool from_mentions, MessageId message_id);

  void do_fix_dialog_last_notification_id(DialogId dialog_id, bool from_mentions,
                                          NotificationId prev_last_notification_id,
                                          Result<vector<Notification>> result);

  void do_delete_message_log_event(const DeleteMessageLogEvent &log_event) const;

  static void attach_message_to_previous(Dialog *d, MessageId message_id, const char *source);

  static void attach_message_to_next(Dialog *d, MessageId message_id, const char *source);

  bool update_message(Dialog *d, Message *old_message, unique_ptr<Message> new_message, bool *need_update_dialog_pos,
                      bool is_message_in_dialog);

  static bool need_message_changed_warning(const Message *old_message);

  bool update_message_content(DialogId dialog_id, Message *old_message, unique_ptr<MessageContent> new_content,
                              bool need_merge_files, bool is_message_in_dialog, bool &is_content_changed);

  void update_message_max_reply_media_timestamp(const Dialog *d, Message *m, bool need_send_update_message_content);

  void update_message_max_own_media_timestamp(const Dialog *d, Message *m);

  void update_message_max_reply_media_timestamp_in_replied_messages(DialogId dialog_id, MessageId reply_to_message_id);

  void register_message_reply(DialogId dialog_id, const Message *m);

  void reregister_message_reply(DialogId dialog_id, const Message *m);

  void unregister_message_reply(DialogId dialog_id, const Message *m);

  void send_update_new_message(const Dialog *d, const Message *m);

  bool get_dialog_show_preview(const Dialog *d) const;

  bool is_message_preview_enabled(const Dialog *d, const Message *m, bool from_mentions);

  static bool is_from_mention_notification_group(const Message *m);

  static bool is_message_notification_active(const Dialog *d, const Message *m);

  static NotificationGroupInfo &get_notification_group_info(Dialog *d, const Message *m);

  NotificationGroupId get_dialog_notification_group_id(DialogId dialog_id, NotificationGroupInfo &group_info);

  NotificationId get_next_notification_id(Dialog *d, NotificationGroupId notification_group_id, MessageId message_id);

  void try_add_pinned_message_notification(Dialog *d, vector<Notification> &res, NotificationId max_notification_id,
                                           int32 limit);

  vector<Notification> get_message_notifications_from_database_force(Dialog *d, bool from_mentions, int32 limit);

  static Result<vector<MessagesDbDialogMessage>> do_get_message_notifications_from_database_force(
      Dialog *d, bool from_mentions, NotificationId from_notification_id, MessageId from_message_id, int32 limit);

  void do_get_message_notifications_from_database(Dialog *d, bool from_mentions,
                                                  NotificationId initial_from_notification_id,
                                                  NotificationId from_notification_id, MessageId from_message_id,
                                                  int32 limit, Promise<vector<Notification>> promise);

  void on_get_message_notifications_from_database(DialogId dialog_id, bool from_mentions,
                                                  NotificationId initial_from_notification_id, int32 limit,
                                                  Result<vector<MessagesDbDialogMessage>> result,
                                                  Promise<vector<Notification>> promise);

  void do_remove_message_notification(DialogId dialog_id, bool from_mentions, NotificationId notification_id,
                                      vector<MessagesDbDialogMessage> result);

  int32 get_dialog_pending_notification_count(const Dialog *d, bool from_mentions) const;

  void update_dialog_mention_notification_count(const Dialog *d);

  bool is_message_notification_disabled(const Dialog *d, const Message *m) const;

  bool is_dialog_message_notification_disabled(DialogId dialog_id, int32 message_date) const;

  bool may_need_message_notification(const Dialog *d, const Message *m) const;

  bool add_new_message_notification(Dialog *d, Message *m, bool force);

  void flush_pending_new_message_notifications(DialogId dialog_id, bool from_mentions, DialogId settings_dialog_id);

  void remove_all_dialog_notifications(Dialog *d, bool from_mentions, const char *source);

  void remove_message_dialog_notifications(Dialog *d, MessageId max_message_id, bool from_mentions, const char *source);

  bool need_skip_bot_commands(DialogId dialog_id, const Message *m) const;

  void send_update_message_send_succeeded(Dialog *d, MessageId old_message_id, const Message *m) const;

  void send_update_message_content(const Dialog *d, Message *m, bool is_message_in_dialog, const char *source);

  void send_update_message_content_impl(DialogId dialog_id, const Message *m, const char *source) const;

  void send_update_message_edited(DialogId dialog_id, const Message *m);

  void send_update_message_interaction_info(DialogId dialog_id, const Message *m) const;

  void send_update_message_unread_reactions(DialogId dialog_id, const Message *m, int32 unread_reaction_count) const;

  void send_update_message_live_location_viewed(FullMessageId full_message_id);

  void send_update_delete_messages(DialogId dialog_id, vector<int64> &&message_ids, bool is_permanent,
                                   bool from_cache) const;

  void send_update_new_chat(Dialog *d);

  void send_update_chat_draft_message(const Dialog *d);

  void send_update_chat_last_message(Dialog *d, const char *source);

  void send_update_chat_last_message_impl(const Dialog *d, const char *source) const;

  void send_update_chat_filters();

  void send_update_unread_message_count(DialogList &list, DialogId dialog_id, bool force, const char *source,
                                        bool from_database = false);

  void send_update_unread_chat_count(DialogList &list, DialogId dialog_id, bool force, const char *source,
                                     bool from_database = false);

  void send_update_chat_read_inbox(const Dialog *d, bool force, const char *source);

  void send_update_chat_read_outbox(const Dialog *d);

  void send_update_chat_unread_mention_count(const Dialog *d);

  void send_update_chat_unread_reaction_count(const Dialog *d, const char *source);

  void send_update_chat_position(DialogListId dialog_list_id, const Dialog *d, const char *source) const;

  void send_update_chat_online_member_count(DialogId dialog_id, int32 online_member_count) const;

  void send_update_secret_chats_with_user_action_bar(const Dialog *d) const;

  void send_update_chat_action_bar(Dialog *d);

  void send_update_chat_available_reactions(const Dialog *d);

  void send_update_secret_chats_with_user_theme(const Dialog *d) const;

  void send_update_chat_theme(const Dialog *d);

  void send_update_chat_pending_join_requests(const Dialog *d);

  void send_update_chat_video_chat(const Dialog *d);

  void send_update_chat_message_sender(const Dialog *d);

  void send_update_chat_message_ttl(const Dialog *d);

  void send_update_chat_has_scheduled_messages(Dialog *d, bool from_deletion);

  void send_update_chat_action(DialogId dialog_id, MessageId top_thread_message_id, DialogId typing_dialog_id,
                               const DialogAction &action);

  void repair_dialog_action_bar(Dialog *d, const char *source);

  void hide_dialog_action_bar(Dialog *d);

  void repair_dialog_active_group_call_id(DialogId dialog_id);

  void do_repair_dialog_active_group_call_id(DialogId dialog_id);

  static Result<int32> get_message_schedule_date(td_api::object_ptr<td_api::MessageSchedulingState> &&scheduling_state);

  tl_object_ptr<td_api::MessageSendingState> get_message_sending_state_object(const Message *m) const;

  static tl_object_ptr<td_api::MessageSchedulingState> get_message_scheduling_state_object(int32 send_date);

  tl_object_ptr<td_api::message> get_message_object(DialogId dialog_id, const Message *m, const char *source,
                                                    bool for_event_log = false) const;

  static tl_object_ptr<td_api::messages> get_messages_object(int32 total_count,
                                                             vector<tl_object_ptr<td_api::message>> &&messages,
                                                             bool skip_not_found);

  vector<DialogId> sort_dialogs_by_order(const vector<DialogId> &dialog_ids, int32 limit) const;

  vector<DialogId> get_peers_dialog_ids(vector<tl_object_ptr<telegram_api::Peer>> &&peers);

  static bool need_unread_counter(int64 dialog_order);

  int32 get_dialog_total_count(const DialogList &list) const;

  void repair_server_dialog_total_count(DialogListId dialog_list_id);

  void repair_secret_chat_total_count(DialogListId dialog_list_id);

  void on_get_secret_chat_total_count(DialogListId dialog_list_id, int32 total_count);

  void recalc_unread_count(DialogListId dialog_list_id, int32 old_dialog_total_count, bool force);

  td_api::object_ptr<td_api::updateChatFilters> get_update_chat_filters_object() const;

  td_api::object_ptr<td_api::updateUnreadMessageCount> get_update_unread_message_count_object(
      const DialogList &list) const;

  td_api::object_ptr<td_api::updateUnreadChatCount> get_update_unread_chat_count_object(const DialogList &list) const;

  static void save_unread_chat_count(const DialogList &list);

  void set_dialog_last_read_inbox_message_id(Dialog *d, MessageId message_id, int32 server_unread_count,
                                             int32 local_unread_count, bool force_update, const char *source);

  void set_dialog_last_read_outbox_message_id(Dialog *d, MessageId message_id);

  void set_dialog_last_message_id(Dialog *d, MessageId last_message_id, const char *source, const Message *m = nullptr);

  void set_dialog_first_database_message_id(Dialog *d, MessageId first_database_message_id, const char *source);

  void set_dialog_last_database_message_id(Dialog *d, MessageId last_database_message_id, const char *source,
                                           bool is_loaded_from_database = false);

  void set_dialog_last_new_message_id(Dialog *d, MessageId last_new_message_id, const char *source);

  void set_dialog_last_clear_history_date(Dialog *d, int32 date, MessageId last_clear_history_message_id,
                                          const char *source, bool is_loaded_from_database = false);

  static void set_dialog_unread_mention_count(Dialog *d, int32 unread_mention_count);

  static void set_dialog_unread_reaction_count(Dialog *d, int32 unread_reaction_count);

  void set_dialog_is_empty(Dialog *d, const char *source);

  void remove_dialog_newer_messages(Dialog *d, MessageId from_message_id, const char *source);

  int32 get_pinned_dialogs_limit(DialogListId dialog_list_id) const;

  static vector<DialogId> remove_secret_chat_dialog_ids(vector<DialogId> dialog_ids);

  bool set_dialog_is_pinned(DialogId dialog_id, bool is_pinned);

  bool set_dialog_is_pinned(DialogListId dialog_list_id, Dialog *d, bool is_pinned,
                            bool need_update_dialog_lists = true);

  void save_pinned_folder_dialog_ids(const DialogList &list) const;

  void set_dialog_is_marked_as_unread(Dialog *d, bool is_marked_as_unread);

  void set_dialog_is_blocked(Dialog *d, bool is_blocked);

  void set_dialog_has_bots(Dialog *d, bool has_bots);

  void set_dialog_last_pinned_message_id(Dialog *d, MessageId last_pinned_message_id);

  void drop_dialog_last_pinned_message_id(Dialog *d);

  void set_dialog_theme_name(Dialog *d, string theme_name);

  void fix_pending_join_requests(DialogId dialog_id, int32 &pending_join_request_count,
                                 vector<UserId> &pending_join_request_user_ids) const;

  void set_dialog_pending_join_requests(Dialog *d, int32 pending_join_request_count,
                                        vector<UserId> pending_join_request_user_ids);

  void repair_dialog_scheduled_messages(Dialog *d);

  void set_dialog_has_scheduled_server_messages(Dialog *d, bool has_scheduled_server_messages);

  void set_dialog_has_scheduled_database_messages(DialogId dialog_id, bool has_scheduled_database_messages);

  void set_dialog_has_scheduled_database_messages_impl(Dialog *d, bool has_scheduled_database_messages);

  void set_dialog_folder_id(Dialog *d, FolderId folder_id);

  void do_set_dialog_folder_id(Dialog *d, FolderId folder_id);

  void toggle_dialog_is_pinned_on_server(DialogId dialog_id, bool is_pinned, uint64 log_event_id);

  void toggle_dialog_is_marked_as_unread_on_server(DialogId dialog_id, bool is_marked_as_unread, uint64 log_event_id);

  void toggle_dialog_is_blocked_on_server(DialogId dialog_id, bool is_blocked, uint64 log_event_id);

  void reorder_pinned_dialogs_on_server(FolderId folder_id, const vector<DialogId> &dialog_ids, uint64 log_event_id);

  void set_dialog_reply_markup(Dialog *d, MessageId message_id);

  void try_restore_dialog_reply_markup(Dialog *d, const Message *m);

  void set_dialog_pinned_message_notification(Dialog *d, MessageId message_id, const char *source);

  void remove_dialog_pinned_message_notification(Dialog *d, const char *source);

  void remove_dialog_mention_notifications(Dialog *d);

  bool set_dialog_last_notification(DialogId dialog_id, NotificationGroupInfo &group_info, int32 last_notification_date,
                                    NotificationId last_notification_id, const char *source);

  bool update_dialog_notification_settings(DialogId dialog_id, DialogNotificationSettings *current_settings,
                                           DialogNotificationSettings &&new_settings);

  void schedule_dialog_unmute(DialogId dialog_id, bool use_default, int32 mute_until);

  void update_dialog_unmute_timeout(Dialog *d, bool &old_use_default, int32 &old_mute_until, bool new_use_default,
                                    int32 new_mute_until);

  void on_dialog_unmute(DialogId dialog_id);

  bool update_dialog_silent_send_message(Dialog *d, bool silent_send_message);

  ChatReactions get_message_available_reactions(const Dialog *d, const Message *m,
                                                bool dissalow_custom_for_non_premium);

  void set_message_reactions(Dialog *d, Message *m, bool is_big, bool add_to_recent, Promise<Unit> &&promise);

  void on_set_message_reactions(FullMessageId full_message_id, Result<Unit> result, Promise<Unit> promise);

  void set_dialog_available_reactions(Dialog *d, ChatReactions &&available_reactions);

  void set_dialog_next_available_reactions_generation(Dialog *d, uint32 generation);

  void hide_dialog_message_reactions(Dialog *d);

  ChatReactions get_active_reactions(const ChatReactions &available_reactions) const;

  ChatReactions get_dialog_active_reactions(const Dialog *d) const;

  ChatReactions get_message_active_reactions(const Dialog *d, const Message *m) const;

  static bool need_poll_dialog_message_reactions(const Dialog *d);

  static bool need_poll_message_reactions(const Dialog *d, const Message *m);

  void queue_message_reactions_reload(FullMessageId full_message_id);

  void queue_message_reactions_reload(DialogId dialog_id, const vector<MessageId> &message_ids);

  bool is_dialog_action_unneeded(DialogId dialog_id) const;

  void on_send_dialog_action_timeout(DialogId dialog_id);

  void on_active_dialog_action_timeout(DialogId dialog_id);

  void clear_active_dialog_actions(DialogId dialog_id);

  void cancel_dialog_action(DialogId dialog_id, const Message *m);

  Dialog *get_dialog_by_message_id(MessageId message_id);

  MessageId get_message_id_by_random_id(Dialog *d, int64 random_id, const char *source);

  Dialog *add_dialog(DialogId dialog_id, const char *source);

  Dialog *add_new_dialog(unique_ptr<Dialog> &&dialog, bool is_loaded_from_database, const char *source);

  void fix_new_dialog(Dialog *d, unique_ptr<Message> &&last_database_message, MessageId last_database_message_id,
                      int64 order, int32 last_clear_history_date, MessageId last_clear_history_message_id,
                      DialogId default_join_group_call_as_dialog_id, DialogId default_send_message_as_dialog_id,
                      bool need_drop_default_send_message_as_dialog_id, bool is_loaded_from_database,
                      const char *source);

  bool add_dialog_last_database_message(Dialog *d, unique_ptr<Message> &&last_database_message);

  void fix_dialog_action_bar(const Dialog *d, DialogActionBar *action_bar);

  td_api::object_ptr<td_api::ChatType> get_chat_type_object(DialogId dialog_id) const;

  td_api::object_ptr<td_api::ChatActionBar> get_chat_action_bar_object(const Dialog *d) const;

  string get_dialog_theme_name(const Dialog *d) const;

  td_api::object_ptr<td_api::chatJoinRequestsInfo> get_chat_join_requests_info_object(const Dialog *d) const;

  td_api::object_ptr<td_api::videoChat> get_video_chat_object(const Dialog *d) const;

  td_api::object_ptr<td_api::MessageSender> get_default_message_sender_object(const Dialog *d) const;

  td_api::object_ptr<td_api::chat> get_chat_object(const Dialog *d) const;

  Dialog *get_dialog(DialogId dialog_id);
  const Dialog *get_dialog(DialogId dialog_id) const;

  Dialog *get_dialog_force(DialogId dialog_id, const char *source = "get_dialog_force");

  Dialog *on_load_dialog_from_database(DialogId dialog_id, BufferSlice &&value, const char *source);

  void on_get_dialogs_from_database(FolderId folder_id, int32 limit, DialogDbGetDialogsResult &&dialogs,
                                    Promise<Unit> &&promise);

  void send_get_dialog_query(DialogId dialog_id, Promise<Unit> &&promise, uint64 log_event_id, const char *source);

  void send_search_public_dialogs_query(const string &query, Promise<Unit> &&promise);

  vector<DialogId> get_pinned_dialog_ids(DialogListId dialog_list_id) const;

  void reload_pinned_dialogs(DialogListId dialog_list_id, Promise<Unit> &&promise);

  static double get_dialog_filters_cache_time();

  void schedule_dialog_filters_reload(double timeout);

  static void on_reload_dialog_filters_timeout(void *messages_manager_ptr);

  void reload_dialog_filters();

  void on_get_dialog_filters(Result<vector<tl_object_ptr<telegram_api::DialogFilter>>> r_filters, bool dummy);

  bool need_synchronize_dialog_filters() const;

  void synchronize_dialog_filters();

  void update_dialogs_hints(const Dialog *d);
  void update_dialogs_hints_rating(const Dialog *d);

  td_api::object_ptr<td_api::chatFilter> get_chat_filter_object(const DialogFilter *filter) const;

  void load_dialog_filter_dialogs(DialogFilterId dialog_filter_id, vector<InputDialogId> &&input_dialog_ids,
                                  Promise<Unit> &&promise);

  void on_load_dialog_filter_dialogs(DialogFilterId dialog_filter_id, vector<DialogId> &&dialog_ids,
                                     Promise<Unit> &&promise);

  void load_dialog_filter(const DialogFilter *filter, bool force, Promise<Unit> &&promise);

  void on_get_recommended_dialog_filters(Result<vector<tl_object_ptr<telegram_api::dialogFilterSuggested>>> result,
                                         Promise<td_api::object_ptr<td_api::recommendedChatFilters>> &&promise);

  void on_load_recommended_dialog_filters(Result<Unit> &&result, vector<RecommendedDialogFilter> &&filters,
                                          Promise<td_api::object_ptr<td_api::recommendedChatFilters>> &&promise);

  InputDialogId get_input_dialog_id(DialogId dialog_id) const;

  void sort_dialog_filter_input_dialog_ids(DialogFilter *dialog_filter, const char *source) const;

  Result<unique_ptr<DialogFilter>> create_dialog_filter(DialogFilterId dialog_filter_id,
                                                        td_api::object_ptr<td_api::chatFilter> filter);

  void update_dialog_filter_on_server(unique_ptr<DialogFilter> &&dialog_filter);

  void on_update_dialog_filter(unique_ptr<DialogFilter> dialog_filter, Status result);

  void delete_dialog_filter_on_server(DialogFilterId dialog_filter_id);

  void on_delete_dialog_filter(DialogFilterId dialog_filter_id, Status result);

  void reorder_dialog_filters_on_server(vector<DialogFilterId> dialog_filter_ids, int32 main_dialog_list_position);

  void on_reorder_dialog_filters(vector<DialogFilterId> dialog_filter_ids, int32 main_dialog_list_position,
                                 Status result);

  void save_dialog_filters();

  void add_dialog_filter(unique_ptr<DialogFilter> dialog_filter, bool at_beginning, const char *source);

  void edit_dialog_filter(unique_ptr<DialogFilter> new_dialog_filter, const char *source);

  void delete_dialog_filter(DialogFilterId dialog_filter_id, const char *source);

  static bool set_dialog_filters_order(vector<unique_ptr<DialogFilter>> &dialog_filters,
                                       vector<DialogFilterId> dialog_filter_ids);

  const DialogFilter *get_server_dialog_filter(DialogFilterId dialog_filter_id) const;

  DialogFilter *get_dialog_filter(DialogFilterId dialog_filter_id);
  const DialogFilter *get_dialog_filter(DialogFilterId dialog_filter_id) const;

  int32 get_server_main_dialog_list_position() const;

  static vector<DialogFilterId> get_dialog_filter_ids(const vector<unique_ptr<DialogFilter>> &dialog_filters);

  static vector<FolderId> get_dialog_filter_folder_ids(const DialogFilter *filter);

  vector<FolderId> get_dialog_list_folder_ids(const DialogList &list) const;

  bool has_dialogs_from_folder(const DialogList &list, const DialogFolder &folder) const;

  static bool is_dialog_in_list(const Dialog *d, DialogListId dialog_list_id);

  static void add_dialog_to_list(Dialog *d, DialogListId dialog_list_id);

  static void remove_dialog_from_list(Dialog *d, DialogListId dialog_list_id);

  bool need_dialog_in_filter(const Dialog *d, const DialogFilter *filter) const;

  bool need_dialog_in_list(const Dialog *d, const DialogList &list) const;

  static bool need_send_update_chat_position(const DialogPositionInList &old_position,
                                             const DialogPositionInList &new_position);

  DialogPositionInList get_dialog_position_in_list(const DialogList *list, const Dialog *d, bool actual = false) const;

  std::unordered_map<DialogListId, DialogPositionInList, DialogListIdHash> get_dialog_positions(const Dialog *d) const;

  static vector<DialogListId> get_dialog_list_ids(const Dialog *d);

  DialogListView get_dialog_lists(const Dialog *d);

  DialogList &add_dialog_list(DialogListId dialog_list_id);

  DialogList *get_dialog_list(DialogListId dialog_list_id);
  const DialogList *get_dialog_list(DialogListId dialog_list_id) const;

  DialogFolder *get_dialog_folder(FolderId folder_id);
  const DialogFolder *get_dialog_folder(FolderId folder_id) const;

  static unique_ptr<Message> *treap_find_message(unique_ptr<Message> *v, MessageId message_id);
  static const unique_ptr<Message> *treap_find_message(const unique_ptr<Message> *v, MessageId message_id);

  static Message *treap_insert_message(unique_ptr<Message> *v, unique_ptr<Message> message);
  static unique_ptr<Message> treap_delete_message(unique_ptr<Message> *v);

  static Message *get_message(Dialog *d, MessageId message_id);
  static const Message *get_message(const Dialog *d, MessageId message_id);

  Message *get_message(FullMessageId full_message_id);
  const Message *get_message(FullMessageId full_message_id) const;

  bool have_message_force(Dialog *d, MessageId message_id, const char *source);

  Message *get_message_force(Dialog *d, MessageId message_id, const char *source);

  Message *get_message_force(FullMessageId full_message_id, const char *source);

  void get_message_force_from_server(Dialog *d, MessageId message_id, Promise<Unit> &&promise,
                                     tl_object_ptr<telegram_api::InputMessage> input_message = nullptr);

  Message *on_get_message_from_database(const MessagesDbMessage &message, bool is_scheduled, const char *source);

  Message *on_get_message_from_database(Dialog *d, const MessagesDbDialogMessage &message, bool is_scheduled,
                                        const char *source);

  Message *on_get_message_from_database(Dialog *d, MessageId message_id, const BufferSlice &value, bool is_scheduled,
                                        const char *source);

  void get_dialog_message_by_date_from_server(const Dialog *d, int32 date, int64 random_id, bool after_database_search,
                                              Promise<Unit> &&promise);

  void on_get_dialog_message_by_date_from_database(DialogId dialog_id, int32 date, int64 random_id,
                                                   Result<MessagesDbDialogMessage> result, Promise<Unit> promise);

  std::pair<bool, int32> get_dialog_mute_until(DialogId dialog_id, const Dialog *d) const;

  int64 get_dialog_notification_ringtone_id(DialogId dialog_id, const Dialog *d) const;

  NotificationSettingsScope get_dialog_notification_setting_scope(DialogId dialog_id) const;

  DialogNotificationSettings *get_dialog_notification_settings(DialogId dialog_id, bool force);

  vector<FileId> get_message_file_ids(const Message *m) const;

  void cancel_upload_message_content_files(const MessageContent *content);

  static void cancel_upload_file(FileId file_id);

  void cancel_send_message_query(DialogId dialog_id, Message *m);

  void cancel_send_deleted_message(DialogId dialog_id, Message *m, bool is_permanently_deleted);

  bool is_discussion_message(DialogId dialog_id, const Message *m) const;

  bool has_message_sender_user_id(DialogId dialog_id, const Message *m) const;

  int32 get_message_own_max_media_timestamp(const Message *m) const;

  static int32 get_message_max_media_timestamp(const Message *m);

  static bool get_message_disable_web_page_preview(const Message *m);

  static int32 get_message_flags(const Message *m);

  tl_object_ptr<telegram_api::InputPeer> get_send_message_as_input_peer(const Message *m) const;

  static bool is_forward_info_sender_hidden(const MessageForwardInfo *forward_info);

  unique_ptr<MessageForwardInfo> get_message_forward_info(
      tl_object_ptr<telegram_api::messageFwdHeader> &&forward_header, FullMessageId full_message_id);

  td_api::object_ptr<td_api::messageForwardInfo> get_message_forward_info_object(
      const unique_ptr<MessageForwardInfo> &forward_info) const;

  void ttl_read_history(Dialog *d, bool is_outgoing, MessageId from_message_id, MessageId till_message_id,
                        double view_date);
  void ttl_read_history_impl(DialogId dialog_id, bool is_outgoing, MessageId from_message_id, MessageId till_message_id,
                             double view_date);
  void ttl_on_view(const Dialog *d, Message *m, double view_date, double now);
  bool ttl_on_open(Dialog *d, Message *m, double now, bool is_local_read);
  void ttl_register_message(DialogId dialog_id, const Message *m, double now);
  void ttl_unregister_message(DialogId dialog_id, const Message *m, const char *source);
  void ttl_period_register_message(DialogId dialog_id, const Message *m, double server_time);
  void ttl_period_unregister_message(DialogId dialog_id, const Message *m);
  void ttl_loop(double now);
  void ttl_update_timeout(double now);

  void on_message_ttl_expired(Dialog *d, Message *m);
  void on_message_ttl_expired_impl(Dialog *d, Message *m);

  void start_up() final;

  void loop() final;

  void tear_down() final;

  void hangup() final;

  void create_folders();
  void init();

  void ttl_db_loop_start(double server_now);
  void ttl_db_loop(double server_now);
  void ttl_db_on_result(Result<std::pair<std::vector<MessagesDbMessage>, int32>> r_result, bool dummy);

  void on_restore_missing_message_after_get_difference(FullMessageId full_message_id, MessageId old_message_id,
                                                       Result<Unit> result);

  void on_get_message_link_dialog(MessageLinkInfo &&info, Promise<MessageLinkInfo> &&promise);

  void on_get_message_link_message(MessageLinkInfo &&info, DialogId dialog_id, Promise<MessageLinkInfo> &&promise);

  void on_get_message_link_discussion_message(MessageLinkInfo &&info, DialogId comment_dialog_id,
                                              Promise<MessageLinkInfo> &&promise);

  void process_discussion_message_impl(telegram_api::object_ptr<telegram_api::messages_discussionMessage> &&result,
                                       DialogId dialog_id, MessageId message_id, DialogId expected_dialog_id,
                                       MessageId expected_message_id, Promise<MessageThreadInfo> promise);

  void on_get_discussion_message(DialogId dialog_id, MessageId message_id, MessageThreadInfo &&message_thread_info,
                                 Promise<MessageThreadInfo> &&promise);

  void on_get_message_viewers(DialogId dialog_id, vector<UserId> user_ids, bool is_recursive,
                              Promise<td_api::object_ptr<td_api::users>> &&promise);

  static MessageId get_first_database_message_id_by_index(const Dialog *d, MessageSearchFilter filter);

  void on_get_message_calendar_from_database(int64 random_id, DialogId dialog_id, MessageId from_message_id,
                                             MessageId first_db_message_id, MessageSearchFilter filter,
                                             Result<MessagesDbCalendar> r_calendar, Promise<Unit> promise);

  void on_search_dialog_messages_db_result(int64 random_id, DialogId dialog_id, MessageId from_message_id,
                                           MessageId first_db_message_id, MessageSearchFilter filter, int32 offset,
                                           int32 limit, Result<vector<MessagesDbDialogMessage>> r_messages,
                                           Promise<Unit> promise);

  void on_messages_db_fts_result(Result<MessagesDbFtsResult> result, string offset, int32 limit, int64 random_id,
                                 Promise<Unit> &&promise);

  void on_messages_db_calls_result(Result<MessagesDbCallsResult> result, int64 random_id, MessageId first_db_message_id,
                                   MessageSearchFilter filter, Promise<Unit> &&promise);

  void on_load_active_live_location_full_message_ids_from_database(string value);

  void on_load_active_live_location_messages_finished();

  void try_add_active_live_location(DialogId dialog_id, const Message *m);

  void add_active_live_location(FullMessageId full_message_id);

  bool delete_active_live_location(DialogId dialog_id, const Message *m);

  void save_active_live_locations();

  void on_message_live_location_viewed(Dialog *d, const Message *m);

  void view_message_live_location_on_server(int64 task_id);

  void view_message_live_location_on_server_impl(int64 task_id, FullMessageId full_message_id);

  void on_message_live_location_viewed_on_server(int64 task_id);

  void try_add_bot_command_message_id(DialogId dialog_id, const Message *m);

  void delete_bot_command_message_id(DialogId dialog_id, MessageId message_id);

  void add_message_file_sources(DialogId dialog_id, const Message *m);

  void remove_message_file_sources(DialogId dialog_id, const Message *m);

  void change_message_files(DialogId dialog_id, const Message *m, const vector<FileId> &old_file_ids);

  Result<unique_ptr<ReplyMarkup>> get_dialog_reply_markup(
      DialogId dialog_id, tl_object_ptr<td_api::ReplyMarkup> &&reply_markup_ptr) const TD_WARN_UNUSED_RESULT;

  const DialogPhoto *get_dialog_photo(DialogId dialog_id) const;

  string get_dialog_username(DialogId dialog_id) const;

  RestrictedRights get_dialog_default_permissions(DialogId dialog_id) const;

  bool get_dialog_has_protected_content(DialogId dialog_id) const;

  bool get_dialog_has_scheduled_messages(const Dialog *d) const;

  static int64 get_dialog_order(MessageId message_id, int32 message_date);

  bool is_dialog_sponsored(const Dialog *d) const;

  int64 get_dialog_base_order(const Dialog *d) const;

  int64 get_dialog_private_order(const DialogList *list, const Dialog *d) const;

  td_api::object_ptr<td_api::chatPosition> get_chat_position_object(DialogListId dialog_list_id, const Dialog *d) const;

  vector<td_api::object_ptr<td_api::chatPosition>> get_chat_positions_object(const Dialog *d) const;

  bool update_dialog_draft_message(Dialog *d, unique_ptr<DraftMessage> &&draft_message, bool from_update,
                                   bool need_update_dialog_pos);

  void save_dialog_draft_message_on_server(DialogId dialog_id);

  void on_saved_dialog_draft_message(DialogId dialog_id, uint64 generation);

  void update_dialog_notification_settings_on_server(DialogId dialog_id, bool from_binlog);

  void send_update_dialog_notification_settings_query(const Dialog *d, Promise<Unit> &&promise);

  void on_updated_dialog_notification_settings(DialogId dialog_id, uint64 generation);

  void reset_all_notification_settings_on_server(uint64 log_event_id);

  void toggle_dialog_report_spam_state_on_server(DialogId dialog_id, bool is_spam_dialog, uint64 log_event_id,
                                                 Promise<Unit> &&promise);

  void set_dialog_folder_id_on_server(DialogId dialog_id, bool from_binlog);

  void on_updated_dialog_folder_id(DialogId dialog_id, uint64 generation);

  int64 get_next_pinned_dialog_order();

  bool is_removed_from_dialog_list(const Dialog *d) const;

  void update_dialog_pos(Dialog *d, const char *source, bool need_send_update = true,
                         bool is_loaded_from_database = false);

  bool set_dialog_order(Dialog *d, int64 new_order, bool need_send_update, bool is_loaded_from_database,
                        const char *source);

  void update_dialog_lists(Dialog *d,
                           std::unordered_map<DialogListId, DialogPositionInList, DialogListIdHash> &&old_positions,
                           bool need_send_update, bool is_loaded_from_database, const char *source);

  void update_last_dialog_date(FolderId folder_id);

  bool do_update_list_last_pinned_dialog_date(DialogList &list) const;

  void update_list_last_pinned_dialog_date(DialogList &list);

  bool do_update_list_last_dialog_date(DialogList &list, const vector<FolderId> &folder_ids) const;

  void update_list_last_dialog_date(DialogList &list);

  static string get_channel_pts_key(DialogId dialog_id);

  int32 load_channel_pts(DialogId dialog_id) const;

  void set_channel_pts(Dialog *d, int32 new_pts, const char *source);

  bool need_channel_difference_to_add_message(DialogId dialog_id,
                                              const tl_object_ptr<telegram_api::Message> &message_ptr);

  void run_after_channel_difference(DialogId dialog_id, Promise<Unit> &&promise);

  bool running_get_channel_difference(DialogId dialog_id) const;

  void on_channel_get_difference_timeout(DialogId dialog_id);

  void get_channel_difference(DialogId dialog_id, int32 pts, bool force, const char *source);

  void do_get_channel_difference(DialogId dialog_id, int32 pts, bool force,
                                 tl_object_ptr<telegram_api::InputChannel> &&input_channel, const char *source);

  void process_get_channel_difference_updates(DialogId dialog_id, int32 new_pts,
                                              vector<tl_object_ptr<telegram_api::Message>> &&new_messages,
                                              vector<tl_object_ptr<telegram_api::Update>> &&other_updates);

  void on_get_channel_dialog(DialogId dialog_id, MessageId last_message_id, MessageId read_inbox_max_message_id,
                             int32 server_unread_count, int32 unread_mention_count, int32 unread_reaction_count,
                             MessageId read_outbox_max_message_id,
                             vector<tl_object_ptr<telegram_api::Message>> &&messages);

  void after_get_channel_difference(DialogId dialog_id, bool success);

  static void on_channel_get_difference_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int);

  static void on_pending_message_views_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int);

  static void on_pending_message_live_location_view_timeout_callback(void *messages_manager_ptr, int64 task_id);

  static void on_pending_draft_message_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int);

  static void on_pending_read_history_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int);

  static void on_pending_updated_dialog_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int);

  static void on_pending_unload_dialog_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int);

  static void on_dialog_unmute_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int);

  static void on_pending_send_dialog_action_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int);

  static void on_active_dialog_action_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int);

  static void on_update_dialog_online_member_count_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int);

  static void on_preload_folder_dialog_list_timeout_callback(void *messages_manager_ptr, int64 folder_id_int);

  static void on_update_viewed_messages_timeout_callback(void *messages_manager_ptr, int64 dialog_id_int);

  void load_secret_thumbnail(FileId thumbnail_file_id);

  void on_upload_media(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file,
                       tl_object_ptr<telegram_api::InputEncryptedFile> input_encrypted_file);
  void on_upload_media_error(FileId file_id, Status status);

  void on_load_secret_thumbnail(FileId thumbnail_file_id, BufferSlice thumbnail);
  void on_upload_thumbnail(FileId thumbnail_file_id, tl_object_ptr<telegram_api::InputFile> thumbnail_input_file);

  void on_upload_dialog_photo(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file);
  void on_upload_dialog_photo_error(FileId file_id, Status status);

  void send_edit_dialog_photo_query(DialogId dialog_id, FileId file_id,
                                    tl_object_ptr<telegram_api::InputChatPhoto> &&input_chat_photo,
                                    Promise<Unit> &&promise);

  void upload_imported_messages(DialogId dialog_id, FileId file_id, vector<FileId> attached_file_ids, bool is_reupload,
                                Promise<Unit> &&promise, vector<int> bad_parts = {});

  void on_upload_imported_messages(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file);
  void on_upload_imported_messages_error(FileId file_id, Status status);

  void upload_imported_message_attachment(DialogId dialog_id, int64 import_id, FileId file_id, bool is_reupload,
                                          Promise<Unit> &&promise, vector<int> bad_parts = {});

  void on_upload_imported_message_attachment(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file);
  void on_upload_imported_message_attachment_error(FileId file_id, Status status);

  void on_imported_message_attachments_uploaded(int64 random_id, Result<Unit> &&result);

  Status can_import_messages(DialogId dialog_id);

  void send_get_message_public_forwards_query(DcId dc_id, FullMessageId full_message_id, string offset, int32 limit,
                                              Promise<td_api::object_ptr<td_api::foundMessages>> &&promise);

  void add_sponsored_dialog(const Dialog *d, DialogSource source);

  void save_sponsored_dialog();

  void set_sponsored_dialog(DialogId dialog_id, DialogSource source);

  Dialog *get_service_notifications_dialog();

  void save_auth_notification_ids();

  static MessageId get_next_message_id(Dialog *d, MessageType type);

  static MessageId get_next_local_message_id(Dialog *d);

  static MessageId get_next_yet_unsent_message_id(Dialog *d);

  static MessageId get_next_yet_unsent_scheduled_message_id(Dialog *d, int32 date);

  void reget_message_from_server_if_needed(DialogId dialog_id, const Message *m);

  void speculatively_update_active_group_call_id(Dialog *d, const Message *m);

  void speculatively_update_channel_participants(DialogId dialog_id, const Message *m);

  void update_sent_message_contents(DialogId dialog_id, const Message *m);

  void update_used_hashtags(DialogId dialog_id, const Message *m);

  void update_top_dialogs(DialogId dialog_id, const Message *m);

  void update_forward_count(DialogId dialog_id, const Message *m);

  void update_forward_count(DialogId dialog_id, MessageId message_id, int32 update_date);

  void update_has_outgoing_messages(DialogId dialog_id, const Message *m);

  string get_message_search_text(const Message *m) const;

  unique_ptr<Message> parse_message(Dialog *d, MessageId expected_message_id, const BufferSlice &value,
                                    bool is_scheduled);

  unique_ptr<Dialog> parse_dialog(DialogId dialog_id, const BufferSlice &value, const char *source);

  void load_calls_db_state();
  void save_calls_db_state();

  static constexpr bool is_debug_message_op_enabled() {
    return !LOG_IS_STRIPPED(ERROR) && false;
  }

  void add_message_dependencies(Dependencies &dependencies, const Message *m);

  static void dump_debug_message_op(const Dialog *d, int priority = 0);

  static void save_send_message_log_event(DialogId dialog_id, const Message *m);

  static uint64 save_toggle_dialog_report_spam_state_on_server_log_event(DialogId dialog_id, bool is_spam_dialog);

  static uint64 save_delete_messages_on_server_log_event(DialogId dialog_id, const vector<MessageId> &message_ids,
                                                         bool revoke);

  static uint64 save_delete_scheduled_messages_on_server_log_event(DialogId dialog_id,
                                                                   const vector<MessageId> &message_ids);

  static uint64 save_delete_dialog_history_on_server_log_event(DialogId dialog_id, MessageId max_message_id,
                                                               bool remove_from_dialog_list, bool revoke);

  static uint64 save_delete_all_call_messages_on_server_log_event(bool revoke);

  static uint64 save_block_message_sender_from_replies_on_server_log_event(MessageId message_id,
                                                                           bool need_delete_message,
                                                                           bool need_delete_all_messages,
                                                                           bool report_spam);

  static uint64 save_delete_all_channel_messages_by_sender_on_server_log_event(ChannelId channel_id,
                                                                               DialogId sender_dialog_id);

  static uint64 save_delete_dialog_messages_by_date_on_server_log_event(DialogId dialog_id, int32 min_date,
                                                                        int32 max_date, bool revoke);

  static uint64 save_read_all_dialog_mentions_on_server_log_event(DialogId dialog_id);

  static uint64 save_read_all_dialog_reactions_on_server_log_event(DialogId dialog_id);

  static uint64 save_toggle_dialog_is_pinned_on_server_log_event(DialogId dialog_id, bool is_pinned);

  static uint64 save_reorder_pinned_dialogs_on_server_log_event(FolderId folder_id, const vector<DialogId> &dialog_ids);

  static uint64 save_toggle_dialog_is_marked_as_unread_on_server_log_event(DialogId dialog_id,
                                                                           bool is_marked_as_unread);

  static uint64 save_toggle_dialog_is_blocked_on_server_log_event(DialogId dialog_id, bool is_blocked);

  static uint64 save_read_message_contents_on_server_log_event(DialogId dialog_id,
                                                               const vector<MessageId> &message_ids);

  static uint64 save_reset_all_notification_settings_on_server_log_event();

  static uint64 save_reget_dialog_log_event(DialogId dialog_id);

  static uint64 save_forward_messages_log_event(DialogId to_dialog_id, DialogId from_dialog_id,
                                                const vector<Message *> &messages, const vector<MessageId> &message_ids,
                                                bool drop_author, bool drop_media_captions);

  static uint64 save_unpin_all_dialog_messages_on_server_log_event(DialogId dialog_id);

  void suffix_load_loop(Dialog *d);
  static void suffix_load_update_first_message_id(Dialog *d);
  void suffix_load_query_ready(DialogId dialog_id);
  void suffix_load_add_query(Dialog *d, std::pair<Promise<>, std::function<bool(const Message *)>> query);
  void suffix_load_till_date(Dialog *d, int32 date, Promise<> promise);
  void suffix_load_till_message_id(Dialog *d, MessageId message_id, Promise<> promise);

  bool is_group_dialog(DialogId dialog_id) const;

  bool is_broadcast_channel(DialogId dialog_id) const;

  bool is_deleted_secret_chat(const Dialog *d) const;

  static int32 get_message_schedule_date(const Message *m);

  static DialogId get_message_original_sender(const Message *m);

  static DialogId get_message_sender(const Message *m);

  RecentDialogList recently_found_dialogs_;
  RecentDialogList recently_opened_dialogs_;

  class UploadMediaCallback;
  class UploadThumbnailCallback;
  class UploadDialogPhotoCallback;
  class UploadImportedMessagesCallback;
  class UploadImportedMessageAttachmentCallback;

  std::shared_ptr<UploadMediaCallback> upload_media_callback_;
  std::shared_ptr<UploadThumbnailCallback> upload_thumbnail_callback_;
  std::shared_ptr<UploadDialogPhotoCallback> upload_dialog_photo_callback_;
  std::shared_ptr<UploadImportedMessagesCallback> upload_imported_messages_callback_;
  std::shared_ptr<UploadImportedMessageAttachmentCallback> upload_imported_message_attachment_callback_;

  double last_channel_pts_jump_warning_time_ = 0;

  FlatHashMap<FileId, std::pair<FullMessageId, FileId>, FileIdHash>
      being_uploaded_files_;  // file_id -> message, thumbnail_file_id
  struct UploadedThumbnailInfo {
    FullMessageId full_message_id;
    FileId file_id;                                     // original file file_id
    tl_object_ptr<telegram_api::InputFile> input_file;  // original file InputFile
  };
  FlatHashMap<FileId, UploadedThumbnailInfo, FileIdHash> being_uploaded_thumbnails_;  // thumbnail_file_id -> ...
  struct UploadedSecretThumbnailInfo {
    FullMessageId full_message_id;
    FileId file_id;                                              // original file file_id
    tl_object_ptr<telegram_api::InputEncryptedFile> input_file;  // original file InputEncryptedFile
  };
  FlatHashMap<FileId, UploadedSecretThumbnailInfo, FileIdHash>
      being_loaded_secret_thumbnails_;  // thumbnail_file_id -> ...

  // TTL
  class TtlNode final : private HeapNode {
   public:
    TtlNode(DialogId dialog_id, MessageId message_id, bool by_ttl_period)
        : full_message_id_(dialog_id, message_id), by_ttl_period_(by_ttl_period) {
    }

    FullMessageId full_message_id_;
    bool by_ttl_period_;

    HeapNode *as_heap_node() const {
      return const_cast<HeapNode *>(static_cast<const HeapNode *>(this));
    }
    static TtlNode *from_heap_node(HeapNode *node) {
      return static_cast<TtlNode *>(node);
    }

    bool operator==(const TtlNode &other) const {
      return full_message_id_ == other.full_message_id_;
    }
  };
  struct TtlNodeHash {
    std::size_t operator()(const TtlNode &ttl_node) const {
      return FullMessageIdHash()(ttl_node.full_message_id_) * 2 + static_cast<size_t>(ttl_node.by_ttl_period_);
    }
  };
  std::unordered_set<TtlNode, TtlNodeHash> ttl_nodes_;
  KHeap<double> ttl_heap_;
  Slot ttl_slot_;

  enum YieldType : int32 { None, TtlDb };  // None must be first
  int32 ttl_db_expires_from_;
  int32 ttl_db_expires_till_;
  bool ttl_db_has_query_;
  Slot ttl_db_slot_;

  FlatHashMap<int64, FullMessageId> being_sent_messages_;  // message_random_id -> message

  FlatHashMap<FullMessageId, MessageId, FullMessageIdHash> update_message_ids_;  // new_message_id -> temporary_id
  FlatHashMap<DialogId, FlatHashMap<ScheduledServerMessageId, MessageId, ScheduledServerMessageIdHash>,
              DialogIdHash>
      update_scheduled_message_ids_;  // new_message_id -> temporary_id

  const char *debug_add_message_to_dialog_fail_reason_ = "";

  struct UploadedDialogPhotoInfo {
    DialogId dialog_id;
    double main_frame_timestamp;
    bool is_animation;
    bool is_reupload;
    Promise<Unit> promise;

    UploadedDialogPhotoInfo(DialogId dialog_id, double main_frame_timestamp, bool is_animation, bool is_reupload,
                            Promise<Unit> promise)
        : dialog_id(dialog_id)
        , main_frame_timestamp(main_frame_timestamp)
        , is_animation(is_animation)
        , is_reupload(is_reupload)
        , promise(std::move(promise)) {
    }
  };
  FlatHashMap<FileId, UploadedDialogPhotoInfo, FileIdHash> being_uploaded_dialog_photos_;

  struct UploadedImportedMessagesInfo {
    DialogId dialog_id;
    vector<FileId> attached_file_ids;
    bool is_reupload;
    Promise<Unit> promise;

    UploadedImportedMessagesInfo(DialogId dialog_id, vector<FileId> &&attached_file_ids, bool is_reupload,
                                 Promise<Unit> &&promise)
        : dialog_id(dialog_id)
        , attached_file_ids(std::move(attached_file_ids))
        , is_reupload(is_reupload)
        , promise(std::move(promise)) {
    }
  };
  FlatHashMap<FileId, unique_ptr<UploadedImportedMessagesInfo>, FileIdHash> being_uploaded_imported_messages_;

  struct UploadedImportedMessageAttachmentInfo {
    DialogId dialog_id;
    int64 import_id;
    bool is_reupload;
    Promise<Unit> promise;

    UploadedImportedMessageAttachmentInfo(DialogId dialog_id, int64 import_id, bool is_reupload,
                                          Promise<Unit> &&promise)
        : dialog_id(dialog_id), import_id(import_id), is_reupload(is_reupload), promise(std::move(promise)) {
    }
  };
  FlatHashMap<FileId, unique_ptr<UploadedImportedMessageAttachmentInfo>, FileIdHash>
      being_uploaded_imported_message_attachments_;

  struct PendingMessageImport {
    MultiPromiseActor upload_files_multipromise{"UploadAttachedFilesMultiPromiseActor"};
    DialogId dialog_id;
    int64 import_id = 0;
    Promise<Unit> promise;
  };
  FlatHashMap<int64, unique_ptr<PendingMessageImport>> pending_message_imports_;

  struct PendingMessageGroupSend {
    DialogId dialog_id;
    size_t finished_count = 0;
    vector<MessageId> message_ids;
    vector<bool> is_finished;
    vector<Status> results;
  };
  FlatHashMap<int64, PendingMessageGroupSend> pending_message_group_sends_;  // media_album_id -> ...

  WaitFreeHashMap<MessageId, DialogId, MessageIdHash> message_id_to_dialog_id_;
  FlatHashMap<MessageId, DialogId, MessageIdHash> last_clear_history_message_id_to_dialog_id_;

  bool created_public_broadcasts_inited_ = false;
  vector<ChannelId> created_public_broadcasts_;

  FlatHashMap<int64, DialogId> created_dialogs_;                                // random_id -> dialog_id
  FlatHashMap<DialogId, Promise<Unit>, DialogIdHash> pending_created_dialogs_;  // dialog_id -> promise

  bool running_get_difference_ = false;  // true after before_get_difference and false after after_get_difference

  WaitFreeHashMap<DialogId, unique_ptr<Dialog>, DialogIdHash> dialogs_;
  int64 added_message_count_ = 0;

  FlatHashSet<DialogId, DialogIdHash> loaded_dialogs_;  // dialogs loaded from database, but not added to dialogs_

  FlatHashSet<DialogId, DialogIdHash> postponed_chat_read_inbox_updates_;

  struct PendingGetMessageRequest {
    MessageId message_id;
    Promise<Unit> promise;
    tl_object_ptr<telegram_api::InputMessage> input_message;

    PendingGetMessageRequest(MessageId message_id, Promise<Unit> promise,
                             tl_object_ptr<telegram_api::InputMessage> input_message)
        : message_id(message_id), promise(std::move(promise)), input_message(std::move(input_message)) {
    }
  };

  FlatHashMap<string, vector<Promise<Unit>>> search_public_dialogs_queries_;
  FlatHashMap<string, vector<DialogId>> found_public_dialogs_;     // TODO time bound cache
  FlatHashMap<string, vector<DialogId>> found_on_server_dialogs_;  // TODO time bound cache

  struct CommonDialogs {
    vector<DialogId> dialog_ids;
    double receive_time = 0;
    int32 total_count = 0;
    bool is_outdated = false;
  };
  FlatHashMap<UserId, CommonDialogs, UserIdHash> found_common_dialogs_;

  FlatHashMap<int64, FullMessageId> get_dialog_message_by_date_results_;

  FlatHashMap<int64, td_api::object_ptr<td_api::messageCalendar>> found_dialog_message_calendars_;
  FlatHashMap<int64, std::pair<int32, vector<MessageId>>>
      found_dialog_messages_;                                     // random_id -> [total_count, [message_id]...]
  FlatHashMap<int64, DialogId> found_dialog_messages_dialog_id_;  // random_id -> dialog_id
  FlatHashMap<int64, std::pair<int32, vector<FullMessageId>>>
      found_messages_;  // random_id -> [total_count, [full_message_id]...]
  FlatHashMap<int64, std::pair<int32, vector<FullMessageId>>>
      found_call_messages_;  // random_id -> [total_count, [full_message_id]...]

  FlatHashMap<int64, FoundMessages> found_fts_messages_;  // random_id -> FoundMessages

  struct MessageEmbeddingCodes {
    FlatHashMap<MessageId, string, MessageIdHash> embedding_codes_;
  };
  FlatHashMap<DialogId, MessageEmbeddingCodes, DialogIdHash> message_embedding_codes_[2];

  FlatHashMap<DialogId, vector<Promise<Unit>>, DialogIdHash> get_dialog_queries_;
  FlatHashMap<DialogId, uint64, DialogIdHash> get_dialog_query_log_event_id_;

  FlatHashMap<FullMessageId, int32, FullMessageIdHash> replied_by_yet_unsent_messages_;
  FlatHashMap<FullMessageId, FlatHashSet<MessageId, MessageIdHash>, FullMessageIdHash> replied_yet_unsent_messages_;

  // full_message_id -> replies with media timestamps
  FlatHashMap<FullMessageId, FlatHashSet<MessageId, MessageIdHash>, FullMessageIdHash>
      replied_by_media_timestamp_messages_;

  struct ActiveDialogAction {
    MessageId top_thread_message_id;
    DialogId typing_dialog_id;
    DialogAction action;
    double start_time;

    ActiveDialogAction(MessageId top_thread_message_id, DialogId typing_dialog_id, DialogAction action,
                       double start_time)
        : top_thread_message_id(top_thread_message_id)
        , typing_dialog_id(typing_dialog_id)
        , action(std::move(action))
        , start_time(start_time) {
    }
  };

  FlatHashMap<DialogId, std::vector<ActiveDialogAction>, DialogIdHash> active_dialog_actions_;

  FlatHashMap<NotificationGroupId, DialogId, NotificationGroupIdHash> notification_group_id_to_dialog_id_;

  uint64 current_message_edit_generation_ = 0;

  std::unordered_set<DialogListId, DialogListIdHash> postponed_unread_message_count_updates_;
  std::unordered_set<DialogListId, DialogListIdHash> postponed_unread_chat_count_updates_;

  int64 current_pinned_dialog_order_ = static_cast<int64>(MIN_PINNED_DIALOG_DATE) << 32;

  std::unordered_map<DialogListId, DialogList, DialogListIdHash> dialog_lists_;
  std::unordered_map<FolderId, DialogFolder, FolderIdHash> dialog_folders_;

  bool are_dialog_filters_being_synchronized_ = false;
  bool are_dialog_filters_being_reloaded_ = false;
  bool need_dialog_filters_reload_ = false;
  bool disable_get_dialog_filter_ = false;
  bool is_update_chat_filters_sent_ = false;
  int32 dialog_filters_updated_date_ = 0;
  vector<unique_ptr<DialogFilter>> server_dialog_filters_;
  vector<unique_ptr<DialogFilter>> dialog_filters_;
  vector<RecommendedDialogFilter> recommended_dialog_filters_;
  vector<Promise<Unit>> dialog_filter_reload_queries_;
  int32 server_main_dialog_list_position_ = 0;  // position of the main dialog list stored on the server
  int32 main_dialog_list_position_ = 0;         // local position of the main dialog list stored on the server

  FlatHashMap<DialogId, string, DialogIdHash> active_get_channel_differencies_;
  FlatHashMap<DialogId, uint64, DialogIdHash> get_channel_difference_to_log_event_id_;
  FlatHashMap<DialogId, int32, DialogIdHash> channel_get_difference_retry_timeouts_;
  FlatHashMap<DialogId, std::multimap<int32, PendingPtsUpdate>, DialogIdHash> postponed_channel_updates_;
  FlatHashSet<DialogId, DialogIdHash> is_channel_difference_finished_;

  MultiTimeout channel_get_difference_timeout_{"ChannelGetDifferenceTimeout"};
  MultiTimeout channel_get_difference_retry_timeout_{"ChannelGetDifferenceRetryTimeout"};
  MultiTimeout pending_message_views_timeout_{"PendingMessageViewsTimeout"};
  MultiTimeout pending_message_live_location_view_timeout_{"PendingMessageLiveLocationViewTimeout"};
  MultiTimeout pending_draft_message_timeout_{"PendingDraftMessageTimeout"};
  MultiTimeout pending_read_history_timeout_{"PendingReadHistoryTimeout"};
  MultiTimeout pending_updated_dialog_timeout_{"PendingUpdatedDialogTimeout"};
  MultiTimeout pending_unload_dialog_timeout_{"PendingUnloadDialogTimeout"};
  MultiTimeout dialog_unmute_timeout_{"DialogUnmuteTimeout"};
  MultiTimeout pending_send_dialog_action_timeout_{"PendingSendDialogActionTimeout"};
  MultiTimeout active_dialog_action_timeout_{"ActiveDialogActionTimeout"};
  MultiTimeout update_dialog_online_member_count_timeout_{"UpdateDialogOnlineMemberCountTimeout"};
  MultiTimeout preload_folder_dialog_list_timeout_{"PreloadFolderDialogListTimeout"};
  MultiTimeout update_viewed_messages_timeout_{"UpdateViewedMessagesTimeout"};

  Timeout reload_dialog_filters_timeout_;

  Hints dialogs_hints_;  // search dialogs by title and username

  FlatHashSet<FullMessageId, FullMessageIdHash> active_live_location_full_message_ids_;
  bool are_active_live_location_messages_loaded_ = false;
  vector<Promise<Unit>> load_active_live_location_messages_queries_;

  FlatHashMap<DialogId, vector<Promise<Unit>>, DialogIdHash> load_scheduled_messages_from_database_queries_;

  struct ResolvedUsername {
    DialogId dialog_id;
    double expires_at;
  };

  FlatHashMap<string, ResolvedUsername> resolved_usernames_;
  FlatHashMap<string, DialogId> inaccessible_resolved_usernames_;
  FlatHashSet<string> reload_voice_chat_on_search_usernames_;

  struct GetDialogsTask {
    DialogListId dialog_list_id;
    int32 limit;
    int32 retry_count;
    DialogDate last_dialog_date = MIN_DIALOG_DATE;
    Promise<td_api::object_ptr<td_api::chats>> promise;
  };

  FlatHashMap<int64, GetDialogsTask> get_dialogs_tasks_;
  int64 current_get_dialogs_task_id_ = 0;

  struct PendingOnGetDialogs {
    FolderId folder_id;
    vector<tl_object_ptr<telegram_api::Dialog>> dialogs;
    int32 total_count;
    vector<tl_object_ptr<telegram_api::Message>> messages;
    Promise<Unit> promise;
  };

  vector<PendingOnGetDialogs> pending_on_get_dialogs_;
  FlatHashMap<DialogId, PendingOnGetDialogs, DialogIdHash> pending_channel_on_get_dialogs_;

  FlatHashMap<DialogId, vector<Promise<Unit>>, DialogIdHash> run_after_get_channel_difference_;

  ChangesProcessor<unique_ptr<PendingSecretMessage>> pending_secret_messages_;

  FlatHashMap<DialogId, FlatHashMap<int64, MessageId>, DialogIdHash>
      pending_secret_message_ids_;  // random_id -> message_id

  FlatHashMap<DialogId, vector<DialogId>, DialogIdHash> pending_add_dialog_last_database_message_dependent_dialogs_;
  FlatHashMap<DialogId, std::pair<int32, unique_ptr<Message>>, DialogIdHash>
      pending_add_dialog_last_database_message_;  // dialog -> dependency counter + message

  FlatHashMap<DialogId, vector<DialogId>, DialogIdHash>
      pending_add_default_join_group_call_as_dialog_id_;  // dialog_id -> dependent dialogs

  FlatHashMap<DialogId, vector<std::pair<DialogId, bool>>, DialogIdHash>
      pending_add_default_send_message_as_dialog_id_;  // dialog_id -> [dependent dialog, need_drop]

  struct MessageIds {
    FlatHashSet<MessageId, MessageIdHash> message_ids;
  };
  FlatHashMap<DialogId, MessageIds, DialogIdHash> dialog_bot_command_message_ids_;

  struct CallsDbState {
    std::array<MessageId, 2> first_calls_database_message_id_by_index;
    std::array<int32, 2> message_count_by_index;

    template <class StorerT>
    void store(StorerT &storer) const;

    template <class ParserT>
    void parse(ParserT &parser);
  };

  CallsDbState calls_db_state_;

  int64 viewed_live_location_task_id_ = 0;
  FlatHashMap<int64, FullMessageId> viewed_live_location_tasks_;  // task_id -> task

  FlatHashMap<uint64, std::map<MessageId, Promise<Message *>>> yet_unsent_media_queues_;

  FlatHashMap<DialogId, NetQueryRef, DialogIdHash> set_typing_query_;

  WaitFreeHashMap<FullMessageId, FileSourceId, FullMessageIdHash> full_message_id_to_file_source_id_;

  FlatHashMap<DialogId, int32, DialogIdHash> last_outgoing_forwarded_message_date_;

  struct ViewedMessagesInfo {
    FlatHashMap<MessageId, uint64, MessageIdHash> message_id_to_view_id;
    std::map<uint64, MessageId> recently_viewed_messages;
    uint64 current_view_id = 0;
  };
  FlatHashMap<DialogId, unique_ptr<ViewedMessagesInfo>, DialogIdHash> dialog_viewed_messages_;

  struct OnlineMemberCountInfo {
    int32 online_member_count = 0;
    double update_time = 0;
    bool is_update_sent = false;
  };
  FlatHashMap<DialogId, OnlineMemberCountInfo, DialogIdHash> dialog_online_member_counts_;

  struct ReactionsToReload {
    FlatHashSet<MessageId, MessageIdHash> message_ids;
    bool is_request_sent = false;
  };
  FlatHashMap<DialogId, ReactionsToReload, DialogIdHash> being_reloaded_reactions_;

  FlatHashMap<DialogId, std::pair<bool, bool>, DialogIdHash> pending_dialog_group_call_updates_;

  FlatHashMap<string, int32> auth_notification_id_date_;

  FlatHashMap<DialogId, MessageId, DialogIdHash> previous_repaired_read_inbox_max_message_id_;

  struct PendingReaction {
    int32 query_count = 0;
    bool was_updated = false;
  };
  FlatHashMap<FullMessageId, PendingReaction, FullMessageIdHash> pending_reactions_;

  vector<string> active_reactions_;
  FlatHashMap<string, size_t> active_reaction_pos_;

  uint32 scheduled_messages_sync_generation_ = 1;

  int64 authorization_date_ = 0;

  DialogId removed_sponsored_dialog_id_;
  DialogId sponsored_dialog_id_;
  DialogSource sponsored_dialog_source_;

  FullMessageId being_readded_message_id_;

  DialogId being_added_dialog_id_;
  DialogId being_added_by_new_message_dialog_id_;
  DialogId being_added_new_dialog_id_;

  DialogId debug_channel_difference_dialog_;
  DialogId debug_last_get_channel_difference_dialog_id_;
  const char *debug_last_get_channel_difference_source_ = "unknown";

  double start_time_ = 0;
  bool is_inited_ = false;

  Td *td_;
  ActorShared<> parent_;
};

}  // namespace td
