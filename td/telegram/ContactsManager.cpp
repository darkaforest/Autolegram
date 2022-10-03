//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/ContactsManager.h"

#include "td/telegram/AnimationsManager.h"
#include "td/telegram/AuthManager.h"
#include "td/telegram/BotMenuButton.h"
#include "td/telegram/ChannelParticipantFilter.h"
#include "td/telegram/ConfigManager.h"
#include "td/telegram/Dependencies.h"
#include "td/telegram/DialogInviteLink.h"
#include "td/telegram/DialogLocation.h"
#include "td/telegram/Document.h"
#include "td/telegram/DocumentsManager.h"
#include "td/telegram/FileReferenceManager.h"
#include "td/telegram/files/FileManager.h"
#include "td/telegram/files/FileType.h"
#include "td/telegram/FolderId.h"
#include "td/telegram/Global.h"
#include "td/telegram/GroupCallManager.h"
#include "td/telegram/InlineQueriesManager.h"
#include "td/telegram/InputGroupCallId.h"
#include "td/telegram/LinkManager.h"
#include "td/telegram/logevent/LogEvent.h"
#include "td/telegram/logevent/LogEventHelper.h"
#include "td/telegram/MessageSender.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/MessageTtl.h"
#include "td/telegram/MinChannel.h"
#include "td/telegram/misc.h"
#include "td/telegram/net/NetQuery.h"
#include "td/telegram/NotificationManager.h"
#include "td/telegram/OptionManager.h"
#include "td/telegram/PasswordManager.h"
#include "td/telegram/Photo.h"
#include "td/telegram/Photo.hpp"
#include "td/telegram/PremiumGiftOption.hpp"
#include "td/telegram/SecretChatLayer.h"
#include "td/telegram/SecretChatsManager.h"
#include "td/telegram/ServerMessageId.h"
#include "td/telegram/StickerSetId.hpp"
#include "td/telegram/StickersManager.h"
#include "td/telegram/Td.h"
#include "td/telegram/TdDb.h"
#include "td/telegram/TdParameters.h"
#include "td/telegram/telegram_api.hpp"
#include "td/telegram/UpdatesManager.h"
#include "td/telegram/Version.h"

#include "td/db/binlog/BinlogEvent.h"
#include "td/db/binlog/BinlogHelper.h"
#include "td/db/SqliteKeyValue.h"
#include "td/db/SqliteKeyValueAsync.h"

#include "td/actor/SleepActor.h"

#include "td/utils/algorithm.h"
#include "td/utils/buffer.h"
#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Random.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/Time.h"
#include "td/utils/tl_helpers.h"
#include "td/utils/utf8.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace td {

class DismissSuggestionQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit DismissSuggestionQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(SuggestedAction action) {
    dialog_id_ = action.dialog_id_;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id_, AccessRights::Read);
    CHECK(input_peer != nullptr);

    send_query(G()->net_query_creator().create(
        telegram_api::help_dismissSuggestion(std::move(input_peer), action.get_suggested_action_str())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::help_dismissSuggestion>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "DismissSuggestionQuery");
    promise_.set_error(std::move(status));
  }
};

class GetContactsQuery final : public Td::ResultHandler {
 public:
  void send(int64 hash) {
    send_query(G()->net_query_creator().create(telegram_api::contacts_getContacts(hash)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::contacts_getContacts>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetContactsQuery: " << to_string(ptr);
    td_->contacts_manager_->on_get_contacts(std::move(ptr));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_contacts_failed(std::move(status));
    td_->updates_manager_->get_difference("GetContactsQuery");
  }
};

class GetContactsStatusesQuery final : public Td::ResultHandler {
 public:
  void send() {
    send_query(G()->net_query_creator().create(telegram_api::contacts_getStatuses()));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::contacts_getStatuses>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    td_->contacts_manager_->on_get_contacts_statuses(result_ptr.move_as_ok());
  }

  void on_error(Status status) final {
    if (!G()->is_expected_error(status)) {
      LOG(ERROR) << "Receive error for GetContactsStatusesQuery: " << status;
    }
  }
};

class AddContactQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  UserId user_id_;

 public:
  explicit AddContactQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(UserId user_id, tl_object_ptr<telegram_api::InputUser> &&input_user, const Contact &contact,
            bool share_phone_number) {
    user_id_ = user_id;
    int32 flags = 0;
    if (share_phone_number) {
      flags |= telegram_api::contacts_addContact::ADD_PHONE_PRIVACY_EXCEPTION_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::contacts_addContact(flags, false /*ignored*/, std::move(input_user), contact.get_first_name(),
                                          contact.get_last_name(), contact.get_phone_number())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::contacts_addContact>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for AddContactQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
    td_->contacts_manager_->reload_contacts(true);
    td_->messages_manager_->reget_dialog_action_bar(DialogId(user_id_), "AddContactQuery");
  }
};

class ResolvePhoneQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  string phone_number_;

 public:
  explicit ResolvePhoneQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(const string &phone_number) {
    phone_number_ = phone_number;
    send_query(G()->net_query_creator().create(telegram_api::contacts_resolvePhone(phone_number)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::contacts_resolvePhone>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for ResolvePhoneQuery: " << to_string(ptr);
    td_->contacts_manager_->on_get_users(std::move(ptr->users_), "ResolvePhoneQuery");
    td_->contacts_manager_->on_get_chats(std::move(ptr->chats_), "ResolvePhoneQuery");

    DialogId dialog_id(ptr->peer_);
    if (dialog_id.get_type() != DialogType::User) {
      LOG(ERROR) << "Receive " << dialog_id << " by " << phone_number_;
      return on_error(Status::Error(500, "Receive invalid response"));
    }

    td_->contacts_manager_->on_resolved_phone_number(phone_number_, dialog_id.get_user_id());

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    if (status.message() == Slice("PHONE_NOT_OCCUPIED")) {
      td_->contacts_manager_->on_resolved_phone_number(phone_number_, UserId());
      return promise_.set_value(Unit());
    }
    promise_.set_error(std::move(status));
  }
};

class AcceptContactQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  UserId user_id_;

 public:
  explicit AcceptContactQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(UserId user_id, tl_object_ptr<telegram_api::InputUser> &&input_user) {
    user_id_ = user_id;
    send_query(G()->net_query_creator().create(telegram_api::contacts_acceptContact(std::move(input_user))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::contacts_acceptContact>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for AcceptContactQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
    td_->contacts_manager_->reload_contacts(true);
    td_->messages_manager_->reget_dialog_action_bar(DialogId(user_id_), "AcceptContactQuery");
  }
};

class ImportContactsQuery final : public Td::ResultHandler {
  int64 random_id_ = 0;
  size_t sent_size_ = 0;

 public:
  void send(vector<tl_object_ptr<telegram_api::inputPhoneContact>> &&input_phone_contacts, int64 random_id) {
    random_id_ = random_id;
    sent_size_ = input_phone_contacts.size();
    send_query(G()->net_query_creator().create(telegram_api::contacts_importContacts(std::move(input_phone_contacts))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::contacts_importContacts>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for ImportContactsQuery: " << to_string(ptr);
    if (sent_size_ == ptr->retry_contacts_.size()) {
      return on_error(Status::Error(429, "Too Many Requests: retry after 3600"));
    }
    td_->contacts_manager_->on_imported_contacts(random_id_, std::move(ptr));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_imported_contacts(random_id_, std::move(status));
  }
};

class DeleteContactsQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit DeleteContactsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(vector<tl_object_ptr<telegram_api::InputUser>> &&input_users) {
    send_query(G()->net_query_creator().create(telegram_api::contacts_deleteContacts(std::move(input_users))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::contacts_deleteContacts>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for DeleteContactsQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
    td_->contacts_manager_->reload_contacts(true);
  }
};

class DeleteContactsByPhoneNumberQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  vector<UserId> user_ids_;

 public:
  explicit DeleteContactsByPhoneNumberQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(vector<string> &&user_phone_numbers, vector<UserId> &&user_ids) {
    if (user_phone_numbers.empty()) {
      return promise_.set_value(Unit());
    }
    user_ids_ = std::move(user_ids);
    send_query(G()->net_query_creator().create(telegram_api::contacts_deleteByPhones(std::move(user_phone_numbers))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::contacts_deleteByPhones>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    if (!result) {
      return on_error(Status::Error(500, "Some contacts can't be deleted"));
    }

    td_->contacts_manager_->on_deleted_contacts(user_ids_);
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
    td_->contacts_manager_->reload_contacts(true);
  }
};

class ResetContactsQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit ResetContactsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send() {
    send_query(G()->net_query_creator().create(telegram_api::contacts_resetSaved()));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::contacts_resetSaved>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    if (!result) {
      LOG(ERROR) << "Failed to delete imported contacts";
      td_->contacts_manager_->reload_contacts(true);
    } else {
      td_->contacts_manager_->on_update_contacts_reset();
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
    td_->contacts_manager_->reload_contacts(true);
  }
};

class SearchDialogsNearbyQuery final : public Td::ResultHandler {
  Promise<tl_object_ptr<telegram_api::Updates>> promise_;

 public:
  explicit SearchDialogsNearbyQuery(Promise<tl_object_ptr<telegram_api::Updates>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(const Location &location, bool from_background, int32 expire_date) {
    int32 flags = 0;
    if (from_background) {
      flags |= telegram_api::contacts_getLocated::BACKGROUND_MASK;
    }
    if (expire_date != -1) {
      flags |= telegram_api::contacts_getLocated::SELF_EXPIRES_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::contacts_getLocated(flags, false /*ignored*/, location.get_input_geo_point(), expire_date)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::contacts_getLocated>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(result_ptr.move_as_ok());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class UploadProfilePhotoQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  FileId file_id_;

 public:
  explicit UploadProfilePhotoQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(FileId file_id, tl_object_ptr<telegram_api::InputFile> &&input_file, bool is_animation,
            double main_frame_timestamp) {
    CHECK(input_file != nullptr);
    CHECK(file_id.is_valid());

    file_id_ = file_id;

    int32 flags = 0;
    tl_object_ptr<telegram_api::InputFile> photo_input_file;
    tl_object_ptr<telegram_api::InputFile> video_input_file;
    if (is_animation) {
      flags |= telegram_api::photos_uploadProfilePhoto::VIDEO_MASK;
      video_input_file = std::move(input_file);

      if (main_frame_timestamp != 0.0) {
        flags |= telegram_api::photos_uploadProfilePhoto::VIDEO_START_TS_MASK;
      }
    } else {
      flags |= telegram_api::photos_uploadProfilePhoto::FILE_MASK;
      photo_input_file = std::move(input_file);
    }
    send_query(G()->net_query_creator().create(
        telegram_api::photos_uploadProfilePhoto(flags, std::move(photo_input_file), std::move(video_input_file),
                                                main_frame_timestamp),
        {{"me"}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::photos_uploadProfilePhoto>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    td_->contacts_manager_->on_set_profile_photo(result_ptr.move_as_ok(), 0);

    td_->file_manager_->delete_partial_remote_location(file_id_);

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
    td_->file_manager_->delete_partial_remote_location(file_id_);
    td_->updates_manager_->get_difference("UploadProfilePhotoQuery");
  }
};

class UpdateProfilePhotoQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  FileId file_id_;
  int64 old_photo_id_;
  string file_reference_;

 public:
  explicit UpdateProfilePhotoQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(FileId file_id, int64 old_photo_id, tl_object_ptr<telegram_api::InputPhoto> &&input_photo) {
    CHECK(input_photo != nullptr);
    file_id_ = file_id;
    old_photo_id_ = old_photo_id;
    file_reference_ = FileManager::extract_file_reference(input_photo);
    send_query(
        G()->net_query_creator().create(telegram_api::photos_updateProfilePhoto(std::move(input_photo)), {{"me"}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::photos_updateProfilePhoto>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    td_->contacts_manager_->on_set_profile_photo(result_ptr.move_as_ok(), old_photo_id_);

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    if (!td_->auth_manager_->is_bot() && FileReferenceManager::is_file_reference_error(status)) {
      if (file_id_.is_valid()) {
        VLOG(file_references) << "Receive " << status << " for " << file_id_;
        td_->file_manager_->delete_file_reference(file_id_, file_reference_);
        td_->file_reference_manager_->repair_file_reference(
            file_id_, PromiseCreator::lambda([file_id = file_id_, old_photo_id = old_photo_id_,
                                              promise = std::move(promise_)](Result<Unit> result) mutable {
              if (result.is_error()) {
                return promise.set_error(Status::Error(400, "Can't find the photo"));
              }

              send_closure(G()->contacts_manager(), &ContactsManager::send_update_profile_photo_query, file_id,
                           old_photo_id, std::move(promise));
            }));
        return;
      } else {
        LOG(ERROR) << "Receive file reference error, but file_id = " << file_id_;
      }
    }

    promise_.set_error(std::move(status));
  }
};

class DeleteProfilePhotoQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  int64 profile_photo_id_;

 public:
  explicit DeleteProfilePhotoQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(int64 profile_photo_id) {
    profile_photo_id_ = profile_photo_id;
    vector<tl_object_ptr<telegram_api::InputPhoto>> input_photo_ids;
    input_photo_ids.push_back(make_tl_object<telegram_api::inputPhoto>(profile_photo_id, 0, BufferSlice()));
    send_query(G()->net_query_creator().create(telegram_api::photos_deletePhotos(std::move(input_photo_ids))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::photos_deletePhotos>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for DeleteProfilePhotoQuery: " << format::as_array(result);
    if (result.size() != 1u) {
      LOG(WARNING) << "Photo can't be deleted";
      return on_error(Status::Error(400, "Photo can't be deleted"));
    }

    td_->contacts_manager_->on_delete_profile_photo(profile_photo_id_, std::move(promise_));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class UpdateProfileQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  int32 flags_;
  string first_name_;
  string last_name_;
  string about_;

 public:
  explicit UpdateProfileQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(int32 flags, const string &first_name, const string &last_name, const string &about) {
    flags_ = flags;
    first_name_ = first_name;
    last_name_ = last_name;
    about_ = about;
    send_query(G()->net_query_creator().create(telegram_api::account_updateProfile(flags, first_name, last_name, about),
                                               {{"me"}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::account_updateProfile>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    LOG(DEBUG) << "Receive result for UpdateProfileQuery: " << to_string(result_ptr.ok());
    td_->contacts_manager_->on_get_user(result_ptr.move_as_ok(), "UpdateProfileQuery");
    td_->contacts_manager_->on_update_profile_success(flags_, first_name_, last_name_, about_);

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class CheckUsernameQuery final : public Td::ResultHandler {
  Promise<bool> promise_;

 public:
  explicit CheckUsernameQuery(Promise<bool> &&promise) : promise_(std::move(promise)) {
  }

  void send(const string &username) {
    send_query(G()->net_query_creator().create(telegram_api::account_checkUsername(username)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::account_checkUsername>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(result_ptr.move_as_ok());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class UpdateUsernameQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit UpdateUsernameQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(const string &username) {
    send_query(G()->net_query_creator().create(telegram_api::account_updateUsername(username), {{"me"}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::account_updateUsername>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    LOG(DEBUG) << "Receive result for UpdateUsernameQuery: " << to_string(result_ptr.ok());
    td_->contacts_manager_->on_get_user(result_ptr.move_as_ok(), "UpdateUsernameQuery");
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    if (status.message() == "USERNAME_NOT_MODIFIED" && !td_->auth_manager_->is_bot()) {
      promise_.set_value(Unit());
      return;
    }
    promise_.set_error(std::move(status));
  }
};

class UpdateEmojiStatusQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit UpdateEmojiStatusQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(EmojiStatus emoji_status) {
    send_query(G()->net_query_creator().create(
        telegram_api::account_updateEmojiStatus(emoji_status.get_input_emoji_status()), {{"me"}}));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::account_updateEmojiStatus>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    LOG(DEBUG) << "Receive result for UpdateEmojiStatusQuery: " << result_ptr.ok();
    if (result_ptr.ok()) {
      promise_.set_value(Unit());
    } else {
      promise_.set_error(Status::Error(400, "Failed to change Premium badge"));
    }
  }

  void on_error(Status status) final {
    get_recent_emoji_statuses(td_, Auto());
    promise_.set_error(std::move(status));
  }
};

class CheckChannelUsernameQuery final : public Td::ResultHandler {
  Promise<bool> promise_;
  ChannelId channel_id_;
  string username_;

 public:
  explicit CheckChannelUsernameQuery(Promise<bool> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, const string &username) {
    channel_id_ = channel_id;
    tl_object_ptr<telegram_api::InputChannel> input_channel;
    if (channel_id.is_valid()) {
      input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    } else {
      input_channel = make_tl_object<telegram_api::inputChannelEmpty>();
    }
    CHECK(input_channel != nullptr);
    send_query(
        G()->net_query_creator().create(telegram_api::channels_checkUsername(std::move(input_channel), username)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_checkUsername>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(result_ptr.move_as_ok());
  }

  void on_error(Status status) final {
    if (channel_id_.is_valid()) {
      td_->contacts_manager_->on_get_channel_error(channel_id_, status, "CheckChannelUsernameQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class UpdateChannelUsernameQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  string username_;

 public:
  explicit UpdateChannelUsernameQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, const string &username) {
    channel_id_ = channel_id;
    username_ = username;
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(
        G()->net_query_creator().create(telegram_api::channels_updateUsername(std::move(input_channel), username)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_updateUsername>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    LOG(DEBUG) << "Receive result for UpdateChannelUsernameQuery: " << result;
    if (!result) {
      return on_error(Status::Error(500, "Supergroup username is not updated"));
    }

    td_->contacts_manager_->on_update_channel_username(channel_id_, std::move(username_));
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    if (status.message() == "USERNAME_NOT_MODIFIED" || status.message() == "CHAT_NOT_MODIFIED") {
      td_->contacts_manager_->on_update_channel_username(channel_id_, std::move(username_));
      if (!td_->auth_manager_->is_bot()) {
        promise_.set_value(Unit());
        return;
      }
    } else {
      td_->contacts_manager_->on_get_channel_error(channel_id_, status, "UpdateChannelUsernameQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class SetChannelStickerSetQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  StickerSetId sticker_set_id_;

 public:
  explicit SetChannelStickerSetQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, StickerSetId sticker_set_id,
            telegram_api::object_ptr<telegram_api::InputStickerSet> &&input_sticker_set) {
    channel_id_ = channel_id;
    sticker_set_id_ = sticker_set_id;
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(
        telegram_api::channels_setStickers(std::move(input_channel), std::move(input_sticker_set))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_setStickers>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    LOG(DEBUG) << "Receive result for SetChannelStickerSetQuery: " << result;
    if (!result) {
      return on_error(Status::Error(500, "Supergroup sticker set not updated"));
    }

    td_->contacts_manager_->on_update_channel_sticker_set(channel_id_, sticker_set_id_);
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    if (status.message() == "CHAT_NOT_MODIFIED") {
      td_->contacts_manager_->on_update_channel_sticker_set(channel_id_, sticker_set_id_);
      if (!td_->auth_manager_->is_bot()) {
        promise_.set_value(Unit());
        return;
      }
    } else {
      td_->contacts_manager_->on_get_channel_error(channel_id_, status, "SetChannelStickerSetQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class ToggleChannelSignaturesQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit ToggleChannelSignaturesQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, bool sign_messages) {
    channel_id_ = channel_id;
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(
        telegram_api::channels_toggleSignatures(std::move(input_channel), sign_messages)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_toggleSignatures>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for ToggleChannelSignaturesQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    if (status.message() == "CHAT_NOT_MODIFIED") {
      if (!td_->auth_manager_->is_bot()) {
        promise_.set_value(Unit());
        return;
      }
    } else {
      td_->contacts_manager_->on_get_channel_error(channel_id_, status, "ToggleChannelSignaturesQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class ToggleChannelJoinToSendQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit ToggleChannelJoinToSendQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, bool join_to_send) {
    channel_id_ = channel_id;
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(
        telegram_api::channels_toggleJoinToSend(std::move(input_channel), join_to_send)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_toggleJoinToSend>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for ToggleChannelJoinToSendQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    if (status.message() == "CHAT_NOT_MODIFIED") {
      if (!td_->auth_manager_->is_bot()) {
        promise_.set_value(Unit());
        return;
      }
    } else {
      td_->contacts_manager_->on_get_channel_error(channel_id_, status, "ToggleChannelJoinToSendQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class ToggleChannelJoinRequestQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit ToggleChannelJoinRequestQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, bool join_request) {
    channel_id_ = channel_id;
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(
        telegram_api::channels_toggleJoinRequest(std::move(input_channel), join_request)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_toggleJoinRequest>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for ToggleChannelJoinRequestQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    if (status.message() == "CHAT_NOT_MODIFIED") {
      if (!td_->auth_manager_->is_bot()) {
        promise_.set_value(Unit());
        return;
      }
    } else {
      td_->contacts_manager_->on_get_channel_error(channel_id_, status, "ToggleChannelJoinRequestQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class TogglePrehistoryHiddenQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  bool is_all_history_available_;

 public:
  explicit TogglePrehistoryHiddenQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, bool is_all_history_available) {
    channel_id_ = channel_id;
    is_all_history_available_ = is_all_history_available;

    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(
        telegram_api::channels_togglePreHistoryHidden(std::move(input_channel), !is_all_history_available)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_togglePreHistoryHidden>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for TogglePrehistoryHiddenQuery: " << to_string(ptr);

    td_->updates_manager_->on_get_updates(
        std::move(ptr),
        PromiseCreator::lambda([actor_id = G()->contacts_manager(), promise = std::move(promise_),
                                channel_id = channel_id_,
                                is_all_history_available = is_all_history_available_](Unit result) mutable {
          send_closure(actor_id, &ContactsManager::on_update_channel_is_all_history_available, channel_id,
                       is_all_history_available, std::move(promise));
        }));
  }

  void on_error(Status status) final {
    if (status.message() == "CHAT_NOT_MODIFIED") {
      if (!td_->auth_manager_->is_bot()) {
        promise_.set_value(Unit());
        return;
      }
    } else {
      td_->contacts_manager_->on_get_channel_error(channel_id_, status, "TogglePrehistoryHiddenQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class ConvertToGigagroupQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit ConvertToGigagroupQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id) {
    channel_id_ = channel_id;

    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(telegram_api::channels_convertToGigagroup(std::move(input_channel))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_convertToGigagroup>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for ConvertToGigagroupQuery: " << to_string(ptr);

    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    if (status.message() == "CHAT_NOT_MODIFIED") {
      promise_.set_value(Unit());
      return;
    } else {
      td_->contacts_manager_->on_get_channel_error(channel_id_, status, "ConvertToGigagroupQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class EditChatAboutQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;
  string about_;

  void on_success() {
    switch (dialog_id_.get_type()) {
      case DialogType::Chat:
        return td_->contacts_manager_->on_update_chat_description(dialog_id_.get_chat_id(), std::move(about_));
      case DialogType::Channel:
        return td_->contacts_manager_->on_update_channel_description(dialog_id_.get_channel_id(), std::move(about_));
      case DialogType::User:
      case DialogType::SecretChat:
      case DialogType::None:
        UNREACHABLE();
    }
  }

 public:
  explicit EditChatAboutQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &about) {
    dialog_id_ = dialog_id;
    about_ = about;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }
    send_query(G()->net_query_creator().create(telegram_api::messages_editChatAbout(std::move(input_peer), about)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_editChatAbout>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    LOG(DEBUG) << "Receive result for EditChatAboutQuery: " << result;
    if (!result) {
      return on_error(Status::Error(500, "Chat description is not updated"));
    }

    on_success();
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    if (status.message() == "CHAT_ABOUT_NOT_MODIFIED" || status.message() == "CHAT_NOT_MODIFIED") {
      on_success();
      if (!td_->auth_manager_->is_bot()) {
        promise_.set_value(Unit());
        return;
      }
    } else {
      td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "EditChatAboutQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class SetDiscussionGroupQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId broadcast_channel_id_;
  ChannelId group_channel_id_;

 public:
  explicit SetDiscussionGroupQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId broadcast_channel_id,
            telegram_api::object_ptr<telegram_api::InputChannel> broadcast_input_channel, ChannelId group_channel_id,
            telegram_api::object_ptr<telegram_api::InputChannel> group_input_channel) {
    broadcast_channel_id_ = broadcast_channel_id;
    group_channel_id_ = group_channel_id;
    send_query(G()->net_query_creator().create(
        telegram_api::channels_setDiscussionGroup(std::move(broadcast_input_channel), std::move(group_input_channel))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_setDiscussionGroup>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    bool result = result_ptr.move_as_ok();
    LOG_IF(INFO, !result) << "Set discussion group has failed";

    td_->contacts_manager_->on_update_channel_linked_channel_id(broadcast_channel_id_, group_channel_id_);
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    if (status.message() == "LINK_NOT_MODIFIED") {
      return promise_.set_value(Unit());
    }
    promise_.set_error(std::move(status));
  }
};

class EditLocationQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  DialogLocation location_;

 public:
  explicit EditLocationQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, const DialogLocation &location) {
    channel_id_ = channel_id;
    location_ = location;

    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);

    send_query(G()->net_query_creator().create(telegram_api::channels_editLocation(
        std::move(input_channel), location_.get_input_geo_point(), location_.get_address())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_editLocation>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    bool result = result_ptr.move_as_ok();
    LOG_IF(INFO, !result) << "Edit chat location has failed";

    td_->contacts_manager_->on_update_channel_location(channel_id_, location_);
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "EditLocationQuery");
    promise_.set_error(std::move(status));
  }
};

class ToggleSlowModeQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  int32 slow_mode_delay_ = 0;

 public:
  explicit ToggleSlowModeQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, int32 slow_mode_delay) {
    channel_id_ = channel_id;
    slow_mode_delay_ = slow_mode_delay;

    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);

    send_query(G()->net_query_creator().create(
        telegram_api::channels_toggleSlowMode(std::move(input_channel), slow_mode_delay)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_toggleSlowMode>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for ToggleSlowModeQuery: " << to_string(ptr);

    td_->updates_manager_->on_get_updates(
        std::move(ptr),
        PromiseCreator::lambda([actor_id = G()->contacts_manager(), promise = std::move(promise_),
                                channel_id = channel_id_, slow_mode_delay = slow_mode_delay_](Unit result) mutable {
          send_closure(actor_id, &ContactsManager::on_update_channel_slow_mode_delay, channel_id, slow_mode_delay,
                       std::move(promise));
        }));
  }

  void on_error(Status status) final {
    if (status.message() == "CHAT_NOT_MODIFIED") {
      td_->contacts_manager_->on_update_channel_slow_mode_delay(channel_id_, slow_mode_delay_, Promise<Unit>());
      if (!td_->auth_manager_->is_bot()) {
        promise_.set_value(Unit());
        return;
      }
    } else {
      td_->contacts_manager_->on_get_channel_error(channel_id_, status, "ToggleSlowModeQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class ReportChannelSpamQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  DialogId sender_dialog_id_;

 public:
  explicit ReportChannelSpamQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, DialogId sender_dialog_id, const vector<MessageId> &message_ids) {
    channel_id_ = channel_id;
    sender_dialog_id_ = sender_dialog_id;

    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);

    auto input_peer = td_->messages_manager_->get_input_peer(sender_dialog_id, AccessRights::Know);
    CHECK(input_peer != nullptr);

    send_query(G()->net_query_creator().create(telegram_api::channels_reportSpam(
        std::move(input_channel), std::move(input_peer), MessagesManager::get_server_message_ids(message_ids))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_reportSpam>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    bool result = result_ptr.move_as_ok();
    LOG_IF(INFO, !result) << "Report spam has failed in " << channel_id_;

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    if (sender_dialog_id_.get_type() != DialogType::Channel) {
      td_->contacts_manager_->on_get_channel_error(channel_id_, status, "ReportChannelSpamQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class DeleteChatQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit DeleteChatQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChatId chat_id) {
    send_query(G()->net_query_creator().create(telegram_api::messages_deleteChat(chat_id.get())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_deleteChat>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    LOG(INFO) << "Receive result for DeleteChatQuery: " << result_ptr.ok();
    td_->updates_manager_->get_difference("DeleteChatQuery");
    td_->updates_manager_->on_get_updates(make_tl_object<telegram_api::updates>(), std::move(promise_));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class DeleteChannelQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit DeleteChannelQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id) {
    channel_id_ = channel_id;
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(telegram_api::channels_deleteChannel(std::move(input_channel))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_deleteChannel>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for DeleteChannelQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "DeleteChannelQuery");
    promise_.set_error(std::move(status));
  }
};

class AddChatUserQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit AddChatUserQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChatId chat_id, tl_object_ptr<telegram_api::InputUser> &&input_user, int32 forward_limit) {
    send_query(G()->net_query_creator().create(
        telegram_api::messages_addChatUser(chat_id.get(), std::move(input_user), forward_limit)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_addChatUser>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for AddChatUserQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
    td_->updates_manager_->get_difference("AddChatUserQuery");
  }
};

class EditChatAdminQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChatId chat_id_;

 public:
  explicit EditChatAdminQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChatId chat_id, tl_object_ptr<telegram_api::InputUser> &&input_user, bool is_administrator) {
    chat_id_ = chat_id;
    send_query(G()->net_query_creator().create(
        telegram_api::messages_editChatAdmin(chat_id.get(), std::move(input_user), is_administrator)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_editChatAdmin>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    bool result = result_ptr.move_as_ok();
    if (!result) {
      LOG(ERROR) << "Receive false as result of messages.editChatAdmin";
      return on_error(Status::Error(400, "Can't edit chat administrators"));
    }

    // result will come in the updates
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
    td_->updates_manager_->get_difference("EditChatAdminQuery");
  }
};

class ExportChatInviteQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chatInviteLink>> promise_;
  DialogId dialog_id_;

 public:
  explicit ExportChatInviteQuery(Promise<td_api::object_ptr<td_api::chatInviteLink>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &title, int32 expire_date, int32 usage_limit, bool creates_join_request,
            bool is_permanent) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    int32 flags = 0;
    if (expire_date > 0) {
      flags |= telegram_api::messages_exportChatInvite::EXPIRE_DATE_MASK;
    }
    if (usage_limit > 0) {
      flags |= telegram_api::messages_exportChatInvite::USAGE_LIMIT_MASK;
    }
    if (creates_join_request) {
      flags |= telegram_api::messages_exportChatInvite::REQUEST_NEEDED_MASK;
    }
    if (is_permanent) {
      flags |= telegram_api::messages_exportChatInvite::LEGACY_REVOKE_PERMANENT_MASK;
    }
    if (!title.empty()) {
      flags |= telegram_api::messages_exportChatInvite::TITLE_MASK;
    }

    send_query(G()->net_query_creator().create(telegram_api::messages_exportChatInvite(
        flags, false /*ignored*/, false /*ignored*/, std::move(input_peer), expire_date, usage_limit, title)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_exportChatInvite>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for ExportChatInviteQuery: " << to_string(ptr);

    DialogInviteLink invite_link(std::move(ptr), "ExportChatInviteQuery");
    if (!invite_link.is_valid()) {
      return on_error(Status::Error(500, "Receive invalid invite link"));
    }
    if (invite_link.get_creator_user_id() != td_->contacts_manager_->get_my_id()) {
      return on_error(Status::Error(500, "Receive invalid invite link creator"));
    }
    if (invite_link.is_permanent()) {
      td_->contacts_manager_->on_get_permanent_dialog_invite_link(dialog_id_, invite_link);
    }
    promise_.set_value(invite_link.get_chat_invite_link_object(td_->contacts_manager_.get()));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "ExportChatInviteQuery");
    promise_.set_error(std::move(status));
  }
};

class EditChatInviteLinkQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chatInviteLink>> promise_;
  DialogId dialog_id_;

 public:
  explicit EditChatInviteLinkQuery(Promise<td_api::object_ptr<td_api::chatInviteLink>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &invite_link, const string &title, int32 expire_date, int32 usage_limit,
            bool creates_join_request) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    int32 flags = telegram_api::messages_editExportedChatInvite::EXPIRE_DATE_MASK |
                  telegram_api::messages_editExportedChatInvite::USAGE_LIMIT_MASK |
                  telegram_api::messages_editExportedChatInvite::REQUEST_NEEDED_MASK |
                  telegram_api::messages_editExportedChatInvite::TITLE_MASK;
    send_query(G()->net_query_creator().create(
        telegram_api::messages_editExportedChatInvite(flags, false /*ignored*/, std::move(input_peer), invite_link,
                                                      expire_date, usage_limit, creates_join_request, title)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_editExportedChatInvite>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for EditChatInviteLinkQuery: " << to_string(result);

    if (result->get_id() != telegram_api::messages_exportedChatInvite::ID) {
      return on_error(Status::Error(500, "Receive unexpected response from server"));
    }

    auto invite = move_tl_object_as<telegram_api::messages_exportedChatInvite>(result);

    td_->contacts_manager_->on_get_users(std::move(invite->users_), "EditChatInviteLinkQuery");

    DialogInviteLink invite_link(std::move(invite->invite_), "EditChatInviteLinkQuery");
    if (!invite_link.is_valid()) {
      return on_error(Status::Error(500, "Receive invalid invite link"));
    }
    promise_.set_value(invite_link.get_chat_invite_link_object(td_->contacts_manager_.get()));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "EditChatInviteLinkQuery");
    promise_.set_error(std::move(status));
  }
};

class GetExportedChatInviteQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chatInviteLink>> promise_;
  DialogId dialog_id_;

 public:
  explicit GetExportedChatInviteQuery(Promise<td_api::object_ptr<td_api::chatInviteLink>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &invite_link) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    send_query(G()->net_query_creator().create(
        telegram_api::messages_getExportedChatInvite(std::move(input_peer), invite_link)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_getExportedChatInvite>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    if (result_ptr.ok()->get_id() != telegram_api::messages_exportedChatInvite::ID) {
      LOG(ERROR) << "Receive wrong result for GetExportedChatInviteQuery: " << to_string(result_ptr.ok());
      return on_error(Status::Error(500, "Receive unexpected response"));
    }

    auto result = move_tl_object_as<telegram_api::messages_exportedChatInvite>(result_ptr.ok_ref());
    LOG(INFO) << "Receive result for GetExportedChatInviteQuery: " << to_string(result);

    td_->contacts_manager_->on_get_users(std::move(result->users_), "GetExportedChatInviteQuery");

    DialogInviteLink invite_link(std::move(result->invite_), "GetExportedChatInviteQuery");
    if (!invite_link.is_valid()) {
      LOG(ERROR) << "Receive invalid invite link in " << dialog_id_;
      return on_error(Status::Error(500, "Receive invalid invite link"));
    }
    promise_.set_value(invite_link.get_chat_invite_link_object(td_->contacts_manager_.get()));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetExportedChatInviteQuery");
    promise_.set_error(std::move(status));
  }
};

class GetExportedChatInvitesQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chatInviteLinks>> promise_;
  DialogId dialog_id_;

 public:
  explicit GetExportedChatInvitesQuery(Promise<td_api::object_ptr<td_api::chatInviteLinks>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, tl_object_ptr<telegram_api::InputUser> &&input_user, bool is_revoked, int32 offset_date,
            const string &offset_invite_link, int32 limit) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    int32 flags = 0;
    if (!offset_invite_link.empty() || offset_date != 0) {
      flags |= telegram_api::messages_getExportedChatInvites::OFFSET_DATE_MASK;
      flags |= telegram_api::messages_getExportedChatInvites::OFFSET_LINK_MASK;
    }
    if (is_revoked) {
      flags |= telegram_api::messages_getExportedChatInvites::REVOKED_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::messages_getExportedChatInvites(flags, false /*ignored*/, std::move(input_peer),
                                                      std::move(input_user), offset_date, offset_invite_link, limit)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_getExportedChatInvites>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetExportedChatInvitesQuery: " << to_string(result);

    td_->contacts_manager_->on_get_users(std::move(result->users_), "GetExportedChatInvitesQuery");

    int32 total_count = result->count_;
    if (total_count < static_cast<int32>(result->invites_.size())) {
      LOG(ERROR) << "Receive wrong total count of invite links " << total_count << " in " << dialog_id_;
      total_count = static_cast<int32>(result->invites_.size());
    }
    vector<td_api::object_ptr<td_api::chatInviteLink>> invite_links;
    for (auto &invite : result->invites_) {
      DialogInviteLink invite_link(std::move(invite), "GetExportedChatInvitesQuery");
      if (!invite_link.is_valid()) {
        LOG(ERROR) << "Receive invalid invite link in " << dialog_id_;
        total_count--;
        continue;
      }
      invite_links.push_back(invite_link.get_chat_invite_link_object(td_->contacts_manager_.get()));
    }
    promise_.set_value(td_api::make_object<td_api::chatInviteLinks>(total_count, std::move(invite_links)));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetExportedChatInvitesQuery");
    promise_.set_error(std::move(status));
  }
};

class GetChatAdminWithInvitesQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chatInviteLinkCounts>> promise_;
  DialogId dialog_id_;

 public:
  explicit GetChatAdminWithInvitesQuery(Promise<td_api::object_ptr<td_api::chatInviteLinkCounts>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    send_query(G()->net_query_creator().create(telegram_api::messages_getAdminsWithInvites(std::move(input_peer))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_getAdminsWithInvites>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetChatAdminWithInvitesQuery: " << to_string(result);

    td_->contacts_manager_->on_get_users(std::move(result->users_), "GetChatAdminWithInvitesQuery");

    vector<td_api::object_ptr<td_api::chatInviteLinkCount>> invite_link_counts;
    for (auto &admin : result->admins_) {
      UserId user_id(admin->admin_id_);
      if (!user_id.is_valid()) {
        LOG(ERROR) << "Receive invalid invite link creator " << user_id << " in " << dialog_id_;
        continue;
      }
      invite_link_counts.push_back(td_api::make_object<td_api::chatInviteLinkCount>(
          td_->contacts_manager_->get_user_id_object(user_id, "chatInviteLinkCount"), admin->invites_count_,
          admin->revoked_invites_count_));
    }
    promise_.set_value(td_api::make_object<td_api::chatInviteLinkCounts>(std::move(invite_link_counts)));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetChatAdminWithInvitesQuery");
    promise_.set_error(std::move(status));
  }
};

class GetChatInviteImportersQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chatInviteLinkMembers>> promise_;
  DialogId dialog_id_;

 public:
  explicit GetChatInviteImportersQuery(Promise<td_api::object_ptr<td_api::chatInviteLinkMembers>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &invite_link, int32 offset_date, UserId offset_user_id, int32 limit) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    auto r_input_user = td_->contacts_manager_->get_input_user(offset_user_id);
    if (r_input_user.is_error()) {
      r_input_user = make_tl_object<telegram_api::inputUserEmpty>();
    }

    int32 flags = telegram_api::messages_getChatInviteImporters::LINK_MASK;
    send_query(G()->net_query_creator().create(
        telegram_api::messages_getChatInviteImporters(flags, false /*ignored*/, std::move(input_peer), invite_link,
                                                      string(), offset_date, r_input_user.move_as_ok(), limit)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_getChatInviteImporters>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetChatInviteImportersQuery: " << to_string(result);

    td_->contacts_manager_->on_get_users(std::move(result->users_), "GetChatInviteImportersQuery");

    int32 total_count = result->count_;
    if (total_count < static_cast<int32>(result->importers_.size())) {
      LOG(ERROR) << "Receive wrong total count of invite link users " << total_count << " in " << dialog_id_;
      total_count = static_cast<int32>(result->importers_.size());
    }
    vector<td_api::object_ptr<td_api::chatInviteLinkMember>> invite_link_members;
    for (auto &importer : result->importers_) {
      UserId user_id(importer->user_id_);
      UserId approver_user_id(importer->approved_by_);
      if (!user_id.is_valid() || (!approver_user_id.is_valid() && approver_user_id != UserId()) ||
          importer->requested_) {
        LOG(ERROR) << "Receive invalid invite link importer: " << to_string(importer);
        total_count--;
        continue;
      }
      invite_link_members.push_back(td_api::make_object<td_api::chatInviteLinkMember>(
          td_->contacts_manager_->get_user_id_object(user_id, "chatInviteLinkMember"), importer->date_,
          td_->contacts_manager_->get_user_id_object(approver_user_id, "chatInviteLinkMember")));
    }
    promise_.set_value(td_api::make_object<td_api::chatInviteLinkMembers>(total_count, std::move(invite_link_members)));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetChatInviteImportersQuery");
    promise_.set_error(std::move(status));
  }
};

class GetChatJoinRequestsQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chatJoinRequests>> promise_;
  DialogId dialog_id_;
  bool is_full_list_ = false;

 public:
  explicit GetChatJoinRequestsQuery(Promise<td_api::object_ptr<td_api::chatJoinRequests>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &invite_link, const string &query, int32 offset_date,
            UserId offset_user_id, int32 limit) {
    dialog_id_ = dialog_id;
    is_full_list_ =
        invite_link.empty() && query.empty() && offset_date == 0 && !offset_user_id.is_valid() && limit >= 3;

    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    auto r_input_user = td_->contacts_manager_->get_input_user(offset_user_id);
    if (r_input_user.is_error()) {
      r_input_user = make_tl_object<telegram_api::inputUserEmpty>();
    }

    int32 flags = telegram_api::messages_getChatInviteImporters::REQUESTED_MASK;
    if (!invite_link.empty()) {
      flags |= telegram_api::messages_getChatInviteImporters::LINK_MASK;
    }
    if (!query.empty()) {
      flags |= telegram_api::messages_getChatInviteImporters::Q_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::messages_getChatInviteImporters(flags, false /*ignored*/, std::move(input_peer), invite_link,
                                                      query, offset_date, r_input_user.move_as_ok(), limit)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_getChatInviteImporters>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetChatJoinRequestsQuery: " << to_string(result);

    td_->contacts_manager_->on_get_users(std::move(result->users_), "GetChatJoinRequestsQuery");

    int32 total_count = result->count_;
    if (total_count < static_cast<int32>(result->importers_.size())) {
      LOG(ERROR) << "Receive wrong total count of join requests " << total_count << " in " << dialog_id_;
      total_count = static_cast<int32>(result->importers_.size());
    }
    vector<td_api::object_ptr<td_api::chatJoinRequest>> join_requests;
    vector<int64> recent_requesters;
    for (auto &request : result->importers_) {
      UserId user_id(request->user_id_);
      UserId approver_user_id(request->approved_by_);
      if (!user_id.is_valid() || approver_user_id.is_valid() || !request->requested_) {
        LOG(ERROR) << "Receive invalid join request: " << to_string(request);
        total_count--;
        continue;
      }
      if (recent_requesters.size() < 3) {
        recent_requesters.push_back(user_id.get());
      }
      join_requests.push_back(td_api::make_object<td_api::chatJoinRequest>(
          td_->contacts_manager_->get_user_id_object(user_id, "chatJoinRequest"), request->date_, request->about_));
    }
    if (is_full_list_) {
      td_->messages_manager_->on_update_dialog_pending_join_requests(dialog_id_, total_count,
                                                                     std::move(recent_requesters));
    }
    promise_.set_value(td_api::make_object<td_api::chatJoinRequests>(total_count, std::move(join_requests)));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetChatJoinRequestsQuery");
    promise_.set_error(std::move(status));
  }
};

class HideChatJoinRequestQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit HideChatJoinRequestQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, UserId user_id, bool approve) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    auto r_input_user = td_->contacts_manager_->get_input_user(user_id);
    if (r_input_user.is_error()) {
      return on_error(r_input_user.move_as_error());
    }

    int32 flags = 0;
    if (approve) {
      flags |= telegram_api::messages_hideChatJoinRequest::APPROVED_MASK;
    }
    send_query(G()->net_query_creator().create(telegram_api::messages_hideChatJoinRequest(
        flags, false /*ignored*/, std::move(input_peer), r_input_user.move_as_ok())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_hideChatJoinRequest>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for HideChatJoinRequestQuery: " << to_string(result);
    td_->updates_manager_->on_get_updates(std::move(result), std::move(promise_));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "HideChatJoinRequestQuery");
    promise_.set_error(std::move(status));
  }
};

class HideAllChatJoinRequestsQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit HideAllChatJoinRequestsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &invite_link, bool approve) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    int32 flags = 0;
    if (approve) {
      flags |= telegram_api::messages_hideAllChatJoinRequests::APPROVED_MASK;
    }
    if (!invite_link.empty()) {
      flags |= telegram_api::messages_hideAllChatJoinRequests::LINK_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::messages_hideAllChatJoinRequests(flags, false /*ignored*/, std::move(input_peer), invite_link)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_hideAllChatJoinRequests>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for HideAllChatJoinRequestsQuery: " << to_string(result);
    td_->updates_manager_->on_get_updates(std::move(result), std::move(promise_));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "HideAllChatJoinRequestsQuery");
    promise_.set_error(std::move(status));
  }
};

class RevokeChatInviteLinkQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::chatInviteLinks>> promise_;
  DialogId dialog_id_;

 public:
  explicit RevokeChatInviteLinkQuery(Promise<td_api::object_ptr<td_api::chatInviteLinks>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &invite_link) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    int32 flags = telegram_api::messages_editExportedChatInvite::REVOKED_MASK;
    send_query(G()->net_query_creator().create(telegram_api::messages_editExportedChatInvite(
        flags, false /*ignored*/, std::move(input_peer), invite_link, 0, 0, false, string())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_editExportedChatInvite>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for RevokeChatInviteLinkQuery: " << to_string(result);

    vector<td_api::object_ptr<td_api::chatInviteLink>> links;
    switch (result->get_id()) {
      case telegram_api::messages_exportedChatInvite::ID: {
        auto invite = move_tl_object_as<telegram_api::messages_exportedChatInvite>(result);

        td_->contacts_manager_->on_get_users(std::move(invite->users_), "RevokeChatInviteLinkQuery");

        DialogInviteLink invite_link(std::move(invite->invite_), "RevokeChatInviteLinkQuery");
        if (!invite_link.is_valid()) {
          return on_error(Status::Error(500, "Receive invalid invite link"));
        }
        links.push_back(invite_link.get_chat_invite_link_object(td_->contacts_manager_.get()));
        break;
      }
      case telegram_api::messages_exportedChatInviteReplaced::ID: {
        auto invite = move_tl_object_as<telegram_api::messages_exportedChatInviteReplaced>(result);

        td_->contacts_manager_->on_get_users(std::move(invite->users_), "RevokeChatInviteLinkQuery replaced");

        DialogInviteLink invite_link(std::move(invite->invite_), "RevokeChatInviteLinkQuery replaced");
        DialogInviteLink new_invite_link(std::move(invite->new_invite_), "RevokeChatInviteLinkQuery new replaced");
        if (!invite_link.is_valid() || !new_invite_link.is_valid()) {
          return on_error(Status::Error(500, "Receive invalid invite link"));
        }
        if (new_invite_link.get_creator_user_id() == td_->contacts_manager_->get_my_id() &&
            new_invite_link.is_permanent()) {
          td_->contacts_manager_->on_get_permanent_dialog_invite_link(dialog_id_, new_invite_link);
        }
        links.push_back(invite_link.get_chat_invite_link_object(td_->contacts_manager_.get()));
        links.push_back(new_invite_link.get_chat_invite_link_object(td_->contacts_manager_.get()));
        break;
      }
      default:
        UNREACHABLE();
    }
    auto total_count = static_cast<int32>(links.size());
    promise_.set_value(td_api::make_object<td_api::chatInviteLinks>(total_count, std::move(links)));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "RevokeChatInviteLinkQuery");
    promise_.set_error(std::move(status));
  }
};

class DeleteExportedChatInviteQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit DeleteExportedChatInviteQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, const string &invite_link) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    send_query(G()->net_query_creator().create(
        telegram_api::messages_deleteExportedChatInvite(std::move(input_peer), invite_link)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_deleteExportedChatInvite>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "DeleteExportedChatInviteQuery");
    promise_.set_error(std::move(status));
  }
};

class DeleteRevokedExportedChatInvitesQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit DeleteRevokedExportedChatInvitesQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, tl_object_ptr<telegram_api::InputUser> &&input_user) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Write);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    send_query(G()->net_query_creator().create(
        telegram_api::messages_deleteRevokedExportedChatInvites(std::move(input_peer), std::move(input_user))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_deleteRevokedExportedChatInvites>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "DeleteRevokedExportedChatInvitesQuery");
    promise_.set_error(std::move(status));
  }
};

class CheckChatInviteQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  string invite_link_;

 public:
  explicit CheckChatInviteQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(const string &invite_link) {
    invite_link_ = invite_link;
    send_query(G()->net_query_creator().create(
        telegram_api::messages_checkChatInvite(LinkManager::get_dialog_invite_link_hash(invite_link_))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_checkChatInvite>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for CheckChatInviteQuery: " << to_string(ptr);

    td_->contacts_manager_->on_get_dialog_invite_link_info(invite_link_, std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class ImportChatInviteQuery final : public Td::ResultHandler {
  Promise<DialogId> promise_;

  string invite_link_;

 public:
  explicit ImportChatInviteQuery(Promise<DialogId> &&promise) : promise_(std::move(promise)) {
  }

  void send(const string &invite_link) {
    invite_link_ = invite_link;
    send_query(G()->net_query_creator().create(
        telegram_api::messages_importChatInvite(LinkManager::get_dialog_invite_link_hash(invite_link_))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_importChatInvite>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for ImportChatInviteQuery: " << to_string(ptr);

    auto dialog_ids = UpdatesManager::get_chat_dialog_ids(ptr.get());
    if (dialog_ids.size() != 1u) {
      LOG(ERROR) << "Receive wrong result for ImportChatInviteQuery: " << to_string(ptr);
      return on_error(Status::Error(500, "Internal Server Error: failed to join chat via invite link"));
    }
    auto dialog_id = dialog_ids[0];

    td_->contacts_manager_->invalidate_invite_link_info(invite_link_);
    td_->updates_manager_->on_get_updates(
        std::move(ptr), PromiseCreator::lambda([promise = std::move(promise_), dialog_id](Unit) mutable {
          promise.set_value(std::move(dialog_id));
        }));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->invalidate_invite_link_info(invite_link_);
    promise_.set_error(std::move(status));
  }
};

class DeleteChatUserQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit DeleteChatUserQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChatId chat_id, tl_object_ptr<telegram_api::InputUser> &&input_user, bool revoke_messages) {
    int32 flags = 0;
    if (revoke_messages) {
      flags |= telegram_api::messages_deleteChatUser::REVOKE_HISTORY_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::messages_deleteChatUser(flags, false /*ignored*/, chat_id.get(), std::move(input_user))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_deleteChatUser>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for DeleteChatUserQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
    td_->updates_manager_->get_difference("DeleteChatUserQuery");
  }
};

class JoinChannelQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit JoinChannelQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id) {
    channel_id_ = channel_id;
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(telegram_api::channels_joinChannel(std::move(input_channel))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_joinChannel>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for JoinChannelQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "JoinChannelQuery");
    promise_.set_error(std::move(status));
    td_->updates_manager_->get_difference("JoinChannelQuery");
  }
};

class InviteToChannelQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit InviteToChannelQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, vector<tl_object_ptr<telegram_api::InputUser>> &&input_users) {
    channel_id_ = channel_id;
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(
        telegram_api::channels_inviteToChannel(std::move(input_channel), std::move(input_users))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_inviteToChannel>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for InviteToChannelQuery: " << to_string(ptr);
    td_->contacts_manager_->invalidate_channel_full(channel_id_, false);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "InviteToChannelQuery");
    promise_.set_error(std::move(status));
    td_->updates_manager_->get_difference("InviteToChannelQuery");
  }
};

class EditChannelAdminQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  UserId user_id_;
  DialogParticipantStatus status_ = DialogParticipantStatus::Left();

 public:
  explicit EditChannelAdminQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, UserId user_id, tl_object_ptr<telegram_api::InputUser> &&input_user,
            const DialogParticipantStatus &status) {
    channel_id_ = channel_id;
    user_id_ = user_id;
    status_ = status;
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(telegram_api::channels_editAdmin(
        std::move(input_channel), std::move(input_user), status.get_chat_admin_rights(), status.get_rank())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_editAdmin>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for EditChannelAdminQuery: " << to_string(ptr);
    td_->contacts_manager_->invalidate_channel_full(channel_id_, false);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
    td_->contacts_manager_->on_set_channel_participant_status(channel_id_, DialogId(user_id_), status_);
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "EditChannelAdminQuery");
    promise_.set_error(std::move(status));
    td_->updates_manager_->get_difference("EditChannelAdminQuery");
  }
};

class EditChannelBannedQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;
  DialogId participant_dialog_id_;
  DialogParticipantStatus status_ = DialogParticipantStatus::Left();

 public:
  explicit EditChannelBannedQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, DialogId participant_dialog_id, tl_object_ptr<telegram_api::InputPeer> &&input_peer,
            const DialogParticipantStatus &status) {
    channel_id_ = channel_id;
    participant_dialog_id_ = participant_dialog_id;
    status_ = status;
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(telegram_api::channels_editBanned(
        std::move(input_channel), std::move(input_peer), status.get_chat_banned_rights())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_editBanned>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for EditChannelBannedQuery: " << to_string(ptr);
    td_->contacts_manager_->invalidate_channel_full(channel_id_, false);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
    td_->contacts_manager_->on_set_channel_participant_status(channel_id_, participant_dialog_id_, status_);
  }

  void on_error(Status status) final {
    if (participant_dialog_id_.get_type() != DialogType::Channel) {
      td_->contacts_manager_->on_get_channel_error(channel_id_, status, "EditChannelBannedQuery");
    }
    promise_.set_error(std::move(status));
    td_->updates_manager_->get_difference("EditChannelBannedQuery");
  }
};

class LeaveChannelQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit LeaveChannelQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id) {
    channel_id_ = channel_id;
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);
    send_query(G()->net_query_creator().create(telegram_api::channels_leaveChannel(std::move(input_channel))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_leaveChannel>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for LeaveChannelQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "LeaveChannelQuery");
    promise_.set_error(std::move(status));
    td_->updates_manager_->get_difference("LeaveChannelQuery");
  }
};

class CanEditChannelCreatorQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit CanEditChannelCreatorQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send() {
    auto r_input_user = td_->contacts_manager_->get_input_user(td_->contacts_manager_->get_my_id());
    CHECK(r_input_user.is_ok());
    send_query(G()->net_query_creator().create(telegram_api::channels_editCreator(
        telegram_api::make_object<telegram_api::inputChannelEmpty>(), r_input_user.move_as_ok(),
        make_tl_object<telegram_api::inputCheckPasswordEmpty>())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_editCreator>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(ERROR) << "Receive result for CanEditChannelCreatorQuery: " << to_string(ptr);
    promise_.set_error(Status::Error(500, "Server doesn't returned error"));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class EditChannelCreatorQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit EditChannelCreatorQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, UserId user_id,
            tl_object_ptr<telegram_api::InputCheckPasswordSRP> input_check_password) {
    channel_id_ = channel_id;
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    if (input_channel == nullptr) {
      return promise_.set_error(Status::Error(400, "Have no access to the chat"));
    }
    auto r_input_user = td_->contacts_manager_->get_input_user(user_id);
    if (r_input_user.is_error()) {
      return promise_.set_error(r_input_user.move_as_error());
    }
    send_query(G()->net_query_creator().create(telegram_api::channels_editCreator(
        std::move(input_channel), r_input_user.move_as_ok(), std::move(input_check_password))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_editCreator>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for EditChannelCreatorQuery: " << to_string(ptr);
    td_->contacts_manager_->invalidate_channel_full(channel_id_, false);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "EditChannelCreatorQuery");
    promise_.set_error(std::move(status));
    td_->updates_manager_->get_difference("EditChannelCreatorQuery");
  }
};

class MigrateChatQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit MigrateChatQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChatId chat_id) {
    send_query(G()->net_query_creator().create(telegram_api::messages_migrateChat(chat_id.get())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_migrateChat>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for MigrateChatQuery: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
    td_->updates_manager_->get_difference("MigrateChatQuery");
  }
};

class GetCreatedPublicChannelsQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  PublicDialogType type_;

 public:
  explicit GetCreatedPublicChannelsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(PublicDialogType type, bool check_limit) {
    type_ = type;
    int32 flags = 0;
    if (type_ == PublicDialogType::IsLocationBased) {
      flags |= telegram_api::channels_getAdminedPublicChannels::BY_LOCATION_MASK;
    }
    if (check_limit) {
      flags |= telegram_api::channels_getAdminedPublicChannels::CHECK_LIMIT_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::channels_getAdminedPublicChannels(flags, false /*ignored*/, false /*ignored*/)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_getAdminedPublicChannels>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto chats_ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetCreatedPublicChannelsQuery: " << to_string(chats_ptr);
    int32 constructor_id = chats_ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_chats::ID: {
        auto chats = move_tl_object_as<telegram_api::messages_chats>(chats_ptr);
        td_->contacts_manager_->on_get_created_public_channels(type_, std::move(chats->chats_));
        break;
      }
      case telegram_api::messages_chatsSlice::ID: {
        auto chats = move_tl_object_as<telegram_api::messages_chatsSlice>(chats_ptr);
        LOG(ERROR) << "Receive chatsSlice in result of GetCreatedPublicChannelsQuery";
        td_->contacts_manager_->on_get_created_public_channels(type_, std::move(chats->chats_));
        break;
      }
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetGroupsForDiscussionQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit GetGroupsForDiscussionQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send() {
    send_query(G()->net_query_creator().create(telegram_api::channels_getGroupsForDiscussion()));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_getGroupsForDiscussion>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto chats_ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetGroupsForDiscussionQuery: " << to_string(chats_ptr);
    int32 constructor_id = chats_ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_chats::ID: {
        auto chats = move_tl_object_as<telegram_api::messages_chats>(chats_ptr);
        td_->contacts_manager_->on_get_dialogs_for_discussion(std::move(chats->chats_));
        break;
      }
      case telegram_api::messages_chatsSlice::ID: {
        auto chats = move_tl_object_as<telegram_api::messages_chatsSlice>(chats_ptr);
        LOG(ERROR) << "Receive chatsSlice in result of GetGroupsForDiscussionQuery";
        td_->contacts_manager_->on_get_dialogs_for_discussion(std::move(chats->chats_));
        break;
      }
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetInactiveChannelsQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit GetInactiveChannelsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send() {
    send_query(G()->net_query_creator().create(telegram_api::channels_getInactiveChannels()));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_getInactiveChannels>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetInactiveChannelsQuery: " << to_string(result);
    // don't need to use result->dates_, because chat.last_message.date is more reliable
    td_->contacts_manager_->on_get_users(std::move(result->users_), "GetInactiveChannelsQuery");
    td_->contacts_manager_->on_get_inactive_channels(std::move(result->chats_), std::move(promise_));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetUsersQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit GetUsersQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(vector<tl_object_ptr<telegram_api::InputUser>> &&input_users) {
    send_query(G()->net_query_creator().create(telegram_api::users_getUsers(std::move(input_users))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::users_getUsers>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    td_->contacts_manager_->on_get_users(result_ptr.move_as_ok(), "GetUsersQuery");

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetFullUserQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit GetFullUserQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(tl_object_ptr<telegram_api::InputUser> &&input_user) {
    send_query(G()->net_query_creator().create(telegram_api::users_getFullUser(std::move(input_user))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::users_getFullUser>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(DEBUG) << "Receive result for GetFullUserQuery: " << to_string(ptr);
    td_->contacts_manager_->on_get_users(std::move(ptr->users_), "GetFullUserQuery");
    td_->contacts_manager_->on_get_chats(std::move(ptr->chats_), "GetFullUserQuery");
    td_->contacts_manager_->on_get_user_full(std::move(ptr->full_user_));
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetUserPhotosQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  UserId user_id_;
  int32 offset_;
  int32 limit_;

 public:
  explicit GetUserPhotosQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(UserId user_id, tl_object_ptr<telegram_api::InputUser> &&input_user, int32 offset, int32 limit,
            int64 photo_id) {
    user_id_ = user_id;
    offset_ = offset;
    limit_ = limit;
    send_query(G()->net_query_creator().create(
        telegram_api::photos_getUserPhotos(std::move(input_user), offset, photo_id, limit)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::photos_getUserPhotos>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();

    LOG(INFO) << "Receive result for GetUserPhotosQuery: " << to_string(ptr);
    int32 constructor_id = ptr->get_id();
    if (constructor_id == telegram_api::photos_photos::ID) {
      auto photos = move_tl_object_as<telegram_api::photos_photos>(ptr);

      td_->contacts_manager_->on_get_users(std::move(photos->users_), "GetUserPhotosQuery");
      auto photos_size = narrow_cast<int32>(photos->photos_.size());
      td_->contacts_manager_->on_get_user_photos(user_id_, offset_, limit_, photos_size, std::move(photos->photos_));
    } else {
      CHECK(constructor_id == telegram_api::photos_photosSlice::ID);
      auto photos = move_tl_object_as<telegram_api::photos_photosSlice>(ptr);

      td_->contacts_manager_->on_get_users(std::move(photos->users_), "GetUserPhotosQuery slice");
      td_->contacts_manager_->on_get_user_photos(user_id_, offset_, limit_, photos->count_, std::move(photos->photos_));
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetChatsQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit GetChatsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(vector<int64> &&chat_ids) {
    send_query(G()->net_query_creator().create(telegram_api::messages_getChats(std::move(chat_ids))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_getChats>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto chats_ptr = result_ptr.move_as_ok();
    int32 constructor_id = chats_ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_chats::ID: {
        auto chats = move_tl_object_as<telegram_api::messages_chats>(chats_ptr);
        td_->contacts_manager_->on_get_chats(std::move(chats->chats_), "GetChatsQuery");
        break;
      }
      case telegram_api::messages_chatsSlice::ID: {
        auto chats = move_tl_object_as<telegram_api::messages_chatsSlice>(chats_ptr);
        LOG(ERROR) << "Receive chatsSlice in result of GetChatsQuery";
        td_->contacts_manager_->on_get_chats(std::move(chats->chats_), "GetChatsQuery slice");
        break;
      }
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetFullChatQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChatId chat_id_;

 public:
  explicit GetFullChatQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChatId chat_id) {
    send_query(G()->net_query_creator().create(telegram_api::messages_getFullChat(chat_id.get())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_getFullChat>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    td_->contacts_manager_->on_get_users(std::move(ptr->users_), "GetFullChatQuery");
    td_->contacts_manager_->on_get_chats(std::move(ptr->chats_), "GetFullChatQuery");
    td_->contacts_manager_->on_get_chat_full(std::move(ptr->full_chat_), std::move(promise_));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_chat_full_failed(chat_id_);
    promise_.set_error(std::move(status));
  }
};

class GetChannelsQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit GetChannelsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(tl_object_ptr<telegram_api::InputChannel> &&input_channel) {
    CHECK(input_channel != nullptr);
    if (input_channel->get_id() == telegram_api::inputChannel::ID) {
      channel_id_ = ChannelId(static_cast<const telegram_api::inputChannel *>(input_channel.get())->channel_id_);
    }

    vector<tl_object_ptr<telegram_api::InputChannel>> input_channels;
    input_channels.push_back(std::move(input_channel));
    send_query(G()->net_query_creator().create(telegram_api::channels_getChannels(std::move(input_channels))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_getChannels>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    //    LOG(INFO) << "Receive result for GetChannelsQuery: " << to_string(result_ptr.ok());
    auto chats_ptr = result_ptr.move_as_ok();
    int32 constructor_id = chats_ptr->get_id();
    switch (constructor_id) {
      case telegram_api::messages_chats::ID: {
        auto chats = move_tl_object_as<telegram_api::messages_chats>(chats_ptr);
        td_->contacts_manager_->on_get_chats(std::move(chats->chats_), "GetChannelsQuery");
        break;
      }
      case telegram_api::messages_chatsSlice::ID: {
        auto chats = move_tl_object_as<telegram_api::messages_chatsSlice>(chats_ptr);
        LOG(ERROR) << "Receive chatsSlice in result of GetChannelsQuery";
        td_->contacts_manager_->on_get_chats(std::move(chats->chats_), "GetChannelsQuery slice");
        break;
      }
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "GetChannelsQuery");
    promise_.set_error(std::move(status));
  }
};

class GetFullChannelQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit GetFullChannelQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, tl_object_ptr<telegram_api::InputChannel> &&input_channel) {
    channel_id_ = channel_id;
    send_query(G()->net_query_creator().create(telegram_api::channels_getFullChannel(std::move(input_channel))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_getFullChannel>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    td_->contacts_manager_->on_get_users(std::move(ptr->users_), "GetFullChannelQuery");
    td_->contacts_manager_->on_get_chats(std::move(ptr->chats_), "GetFullChannelQuery");
    td_->contacts_manager_->on_get_chat_full(std::move(ptr->full_chat_), std::move(promise_));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "GetFullChannelQuery");
    td_->contacts_manager_->on_get_channel_full_failed(channel_id_);
    promise_.set_error(std::move(status));
  }
};

class GetChannelParticipantQuery final : public Td::ResultHandler {
  Promise<DialogParticipant> promise_;
  ChannelId channel_id_;
  DialogId participant_dialog_id_;

 public:
  explicit GetChannelParticipantQuery(Promise<DialogParticipant> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, DialogId participant_dialog_id, tl_object_ptr<telegram_api::InputPeer> &&input_peer) {
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    if (input_channel == nullptr) {
      return promise_.set_error(Status::Error(400, "Supergroup not found"));
    }

    CHECK(input_peer != nullptr);

    channel_id_ = channel_id;
    participant_dialog_id_ = participant_dialog_id;
    send_query(G()->net_query_creator().create(
        telegram_api::channels_getParticipant(std::move(input_channel), std::move(input_peer))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_getParticipant>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto participant = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetChannelParticipantQuery: " << to_string(participant);

    td_->contacts_manager_->on_get_users(std::move(participant->users_), "GetChannelParticipantQuery");
    td_->contacts_manager_->on_get_chats(std::move(participant->chats_), "GetChannelParticipantQuery");
    DialogParticipant result(std::move(participant->participant_),
                             td_->contacts_manager_->get_channel_type(channel_id_));
    if (!result.is_valid()) {
      LOG(ERROR) << "Receive invalid " << result;
      return promise_.set_error(Status::Error(500, "Receive invalid chat member"));
    }
    promise_.set_value(std::move(result));
  }

  void on_error(Status status) final {
    if (status.message() == "USER_NOT_PARTICIPANT") {
      promise_.set_value(DialogParticipant::left(participant_dialog_id_));
      return;
    }

    if (participant_dialog_id_.get_type() != DialogType::Channel) {
      td_->contacts_manager_->on_get_channel_error(channel_id_, status, "GetChannelParticipantQuery");
    }
    promise_.set_error(std::move(status));
  }
};

class GetChannelParticipantsQuery final : public Td::ResultHandler {
  Promise<tl_object_ptr<telegram_api::channels_channelParticipants>> promise_;
  ChannelId channel_id_;

 public:
  explicit GetChannelParticipantsQuery(Promise<tl_object_ptr<telegram_api::channels_channelParticipants>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, const ChannelParticipantFilter &filter, int32 offset, int32 limit) {
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    if (input_channel == nullptr) {
      return promise_.set_error(Status::Error(400, "Supergroup not found"));
    }

    channel_id_ = channel_id;
    send_query(G()->net_query_creator().create(telegram_api::channels_getParticipants(
        std::move(input_channel), filter.get_input_channel_participants_filter(), offset, limit, 0)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_getParticipants>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto participants_ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetChannelParticipantsQuery: " << to_string(participants_ptr);
    switch (participants_ptr->get_id()) {
      case telegram_api::channels_channelParticipants::ID: {
        promise_.set_value(telegram_api::move_object_as<telegram_api::channels_channelParticipants>(participants_ptr));
        break;
      }
      case telegram_api::channels_channelParticipantsNotModified::ID:
        LOG(ERROR) << "Receive channelParticipantsNotModified";
        return on_error(Status::Error(500, "Receive channelParticipantsNotModified"));
      default:
        UNREACHABLE();
    }
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "GetChannelParticipantsQuery");
    promise_.set_error(std::move(status));
  }
};

class GetChannelAdministratorsQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;
  ChannelId channel_id_;

 public:
  explicit GetChannelAdministratorsQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, int64 hash) {
    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    if (input_channel == nullptr) {
      return promise_.set_error(Status::Error(400, "Supergroup not found"));
    }

    hash = 0;  // to load even only ranks or creator changed

    channel_id_ = channel_id;
    send_query(G()->net_query_creator().create(telegram_api::channels_getParticipants(
        std::move(input_channel), telegram_api::make_object<telegram_api::channelParticipantsAdmins>(), 0,
        std::numeric_limits<int32>::max(), hash)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::channels_getParticipants>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto participants_ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetChannelAdministratorsQuery: " << to_string(participants_ptr);
    switch (participants_ptr->get_id()) {
      case telegram_api::channels_channelParticipants::ID: {
        auto participants = telegram_api::move_object_as<telegram_api::channels_channelParticipants>(participants_ptr);
        td_->contacts_manager_->on_get_users(std::move(participants->users_), "GetChannelAdministratorsQuery");
        td_->contacts_manager_->on_get_chats(std::move(participants->chats_), "GetChannelAdministratorsQuery");

        auto channel_type = td_->contacts_manager_->get_channel_type(channel_id_);
        vector<DialogAdministrator> administrators;
        administrators.reserve(participants->participants_.size());
        for (auto &participant : participants->participants_) {
          DialogParticipant dialog_participant(std::move(participant), channel_type);
          if (!dialog_participant.is_valid() || !dialog_participant.status_.is_administrator() ||
              dialog_participant.dialog_id_.get_type() != DialogType::User) {
            LOG(ERROR) << "Receive " << dialog_participant << " as an administrator of " << channel_id_;
            continue;
          }
          administrators.emplace_back(dialog_participant.dialog_id_.get_user_id(),
                                      dialog_participant.status_.get_rank(), dialog_participant.status_.is_creator());
        }

        td_->contacts_manager_->on_update_channel_administrator_count(channel_id_,
                                                                      narrow_cast<int32>(administrators.size()));
        td_->contacts_manager_->on_update_dialog_administrators(DialogId(channel_id_), std::move(administrators), true,
                                                                false);

        break;
      }
      case telegram_api::channels_channelParticipantsNotModified::ID:
        break;
      default:
        UNREACHABLE();
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "GetChannelAdministratorsQuery");
    promise_.set_error(std::move(status));
  }
};

class GetSupportUserQuery final : public Td::ResultHandler {
  Promise<UserId> promise_;

 public:
  explicit GetSupportUserQuery(Promise<UserId> &&promise) : promise_(std::move(promise)) {
  }

  void send() {
    send_query(G()->net_query_creator().create(telegram_api::help_getSupport()));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::help_getSupport>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetSupportUserQuery: " << to_string(ptr);

    auto user_id = ContactsManager::get_user_id(ptr->user_);
    td_->contacts_manager_->on_get_user(std::move(ptr->user_), "GetSupportUserQuery", false);

    promise_.set_value(std::move(user_id));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

tl_object_ptr<td_api::dateRange> ContactsManager::convert_date_range(
    const tl_object_ptr<telegram_api::statsDateRangeDays> &obj) {
  return make_tl_object<td_api::dateRange>(obj->min_date_, obj->max_date_);
}

tl_object_ptr<td_api::StatisticalGraph> ContactsManager::convert_stats_graph(
    tl_object_ptr<telegram_api::StatsGraph> obj) {
  CHECK(obj != nullptr);

  switch (obj->get_id()) {
    case telegram_api::statsGraphAsync::ID: {
      auto graph = move_tl_object_as<telegram_api::statsGraphAsync>(obj);
      return make_tl_object<td_api::statisticalGraphAsync>(std::move(graph->token_));
    }
    case telegram_api::statsGraphError::ID: {
      auto graph = move_tl_object_as<telegram_api::statsGraphError>(obj);
      return make_tl_object<td_api::statisticalGraphError>(std::move(graph->error_));
    }
    case telegram_api::statsGraph::ID: {
      auto graph = move_tl_object_as<telegram_api::statsGraph>(obj);
      return make_tl_object<td_api::statisticalGraphData>(std::move(graph->json_->data_),
                                                          std::move(graph->zoom_token_));
    }
    default:
      UNREACHABLE();
      return nullptr;
  }
}

double ContactsManager::get_percentage_value(double part, double total) {
  if (total < 1e-6 && total > -1e-6) {
    if (part < 1e-6 && part > -1e-6) {
      return 0.0;
    }
    return 100.0;
  }
  if (part > 1e20) {
    return 100.0;
  }
  return part / total * 100;
}

tl_object_ptr<td_api::statisticalValue> ContactsManager::convert_stats_absolute_value(
    const tl_object_ptr<telegram_api::statsAbsValueAndPrev> &obj) {
  return make_tl_object<td_api::statisticalValue>(obj->current_, obj->previous_,
                                                  get_percentage_value(obj->current_ - obj->previous_, obj->previous_));
}

tl_object_ptr<td_api::chatStatisticsSupergroup> ContactsManager::convert_megagroup_stats(
    tl_object_ptr<telegram_api::stats_megagroupStats> obj) {
  CHECK(obj != nullptr);

  on_get_users(std::move(obj->users_), "convert_megagroup_stats");

  // just in case
  td::remove_if(obj->top_posters_, [](auto &obj) {
    return !UserId(obj->user_id_).is_valid() || obj->messages_ < 0 || obj->avg_chars_ < 0;
  });
  td::remove_if(obj->top_admins_, [](auto &obj) {
    return !UserId(obj->user_id_).is_valid() || obj->deleted_ < 0 || obj->kicked_ < 0 || obj->banned_ < 0;
  });
  td::remove_if(obj->top_inviters_,
                [](auto &obj) { return !UserId(obj->user_id_).is_valid() || obj->invitations_ < 0; });

  auto top_senders =
      transform(std::move(obj->top_posters_), [this](tl_object_ptr<telegram_api::statsGroupTopPoster> &&top_poster) {
        return td_api::make_object<td_api::chatStatisticsMessageSenderInfo>(
            get_user_id_object(UserId(top_poster->user_id_), "get_top_senders"), top_poster->messages_,
            top_poster->avg_chars_);
      });
  auto top_administrators =
      transform(std::move(obj->top_admins_), [this](tl_object_ptr<telegram_api::statsGroupTopAdmin> &&top_admin) {
        return td_api::make_object<td_api::chatStatisticsAdministratorActionsInfo>(
            get_user_id_object(UserId(top_admin->user_id_), "get_top_administrators"), top_admin->deleted_,
            top_admin->kicked_, top_admin->banned_);
      });
  auto top_inviters =
      transform(std::move(obj->top_inviters_), [this](tl_object_ptr<telegram_api::statsGroupTopInviter> &&top_inviter) {
        return td_api::make_object<td_api::chatStatisticsInviterInfo>(
            get_user_id_object(UserId(top_inviter->user_id_), "get_top_inviters"), top_inviter->invitations_);
      });

  return make_tl_object<td_api::chatStatisticsSupergroup>(
      convert_date_range(obj->period_), convert_stats_absolute_value(obj->members_),
      convert_stats_absolute_value(obj->messages_), convert_stats_absolute_value(obj->viewers_),
      convert_stats_absolute_value(obj->posters_), convert_stats_graph(std::move(obj->growth_graph_)),
      convert_stats_graph(std::move(obj->members_graph_)),
      convert_stats_graph(std::move(obj->new_members_by_source_graph_)),
      convert_stats_graph(std::move(obj->languages_graph_)), convert_stats_graph(std::move(obj->messages_graph_)),
      convert_stats_graph(std::move(obj->actions_graph_)), convert_stats_graph(std::move(obj->top_hours_graph_)),
      convert_stats_graph(std::move(obj->weekdays_graph_)), std::move(top_senders), std::move(top_administrators),
      std::move(top_inviters));
}

tl_object_ptr<td_api::chatStatisticsChannel> ContactsManager::convert_broadcast_stats(
    tl_object_ptr<telegram_api::stats_broadcastStats> obj) {
  CHECK(obj != nullptr);

  auto recent_message_interactions = transform(std::move(obj->recent_message_interactions_), [](auto &&interaction) {
    return make_tl_object<td_api::chatStatisticsMessageInteractionInfo>(
        MessageId(ServerMessageId(interaction->msg_id_)).get(), interaction->views_, interaction->forwards_);
  });

  return make_tl_object<td_api::chatStatisticsChannel>(
      convert_date_range(obj->period_), convert_stats_absolute_value(obj->followers_),
      convert_stats_absolute_value(obj->views_per_post_), convert_stats_absolute_value(obj->shares_per_post_),
      get_percentage_value(obj->enabled_notifications_->part_, obj->enabled_notifications_->total_),
      convert_stats_graph(std::move(obj->growth_graph_)), convert_stats_graph(std::move(obj->followers_graph_)),
      convert_stats_graph(std::move(obj->mute_graph_)), convert_stats_graph(std::move(obj->top_hours_graph_)),
      convert_stats_graph(std::move(obj->views_by_source_graph_)),
      convert_stats_graph(std::move(obj->new_followers_by_source_graph_)),
      convert_stats_graph(std::move(obj->languages_graph_)), convert_stats_graph(std::move(obj->interactions_graph_)),
      convert_stats_graph(std::move(obj->iv_interactions_graph_)), std::move(recent_message_interactions));
}

class GetMegagroupStatsQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::ChatStatistics>> promise_;
  ChannelId channel_id_;

 public:
  explicit GetMegagroupStatsQuery(Promise<td_api::object_ptr<td_api::ChatStatistics>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, bool is_dark, DcId dc_id) {
    channel_id_ = channel_id;

    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);

    int32 flags = 0;
    if (is_dark) {
      flags |= telegram_api::stats_getMegagroupStats::DARK_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::stats_getMegagroupStats(flags, false /*ignored*/, std::move(input_channel)), {}, dc_id));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stats_getMegagroupStats>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(td_->contacts_manager_->convert_megagroup_stats(result_ptr.move_as_ok()));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "GetMegagroupStatsQuery");
    promise_.set_error(std::move(status));
  }
};

class GetBroadcastStatsQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::ChatStatistics>> promise_;
  ChannelId channel_id_;

 public:
  explicit GetBroadcastStatsQuery(Promise<td_api::object_ptr<td_api::ChatStatistics>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, bool is_dark, DcId dc_id) {
    channel_id_ = channel_id;

    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);

    int32 flags = 0;
    if (is_dark) {
      flags |= telegram_api::stats_getBroadcastStats::DARK_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::stats_getBroadcastStats(flags, false /*ignored*/, std::move(input_channel)), {}, dc_id));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stats_getBroadcastStats>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = ContactsManager::convert_broadcast_stats(result_ptr.move_as_ok());
    for (auto &info : result->recent_message_interactions_) {
      td_->messages_manager_->on_update_message_interaction_info({DialogId(channel_id_), MessageId(info->message_id_)},
                                                                 info->view_count_, info->forward_count_, false,
                                                                 nullptr);
    }
    promise_.set_value(std::move(result));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "GetBroadcastStatsQuery");
    promise_.set_error(std::move(status));
  }
};

tl_object_ptr<td_api::messageStatistics> ContactsManager::convert_message_stats(
    tl_object_ptr<telegram_api::stats_messageStats> obj) {
  return make_tl_object<td_api::messageStatistics>(convert_stats_graph(std::move(obj->views_graph_)));
}

class GetMessageStatsQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::messageStatistics>> promise_;
  ChannelId channel_id_;

 public:
  explicit GetMessageStatsQuery(Promise<td_api::object_ptr<td_api::messageStatistics>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(ChannelId channel_id, MessageId message_id, bool is_dark, DcId dc_id) {
    channel_id_ = channel_id;

    auto input_channel = td_->contacts_manager_->get_input_channel(channel_id);
    CHECK(input_channel != nullptr);

    int32 flags = 0;
    if (is_dark) {
      flags |= telegram_api::stats_getMessageStats::DARK_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::stats_getMessageStats(flags, false /*ignored*/, std::move(input_channel),
                                            message_id.get_server_message_id().get()),
        {}, dc_id));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stats_getMessageStats>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(td_->contacts_manager_->convert_message_stats(result_ptr.move_as_ok()));
  }

  void on_error(Status status) final {
    td_->contacts_manager_->on_get_channel_error(channel_id_, status, "GetMessageStatsQuery");
    promise_.set_error(std::move(status));
  }
};

class LoadAsyncGraphQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::StatisticalGraph>> promise_;

 public:
  explicit LoadAsyncGraphQuery(Promise<td_api::object_ptr<td_api::StatisticalGraph>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(const string &token, int64 x, DcId dc_id) {
    int32 flags = 0;
    if (x != 0) {
      flags |= telegram_api::stats_loadAsyncGraph::X_MASK;
    }
    send_query(G()->net_query_creator().create(telegram_api::stats_loadAsyncGraph(flags, token, x), {}, dc_id));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::stats_loadAsyncGraph>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto result = result_ptr.move_as_ok();
    promise_.set_value(ContactsManager::convert_stats_graph(std::move(result)));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class ContactsManager::UploadProfilePhotoCallback final : public FileManager::UploadCallback {
 public:
  void on_upload_ok(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file) final {
    send_closure_later(G()->contacts_manager(), &ContactsManager::on_upload_profile_photo, file_id,
                       std::move(input_file));
  }
  void on_upload_encrypted_ok(FileId file_id, tl_object_ptr<telegram_api::InputEncryptedFile> input_file) final {
    UNREACHABLE();
  }
  void on_upload_secure_ok(FileId file_id, tl_object_ptr<telegram_api::InputSecureFile> input_file) final {
    UNREACHABLE();
  }
  void on_upload_error(FileId file_id, Status error) final {
    send_closure_later(G()->contacts_manager(), &ContactsManager::on_upload_profile_photo_error, file_id,
                       std::move(error));
  }
};

ContactsManager::ContactsManager(Td *td, ActorShared<> parent) : td_(td), parent_(std::move(parent)) {
  upload_profile_photo_callback_ = std::make_shared<UploadProfilePhotoCallback>();

  my_id_ = load_my_id();

  td_->option_manager_->set_option_integer("telegram_service_notifications_chat_id",
                                           DialogId(get_service_notifications_user_id()).get());
  td_->option_manager_->set_option_integer("replies_bot_chat_id", DialogId(get_replies_bot_user_id()).get());
  td_->option_manager_->set_option_integer("group_anonymous_bot_user_id", get_anonymous_bot_user_id().get());
  td_->option_manager_->set_option_integer("channel_bot_user_id", get_channel_bot_user_id().get());

  if (G()->parameters().use_chat_info_db) {
    auto next_contacts_sync_date_string = G()->td_db()->get_binlog_pmc()->get("next_contacts_sync_date");
    if (!next_contacts_sync_date_string.empty()) {
      next_contacts_sync_date_ = min(to_integer<int32>(next_contacts_sync_date_string), G()->unix_time() + 100000);
    }

    auto saved_contact_count_string = G()->td_db()->get_binlog_pmc()->get("saved_contact_count");
    if (!saved_contact_count_string.empty()) {
      saved_contact_count_ = to_integer<int32>(saved_contact_count_string);
    }
  } else {
    G()->td_db()->get_binlog_pmc()->erase("next_contacts_sync_date");
    G()->td_db()->get_binlog_pmc()->erase("saved_contact_count");
  }
  if (G()->parameters().use_file_db) {
    G()->td_db()->get_sqlite_pmc()->erase_by_prefix("us_bot_info", Auto());
  }

  was_online_local_ = to_integer<int32>(G()->td_db()->get_binlog_pmc()->get("my_was_online_local"));
  was_online_remote_ = to_integer<int32>(G()->td_db()->get_binlog_pmc()->get("my_was_online_remote"));
  if (was_online_local_ >= G()->unix_time_cached() && !td_->is_online()) {
    was_online_local_ = G()->unix_time_cached() - 1;
  }

  location_visibility_expire_date_ =
      to_integer<int32>(G()->td_db()->get_binlog_pmc()->get("location_visibility_expire_date"));
  if (location_visibility_expire_date_ != 0 && location_visibility_expire_date_ <= G()->unix_time()) {
    location_visibility_expire_date_ = 0;
    G()->td_db()->get_binlog_pmc()->erase("location_visibility_expire_date");
  }
  auto pending_location_visibility_expire_date_string =
      G()->td_db()->get_binlog_pmc()->get("pending_location_visibility_expire_date");
  if (!pending_location_visibility_expire_date_string.empty()) {
    pending_location_visibility_expire_date_ = to_integer<int32>(pending_location_visibility_expire_date_string);
  }
  update_is_location_visible();
  LOG(INFO) << "Loaded location_visibility_expire_date = " << location_visibility_expire_date_
            << " and pending_location_visibility_expire_date = " << pending_location_visibility_expire_date_;

  user_online_timeout_.set_callback(on_user_online_timeout_callback);
  user_online_timeout_.set_callback_data(static_cast<void *>(this));

  user_emoji_status_timeout_.set_callback(on_user_emoji_status_timeout_callback);
  user_emoji_status_timeout_.set_callback_data(static_cast<void *>(this));

  channel_unban_timeout_.set_callback(on_channel_unban_timeout_callback);
  channel_unban_timeout_.set_callback_data(static_cast<void *>(this));

  user_nearby_timeout_.set_callback(on_user_nearby_timeout_callback);
  user_nearby_timeout_.set_callback_data(static_cast<void *>(this));

  slow_mode_delay_timeout_.set_callback(on_slow_mode_delay_timeout_callback);
  slow_mode_delay_timeout_.set_callback_data(static_cast<void *>(this));

  invite_link_info_expire_timeout_.set_callback(on_invite_link_info_expire_timeout_callback);
  invite_link_info_expire_timeout_.set_callback_data(static_cast<void *>(this));

  channel_participant_cache_timeout_.set_callback(on_channel_participant_cache_timeout_callback);
  channel_participant_cache_timeout_.set_callback_data(static_cast<void *>(this));
}

ContactsManager::~ContactsManager() {
  Scheduler::instance()->destroy_on_scheduler(
      G()->get_gc_scheduler_id(), users_, users_full_, user_photos_, unknown_users_, pending_user_photos_,
      user_profile_photo_file_source_ids_, my_photo_file_id_, user_full_file_source_ids_, chats_, chats_full_,
      unknown_chats_, chat_full_file_source_ids_, min_channels_, channels_, channels_full_, unknown_channels_,
      invalidated_channels_full_, channel_full_file_source_ids_, secret_chats_, unknown_secret_chats_,
      secret_chats_with_user_, invite_link_infos_, dialog_access_by_invite_link_, loaded_from_database_users_,
      unavailable_user_fulls_, loaded_from_database_chats_, unavailable_chat_fulls_, loaded_from_database_channels_,
      unavailable_channel_fulls_, loaded_from_database_secret_chats_, dialog_administrators_,
      cached_channel_participants_, resolved_phone_numbers_, channel_participants_, all_imported_contacts_,
      linked_channel_ids_, restricted_user_ids_, restricted_channel_ids_);
}

void ContactsManager::start_up() {
  if (!pending_location_visibility_expire_date_) {
    try_send_set_location_visibility_query();
  }
}

void ContactsManager::tear_down() {
  parent_.reset();

  LOG(DEBUG) << "Have " << users_.size() << " users, " << chats_.size() << " basic groups, " << channels_.size()
             << " supergroups and " << secret_chats_.size() << " secret chats to free";
  LOG(DEBUG) << "Have " << users_full_.size() << " full users, " << chats_full_.size() << " full basic groups and "
             << channels_full_.size() << " full supergroups to free";
}

UserId ContactsManager::load_my_id() {
  auto id_string = G()->td_db()->get_binlog_pmc()->get("my_id");
  if (!id_string.empty()) {
    UserId my_id(to_integer<int64>(id_string));
    if (my_id.is_valid()) {
      return my_id;
    }

    my_id = UserId(to_integer<int64>(Slice(id_string).substr(5)));
    if (my_id.is_valid()) {
      G()->td_db()->get_binlog_pmc()->set("my_id", to_string(my_id.get()));
      return my_id;
    }

    LOG(ERROR) << "Wrong my ID = \"" << id_string << "\" stored in database";
  }
  return UserId();
}

void ContactsManager::on_user_online_timeout_callback(void *contacts_manager_ptr, int64 user_id_long) {
  if (G()->close_flag()) {
    return;
  }

  auto contacts_manager = static_cast<ContactsManager *>(contacts_manager_ptr);
  send_closure_later(contacts_manager->actor_id(contacts_manager), &ContactsManager::on_user_online_timeout,
                     UserId(user_id_long));
}

void ContactsManager::on_user_online_timeout(UserId user_id) {
  if (G()->close_flag()) {
    return;
  }

  auto u = get_user(user_id);
  CHECK(u != nullptr);
  CHECK(u->is_update_user_sent);

  LOG(INFO) << "Update " << user_id << " online status to offline";
  send_closure(G()->td(), &Td::send_update,
               td_api::make_object<td_api::updateUserStatus>(user_id.get(), get_user_status_object(user_id, u)));

  update_user_online_member_count(u);
}

void ContactsManager::on_user_emoji_status_timeout_callback(void *contacts_manager_ptr, int64 user_id_long) {
  if (G()->close_flag()) {
    return;
  }

  auto contacts_manager = static_cast<ContactsManager *>(contacts_manager_ptr);
  send_closure_later(contacts_manager->actor_id(contacts_manager), &ContactsManager::on_user_emoji_status_timeout,
                     UserId(user_id_long));
}

void ContactsManager::on_user_emoji_status_timeout(UserId user_id) {
  if (G()->close_flag()) {
    return;
  }

  auto u = get_user(user_id);
  CHECK(u != nullptr);
  CHECK(u->is_update_user_sent);

  update_user(u, user_id);
}

void ContactsManager::on_channel_unban_timeout_callback(void *contacts_manager_ptr, int64 channel_id_long) {
  if (G()->close_flag()) {
    return;
  }

  auto contacts_manager = static_cast<ContactsManager *>(contacts_manager_ptr);
  send_closure_later(contacts_manager->actor_id(contacts_manager), &ContactsManager::on_channel_unban_timeout,
                     ChannelId(channel_id_long));
}

void ContactsManager::on_channel_unban_timeout(ChannelId channel_id) {
  if (G()->close_flag()) {
    return;
  }

  auto c = get_channel(channel_id);
  CHECK(c != nullptr);

  auto old_status = c->status;
  c->status.update_restrictions();
  if (c->status == old_status) {
    LOG_IF(ERROR, c->status.is_restricted() || c->status.is_banned())
        << "Status of " << channel_id << " wasn't updated: " << c->status;
  } else {
    c->is_changed = true;
  }

  LOG(INFO) << "Update " << channel_id << " status";
  c->is_status_changed = true;
  invalidate_channel_full(channel_id, !c->is_slow_mode_enabled);
  update_channel(c, channel_id);  // always call, because in case of failure we need to reactivate timeout
}

void ContactsManager::on_user_nearby_timeout_callback(void *contacts_manager_ptr, int64 user_id_long) {
  if (G()->close_flag()) {
    return;
  }

  auto contacts_manager = static_cast<ContactsManager *>(contacts_manager_ptr);
  send_closure_later(contacts_manager->actor_id(contacts_manager), &ContactsManager::on_user_nearby_timeout,
                     UserId(user_id_long));
}

void ContactsManager::on_user_nearby_timeout(UserId user_id) {
  if (G()->close_flag()) {
    return;
  }

  auto u = get_user(user_id);
  CHECK(u != nullptr);

  LOG(INFO) << "Remove " << user_id << " from nearby list";
  DialogId dialog_id(user_id);
  for (size_t i = 0; i < users_nearby_.size(); i++) {
    if (users_nearby_[i].dialog_id == dialog_id) {
      users_nearby_.erase(users_nearby_.begin() + i);
      send_update_users_nearby();
      return;
    }
  }
}

void ContactsManager::on_slow_mode_delay_timeout_callback(void *contacts_manager_ptr, int64 channel_id_long) {
  if (G()->close_flag()) {
    return;
  }

  auto contacts_manager = static_cast<ContactsManager *>(contacts_manager_ptr);
  send_closure_later(contacts_manager->actor_id(contacts_manager), &ContactsManager::on_slow_mode_delay_timeout,
                     ChannelId(channel_id_long));
}

void ContactsManager::on_slow_mode_delay_timeout(ChannelId channel_id) {
  if (G()->close_flag()) {
    return;
  }

  on_update_channel_slow_mode_next_send_date(channel_id, 0);
}

void ContactsManager::on_invite_link_info_expire_timeout_callback(void *contacts_manager_ptr, int64 dialog_id_long) {
  if (G()->close_flag()) {
    return;
  }

  auto contacts_manager = static_cast<ContactsManager *>(contacts_manager_ptr);
  send_closure_later(contacts_manager->actor_id(contacts_manager), &ContactsManager::on_invite_link_info_expire_timeout,
                     DialogId(dialog_id_long));
}

void ContactsManager::on_invite_link_info_expire_timeout(DialogId dialog_id) {
  if (G()->close_flag()) {
    return;
  }

  auto access_it = dialog_access_by_invite_link_.find(dialog_id);
  if (access_it == dialog_access_by_invite_link_.end()) {
    return;
  }
  auto expires_in = access_it->second.accessible_before - G()->unix_time() - 1;
  if (expires_in >= 3) {
    invite_link_info_expire_timeout_.set_timeout_in(dialog_id.get(), expires_in);
    return;
  }

  remove_dialog_access_by_invite_link(dialog_id);
}

void ContactsManager::on_channel_participant_cache_timeout_callback(void *contacts_manager_ptr, int64 channel_id_long) {
  if (G()->close_flag()) {
    return;
  }

  auto contacts_manager = static_cast<ContactsManager *>(contacts_manager_ptr);
  send_closure_later(contacts_manager->actor_id(contacts_manager),
                     &ContactsManager::on_channel_participant_cache_timeout, ChannelId(channel_id_long));
}

void ContactsManager::on_channel_participant_cache_timeout(ChannelId channel_id) {
  if (G()->close_flag()) {
    return;
  }

  auto channel_participants_it = channel_participants_.find(channel_id);
  if (channel_participants_it == channel_participants_.end()) {
    return;
  }

  auto &participants = channel_participants_it->second.participants_;
  auto min_access_date = G()->unix_time() - CHANNEL_PARTICIPANT_CACHE_TIME;
  table_remove_if(participants,
                  [min_access_date](const auto &it) { return it.second.last_access_date_ < min_access_date; });

  if (participants.empty()) {
    channel_participants_.erase(channel_participants_it);
  } else {
    channel_participant_cache_timeout_.set_timeout_in(channel_id.get(), CHANNEL_PARTICIPANT_CACHE_TIME);
  }
}

template <class StorerT>
void ContactsManager::User::store(StorerT &storer) const {
  using td::store;
  bool has_last_name = !last_name.empty();
  bool has_username = !username.empty();
  bool has_photo = photo.small_file_id.is_valid();
  bool has_language_code = !language_code.empty();
  bool have_access_hash = access_hash != -1;
  bool has_cache_version = cache_version != 0;
  bool has_is_contact = true;
  bool has_restriction_reasons = !restriction_reasons.empty();
  bool has_emoji_status = !emoji_status.is_empty();
  BEGIN_STORE_FLAGS();
  STORE_FLAG(is_received);
  STORE_FLAG(is_verified);
  STORE_FLAG(is_deleted);
  STORE_FLAG(is_bot);
  STORE_FLAG(can_join_groups);
  STORE_FLAG(can_read_all_group_messages);  // 5
  STORE_FLAG(is_inline_bot);
  STORE_FLAG(need_location_bot);
  STORE_FLAG(has_last_name);
  STORE_FLAG(has_username);
  STORE_FLAG(has_photo);  // 10
  STORE_FLAG(false);      // legacy is_restricted
  STORE_FLAG(has_language_code);
  STORE_FLAG(have_access_hash);
  STORE_FLAG(is_support);
  STORE_FLAG(is_min_access_hash);  // 15
  STORE_FLAG(is_scam);
  STORE_FLAG(has_cache_version);
  STORE_FLAG(has_is_contact);
  STORE_FLAG(is_contact);
  STORE_FLAG(is_mutual_contact);  // 20
  STORE_FLAG(has_restriction_reasons);
  STORE_FLAG(need_apply_min_photo);
  STORE_FLAG(is_fake);
  STORE_FLAG(can_be_added_to_attach_menu);
  STORE_FLAG(is_premium);  // 25
  STORE_FLAG(attach_menu_enabled);
  STORE_FLAG(has_emoji_status);
  END_STORE_FLAGS();
  store(first_name, storer);
  if (has_last_name) {
    store(last_name, storer);
  }
  if (has_username) {
    store(username, storer);
  }
  store(phone_number, storer);
  if (have_access_hash) {
    store(access_hash, storer);
  }
  if (has_photo) {
    store(photo, storer);
  }
  store(was_online, storer);
  if (has_restriction_reasons) {
    store(restriction_reasons, storer);
  }
  if (is_inline_bot) {
    store(inline_query_placeholder, storer);
  }
  if (is_bot) {
    store(bot_info_version, storer);
  }
  if (has_language_code) {
    store(language_code, storer);
  }
  if (has_cache_version) {
    store(cache_version, storer);
  }
  if (has_emoji_status) {
    store(emoji_status, storer);
  }
}

template <class ParserT>
void ContactsManager::User::parse(ParserT &parser) {
  using td::parse;
  bool has_last_name;
  bool has_username;
  bool has_photo;
  bool legacy_is_restricted;
  bool has_language_code;
  bool have_access_hash;
  bool has_cache_version;
  bool has_is_contact;
  bool has_restriction_reasons;
  bool has_emoji_status;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(is_received);
  PARSE_FLAG(is_verified);
  PARSE_FLAG(is_deleted);
  PARSE_FLAG(is_bot);
  PARSE_FLAG(can_join_groups);
  PARSE_FLAG(can_read_all_group_messages);
  PARSE_FLAG(is_inline_bot);
  PARSE_FLAG(need_location_bot);
  PARSE_FLAG(has_last_name);
  PARSE_FLAG(has_username);
  PARSE_FLAG(has_photo);
  PARSE_FLAG(legacy_is_restricted);
  PARSE_FLAG(has_language_code);
  PARSE_FLAG(have_access_hash);
  PARSE_FLAG(is_support);
  PARSE_FLAG(is_min_access_hash);
  PARSE_FLAG(is_scam);
  PARSE_FLAG(has_cache_version);
  PARSE_FLAG(has_is_contact);
  PARSE_FLAG(is_contact);
  PARSE_FLAG(is_mutual_contact);
  PARSE_FLAG(has_restriction_reasons);
  PARSE_FLAG(need_apply_min_photo);
  PARSE_FLAG(is_fake);
  PARSE_FLAG(can_be_added_to_attach_menu);
  PARSE_FLAG(is_premium);
  PARSE_FLAG(attach_menu_enabled);
  PARSE_FLAG(has_emoji_status);
  END_PARSE_FLAGS();
  parse(first_name, parser);
  if (has_last_name) {
    parse(last_name, parser);
  }
  if (has_username) {
    parse(username, parser);
  }
  parse(phone_number, parser);
  if (parser.version() < static_cast<int32>(Version::FixMinUsers)) {
    have_access_hash = is_received;
  }
  if (have_access_hash) {
    parse(access_hash, parser);
  } else {
    is_min_access_hash = true;
  }
  if (has_photo) {
    parse(photo, parser);
  }
  if (!has_is_contact) {
    // enum class LinkState : uint8 { Unknown, None, KnowsPhoneNumber, Contact };

    uint32 link_state_inbound;
    uint32 link_state_outbound;
    parse(link_state_inbound, parser);
    parse(link_state_outbound, parser);

    is_contact = link_state_outbound == 3;
    is_mutual_contact = is_contact && link_state_inbound == 3;
  }
  parse(was_online, parser);
  if (legacy_is_restricted) {
    string restriction_reason;
    parse(restriction_reason, parser);
    restriction_reasons = get_restriction_reasons(restriction_reason);
  } else if (has_restriction_reasons) {
    parse(restriction_reasons, parser);
  }
  if (is_inline_bot) {
    parse(inline_query_placeholder, parser);
  }
  if (is_bot) {
    parse(bot_info_version, parser);
  }
  if (has_language_code) {
    parse(language_code, parser);
  }
  if (has_cache_version) {
    parse(cache_version, parser);
  }
  if (has_emoji_status) {
    parse(emoji_status, parser);
  }

  if (!check_utf8(first_name)) {
    LOG(ERROR) << "Have invalid first name \"" << first_name << '"';
    first_name.clear();
    cache_version = 0;
  }
  if (!check_utf8(last_name)) {
    LOG(ERROR) << "Have invalid last name \"" << last_name << '"';
    last_name.clear();
    cache_version = 0;
  }
  if (!check_utf8(username)) {
    LOG(ERROR) << "Have invalid username \"" << username << '"';
    username.clear();
    cache_version = 0;
  }

  clean_phone_number(phone_number);
  if (first_name.empty() && last_name.empty()) {
    first_name = phone_number;
  }
  if (!is_contact && is_mutual_contact) {
    LOG(ERROR) << "Have invalid flag is_mutual_contact";
    is_mutual_contact = false;
    cache_version = 0;
  }
}

template <class StorerT>
void ContactsManager::UserFull::store(StorerT &storer) const {
  using td::store;
  bool has_about = !about.empty();
  bool has_photo = !photo.is_empty();
  bool has_description = !description.empty();
  bool has_commands = !commands.empty();
  bool has_private_forward_name = !private_forward_name.empty();
  bool has_group_administrator_rights = group_administrator_rights != AdministratorRights();
  bool has_broadcast_administrator_rights = broadcast_administrator_rights != AdministratorRights();
  bool has_menu_button = menu_button != nullptr;
  bool has_description_photo = !description_photo.is_empty();
  bool has_description_animation = description_animation_file_id.is_valid();
  bool has_premium_gift_options = !premium_gift_options.empty();
  BEGIN_STORE_FLAGS();
  STORE_FLAG(has_about);
  STORE_FLAG(is_blocked);
  STORE_FLAG(can_be_called);
  STORE_FLAG(has_private_calls);
  STORE_FLAG(can_pin_messages);
  STORE_FLAG(need_phone_number_privacy_exception);
  STORE_FLAG(has_photo);
  STORE_FLAG(supports_video_calls);
  STORE_FLAG(has_description);
  STORE_FLAG(has_commands);
  STORE_FLAG(has_private_forward_name);
  STORE_FLAG(has_group_administrator_rights);
  STORE_FLAG(has_broadcast_administrator_rights);
  STORE_FLAG(has_menu_button);
  STORE_FLAG(has_description_photo);
  STORE_FLAG(has_description_animation);
  STORE_FLAG(has_premium_gift_options);
  STORE_FLAG(voice_messages_forbidden);
  END_STORE_FLAGS();
  if (has_about) {
    store(about, storer);
  }
  store(common_chat_count, storer);
  store_time(expires_at, storer);
  if (has_photo) {
    store(photo, storer);
  }
  if (has_description) {
    store(description, storer);
  }
  if (has_commands) {
    store(commands, storer);
  }
  if (has_private_forward_name) {
    store(private_forward_name, storer);
  }
  if (has_group_administrator_rights) {
    store(group_administrator_rights, storer);
  }
  if (has_broadcast_administrator_rights) {
    store(broadcast_administrator_rights, storer);
  }
  if (has_menu_button) {
    store(menu_button, storer);
  }
  if (has_description_photo) {
    store(description_photo, storer);
  }
  if (has_description_animation) {
    storer.context()->td().get_actor_unsafe()->animations_manager_->store_animation(description_animation_file_id,
                                                                                    storer);
  }
  if (has_premium_gift_options) {
    store(premium_gift_options, storer);
  }
}

template <class ParserT>
void ContactsManager::UserFull::parse(ParserT &parser) {
  using td::parse;
  bool has_about;
  bool has_photo;
  bool has_description;
  bool has_commands;
  bool has_private_forward_name;
  bool has_group_administrator_rights;
  bool has_broadcast_administrator_rights;
  bool has_menu_button;
  bool has_description_photo;
  bool has_description_animation;
  bool has_premium_gift_options;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(has_about);
  PARSE_FLAG(is_blocked);
  PARSE_FLAG(can_be_called);
  PARSE_FLAG(has_private_calls);
  PARSE_FLAG(can_pin_messages);
  PARSE_FLAG(need_phone_number_privacy_exception);
  PARSE_FLAG(has_photo);
  PARSE_FLAG(supports_video_calls);
  PARSE_FLAG(has_description);
  PARSE_FLAG(has_commands);
  PARSE_FLAG(has_private_forward_name);
  PARSE_FLAG(has_group_administrator_rights);
  PARSE_FLAG(has_broadcast_administrator_rights);
  PARSE_FLAG(has_menu_button);
  PARSE_FLAG(has_description_photo);
  PARSE_FLAG(has_description_animation);
  PARSE_FLAG(has_premium_gift_options);
  PARSE_FLAG(voice_messages_forbidden);
  END_PARSE_FLAGS();
  if (has_about) {
    parse(about, parser);
  }
  parse(common_chat_count, parser);
  parse_time(expires_at, parser);
  if (has_photo) {
    parse(photo, parser);
  }
  if (has_description) {
    parse(description, parser);
  }
  if (has_commands) {
    parse(commands, parser);
  }
  if (has_private_forward_name) {
    parse(private_forward_name, parser);
  }
  if (has_group_administrator_rights) {
    parse(group_administrator_rights, parser);
  }
  if (has_broadcast_administrator_rights) {
    parse(broadcast_administrator_rights, parser);
  }
  if (has_menu_button) {
    parse(menu_button, parser);
  }
  if (has_description_photo) {
    parse(description_photo, parser);
  }
  if (has_description_animation) {
    description_animation_file_id =
        parser.context()->td().get_actor_unsafe()->animations_manager_->parse_animation(parser);
  }
  if (has_premium_gift_options) {
    parse(premium_gift_options, parser);
  }
}

template <class StorerT>
void ContactsManager::Chat::store(StorerT &storer) const {
  using td::store;
  bool has_photo = photo.small_file_id.is_valid();
  bool use_new_rights = true;
  bool has_default_permissions_version = default_permissions_version != -1;
  bool has_pinned_message_version = pinned_message_version != -1;
  bool has_cache_version = cache_version != 0;
  BEGIN_STORE_FLAGS();
  STORE_FLAG(false);
  STORE_FLAG(false);
  STORE_FLAG(false);
  STORE_FLAG(false);
  STORE_FLAG(false);
  STORE_FLAG(false);
  STORE_FLAG(is_active);
  STORE_FLAG(has_photo);
  STORE_FLAG(use_new_rights);
  STORE_FLAG(has_default_permissions_version);
  STORE_FLAG(has_pinned_message_version);
  STORE_FLAG(has_cache_version);
  STORE_FLAG(noforwards);
  END_STORE_FLAGS();

  store(title, storer);
  if (has_photo) {
    store(photo, storer);
  }
  store(participant_count, storer);
  store(date, storer);
  store(migrated_to_channel_id, storer);
  store(version, storer);
  store(status, storer);
  store(default_permissions, storer);
  if (has_default_permissions_version) {
    store(default_permissions_version, storer);
  }
  if (has_pinned_message_version) {
    store(pinned_message_version, storer);
  }
  if (has_cache_version) {
    store(cache_version, storer);
  }
}

template <class ParserT>
void ContactsManager::Chat::parse(ParserT &parser) {
  using td::parse;
  bool has_photo;
  bool left;
  bool kicked;
  bool is_creator;
  bool is_administrator;
  bool everyone_is_administrator;
  bool can_edit;
  bool use_new_rights;
  bool has_default_permissions_version;
  bool has_pinned_message_version;
  bool has_cache_version;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(left);
  PARSE_FLAG(kicked);
  PARSE_FLAG(is_creator);
  PARSE_FLAG(is_administrator);
  PARSE_FLAG(everyone_is_administrator);
  PARSE_FLAG(can_edit);
  PARSE_FLAG(is_active);
  PARSE_FLAG(has_photo);
  PARSE_FLAG(use_new_rights);
  PARSE_FLAG(has_default_permissions_version);
  PARSE_FLAG(has_pinned_message_version);
  PARSE_FLAG(has_cache_version);
  PARSE_FLAG(noforwards);
  END_PARSE_FLAGS();

  parse(title, parser);
  if (has_photo) {
    parse(photo, parser);
  }
  parse(participant_count, parser);
  parse(date, parser);
  parse(migrated_to_channel_id, parser);
  parse(version, parser);
  if (use_new_rights) {
    parse(status, parser);
    parse(default_permissions, parser);
  } else {
    if (can_edit != (is_creator || is_administrator || everyone_is_administrator)) {
      LOG(ERROR) << "Have wrong can_edit flag";
    }

    if (kicked || !is_active) {
      status = DialogParticipantStatus::Banned(0);
    } else if (left) {
      status = DialogParticipantStatus::Left();
    } else if (is_creator) {
      status = DialogParticipantStatus::Creator(true, false, string());
    } else if (is_administrator && !everyone_is_administrator) {
      status = DialogParticipantStatus::GroupAdministrator(false);
    } else {
      status = DialogParticipantStatus::Member();
    }
    default_permissions = RestrictedRights(true, true, true, true, true, true, true, true, everyone_is_administrator,
                                           everyone_is_administrator, everyone_is_administrator);
  }
  if (has_default_permissions_version) {
    parse(default_permissions_version, parser);
  }
  if (has_pinned_message_version) {
    parse(pinned_message_version, parser);
  }
  if (has_cache_version) {
    parse(cache_version, parser);
  }

  if (!check_utf8(title)) {
    LOG(ERROR) << "Have invalid title \"" << title << '"';
    title.clear();
    cache_version = 0;
  }

  if (status.is_administrator() && !status.is_creator()) {
    status = DialogParticipantStatus::GroupAdministrator(false);
  }
}

template <class StorerT>
void ContactsManager::ChatFull::store(StorerT &storer) const {
  using td::store;
  bool has_description = !description.empty();
  bool has_legacy_invite_link = false;
  bool has_photo = !photo.is_empty();
  bool has_invite_link = invite_link.is_valid();
  bool has_bot_commands = !bot_commands.empty();
  BEGIN_STORE_FLAGS();
  STORE_FLAG(has_description);
  STORE_FLAG(has_legacy_invite_link);
  STORE_FLAG(can_set_username);
  STORE_FLAG(has_photo);
  STORE_FLAG(has_invite_link);
  STORE_FLAG(has_bot_commands);
  END_STORE_FLAGS();
  store(version, storer);
  store(creator_user_id, storer);
  store(participants, storer);
  if (has_description) {
    store(description, storer);
  }
  if (has_photo) {
    store(photo, storer);
  }
  if (has_invite_link) {
    store(invite_link, storer);
  }
  if (has_bot_commands) {
    store(bot_commands, storer);
  }
}

template <class ParserT>
void ContactsManager::ChatFull::parse(ParserT &parser) {
  using td::parse;
  bool has_description;
  bool legacy_has_invite_link;
  bool has_photo;
  bool has_invite_link;
  bool has_bot_commands;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(has_description);
  PARSE_FLAG(legacy_has_invite_link);
  PARSE_FLAG(can_set_username);
  PARSE_FLAG(has_photo);
  PARSE_FLAG(has_invite_link);
  PARSE_FLAG(has_bot_commands);
  END_PARSE_FLAGS();
  parse(version, parser);
  parse(creator_user_id, parser);
  parse(participants, parser);
  if (has_description) {
    parse(description, parser);
  }
  if (legacy_has_invite_link) {
    string legacy_invite_link;
    parse(legacy_invite_link, parser);
  }
  if (has_photo) {
    parse(photo, parser);
  }
  if (has_invite_link) {
    parse(invite_link, parser);
  }
  if (has_bot_commands) {
    parse(bot_commands, parser);
  }
}

template <class StorerT>
void ContactsManager::Channel::store(StorerT &storer) const {
  using td::store;
  bool has_photo = photo.small_file_id.is_valid();
  bool has_username = !username.empty();
  bool use_new_rights = true;
  bool has_participant_count = participant_count != 0;
  bool have_default_permissions = true;
  bool has_cache_version = cache_version != 0;
  bool has_restriction_reasons = !restriction_reasons.empty();
  bool legacy_has_active_group_call = false;
  BEGIN_STORE_FLAGS();
  STORE_FLAG(false);
  STORE_FLAG(false);
  STORE_FLAG(false);
  STORE_FLAG(sign_messages);
  STORE_FLAG(false);
  STORE_FLAG(false);  // 5
  STORE_FLAG(false);
  STORE_FLAG(is_megagroup);
  STORE_FLAG(is_verified);
  STORE_FLAG(has_photo);
  STORE_FLAG(has_username);  // 10
  STORE_FLAG(false);
  STORE_FLAG(use_new_rights);
  STORE_FLAG(has_participant_count);
  STORE_FLAG(have_default_permissions);
  STORE_FLAG(is_scam);  // 15
  STORE_FLAG(has_cache_version);
  STORE_FLAG(has_linked_channel);
  STORE_FLAG(has_location);
  STORE_FLAG(is_slow_mode_enabled);
  STORE_FLAG(has_restriction_reasons);  // 20
  STORE_FLAG(legacy_has_active_group_call);
  STORE_FLAG(is_fake);
  STORE_FLAG(is_gigagroup);
  STORE_FLAG(noforwards);
  STORE_FLAG(can_be_deleted);  // 25
  STORE_FLAG(join_to_send);
  STORE_FLAG(join_request);
  END_STORE_FLAGS();

  store(status, storer);
  store(access_hash, storer);
  store(title, storer);
  if (has_photo) {
    store(photo, storer);
  }
  if (has_username) {
    store(username, storer);
  }
  store(date, storer);
  if (has_restriction_reasons) {
    store(restriction_reasons, storer);
  }
  if (has_participant_count) {
    store(participant_count, storer);
  }
  if (is_megagroup) {
    store(default_permissions, storer);
  }
  if (has_cache_version) {
    store(cache_version, storer);
  }
}

template <class ParserT>
void ContactsManager::Channel::parse(ParserT &parser) {
  using td::parse;
  bool has_photo;
  bool has_username;
  bool legacy_is_restricted;
  bool left;
  bool kicked;
  bool is_creator;
  bool can_edit;
  bool can_moderate;
  bool anyone_can_invite;
  bool use_new_rights;
  bool has_participant_count;
  bool have_default_permissions;
  bool has_cache_version;
  bool has_restriction_reasons;
  bool legacy_has_active_group_call;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(left);
  PARSE_FLAG(kicked);
  PARSE_FLAG(anyone_can_invite);
  PARSE_FLAG(sign_messages);
  PARSE_FLAG(is_creator);
  PARSE_FLAG(can_edit);
  PARSE_FLAG(can_moderate);
  PARSE_FLAG(is_megagroup);
  PARSE_FLAG(is_verified);
  PARSE_FLAG(has_photo);
  PARSE_FLAG(has_username);
  PARSE_FLAG(legacy_is_restricted);
  PARSE_FLAG(use_new_rights);
  PARSE_FLAG(has_participant_count);
  PARSE_FLAG(have_default_permissions);
  PARSE_FLAG(is_scam);
  PARSE_FLAG(has_cache_version);
  PARSE_FLAG(has_linked_channel);
  PARSE_FLAG(has_location);
  PARSE_FLAG(is_slow_mode_enabled);
  PARSE_FLAG(has_restriction_reasons);
  PARSE_FLAG(legacy_has_active_group_call);
  PARSE_FLAG(is_fake);
  PARSE_FLAG(is_gigagroup);
  PARSE_FLAG(noforwards);
  PARSE_FLAG(can_be_deleted);
  PARSE_FLAG(join_to_send);
  PARSE_FLAG(join_request);
  END_PARSE_FLAGS();

  if (use_new_rights) {
    parse(status, parser);
  } else {
    if (kicked) {
      status = DialogParticipantStatus::Banned(0);
    } else if (left) {
      status = DialogParticipantStatus::Left();
    } else if (is_creator) {
      status = DialogParticipantStatus::Creator(true, false, string());
    } else if (can_edit || can_moderate) {
      status = DialogParticipantStatus::ChannelAdministrator(false, is_megagroup);
    } else {
      status = DialogParticipantStatus::Member();
    }
  }
  parse(access_hash, parser);
  parse(title, parser);
  if (has_photo) {
    parse(photo, parser);
  }
  if (has_username) {
    parse(username, parser);
  }
  parse(date, parser);
  if (legacy_is_restricted) {
    string restriction_reason;
    parse(restriction_reason, parser);
    restriction_reasons = get_restriction_reasons(restriction_reason);
  } else if (has_restriction_reasons) {
    parse(restriction_reasons, parser);
  }
  if (has_participant_count) {
    parse(participant_count, parser);
  }
  if (is_megagroup) {
    if (have_default_permissions) {
      parse(default_permissions, parser);
    } else {
      default_permissions =
          RestrictedRights(true, true, true, true, true, true, true, true, false, anyone_can_invite, false);
    }
  }
  if (has_cache_version) {
    parse(cache_version, parser);
  }

  if (!check_utf8(title)) {
    LOG(ERROR) << "Have invalid title \"" << title << '"';
    title.clear();
    cache_version = 0;
  }
  if (!check_utf8(username)) {
    LOG(ERROR) << "Have invalid username \"" << username << '"';
    username.clear();
    cache_version = 0;
  }
  if (legacy_has_active_group_call) {
    cache_version = 0;
  }
}

template <class StorerT>
void ContactsManager::ChannelFull::store(StorerT &storer) const {
  using td::store;
  bool has_description = !description.empty();
  bool has_administrator_count = administrator_count != 0;
  bool has_restricted_count = restricted_count != 0;
  bool has_banned_count = banned_count != 0;
  bool legacy_has_invite_link = false;
  bool has_sticker_set = sticker_set_id.is_valid();
  bool has_linked_channel_id = linked_channel_id.is_valid();
  bool has_migrated_from_max_message_id = migrated_from_max_message_id.is_valid();
  bool has_migrated_from_chat_id = migrated_from_chat_id.is_valid();
  bool has_location = !location.empty();
  bool has_bot_user_ids = !bot_user_ids.empty();
  bool is_slow_mode_enabled = slow_mode_delay != 0;
  bool is_slow_mode_delay_active = slow_mode_next_send_date != 0;
  bool has_stats_dc_id = stats_dc_id.is_exact();
  bool has_photo = !photo.is_empty();
  bool legacy_has_active_group_call_id = false;
  bool has_invite_link = invite_link.is_valid();
  bool has_bot_commands = !bot_commands.empty();
  BEGIN_STORE_FLAGS();
  STORE_FLAG(has_description);
  STORE_FLAG(has_administrator_count);
  STORE_FLAG(has_restricted_count);
  STORE_FLAG(has_banned_count);
  STORE_FLAG(legacy_has_invite_link);
  STORE_FLAG(has_sticker_set);  // 5
  STORE_FLAG(has_linked_channel_id);
  STORE_FLAG(has_migrated_from_max_message_id);
  STORE_FLAG(has_migrated_from_chat_id);
  STORE_FLAG(can_get_participants);
  STORE_FLAG(can_set_username);  // 10
  STORE_FLAG(can_set_sticker_set);
  STORE_FLAG(false);  // legacy_can_view_statistics
  STORE_FLAG(is_all_history_available);
  STORE_FLAG(can_set_location);
  STORE_FLAG(has_location);  // 15
  STORE_FLAG(has_bot_user_ids);
  STORE_FLAG(is_slow_mode_enabled);
  STORE_FLAG(is_slow_mode_delay_active);
  STORE_FLAG(has_stats_dc_id);
  STORE_FLAG(has_photo);  // 20
  STORE_FLAG(is_can_view_statistics_inited);
  STORE_FLAG(can_view_statistics);
  STORE_FLAG(legacy_has_active_group_call_id);
  STORE_FLAG(has_invite_link);
  STORE_FLAG(has_bot_commands);  // 25
  STORE_FLAG(can_be_deleted);
  END_STORE_FLAGS();
  if (has_description) {
    store(description, storer);
  }
  store(participant_count, storer);
  if (has_administrator_count) {
    store(administrator_count, storer);
  }
  if (has_restricted_count) {
    store(restricted_count, storer);
  }
  if (has_banned_count) {
    store(banned_count, storer);
  }
  if (has_sticker_set) {
    store(sticker_set_id, storer);
  }
  if (has_linked_channel_id) {
    store(linked_channel_id, storer);
  }
  if (has_location) {
    store(location, storer);
  }
  if (has_bot_user_ids) {
    store(bot_user_ids, storer);
  }
  if (has_migrated_from_max_message_id) {
    store(migrated_from_max_message_id, storer);
  }
  if (has_migrated_from_chat_id) {
    store(migrated_from_chat_id, storer);
  }
  if (is_slow_mode_enabled) {
    store(slow_mode_delay, storer);
  }
  if (is_slow_mode_delay_active) {
    store(slow_mode_next_send_date, storer);
  }
  store_time(expires_at, storer);
  if (has_stats_dc_id) {
    store(stats_dc_id.get_raw_id(), storer);
  }
  if (has_photo) {
    store(photo, storer);
  }
  if (has_invite_link) {
    store(invite_link, storer);
  }
  if (has_bot_commands) {
    store(bot_commands, storer);
  }
}

template <class ParserT>
void ContactsManager::ChannelFull::parse(ParserT &parser) {
  using td::parse;
  bool has_description;
  bool has_administrator_count;
  bool has_restricted_count;
  bool has_banned_count;
  bool legacy_has_invite_link;
  bool has_sticker_set;
  bool has_linked_channel_id;
  bool has_migrated_from_max_message_id;
  bool has_migrated_from_chat_id;
  bool legacy_can_view_statistics;
  bool has_location;
  bool has_bot_user_ids;
  bool is_slow_mode_enabled;
  bool is_slow_mode_delay_active;
  bool has_stats_dc_id;
  bool has_photo;
  bool legacy_has_active_group_call_id;
  bool has_invite_link;
  bool has_bot_commands;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(has_description);
  PARSE_FLAG(has_administrator_count);
  PARSE_FLAG(has_restricted_count);
  PARSE_FLAG(has_banned_count);
  PARSE_FLAG(legacy_has_invite_link);
  PARSE_FLAG(has_sticker_set);
  PARSE_FLAG(has_linked_channel_id);
  PARSE_FLAG(has_migrated_from_max_message_id);
  PARSE_FLAG(has_migrated_from_chat_id);
  PARSE_FLAG(can_get_participants);
  PARSE_FLAG(can_set_username);
  PARSE_FLAG(can_set_sticker_set);
  PARSE_FLAG(legacy_can_view_statistics);
  PARSE_FLAG(is_all_history_available);
  PARSE_FLAG(can_set_location);
  PARSE_FLAG(has_location);
  PARSE_FLAG(has_bot_user_ids);
  PARSE_FLAG(is_slow_mode_enabled);
  PARSE_FLAG(is_slow_mode_delay_active);
  PARSE_FLAG(has_stats_dc_id);
  PARSE_FLAG(has_photo);
  PARSE_FLAG(is_can_view_statistics_inited);
  PARSE_FLAG(can_view_statistics);
  PARSE_FLAG(legacy_has_active_group_call_id);
  PARSE_FLAG(has_invite_link);
  PARSE_FLAG(has_bot_commands);
  PARSE_FLAG(can_be_deleted);
  END_PARSE_FLAGS();
  if (has_description) {
    parse(description, parser);
  }
  parse(participant_count, parser);
  if (has_administrator_count) {
    parse(administrator_count, parser);
  }
  if (has_restricted_count) {
    parse(restricted_count, parser);
  }
  if (has_banned_count) {
    parse(banned_count, parser);
  }
  if (legacy_has_invite_link) {
    string legacy_invite_link;
    parse(legacy_invite_link, parser);
  }
  if (has_sticker_set) {
    parse(sticker_set_id, parser);
  }
  if (has_linked_channel_id) {
    parse(linked_channel_id, parser);
  }
  if (has_location) {
    parse(location, parser);
  }
  if (has_bot_user_ids) {
    parse(bot_user_ids, parser);
  }
  if (has_migrated_from_max_message_id) {
    parse(migrated_from_max_message_id, parser);
  }
  if (has_migrated_from_chat_id) {
    parse(migrated_from_chat_id, parser);
  }
  if (is_slow_mode_enabled) {
    parse(slow_mode_delay, parser);
  }
  if (is_slow_mode_delay_active) {
    parse(slow_mode_next_send_date, parser);
  }
  parse_time(expires_at, parser);
  if (has_stats_dc_id) {
    stats_dc_id = DcId::create(parser.fetch_int());
  }
  if (has_photo) {
    parse(photo, parser);
  }
  if (legacy_has_active_group_call_id) {
    InputGroupCallId input_group_call_id;
    parse(input_group_call_id, parser);
  }
  if (has_invite_link) {
    parse(invite_link, parser);
  }
  if (has_bot_commands) {
    parse(bot_commands, parser);
  }

  if (legacy_can_view_statistics) {
    LOG(DEBUG) << "Ignore legacy can view statistics flag";
  }
  if (!is_can_view_statistics_inited) {
    can_view_statistics = stats_dc_id.is_exact();
  }
}

template <class StorerT>
void ContactsManager::SecretChat::store(StorerT &storer) const {
  using td::store;
  bool has_layer = layer > static_cast<int32>(SecretChatLayer::Default);
  bool has_initial_folder_id = initial_folder_id != FolderId();
  BEGIN_STORE_FLAGS();
  STORE_FLAG(is_outbound);
  STORE_FLAG(has_layer);
  STORE_FLAG(has_initial_folder_id);
  END_STORE_FLAGS();

  store(access_hash, storer);
  store(user_id, storer);
  store(state, storer);
  store(ttl, storer);
  store(date, storer);
  store(key_hash, storer);
  if (has_layer) {
    store(layer, storer);
  }
  if (has_initial_folder_id) {
    store(initial_folder_id, storer);
  }
}

template <class ParserT>
void ContactsManager::SecretChat::parse(ParserT &parser) {
  using td::parse;
  bool has_layer;
  bool has_initial_folder_id;
  BEGIN_PARSE_FLAGS();
  PARSE_FLAG(is_outbound);
  PARSE_FLAG(has_layer);
  PARSE_FLAG(has_initial_folder_id);
  END_PARSE_FLAGS();

  if (parser.version() >= static_cast<int32>(Version::AddAccessHashToSecretChat)) {
    parse(access_hash, parser);
  }
  parse(user_id, parser);
  parse(state, parser);
  parse(ttl, parser);
  parse(date, parser);
  if (parser.version() >= static_cast<int32>(Version::AddKeyHashToSecretChat)) {
    parse(key_hash, parser);
  }
  if (has_layer) {
    parse(layer, parser);
  } else {
    layer = static_cast<int32>(SecretChatLayer::Default);
  }
  if (has_initial_folder_id) {
    parse(initial_folder_id, parser);
  }
}

Result<tl_object_ptr<telegram_api::InputUser>> ContactsManager::get_input_user(UserId user_id) const {
  if (user_id == get_my_id()) {
    return make_tl_object<telegram_api::inputUserSelf>();
  }

  const User *u = get_user(user_id);
  if (u == nullptr || u->access_hash == -1 || u->is_min_access_hash) {
    if (td_->auth_manager_->is_bot() && user_id.is_valid()) {
      return make_tl_object<telegram_api::inputUser>(user_id.get(), 0);
    }
    if (u == nullptr) {
      return Status::Error(400, "User not found");
    } else {
      return Status::Error(400, "Have no access to the user");
    }
  }

  return make_tl_object<telegram_api::inputUser>(user_id.get(), u->access_hash);
}

tl_object_ptr<telegram_api::InputChannel> ContactsManager::get_input_channel(ChannelId channel_id) const {
  const Channel *c = get_channel(channel_id);
  if (c == nullptr) {
    if (td_->auth_manager_->is_bot() && channel_id.is_valid()) {
      return make_tl_object<telegram_api::inputChannel>(channel_id.get(), 0);
    }
    return nullptr;
  }

  return make_tl_object<telegram_api::inputChannel>(channel_id.get(), c->access_hash);
}

bool ContactsManager::have_input_peer_user(UserId user_id, AccessRights access_rights) const {
  if (user_id == get_my_id()) {
    return true;
  }
  return have_input_peer_user(get_user(user_id), access_rights);
}

bool ContactsManager::have_input_peer_user(const User *u, AccessRights access_rights) {
  if (u == nullptr) {
    LOG(DEBUG) << "Have no user";
    return false;
  }
  if (u->access_hash == -1 || u->is_min_access_hash) {
    LOG(DEBUG) << "Have user without access hash";
    return false;
  }
  if (access_rights == AccessRights::Know) {
    return true;
  }
  if (access_rights == AccessRights::Read) {
    return true;
  }
  if (u->is_deleted) {
    LOG(DEBUG) << "Have a deleted user";
    return false;
  }
  return true;
}

tl_object_ptr<telegram_api::InputPeer> ContactsManager::get_input_peer_user(UserId user_id,
                                                                            AccessRights access_rights) const {
  if (user_id == get_my_id()) {
    return make_tl_object<telegram_api::inputPeerSelf>();
  }
  const User *u = get_user(user_id);
  if (!have_input_peer_user(u, access_rights)) {
    if ((u == nullptr || u->access_hash == -1 || u->is_min_access_hash) && td_->auth_manager_->is_bot() &&
        user_id.is_valid()) {
      return make_tl_object<telegram_api::inputPeerUser>(user_id.get(), 0);
    }
    return nullptr;
  }

  return make_tl_object<telegram_api::inputPeerUser>(user_id.get(), u->access_hash);
}

bool ContactsManager::have_input_peer_chat(ChatId chat_id, AccessRights access_rights) const {
  return have_input_peer_chat(get_chat(chat_id), access_rights);
}

bool ContactsManager::have_input_peer_chat(const Chat *c, AccessRights access_rights) {
  if (c == nullptr) {
    LOG(DEBUG) << "Have no basic group";
    return false;
  }
  if (access_rights == AccessRights::Know) {
    return true;
  }
  if (access_rights == AccessRights::Read) {
    return true;
  }
  if (c->status.is_left()) {
    LOG(DEBUG) << "Have left basic group";
    return false;
  }
  if (access_rights == AccessRights::Write && !c->is_active) {
    LOG(DEBUG) << "Have inactive basic group";
    return false;
  }
  return true;
}

tl_object_ptr<telegram_api::InputPeer> ContactsManager::get_input_peer_chat(ChatId chat_id,
                                                                            AccessRights access_rights) const {
  auto c = get_chat(chat_id);
  if (!have_input_peer_chat(c, access_rights)) {
    return nullptr;
  }

  return make_tl_object<telegram_api::inputPeerChat>(chat_id.get());
}

bool ContactsManager::have_input_peer_channel(ChannelId channel_id, AccessRights access_rights) const {
  const Channel *c = get_channel(channel_id);
  return have_input_peer_channel(c, channel_id, access_rights);
}

tl_object_ptr<telegram_api::InputPeer> ContactsManager::get_input_peer_channel(ChannelId channel_id,
                                                                               AccessRights access_rights) const {
  const Channel *c = get_channel(channel_id);
  if (!have_input_peer_channel(c, channel_id, access_rights)) {
    if (c == nullptr && td_->auth_manager_->is_bot() && channel_id.is_valid()) {
      return make_tl_object<telegram_api::inputPeerChannel>(channel_id.get(), 0);
    }
    return nullptr;
  }

  return make_tl_object<telegram_api::inputPeerChannel>(channel_id.get(), c->access_hash);
}

bool ContactsManager::have_input_peer_channel(const Channel *c, ChannelId channel_id, AccessRights access_rights,
                                              bool from_linked) const {
  if (c == nullptr) {
    LOG(DEBUG) << "Have no " << channel_id;
    return false;
  }
  if (access_rights == AccessRights::Know) {
    return true;
  }
  if (c->status.is_administrator()) {
    return true;
  }
  if (c->status.is_banned()) {
    LOG(DEBUG) << "Was banned in " << channel_id;
    return false;
  }
  if (c->status.is_member()) {
    return true;
  }

  bool is_public = is_channel_public(c);
  if (access_rights == AccessRights::Read) {
    if (is_public) {
      return true;
    }
    if (!from_linked && c->has_linked_channel) {
      auto linked_channel_id = get_linked_channel_id(channel_id);
      if (linked_channel_id.is_valid() && have_channel(linked_channel_id)) {
        if (have_input_peer_channel(get_channel(linked_channel_id), linked_channel_id, access_rights, true)) {
          return true;
        }
      } else {
        return true;
      }
    }
    if (!from_linked && dialog_access_by_invite_link_.count(DialogId(channel_id))) {
      return true;
    }
  } else {
    if (!from_linked && c->is_megagroup && !td_->auth_manager_->is_bot() && c->has_linked_channel) {
      auto linked_channel_id = get_linked_channel_id(channel_id);
      if (linked_channel_id.is_valid() && (is_public || have_channel(linked_channel_id))) {
        return is_public ||
               have_input_peer_channel(get_channel(linked_channel_id), linked_channel_id, AccessRights::Read, true);
      } else {
        return true;
      }
    }
  }
  LOG(DEBUG) << "Have no access to " << channel_id;
  return false;
}

bool ContactsManager::have_input_encrypted_peer(SecretChatId secret_chat_id, AccessRights access_rights) const {
  return have_input_encrypted_peer(get_secret_chat(secret_chat_id), access_rights);
}

bool ContactsManager::have_input_encrypted_peer(const SecretChat *secret_chat, AccessRights access_rights) {
  if (secret_chat == nullptr) {
    LOG(DEBUG) << "Have no secret chat";
    return false;
  }
  if (access_rights == AccessRights::Know) {
    return true;
  }
  if (access_rights == AccessRights::Read) {
    return true;
  }
  return secret_chat->state == SecretChatState::Active;
}

tl_object_ptr<telegram_api::inputEncryptedChat> ContactsManager::get_input_encrypted_chat(
    SecretChatId secret_chat_id, AccessRights access_rights) const {
  auto sc = get_secret_chat(secret_chat_id);
  if (!have_input_encrypted_peer(sc, access_rights)) {
    return nullptr;
  }

  return make_tl_object<telegram_api::inputEncryptedChat>(secret_chat_id.get(), sc->access_hash);
}

void ContactsManager::apply_pending_user_photo(User *u, UserId user_id) {
  if (u == nullptr || u->is_photo_inited) {
    return;
  }

  auto it = pending_user_photos_.find(user_id);
  if (it != pending_user_photos_.end()) {
    do_update_user_photo(u, user_id, std::move(it->second), "apply_pending_user_photo");
    pending_user_photos_.erase(it);
    update_user(u, user_id);
  }
}

const DialogPhoto *ContactsManager::get_user_dialog_photo(UserId user_id) {
  auto u = get_user(user_id);
  if (u == nullptr) {
    return nullptr;
  }

  apply_pending_user_photo(u, user_id);
  return &u->photo;
}

const DialogPhoto *ContactsManager::get_chat_dialog_photo(ChatId chat_id) const {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return nullptr;
  }
  return &c->photo;
}

const DialogPhoto *ContactsManager::get_channel_dialog_photo(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    auto min_channel = get_min_channel(channel_id);
    if (min_channel != nullptr) {
      return &min_channel->photo_;
    }
    return nullptr;
  }
  return &c->photo;
}

const DialogPhoto *ContactsManager::get_secret_chat_dialog_photo(SecretChatId secret_chat_id) {
  auto c = get_secret_chat(secret_chat_id);
  if (c == nullptr) {
    return nullptr;
  }
  return get_user_dialog_photo(c->user_id);
}

string ContactsManager::get_user_title(UserId user_id) const {
  auto u = get_user(user_id);
  if (u == nullptr) {
    return string();
  }
  if (u->last_name.empty()) {
    return u->first_name;
  }
  if (u->first_name.empty()) {
    return u->last_name;
  }
  return PSTRING() << u->first_name << ' ' << u->last_name;
}

string ContactsManager::get_chat_title(ChatId chat_id) const {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return string();
  }
  return c->title;
}

string ContactsManager::get_channel_title(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    auto min_channel = get_min_channel(channel_id);
    if (min_channel != nullptr) {
      return min_channel->title_;
    }
    return string();
  }
  return c->title;
}

string ContactsManager::get_secret_chat_title(SecretChatId secret_chat_id) const {
  auto c = get_secret_chat(secret_chat_id);
  if (c == nullptr) {
    return string();
  }
  return get_user_title(c->user_id);
}

RestrictedRights ContactsManager::get_user_default_permissions(UserId user_id) const {
  auto u = get_user(user_id);
  if (u == nullptr || user_id == get_replies_bot_user_id()) {
    return RestrictedRights(false, false, false, false, false, false, false, false, false, false, u != nullptr);
  }
  return RestrictedRights(true, true, true, true, true, true, true, true, false, false, true);
}

RestrictedRights ContactsManager::get_chat_default_permissions(ChatId chat_id) const {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return RestrictedRights(false, false, false, false, false, false, false, false, false, false, false);
  }
  return c->default_permissions;
}

RestrictedRights ContactsManager::get_channel_default_permissions(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return RestrictedRights(false, false, false, false, false, false, false, false, false, false, false);
  }
  return c->default_permissions;
}

RestrictedRights ContactsManager::get_secret_chat_default_permissions(SecretChatId secret_chat_id) const {
  auto c = get_secret_chat(secret_chat_id);
  if (c == nullptr) {
    return RestrictedRights(false, false, false, false, false, false, false, false, false, false, false);
  }
  return RestrictedRights(true, true, true, true, true, true, true, true, false, false, false);
}

bool ContactsManager::get_chat_has_protected_content(ChatId chat_id) const {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return false;
  }
  return c->noforwards;
}

bool ContactsManager::get_channel_has_protected_content(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return false;
  }
  return c->noforwards;
}

string ContactsManager::get_user_private_forward_name(UserId user_id) {
  auto user_full = get_user_full_force(user_id);
  if (user_full != nullptr) {
    return user_full->private_forward_name;
  }
  return string();
}

bool ContactsManager::get_user_voice_messages_forbidden(UserId user_id) const {
  if (!is_user_premium(user_id)) {
    return false;
  }
  auto user_full = get_user_full(user_id);
  if (user_full != nullptr) {
    return user_full->voice_messages_forbidden;
  }
  return false;
}

string ContactsManager::get_dialog_about(DialogId dialog_id) {
  switch (dialog_id.get_type()) {
    case DialogType::User: {
      auto user_full = get_user_full_force(dialog_id.get_user_id());
      if (user_full != nullptr) {
        return user_full->about;
      }
      break;
    }
    case DialogType::Chat: {
      auto chat_full = get_chat_full_force(dialog_id.get_chat_id(), "get_dialog_about");
      if (chat_full != nullptr) {
        return chat_full->description;
      }
      break;
    }
    case DialogType::Channel: {
      auto channel_full = get_channel_full_force(dialog_id.get_channel_id(), false, "get_dialog_about");
      if (channel_full != nullptr) {
        return channel_full->description;
      }
      break;
    }
    case DialogType::SecretChat: {
      auto user_full = get_user_full_force(get_secret_chat_user_id(dialog_id.get_secret_chat_id()));
      if (user_full != nullptr) {
        return user_full->about;
      }
      break;
    }
    case DialogType::None:
    default:
      UNREACHABLE();
  }
  return string();
}

int32 ContactsManager::get_secret_chat_date(SecretChatId secret_chat_id) const {
  auto c = get_secret_chat(secret_chat_id);
  if (c == nullptr) {
    return 0;
  }
  return c->date;
}

int32 ContactsManager::get_secret_chat_ttl(SecretChatId secret_chat_id) const {
  auto c = get_secret_chat(secret_chat_id);
  if (c == nullptr) {
    return 0;
  }
  return c->ttl;
}

string ContactsManager::get_user_username(UserId user_id) const {
  if (!user_id.is_valid()) {
    return string();
  }

  auto u = get_user(user_id);
  if (u == nullptr) {
    return string();
  }
  return u->username;
}

string ContactsManager::get_secret_chat_username(SecretChatId secret_chat_id) const {
  auto c = get_secret_chat(secret_chat_id);
  if (c == nullptr) {
    return string();
  }
  return get_user_username(c->user_id);
}

string ContactsManager::get_channel_username(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return string();
  }
  return c->username;
}

UserId ContactsManager::get_secret_chat_user_id(SecretChatId secret_chat_id) const {
  auto c = get_secret_chat(secret_chat_id);
  if (c == nullptr) {
    return UserId();
  }
  return c->user_id;
}

bool ContactsManager::get_secret_chat_is_outbound(SecretChatId secret_chat_id) const {
  auto c = get_secret_chat(secret_chat_id);
  if (c == nullptr) {
    return false;
  }
  return c->is_outbound;
}

SecretChatState ContactsManager::get_secret_chat_state(SecretChatId secret_chat_id) const {
  auto c = get_secret_chat(secret_chat_id);
  if (c == nullptr) {
    return SecretChatState::Unknown;
  }
  return c->state;
}

int32 ContactsManager::get_secret_chat_layer(SecretChatId secret_chat_id) const {
  auto c = get_secret_chat(secret_chat_id);
  if (c == nullptr) {
    return 0;
  }
  return c->layer;
}

FolderId ContactsManager::get_secret_chat_initial_folder_id(SecretChatId secret_chat_id) const {
  auto c = get_secret_chat(secret_chat_id);
  if (c == nullptr) {
    return FolderId::main();
  }
  return c->initial_folder_id;
}

UserId ContactsManager::get_my_id() const {
  LOG_IF(ERROR, !my_id_.is_valid()) << "Wrong or unknown my ID returned";
  return my_id_;
}

void ContactsManager::set_my_id(UserId my_id) {
  UserId my_old_id = my_id_;
  if (my_old_id.is_valid() && my_old_id != my_id) {
    LOG(ERROR) << "Already know that me is " << my_old_id << " but received userSelf with " << my_id;
  }
  if (!my_id.is_valid()) {
    LOG(ERROR) << "Receive invalid my ID " << my_id;
    return;
  }
  if (my_old_id != my_id) {
    my_id_ = my_id;
    G()->td_db()->get_binlog_pmc()->set("my_id", to_string(my_id.get()));
    td_->option_manager_->set_option_integer("my_id", my_id_.get());
    G()->td_db()->get_binlog_pmc()->force_sync(Promise<Unit>());
  }
}

void ContactsManager::set_my_online_status(bool is_online, bool send_update, bool is_local) {
  if (td_->auth_manager_->is_bot()) {
    return;  // just in case
  }

  auto my_id = get_my_id();
  User *u = get_user_force(my_id);
  if (u != nullptr) {
    int32 new_online;
    int32 now = G()->unix_time();
    if (is_online) {
      new_online = now + 300;
    } else {
      new_online = now - 1;
    }

    if (is_local) {
      LOG(INFO) << "Update my local online from " << my_was_online_local_ << " to " << new_online;
      if (!is_online) {
        new_online = min(new_online, u->was_online);
      }
      if (new_online != my_was_online_local_) {
        my_was_online_local_ = new_online;
        u->is_status_changed = true;
        u->is_online_status_changed = true;
      }
    } else {
      if (my_was_online_local_ != 0 || new_online != u->was_online) {
        LOG(INFO) << "Update my online from " << u->was_online << " to " << new_online;
        my_was_online_local_ = 0;
        u->was_online = new_online;
        u->is_status_changed = true;
        u->is_online_status_changed = true;
      }
    }

    if (was_online_local_ != new_online) {
      was_online_local_ = new_online;
      VLOG(notifications) << "Set was_online_local to " << was_online_local_;
      G()->td_db()->get_binlog_pmc()->set("my_was_online_local", to_string(was_online_local_));
    }

    if (send_update) {
      update_user(u, my_id);
    }
  }
}

ContactsManager::MyOnlineStatusInfo ContactsManager::get_my_online_status() const {
  MyOnlineStatusInfo status_info;
  status_info.is_online_local = td_->is_online();
  status_info.is_online_remote = was_online_remote_ > G()->unix_time_cached();
  status_info.was_online_local = was_online_local_;
  status_info.was_online_remote = was_online_remote_;

  return status_info;
}

UserId ContactsManager::get_service_notifications_user_id() {
  return UserId(static_cast<int64>(777000));
}

UserId ContactsManager::add_service_notifications_user() {
  auto user_id = get_service_notifications_user_id();
  if (!have_user_force(user_id)) {
    LOG(FATAL) << "Failed to load service notification user";
  }
  return user_id;
}

UserId ContactsManager::get_replies_bot_user_id() {
  return UserId(static_cast<int64>(G()->is_test_dc() ? 708513 : 1271266957));
}

UserId ContactsManager::get_anonymous_bot_user_id() {
  return UserId(static_cast<int64>(G()->is_test_dc() ? 552888 : 1087968824));
}

UserId ContactsManager::get_channel_bot_user_id() {
  return UserId(static_cast<int64>(G()->is_test_dc() ? 936174 : 136817688));
}

UserId ContactsManager::add_anonymous_bot_user() {
  auto user_id = get_anonymous_bot_user_id();
  if (!have_user_force(user_id)) {
    LOG(FATAL) << "Failed to load anonymous bot user";
  }
  return user_id;
}

UserId ContactsManager::add_channel_bot_user() {
  auto user_id = get_channel_bot_user_id();
  if (!have_user_force(user_id)) {
    LOG(FATAL) << "Failed to load channel bot user";
  }
  return user_id;
}

void ContactsManager::check_dialog_username(DialogId dialog_id, const string &username,
                                            Promise<CheckDialogUsernameResult> &&promise) {
  if (dialog_id != DialogId() && !dialog_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User: {
      if (dialog_id.get_user_id() != get_my_id()) {
        return promise.set_error(Status::Error(400, "Can't check username for private chat with other user"));
      }
      break;
    }
    case DialogType::Channel: {
      auto c = get_channel(dialog_id.get_channel_id());
      if (c == nullptr) {
        return promise.set_error(Status::Error(400, "Chat not found"));
      }
      if (!get_channel_status(c).is_creator()) {
        return promise.set_error(Status::Error(400, "Not enough rights to change username"));
      }

      if (username == c->username) {
        return promise.set_value(CheckDialogUsernameResult::Ok);
      }
      break;
    }
    case DialogType::None:
      break;
    case DialogType::Chat:
    case DialogType::SecretChat:
      if (username.empty()) {
        return promise.set_value(CheckDialogUsernameResult::Ok);
      }
      return promise.set_error(Status::Error(400, "Chat can't have username"));
    default:
      UNREACHABLE();
      return;
  }

  if (username.empty()) {
    return promise.set_value(CheckDialogUsernameResult::Ok);
  }
  if (!is_valid_username(username)) {
    return promise.set_value(CheckDialogUsernameResult::Invalid);
  }

  auto request_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<bool> result) mutable {
    if (result.is_error()) {
      auto error = result.move_as_error();
      if (error.message() == "CHANNEL_PUBLIC_GROUP_NA") {
        return promise.set_value(CheckDialogUsernameResult::PublicGroupsUnavailable);
      }
      if (error.message() == "CHANNELS_ADMIN_PUBLIC_TOO_MUCH") {
        return promise.set_value(CheckDialogUsernameResult::PublicDialogsTooMuch);
      }
      if (error.message() == "USERNAME_INVALID") {
        return promise.set_value(CheckDialogUsernameResult::Invalid);
      }
      return promise.set_error(std::move(error));
    }

    promise.set_value(result.ok() ? CheckDialogUsernameResult::Ok : CheckDialogUsernameResult::Occupied);
  });

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return td_->create_handler<CheckUsernameQuery>(std::move(request_promise))->send(username);
    case DialogType::Channel:
      return td_->create_handler<CheckChannelUsernameQuery>(std::move(request_promise))
          ->send(dialog_id.get_channel_id(), username);
    case DialogType::None:
      return td_->create_handler<CheckChannelUsernameQuery>(std::move(request_promise))->send(ChannelId(), username);
    case DialogType::Chat:
    case DialogType::SecretChat:
    default:
      UNREACHABLE();
  }
}

td_api::object_ptr<td_api::CheckChatUsernameResult> ContactsManager::get_check_chat_username_result_object(
    CheckDialogUsernameResult result) {
  switch (result) {
    case CheckDialogUsernameResult::Ok:
      return td_api::make_object<td_api::checkChatUsernameResultOk>();
    case CheckDialogUsernameResult::Invalid:
      return td_api::make_object<td_api::checkChatUsernameResultUsernameInvalid>();
    case CheckDialogUsernameResult::Occupied:
      return td_api::make_object<td_api::checkChatUsernameResultUsernameOccupied>();
    case CheckDialogUsernameResult::PublicDialogsTooMuch:
      return td_api::make_object<td_api::checkChatUsernameResultPublicChatsTooMuch>();
    case CheckDialogUsernameResult::PublicGroupsUnavailable:
      return td_api::make_object<td_api::checkChatUsernameResultPublicGroupsUnavailable>();
    default:
      UNREACHABLE();
      return nullptr;
  }
}

bool ContactsManager::is_valid_username(const string &username) {
  if (username.size() < 5 || username.size() > 32) {
    return false;
  }
  if (!is_alpha(username[0])) {
    return false;
  }
  for (auto c : username) {
    if (!is_alpha(c) && !is_digit(c) && c != '_') {
      return false;
    }
  }
  if (username.back() == '_') {
    return false;
  }
  for (size_t i = 1; i < username.size(); i++) {
    if (username[i - 1] == '_' && username[i] == '_') {
      return false;
    }
  }
  if (username.find("admin") == 0 || username.find("telegram") == 0 || username.find("support") == 0 ||
      username.find("security") == 0 || username.find("settings") == 0 || username.find("contacts") == 0 ||
      username.find("service") == 0 || username.find("telegraph") == 0) {
    return false;
  }
  return true;
}

int32 ContactsManager::get_user_was_online(const User *u, UserId user_id) const {
  if (u == nullptr || u->is_deleted) {
    return 0;
  }

  int32 was_online = u->was_online;
  if (user_id == get_my_id()) {
    if (my_was_online_local_ != 0) {
      was_online = my_was_online_local_;
    }
  } else {
    if (u->local_was_online > 0 && u->local_was_online > was_online && u->local_was_online > G()->unix_time_cached()) {
      was_online = u->local_was_online;
    }
  }
  return was_online;
}

void ContactsManager::load_contacts(Promise<Unit> &&promise) {
  if (td_->auth_manager_->is_bot()) {
    are_contacts_loaded_ = true;
    saved_contact_count_ = 0;
  }
  if (are_contacts_loaded_ && saved_contact_count_ != -1) {
    LOG(INFO) << "Contacts are already loaded";
    promise.set_value(Unit());
    return;
  }
  load_contacts_queries_.push_back(std::move(promise));
  if (load_contacts_queries_.size() == 1u) {
    if (G()->parameters().use_chat_info_db && next_contacts_sync_date_ > 0 && saved_contact_count_ != -1) {
      LOG(INFO) << "Load contacts from database";
      G()->td_db()->get_sqlite_pmc()->get(
          "user_contacts", PromiseCreator::lambda([](string value) {
            send_closure(G()->contacts_manager(), &ContactsManager::on_load_contacts_from_database, std::move(value));
          }));
    } else {
      LOG(INFO) << "Load contacts from server";
      reload_contacts(true);
    }
  } else {
    LOG(INFO) << "Load contacts request has already been sent";
  }
}

int64 ContactsManager::get_contacts_hash() {
  if (!are_contacts_loaded_) {
    return 0;
  }

  vector<int64> user_ids = contacts_hints_.search_empty(100000).second;
  CHECK(std::is_sorted(user_ids.begin(), user_ids.end()));
  auto my_id = get_my_id();
  const User *u = get_user_force(my_id);
  if (u != nullptr && u->is_contact) {
    user_ids.insert(std::upper_bound(user_ids.begin(), user_ids.end(), my_id.get()), my_id.get());
  }

  vector<uint64> numbers;
  numbers.reserve(user_ids.size() + 1);
  numbers.push_back(saved_contact_count_);
  for (auto user_id : user_ids) {
    numbers.push_back(user_id);
  }
  return get_vector_hash(numbers);
}

void ContactsManager::reload_contacts(bool force) {
  if (!td_->auth_manager_->is_bot() && next_contacts_sync_date_ != std::numeric_limits<int32>::max() &&
      (next_contacts_sync_date_ < G()->unix_time() || force)) {
    next_contacts_sync_date_ = std::numeric_limits<int32>::max();
    td_->create_handler<GetContactsQuery>()->send(get_contacts_hash());
  }
}

void ContactsManager::add_contact(Contact contact, bool share_phone_number, Promise<Unit> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  if (!are_contacts_loaded_) {
    load_contacts(PromiseCreator::lambda([actor_id = actor_id(this), contact = std::move(contact), share_phone_number,
                                          promise = std::move(promise)](Result<Unit> &&) mutable {
      send_closure(actor_id, &ContactsManager::add_contact, std::move(contact), share_phone_number, std::move(promise));
    }));
    return;
  }

  LOG(INFO) << "Add " << contact << " with share_phone_number = " << share_phone_number;

  auto user_id = contact.get_user_id();
  auto r_input_user = get_input_user(user_id);
  if (r_input_user.is_error()) {
    return promise.set_error(r_input_user.move_as_error());
  }

  td_->create_handler<AddContactQuery>(std::move(promise))
      ->send(user_id, r_input_user.move_as_ok(), contact, share_phone_number);
}

std::pair<vector<UserId>, vector<int32>> ContactsManager::import_contacts(const vector<Contact> &contacts,
                                                                          int64 &random_id, Promise<Unit> &&promise) {
  if (!are_contacts_loaded_) {
    load_contacts(std::move(promise));
    return {};
  }

  LOG(INFO) << "Asked to import " << contacts.size() << " contacts with random_id = " << random_id;
  if (random_id != 0) {
    // request has already been sent before
    auto it = imported_contacts_.find(random_id);
    CHECK(it != imported_contacts_.end());
    auto result = std::move(it->second);
    imported_contacts_.erase(it);

    promise.set_value(Unit());
    return result;
  }

  do {
    random_id = Random::secure_int64();
  } while (random_id == 0 || random_id == 1 || imported_contacts_.count(random_id) > 0);
  imported_contacts_[random_id];  // reserve place for result

  do_import_contacts(contacts, random_id, std::move(promise));
  return {};
}

void ContactsManager::do_import_contacts(vector<Contact> contacts, int64 random_id, Promise<Unit> &&promise) {
  size_t size = contacts.size();
  if (size == 0) {
    on_import_contacts_finished(random_id, {}, {});
    return promise.set_value(Unit());
  }

  vector<tl_object_ptr<telegram_api::inputPhoneContact>> input_phone_contacts;
  input_phone_contacts.reserve(size);
  for (size_t i = 0; i < size; i++) {
    input_phone_contacts.push_back(contacts[i].get_input_phone_contact(static_cast<int64>(i)));
  }

  auto task = make_unique<ImportContactsTask>();
  task->promise_ = std::move(promise);
  task->input_contacts_ = std::move(contacts);
  task->imported_user_ids_.resize(size);
  task->unimported_contact_invites_.resize(size);

  bool is_added = import_contact_tasks_.emplace(random_id, std::move(task)).second;
  CHECK(is_added);

  td_->create_handler<ImportContactsQuery>()->send(std::move(input_phone_contacts), random_id);
}

void ContactsManager::on_imported_contacts(int64 random_id,
                                           Result<tl_object_ptr<telegram_api::contacts_importedContacts>> result) {
  auto it = import_contact_tasks_.find(random_id);
  CHECK(it != import_contact_tasks_.end());
  CHECK(it->second != nullptr);

  auto task = it->second.get();
  if (result.is_error()) {
    auto promise = std::move(task->promise_);
    import_contact_tasks_.erase(it);
    return promise.set_error(result.move_as_error());
  }

  auto imported_contacts = result.move_as_ok();
  on_get_users(std::move(imported_contacts->users_), "on_imported_contacts");

  for (auto &imported_contact : imported_contacts->imported_) {
    int64 client_id = imported_contact->client_id_;
    if (client_id < 0 || client_id >= static_cast<int64>(task->imported_user_ids_.size())) {
      LOG(ERROR) << "Wrong client_id " << client_id << " returned";
      continue;
    }

    task->imported_user_ids_[static_cast<size_t>(client_id)] = UserId(imported_contact->user_id_);
  }
  for (auto &popular_contact : imported_contacts->popular_invites_) {
    int64 client_id = popular_contact->client_id_;
    if (client_id < 0 || client_id >= static_cast<int64>(task->unimported_contact_invites_.size())) {
      LOG(ERROR) << "Wrong client_id " << client_id << " returned";
      continue;
    }
    if (popular_contact->importers_ < 0) {
      LOG(ERROR) << "Wrong number of importers " << popular_contact->importers_ << " returned";
      continue;
    }

    task->unimported_contact_invites_[static_cast<size_t>(client_id)] = popular_contact->importers_;
  }

  if (!imported_contacts->retry_contacts_.empty()) {
    auto total_size = static_cast<int64>(task->input_contacts_.size());
    vector<tl_object_ptr<telegram_api::inputPhoneContact>> input_phone_contacts;
    input_phone_contacts.reserve(imported_contacts->retry_contacts_.size());
    for (auto &client_id : imported_contacts->retry_contacts_) {
      if (client_id < 0 || client_id >= total_size) {
        LOG(ERROR) << "Wrong client_id " << client_id << " returned";
        continue;
      }
      auto i = static_cast<size_t>(client_id);
      input_phone_contacts.push_back(task->input_contacts_[i].get_input_phone_contact(client_id));
    }
    td_->create_handler<ImportContactsQuery>()->send(std::move(input_phone_contacts), random_id);
    return;
  }

  auto promise = std::move(task->promise_);
  on_import_contacts_finished(random_id, std::move(task->imported_user_ids_),
                              std::move(task->unimported_contact_invites_));
  import_contact_tasks_.erase(it);
  promise.set_value(Unit());
}

void ContactsManager::remove_contacts(const vector<UserId> &user_ids, Promise<Unit> &&promise) {
  LOG(INFO) << "Delete contacts: " << format::as_array(user_ids);
  if (!are_contacts_loaded_) {
    load_contacts(std::move(promise));
    return;
  }

  vector<UserId> to_delete_user_ids;
  vector<tl_object_ptr<telegram_api::InputUser>> input_users;
  for (auto &user_id : user_ids) {
    const User *u = get_user(user_id);
    if (u != nullptr && u->is_contact) {
      auto r_input_user = get_input_user(user_id);
      if (r_input_user.is_ok()) {
        to_delete_user_ids.push_back(user_id);
        input_users.push_back(r_input_user.move_as_ok());
      }
    }
  }

  if (input_users.empty()) {
    return promise.set_value(Unit());
  }

  td_->create_handler<DeleteContactsQuery>(std::move(promise))->send(std::move(input_users));
}

void ContactsManager::remove_contacts_by_phone_number(vector<string> user_phone_numbers, vector<UserId> user_ids,
                                                      Promise<Unit> &&promise) {
  LOG(INFO) << "Delete contacts by phone number: " << format::as_array(user_phone_numbers);
  if (!are_contacts_loaded_) {
    load_contacts(std::move(promise));
    return;
  }

  td_->create_handler<DeleteContactsByPhoneNumberQuery>(std::move(promise))
      ->send(std::move(user_phone_numbers), std::move(user_ids));
}

int32 ContactsManager::get_imported_contact_count(Promise<Unit> &&promise) {
  LOG(INFO) << "Get imported contact count";

  if (!are_contacts_loaded_ || saved_contact_count_ == -1) {
    load_contacts(std::move(promise));
    return 0;
  }
  reload_contacts(false);

  promise.set_value(Unit());
  return saved_contact_count_;
}

void ContactsManager::load_imported_contacts(Promise<Unit> &&promise) {
  if (td_->auth_manager_->is_bot()) {
    are_imported_contacts_loaded_ = true;
  }
  if (are_imported_contacts_loaded_) {
    LOG(INFO) << "Imported contacts are already loaded";
    promise.set_value(Unit());
    return;
  }
  load_imported_contacts_queries_.push_back(std::move(promise));
  if (load_imported_contacts_queries_.size() == 1u) {
    if (G()->parameters().use_chat_info_db) {
      LOG(INFO) << "Load imported contacts from database";
      G()->td_db()->get_sqlite_pmc()->get(
          "user_imported_contacts", PromiseCreator::lambda([](string value) {
            send_closure_later(G()->contacts_manager(), &ContactsManager::on_load_imported_contacts_from_database,
                               std::move(value));
          }));
    } else {
      LOG(INFO) << "Have no previously imported contacts";
      send_closure_later(G()->contacts_manager(), &ContactsManager::on_load_imported_contacts_from_database, string());
    }
  } else {
    LOG(INFO) << "Load imported contacts request has already been sent";
  }
}

void ContactsManager::on_load_imported_contacts_from_database(string value) {
  if (G()->close_flag()) {
    return;
  }

  CHECK(!are_imported_contacts_loaded_);
  if (need_clear_imported_contacts_) {
    need_clear_imported_contacts_ = false;
    value.clear();
  }
  if (value.empty()) {
    CHECK(all_imported_contacts_.empty());
  } else {
    log_event_parse(all_imported_contacts_, value).ensure();
    LOG(INFO) << "Successfully loaded " << all_imported_contacts_.size() << " imported contacts from database";
  }

  load_imported_contact_users_multipromise_.add_promise(
      PromiseCreator::lambda([actor_id = actor_id(this)](Result<Unit> result) {
        if (result.is_ok()) {
          send_closure_later(actor_id, &ContactsManager::on_load_imported_contacts_finished);
        }
      }));

  auto lock_promise = load_imported_contact_users_multipromise_.get_promise();

  for (const auto &contact : all_imported_contacts_) {
    auto user_id = contact.get_user_id();
    if (user_id.is_valid()) {
      get_user(user_id, 3, load_imported_contact_users_multipromise_.get_promise());
    }
  }

  lock_promise.set_value(Unit());
}

void ContactsManager::on_load_imported_contacts_finished() {
  LOG(INFO) << "Finished to load " << all_imported_contacts_.size() << " imported contacts";

  for (const auto &contact : all_imported_contacts_) {
    get_user_id_object(contact.get_user_id(), "on_load_imported_contacts_finished");  // to ensure updateUser
  }

  if (need_clear_imported_contacts_) {
    need_clear_imported_contacts_ = false;
    all_imported_contacts_.clear();
  }
  are_imported_contacts_loaded_ = true;
  set_promises(load_imported_contacts_queries_);
}

std::pair<vector<UserId>, vector<int32>> ContactsManager::change_imported_contacts(vector<Contact> &contacts,
                                                                                   int64 &random_id,
                                                                                   Promise<Unit> &&promise) {
  if (!are_contacts_loaded_) {
    load_contacts(std::move(promise));
    return {};
  }
  if (!are_imported_contacts_loaded_) {
    load_imported_contacts(std::move(promise));
    return {};
  }

  LOG(INFO) << "Asked to change imported contacts to a list of " << contacts.size()
            << " contacts with random_id = " << random_id;
  if (random_id != 0) {
    // request has already been sent before
    if (need_clear_imported_contacts_) {
      need_clear_imported_contacts_ = false;
      all_imported_contacts_.clear();
      if (G()->parameters().use_chat_info_db) {
        G()->td_db()->get_sqlite_pmc()->erase("user_imported_contacts", Auto());
      }
      reload_contacts(true);
    }

    CHECK(are_imported_contacts_changing_);
    are_imported_contacts_changing_ = false;

    auto unimported_contact_invites = std::move(unimported_contact_invites_);
    unimported_contact_invites_.clear();

    auto imported_contact_user_ids = std::move(imported_contact_user_ids_);
    imported_contact_user_ids_.clear();

    promise.set_value(Unit());
    return {std::move(imported_contact_user_ids), std::move(unimported_contact_invites)};
  }

  if (are_imported_contacts_changing_) {
    promise.set_error(Status::Error(400, "ChangeImportedContacts can be called only once at the same time"));
    return {};
  }

  vector<size_t> new_contacts_unique_id(contacts.size());
  vector<Contact> unique_new_contacts;
  unique_new_contacts.reserve(contacts.size());
  std::unordered_map<Contact, size_t, ContactHash, ContactEqual> different_new_contacts;
  std::unordered_set<string> different_new_phone_numbers;
  size_t unique_size = 0;
  for (size_t i = 0; i < contacts.size(); i++) {
    auto it_success = different_new_contacts.emplace(std::move(contacts[i]), unique_size);
    new_contacts_unique_id[i] = it_success.first->second;
    if (it_success.second) {
      unique_new_contacts.push_back(it_success.first->first);
      different_new_phone_numbers.insert(unique_new_contacts.back().get_phone_number());
      unique_size++;
    }
  }

  vector<string> to_delete;
  vector<UserId> to_delete_user_ids;
  for (auto &old_contact : all_imported_contacts_) {
    auto user_id = old_contact.get_user_id();
    auto it = different_new_contacts.find(old_contact);
    if (it == different_new_contacts.end()) {
      auto phone_number = old_contact.get_phone_number();
      if (different_new_phone_numbers.count(phone_number) == 0) {
        to_delete.push_back(std::move(phone_number));
        if (user_id.is_valid()) {
          to_delete_user_ids.push_back(user_id);
        }
      }
    } else {
      unique_new_contacts[it->second].set_user_id(user_id);
      different_new_contacts.erase(it);
    }
  }
  std::pair<vector<size_t>, vector<Contact>> to_add;
  for (auto &new_contact : different_new_contacts) {
    to_add.first.push_back(new_contact.second);
    to_add.second.push_back(new_contact.first);
  }

  if (to_add.first.empty() && to_delete.empty()) {
    for (size_t i = 0; i < contacts.size(); i++) {
      auto unique_id = new_contacts_unique_id[i];
      contacts[i].set_user_id(unique_new_contacts[unique_id].get_user_id());
    }

    promise.set_value(Unit());
    return {transform(contacts, [&](const Contact &contact) { return contact.get_user_id(); }),
            vector<int32>(contacts.size())};
  }

  are_imported_contacts_changing_ = true;
  random_id = 1;

  remove_contacts_by_phone_number(
      std::move(to_delete), std::move(to_delete_user_ids),
      PromiseCreator::lambda([new_contacts = std::move(unique_new_contacts),
                              new_contacts_unique_id = std::move(new_contacts_unique_id), to_add = std::move(to_add),
                              promise = std::move(promise)](Result<> result) mutable {
        if (result.is_ok()) {
          send_closure_later(G()->contacts_manager(), &ContactsManager::on_clear_imported_contacts,
                             std::move(new_contacts), std::move(new_contacts_unique_id), std::move(to_add),
                             std::move(promise));
        } else {
          promise.set_error(result.move_as_error());
        }
      }));
  return {};
}

void ContactsManager::on_clear_imported_contacts(vector<Contact> &&contacts, vector<size_t> contacts_unique_id,
                                                 std::pair<vector<size_t>, vector<Contact>> &&to_add,
                                                 Promise<Unit> &&promise) {
  LOG(INFO) << "Add " << to_add.first.size() << " contacts";
  next_all_imported_contacts_ = std::move(contacts);
  imported_contacts_unique_id_ = std::move(contacts_unique_id);
  imported_contacts_pos_ = std::move(to_add.first);

  do_import_contacts(std::move(to_add.second), 1, std::move(promise));
}

void ContactsManager::clear_imported_contacts(Promise<Unit> &&promise) {
  LOG(INFO) << "Delete imported contacts";

  if (saved_contact_count_ == 0) {
    promise.set_value(Unit());
    return;
  }

  td_->create_handler<ResetContactsQuery>(std::move(promise))->send();
}

void ContactsManager::on_update_contacts_reset() {
  /*
  UserId my_id = get_my_id();
  users_.foreach([&](const UserId &user_id, unique_ptr<User> &u) {
    if (u->is_contact) {
      LOG(INFO) << "Drop contact with " << user_id;
      if (user_id != my_id) {
        CHECK(contacts_hints_.has_key(user_id.get()));
      }
      on_update_user_is_contact(u, user_id, false, false);
      CHECK(u->is_is_contact_changed);
      u->cache_version = 0;
      u->is_repaired = false;
      update_user(u, user_id);
      CHECK(!u->is_contact);
      if (user_id != my_id) {
        CHECK(!contacts_hints_.has_key(user_id.get()));
      }
    }
  });
  */

  saved_contact_count_ = 0;
  if (G()->parameters().use_chat_info_db) {
    G()->td_db()->get_binlog_pmc()->set("saved_contact_count", "0");
    G()->td_db()->get_sqlite_pmc()->erase("user_imported_contacts", Auto());
  }
  if (!are_imported_contacts_loaded_) {
    CHECK(all_imported_contacts_.empty());
    if (load_imported_contacts_queries_.empty()) {
      LOG(INFO) << "Imported contacts was never loaded, just clear them";
    } else {
      LOG(INFO) << "Imported contacts are being loaded, clear them also when they will be loaded";
      need_clear_imported_contacts_ = true;
    }
  } else {
    if (!are_imported_contacts_changing_) {
      LOG(INFO) << "Imported contacts was loaded, but aren't changing now, just clear them";
      all_imported_contacts_.clear();
    } else {
      LOG(INFO) << "Imported contacts are changing now, clear them also after they will be loaded";
      need_clear_imported_contacts_ = true;
    }
  }
  reload_contacts(true);
}

std::pair<int32, vector<UserId>> ContactsManager::search_contacts(const string &query, int32 limit,
                                                                  Promise<Unit> &&promise) {
  LOG(INFO) << "Search contacts with query = \"" << query << "\" and limit = " << limit;

  if (limit < 0) {
    promise.set_error(Status::Error(400, "Limit must be non-negative"));
    return {};
  }

  if (!are_contacts_loaded_) {
    load_contacts(std::move(promise));
    return {};
  }
  reload_contacts(false);

  std::pair<size_t, vector<int64>> result;
  if (query.empty()) {
    result = contacts_hints_.search_empty(limit);
  } else {
    result = contacts_hints_.search(query, limit);
  }

  vector<UserId> user_ids;
  user_ids.reserve(result.second.size());
  for (auto key : result.second) {
    user_ids.emplace_back(key);
  }

  promise.set_value(Unit());
  return {narrow_cast<int32>(result.first), std::move(user_ids)};
}

UserId ContactsManager::search_user_by_phone_number(string phone_number, Promise<Unit> &&promise) {
  clean_phone_number(phone_number);
  if (phone_number.empty()) {
    promise.set_error(Status::Error(200, "Phone number is invalid"));
    return UserId();
  }

  auto it = resolved_phone_numbers_.find(phone_number);
  if (it != resolved_phone_numbers_.end()) {
    promise.set_value(Unit());
    return it->second;
  }

  td_->create_handler<ResolvePhoneQuery>(std::move(promise))->send(phone_number);
  return UserId();
}

void ContactsManager::on_resolved_phone_number(const string &phone_number, UserId user_id) {
  if (!user_id.is_valid()) {
    resolved_phone_numbers_.emplace(phone_number, UserId());  // negative cache
    return;
  }

  auto it = resolved_phone_numbers_.find(phone_number);
  if (it != resolved_phone_numbers_.end()) {
    if (it->second != user_id) {
      LOG(WARNING) << "Resolve phone number \"" << phone_number << "\" to " << user_id << ", but have it in "
                   << it->second;
      it->second = user_id;
    }
    return;
  }

  LOG(ERROR) << "Resolve phone number \"" << phone_number << "\" to " << user_id << ", but doesn't have it";
  resolved_phone_numbers_[phone_number] = user_id;  // always update cached value
}

void ContactsManager::share_phone_number(UserId user_id, Promise<Unit> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  if (!are_contacts_loaded_) {
    load_contacts(PromiseCreator::lambda(
        [actor_id = actor_id(this), user_id, promise = std::move(promise)](Result<Unit> &&) mutable {
          send_closure(actor_id, &ContactsManager::share_phone_number, user_id, std::move(promise));
        }));
    return;
  }

  LOG(INFO) << "Share phone number with " << user_id;
  auto r_input_user = get_input_user(user_id);
  if (r_input_user.is_error()) {
    return promise.set_error(r_input_user.move_as_error());
  }

  td_->messages_manager_->hide_dialog_action_bar(DialogId(user_id));

  td_->create_handler<AcceptContactQuery>(std::move(promise))->send(user_id, r_input_user.move_as_ok());
}

void ContactsManager::search_dialogs_nearby(const Location &location,
                                            Promise<td_api::object_ptr<td_api::chatsNearby>> &&promise) {
  if (location.empty()) {
    return promise.set_error(Status::Error(400, "Invalid location specified"));
  }
  last_user_location_ = location;
  try_send_set_location_visibility_query();

  auto query_promise = PromiseCreator::lambda([actor_id = actor_id(this), promise = std::move(promise)](
                                                  Result<tl_object_ptr<telegram_api::Updates>> result) mutable {
    send_closure(actor_id, &ContactsManager::on_get_dialogs_nearby, std::move(result), std::move(promise));
  });
  td_->create_handler<SearchDialogsNearbyQuery>(std::move(query_promise))->send(location, false, -1);
}

vector<td_api::object_ptr<td_api::chatNearby>> ContactsManager::get_chats_nearby_object(
    const vector<DialogNearby> &dialogs_nearby) {
  return transform(dialogs_nearby, [](const DialogNearby &dialog_nearby) {
    return td_api::make_object<td_api::chatNearby>(dialog_nearby.dialog_id.get(), dialog_nearby.distance);
  });
}

void ContactsManager::send_update_users_nearby() const {
  send_closure(G()->td(), &Td::send_update,
               td_api::make_object<td_api::updateUsersNearby>(get_chats_nearby_object(users_nearby_)));
}

void ContactsManager::on_get_dialogs_nearby(Result<tl_object_ptr<telegram_api::Updates>> result,
                                            Promise<td_api::object_ptr<td_api::chatsNearby>> &&promise) {
  if (result.is_error()) {
    return promise.set_error(result.move_as_error());
  }

  auto updates_ptr = result.move_as_ok();
  if (updates_ptr->get_id() != telegram_api::updates::ID) {
    LOG(ERROR) << "Receive " << oneline(to_string(*updates_ptr)) << " instead of updates";
    return promise.set_error(Status::Error(500, "Receive unsupported response from the server"));
  }

  auto update = telegram_api::move_object_as<telegram_api::updates>(updates_ptr);
  LOG(INFO) << "Receive chats nearby in " << to_string(update);

  on_get_users(std::move(update->users_), "on_get_dialogs_nearby");
  on_get_chats(std::move(update->chats_), "on_get_dialogs_nearby");

  for (auto &dialog_nearby : users_nearby_) {
    user_nearby_timeout_.cancel_timeout(dialog_nearby.dialog_id.get_user_id().get());
  }
  auto old_users_nearby = std::move(users_nearby_);
  users_nearby_.clear();
  channels_nearby_.clear();
  int32 location_visibility_expire_date = 0;
  for (auto &update_ptr : update->updates_) {
    if (update_ptr->get_id() != telegram_api::updatePeerLocated::ID) {
      LOG(ERROR) << "Receive unexpected " << to_string(update);
      continue;
    }

    auto expire_date = on_update_peer_located(
        std::move(static_cast<telegram_api::updatePeerLocated *>(update_ptr.get())->peers_), false);
    if (expire_date != -1) {
      location_visibility_expire_date = expire_date;
    }
  }
  if (location_visibility_expire_date != location_visibility_expire_date_) {
    set_location_visibility_expire_date(location_visibility_expire_date);
    update_is_location_visible();
  }

  std::sort(users_nearby_.begin(), users_nearby_.end());
  if (old_users_nearby != users_nearby_) {
    send_update_users_nearby();  // for other clients connected to the same TDLib instance
  }
  promise.set_value(td_api::make_object<td_api::chatsNearby>(get_chats_nearby_object(users_nearby_),
                                                             get_chats_nearby_object(channels_nearby_)));
}

void ContactsManager::set_location(const Location &location, Promise<Unit> &&promise) {
  if (location.empty()) {
    return promise.set_error(Status::Error(400, "Invalid location specified"));
  }
  last_user_location_ = location;
  try_send_set_location_visibility_query();

  auto query_promise = PromiseCreator::lambda(
      [promise = std::move(promise)](Result<tl_object_ptr<telegram_api::Updates>> result) mutable {
        promise.set_value(Unit());
      });
  td_->create_handler<SearchDialogsNearbyQuery>(std::move(query_promise))->send(location, true, -1);
}

void ContactsManager::set_location_visibility(Td *td) {
  bool is_location_visible = td->option_manager_->get_option_boolean("is_location_visible");
  auto pending_location_visibility_expire_date = is_location_visible ? std::numeric_limits<int32>::max() : 0;
  if (td->contacts_manager_ == nullptr) {
    G()->td_db()->get_binlog_pmc()->set("pending_location_visibility_expire_date",
                                        to_string(pending_location_visibility_expire_date));
    return;
  }
  if (td->contacts_manager_->pending_location_visibility_expire_date_ == -1 &&
      pending_location_visibility_expire_date == td->contacts_manager_->location_visibility_expire_date_) {
    return;
  }
  if (td->contacts_manager_->pending_location_visibility_expire_date_ != pending_location_visibility_expire_date) {
    td->contacts_manager_->pending_location_visibility_expire_date_ = pending_location_visibility_expire_date;
    G()->td_db()->get_binlog_pmc()->set("pending_location_visibility_expire_date",
                                        to_string(pending_location_visibility_expire_date));
  }
  td->contacts_manager_->try_send_set_location_visibility_query();
}

void ContactsManager::try_send_set_location_visibility_query() {
  if (G()->close_flag()) {
    return;
  }
  if (pending_location_visibility_expire_date_ == -1) {
    return;
  }

  LOG(INFO) << "Trying to send set location visibility query";
  if (is_set_location_visibility_request_sent_) {
    return;
  }
  if (pending_location_visibility_expire_date_ != 0 && last_user_location_.empty()) {
    return;
  }

  is_set_location_visibility_request_sent_ = true;
  auto query_promise =
      PromiseCreator::lambda([actor_id = actor_id(this), set_expire_date = pending_location_visibility_expire_date_](
                                 Result<tl_object_ptr<telegram_api::Updates>> result) {
        send_closure(actor_id, &ContactsManager::on_set_location_visibility_expire_date, set_expire_date,
                     result.is_ok() ? 0 : result.error().code());
      });
  td_->create_handler<SearchDialogsNearbyQuery>(std::move(query_promise))
      ->send(last_user_location_, true, pending_location_visibility_expire_date_);
}

void ContactsManager::on_set_location_visibility_expire_date(int32 set_expire_date, int32 error_code) {
  bool success = error_code == 0;
  is_set_location_visibility_request_sent_ = false;

  if (set_expire_date != pending_location_visibility_expire_date_) {
    try_send_set_location_visibility_query();
    return;
  }

  if (success) {
    set_location_visibility_expire_date(pending_location_visibility_expire_date_);
  } else {
    if (G()->close_flag()) {
      // request will be re-sent after restart
      return;
    }
    if (error_code != 406) {
      LOG(ERROR) << "Failed to set location visibility expire date to " << pending_location_visibility_expire_date_;
    }
  }
  G()->td_db()->get_binlog_pmc()->erase("pending_location_visibility_expire_date");
  pending_location_visibility_expire_date_ = -1;
  update_is_location_visible();
}

void ContactsManager::get_is_location_visible(Promise<Unit> &&promise) {
  auto query_promise = PromiseCreator::lambda([actor_id = actor_id(this), promise = std::move(promise)](
                                                  Result<tl_object_ptr<telegram_api::Updates>> result) mutable {
    send_closure(actor_id, &ContactsManager::on_get_is_location_visible, std::move(result), std::move(promise));
  });
  td_->create_handler<SearchDialogsNearbyQuery>(std::move(query_promise))->send(Location(), true, -1);
}

void ContactsManager::on_get_is_location_visible(Result<tl_object_ptr<telegram_api::Updates>> &&result,
                                                 Promise<Unit> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());
  if (result.is_error()) {
    if (result.error().message() == "GEO_POINT_INVALID" && pending_location_visibility_expire_date_ == -1 &&
        location_visibility_expire_date_ > 0) {
      set_location_visibility_expire_date(0);
      update_is_location_visible();
    }
    return promise.set_value(Unit());
  }

  auto updates_ptr = result.move_as_ok();
  if (updates_ptr->get_id() != telegram_api::updates::ID) {
    LOG(ERROR) << "Receive " << oneline(to_string(*updates_ptr)) << " instead of updates";
    return promise.set_value(Unit());
  }

  auto updates = std::move(telegram_api::move_object_as<telegram_api::updates>(updates_ptr)->updates_);
  if (updates.size() != 1 || updates[0]->get_id() != telegram_api::updatePeerLocated::ID) {
    LOG(ERROR) << "Receive unexpected " << to_string(updates);
    return promise.set_value(Unit());
  }

  auto peers = std::move(static_cast<telegram_api::updatePeerLocated *>(updates[0].get())->peers_);
  if (peers.size() != 1 || peers[0]->get_id() != telegram_api::peerSelfLocated::ID) {
    LOG(ERROR) << "Receive unexpected " << to_string(peers);
    return promise.set_value(Unit());
  }

  auto location_visibility_expire_date = static_cast<telegram_api::peerSelfLocated *>(peers[0].get())->expires_;
  if (location_visibility_expire_date != location_visibility_expire_date_) {
    set_location_visibility_expire_date(location_visibility_expire_date);
    update_is_location_visible();
  }

  promise.set_value(Unit());
}

int32 ContactsManager::on_update_peer_located(vector<tl_object_ptr<telegram_api::PeerLocated>> &&peers,
                                              bool from_update) {
  auto now = G()->unix_time();
  bool need_update = false;
  int32 location_visibility_expire_date = -1;
  for (auto &peer_located_ptr : peers) {
    if (peer_located_ptr->get_id() == telegram_api::peerSelfLocated::ID) {
      auto peer_self_located = telegram_api::move_object_as<telegram_api::peerSelfLocated>(peer_located_ptr);
      if (peer_self_located->expires_ == 0 || peer_self_located->expires_ > G()->unix_time()) {
        location_visibility_expire_date = peer_self_located->expires_;
      }
      continue;
    }

    CHECK(peer_located_ptr->get_id() == telegram_api::peerLocated::ID);
    auto peer_located = telegram_api::move_object_as<telegram_api::peerLocated>(peer_located_ptr);
    DialogId dialog_id(peer_located->peer_);
    int32 expires_at = peer_located->expires_;
    int32 distance = peer_located->distance_;
    if (distance < 0 || distance > 50000000) {
      LOG(ERROR) << "Receive wrong distance to " << to_string(peer_located);
      continue;
    }
    if (expires_at <= now) {
      LOG(INFO) << "Skip expired result " << to_string(peer_located);
      continue;
    }

    auto dialog_type = dialog_id.get_type();
    if (dialog_type == DialogType::User) {
      auto user_id = dialog_id.get_user_id();
      if (!have_user(user_id)) {
        LOG(ERROR) << "Can't find " << user_id;
        continue;
      }
      if (expires_at < now + 86400) {
        user_nearby_timeout_.set_timeout_in(user_id.get(), expires_at - now + 1);
      }
    } else if (dialog_type == DialogType::Channel) {
      auto channel_id = dialog_id.get_channel_id();
      if (!have_channel(channel_id)) {
        LOG(ERROR) << "Can't find " << channel_id;
        continue;
      }
      if (expires_at != std::numeric_limits<int32>::max()) {
        LOG(ERROR) << "Receive expiring at " << expires_at << " group location in " << to_string(peer_located);
      }
      if (from_update) {
        LOG(ERROR) << "Receive nearby " << channel_id << " from update";
        continue;
      }
    } else {
      LOG(ERROR) << "Receive chat of wrong type in " << to_string(peer_located);
      continue;
    }

    td_->messages_manager_->force_create_dialog(dialog_id, "on_update_peer_located");

    if (from_update) {
      CHECK(dialog_type == DialogType::User);
      bool is_found = false;
      for (auto &dialog_nearby : users_nearby_) {
        if (dialog_nearby.dialog_id == dialog_id) {
          if (dialog_nearby.distance != distance) {
            dialog_nearby.distance = distance;
            need_update = true;
          }
          is_found = true;
          break;
        }
      }
      if (!is_found) {
        users_nearby_.emplace_back(dialog_id, distance);
        all_users_nearby_.insert(dialog_id.get_user_id());
        need_update = true;
      }
    } else {
      if (dialog_type == DialogType::User) {
        users_nearby_.emplace_back(dialog_id, distance);
        all_users_nearby_.insert(dialog_id.get_user_id());
      } else {
        channels_nearby_.emplace_back(dialog_id, distance);
      }
    }
  }
  if (need_update) {
    std::sort(users_nearby_.begin(), users_nearby_.end());
    send_update_users_nearby();
  }
  return location_visibility_expire_date;
}

void ContactsManager::set_location_visibility_expire_date(int32 expire_date) {
  if (location_visibility_expire_date_ == expire_date) {
    return;
  }

  LOG(INFO) << "Set set_location_visibility_expire_date to " << expire_date;
  location_visibility_expire_date_ = expire_date;
  if (expire_date == 0) {
    G()->td_db()->get_binlog_pmc()->erase("location_visibility_expire_date");
  } else {
    G()->td_db()->get_binlog_pmc()->set("location_visibility_expire_date", to_string(expire_date));
  }
  // the caller must call update_is_location_visible() itself
}

void ContactsManager::update_is_location_visible() {
  auto expire_date = pending_location_visibility_expire_date_ != -1 ? pending_location_visibility_expire_date_
                                                                    : location_visibility_expire_date_;
  td_->option_manager_->set_option_boolean("is_location_visible", expire_date != 0);
}

void ContactsManager::on_update_bot_commands(DialogId dialog_id, UserId bot_user_id,
                                             vector<tl_object_ptr<telegram_api::botCommand>> &&bot_commands) {
  if (!bot_user_id.is_valid()) {
    LOG(ERROR) << "Receive updateBotCOmmands about invalid " << bot_user_id;
    return;
  }
  if (!have_user(bot_user_id) || !is_user_bot(bot_user_id)) {
    return;
  }
  if (td_->auth_manager_->is_bot()) {
    return;
  }

  auto is_from_bot = [bot_user_id](const BotCommands &commands) {
    return commands.get_bot_user_id() == bot_user_id;
  };

  switch (dialog_id.get_type()) {
    case DialogType::User: {
      UserId user_id(dialog_id.get_user_id());
      auto user_full = get_user_full(user_id);
      if (user_full != nullptr) {
        on_update_user_full_commands(user_full, user_id, std::move(bot_commands));
        update_user_full(user_full, user_id, "on_update_bot_commands");
      }
      break;
    }
    case DialogType::Chat: {
      ChatId chat_id(dialog_id.get_chat_id());
      auto chat_full = get_chat_full(chat_id);
      if (chat_full != nullptr) {
        if (bot_commands.empty()) {
          if (td::remove_if(chat_full->bot_commands, is_from_bot)) {
            chat_full->is_changed = true;
          }
        } else {
          BotCommands commands(bot_user_id, std::move(bot_commands));
          auto it = std::find_if(chat_full->bot_commands.begin(), chat_full->bot_commands.end(), is_from_bot);
          if (it != chat_full->bot_commands.end()) {
            if (*it != commands) {
              *it = std::move(commands);
              chat_full->is_changed = true;
            }
          } else {
            chat_full->bot_commands.push_back(std::move(commands));
            chat_full->is_changed = true;
          }
        }
        update_chat_full(chat_full, chat_id, "on_update_bot_commands");
      }
      break;
    }
    case DialogType::Channel: {
      ChannelId channel_id(dialog_id.get_channel_id());
      auto channel_full = get_channel_full(channel_id, true, "on_update_bot_commands");
      if (channel_full != nullptr) {
        if (bot_commands.empty()) {
          if (td::remove_if(channel_full->bot_commands, is_from_bot)) {
            channel_full->is_changed = true;
          }
        } else {
          BotCommands commands(bot_user_id, std::move(bot_commands));
          auto it = std::find_if(channel_full->bot_commands.begin(), channel_full->bot_commands.end(), is_from_bot);
          if (it != channel_full->bot_commands.end()) {
            if (*it != commands) {
              *it = std::move(commands);
              channel_full->is_changed = true;
            }
          } else {
            channel_full->bot_commands.push_back(std::move(commands));
            channel_full->is_changed = true;
          }
        }
        update_channel_full(channel_full, channel_id, "on_update_bot_commands");
      }
      break;
    }
    case DialogType::SecretChat:
    default:
      LOG(ERROR) << "Receive updateBotCommands in " << dialog_id;
      break;
  }
}

void ContactsManager::on_update_bot_menu_button(UserId bot_user_id,
                                                tl_object_ptr<telegram_api::BotMenuButton> &&bot_menu_button) {
  if (!bot_user_id.is_valid()) {
    LOG(ERROR) << "Receive updateBotCOmmands about invalid " << bot_user_id;
    return;
  }
  if (!have_user_force(bot_user_id) || !is_user_bot(bot_user_id)) {
    return;
  }
  if (td_->auth_manager_->is_bot()) {
    return;
  }

  auto user_full = get_user_full_force(bot_user_id);
  if (user_full != nullptr) {
    on_update_user_full_menu_button(user_full, bot_user_id, std::move(bot_menu_button));
    update_user_full(user_full, bot_user_id, "on_update_bot_menu_button");
  }
}

FileId ContactsManager::get_profile_photo_file_id(int64 photo_id) const {
  auto it = my_photo_file_id_.find(photo_id);
  if (it == my_photo_file_id_.end()) {
    return FileId();
  }
  return it->second;
}

void ContactsManager::set_profile_photo(const td_api::object_ptr<td_api::InputChatPhoto> &input_photo,
                                        Promise<Unit> &&promise) {
  if (input_photo == nullptr) {
    return promise.set_error(Status::Error(400, "New profile photo must be non-empty"));
  }

  const td_api::object_ptr<td_api::InputFile> *input_file = nullptr;
  double main_frame_timestamp = 0.0;
  bool is_animation = false;
  switch (input_photo->get_id()) {
    case td_api::inputChatPhotoPrevious::ID: {
      auto photo = static_cast<const td_api::inputChatPhotoPrevious *>(input_photo.get());
      auto photo_id = photo->chat_photo_id_;
      auto *u = get_user(get_my_id());
      if (u != nullptr && u->photo.id > 0 && photo_id == u->photo.id) {
        return promise.set_value(Unit());
      }

      auto file_id = get_profile_photo_file_id(photo_id);
      if (!file_id.is_valid()) {
        return promise.set_error(Status::Error(400, "Unknown profile photo ID specified"));
      }
      return send_update_profile_photo_query(td_->file_manager_->dup_file_id(file_id), photo_id, std::move(promise));
    }
    case td_api::inputChatPhotoStatic::ID: {
      auto photo = static_cast<const td_api::inputChatPhotoStatic *>(input_photo.get());
      input_file = &photo->photo_;
      break;
    }
    case td_api::inputChatPhotoAnimation::ID: {
      auto photo = static_cast<const td_api::inputChatPhotoAnimation *>(input_photo.get());
      input_file = &photo->animation_;
      main_frame_timestamp = photo->main_frame_timestamp_;
      is_animation = true;
      break;
    }
    default:
      UNREACHABLE();
      break;
  }

  const double MAX_ANIMATION_DURATION = 10.0;
  if (main_frame_timestamp < 0.0 || main_frame_timestamp > MAX_ANIMATION_DURATION) {
    return promise.set_error(Status::Error(400, "Wrong main frame timestamp specified"));
  }

  auto file_type = is_animation ? FileType::Animation : FileType::Photo;
  auto r_file_id = td_->file_manager_->get_input_file_id(file_type, *input_file, DialogId(get_my_id()), false, false);
  if (r_file_id.is_error()) {
    // TODO promise.set_error(std::move(status));
    return promise.set_error(Status::Error(400, r_file_id.error().message()));
  }
  FileId file_id = r_file_id.ok();
  CHECK(file_id.is_valid());

  upload_profile_photo(td_->file_manager_->dup_file_id(file_id), is_animation, main_frame_timestamp,
                       std::move(promise));
}

void ContactsManager::send_update_profile_photo_query(FileId file_id, int64 old_photo_id, Promise<Unit> &&promise) {
  FileView file_view = td_->file_manager_->get_file_view(file_id);
  td_->create_handler<UpdateProfilePhotoQuery>(std::move(promise))
      ->send(file_id, old_photo_id, file_view.main_remote_location().as_input_photo());
}

void ContactsManager::upload_profile_photo(FileId file_id, bool is_animation, double main_frame_timestamp,
                                           Promise<Unit> &&promise, int reupload_count, vector<int> bad_parts) {
  CHECK(file_id.is_valid());
  bool is_inserted = uploaded_profile_photos_
                         .emplace(file_id, UploadedProfilePhoto{main_frame_timestamp, is_animation, reupload_count,
                                                                std::move(promise)})
                         .second;
  CHECK(is_inserted);
  LOG(INFO) << "Ask to upload " << (is_animation ? "animated" : "static") << " profile photo " << file_id
            << " with bad parts " << bad_parts;
  // TODO use force_reupload if reupload_count >= 1, replace reupload_count with is_reupload
  td_->file_manager_->resume_upload(file_id, std::move(bad_parts), upload_profile_photo_callback_, 32, 0);
}

void ContactsManager::delete_profile_photo(int64 profile_photo_id, Promise<Unit> &&promise) {
  const User *u = get_user(get_my_id());
  if (u != nullptr && u->photo.id == profile_photo_id) {
    td_->create_handler<UpdateProfilePhotoQuery>(std::move(promise))
        ->send(FileId(), profile_photo_id, make_tl_object<telegram_api::inputPhotoEmpty>());
    return;
  }

  td_->create_handler<DeleteProfilePhotoQuery>(std::move(promise))->send(profile_photo_id);
}

void ContactsManager::set_name(const string &first_name, const string &last_name, Promise<Unit> &&promise) {
  auto new_first_name = clean_name(first_name, MAX_NAME_LENGTH);
  auto new_last_name = clean_name(last_name, MAX_NAME_LENGTH);
  if (new_first_name.empty()) {
    return promise.set_error(Status::Error(400, "First name must be non-empty"));
  }

  const User *u = get_user(get_my_id());
  int32 flags = 0;
  // TODO we can already send request for changing first_name and last_name and wanting to set initial values
  // TODO need to be rewritten using invoke after and cancelling previous request
  if (u == nullptr || u->first_name != new_first_name) {
    flags |= ACCOUNT_UPDATE_FIRST_NAME;
  }
  if (u == nullptr || u->last_name != new_last_name) {
    flags |= ACCOUNT_UPDATE_LAST_NAME;
  }
  if (flags == 0) {
    return promise.set_value(Unit());
  }

  td_->create_handler<UpdateProfileQuery>(std::move(promise))->send(flags, new_first_name, new_last_name, "");
}

void ContactsManager::set_bio(const string &bio, Promise<Unit> &&promise) {
  auto max_bio_length = static_cast<size_t>(td_->option_manager_->get_option_integer("bio_length_max"));
  auto new_bio = strip_empty_characters(bio, max_bio_length);
  for (auto &c : new_bio) {
    if (c == '\n') {
      c = ' ';
    }
  }

  const UserFull *user_full = get_user_full(get_my_id());
  int32 flags = 0;
  // TODO we can already send request for changing bio and wanting to set initial values
  // TODO need to be rewritten using invoke after and cancelling previous request
  if (user_full == nullptr || user_full->about != new_bio) {
    flags |= ACCOUNT_UPDATE_ABOUT;
  }
  if (flags == 0) {
    return promise.set_value(Unit());
  }

  td_->create_handler<UpdateProfileQuery>(std::move(promise))->send(flags, "", "", new_bio);
}

void ContactsManager::on_update_profile_success(int32 flags, const string &first_name, const string &last_name,
                                                const string &about) {
  CHECK(flags != 0);

  auto my_user_id = get_my_id();
  const User *u = get_user(my_user_id);
  if (u == nullptr) {
    LOG(ERROR) << "Doesn't receive info about me during update profile";
    return;
  }
  LOG_IF(ERROR, (flags & ACCOUNT_UPDATE_FIRST_NAME) != 0 && u->first_name != first_name)
      << "Wrong first name \"" << u->first_name << "\", expected \"" << first_name << '"';
  LOG_IF(ERROR, (flags & ACCOUNT_UPDATE_LAST_NAME) != 0 && u->last_name != last_name)
      << "Wrong last name \"" << u->last_name << "\", expected \"" << last_name << '"';

  if ((flags & ACCOUNT_UPDATE_ABOUT) != 0) {
    UserFull *user_full = get_user_full_force(my_user_id);
    if (user_full != nullptr) {
      user_full->about = about;
      user_full->is_changed = true;
      update_user_full(user_full, my_user_id, "on_update_profile_success");
      td_->group_call_manager_->on_update_dialog_about(DialogId(my_user_id), user_full->about, true);
    }
  }
}

void ContactsManager::set_username(const string &username, Promise<Unit> &&promise) {
  if (!username.empty() && !is_valid_username(username)) {
    return promise.set_error(Status::Error(400, "Username is invalid"));
  }
  td_->create_handler<UpdateUsernameQuery>(std::move(promise))->send(username);
}

void ContactsManager::set_emoji_status(EmojiStatus emoji_status, Promise<Unit> &&promise) {
  if (!td_->option_manager_->get_option_boolean("is_premium")) {
    return promise.set_error(Status::Error(400, "The method is available only for Telegram Premium users"));
  }
  add_recent_emoji_status(td_, emoji_status);
  auto query_promise = PromiseCreator::lambda(
      [actor_id = actor_id(this), emoji_status, promise = std::move(promise)](Result<Unit> result) mutable {
        if (result.is_ok()) {
          send_closure(actor_id, &ContactsManager::on_set_emoji_status, emoji_status, std::move(promise));
        } else {
          promise.set_error(result.move_as_error());
        }
      });
  td_->create_handler<UpdateEmojiStatusQuery>(std::move(query_promise))->send(emoji_status);
}

void ContactsManager::on_set_emoji_status(EmojiStatus emoji_status, Promise<Unit> &&promise) {
  auto user_id = get_my_id();
  User *u = get_user(user_id);
  if (u != nullptr) {
    on_update_user_emoji_status(u, user_id, emoji_status);
    update_user(u, user_id);
  }
  promise.set_value(Unit());
}

void ContactsManager::set_chat_description(ChatId chat_id, const string &description, Promise<Unit> &&promise) {
  auto new_description = strip_empty_characters(description, MAX_DESCRIPTION_LENGTH);
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }
  if (!get_chat_permissions(c).can_change_info_and_settings()) {
    return promise.set_error(Status::Error(400, "Not enough rights to set chat description"));
  }

  td_->create_handler<EditChatAboutQuery>(std::move(promise))->send(DialogId(chat_id), new_description);
}

void ContactsManager::set_channel_username(ChannelId channel_id, const string &username, Promise<Unit> &&promise) {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Supergroup not found"));
  }
  if (!get_channel_status(c).is_creator()) {
    return promise.set_error(Status::Error(400, "Not enough rights to change supergroup username"));
  }

  if (!username.empty() && !is_valid_username(username)) {
    return promise.set_error(Status::Error(400, "Username is invalid"));
  }

  if (!username.empty() && c->username.empty()) {
    auto channel_full = get_channel_full(channel_id, false, "set_channel_username");
    if (channel_full != nullptr && !channel_full->can_set_username) {
      return promise.set_error(Status::Error(400, "Can't set supergroup username"));
    }
  }

  td_->create_handler<UpdateChannelUsernameQuery>(std::move(promise))->send(channel_id, username);
}

void ContactsManager::set_channel_sticker_set(ChannelId channel_id, StickerSetId sticker_set_id,
                                              Promise<Unit> &&promise) {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Supergroup not found"));
  }
  if (!c->is_megagroup) {
    return promise.set_error(Status::Error(400, "Chat sticker set can be set only for supergroups"));
  }
  if (!get_channel_permissions(c).can_change_info_and_settings()) {
    return promise.set_error(Status::Error(400, "Not enough rights to change supergroup sticker set"));
  }

  telegram_api::object_ptr<telegram_api::InputStickerSet> input_sticker_set;
  if (!sticker_set_id.is_valid()) {
    input_sticker_set = telegram_api::make_object<telegram_api::inputStickerSetEmpty>();
  } else {
    input_sticker_set = td_->stickers_manager_->get_input_sticker_set(sticker_set_id);
    if (input_sticker_set == nullptr) {
      return promise.set_error(Status::Error(400, "Sticker set not found"));
    }
  }

  auto channel_full = get_channel_full(channel_id, false, "set_channel_sticker_set");
  if (channel_full != nullptr && !channel_full->can_set_sticker_set) {
    return promise.set_error(Status::Error(400, "Can't set supergroup sticker set"));
  }

  td_->create_handler<SetChannelStickerSetQuery>(std::move(promise))
      ->send(channel_id, sticker_set_id, std::move(input_sticker_set));
}

void ContactsManager::toggle_channel_sign_messages(ChannelId channel_id, bool sign_messages, Promise<Unit> &&promise) {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Supergroup not found"));
  }
  if (get_channel_type(c) == ChannelType::Megagroup) {
    return promise.set_error(Status::Error(400, "Message signatures can't be toggled in supergroups"));
  }
  if (!get_channel_permissions(c).can_change_info_and_settings()) {
    return promise.set_error(Status::Error(400, "Not enough rights to toggle channel sign messages"));
  }

  td_->create_handler<ToggleChannelSignaturesQuery>(std::move(promise))->send(channel_id, sign_messages);
}

void ContactsManager::toggle_channel_join_to_send(ChannelId channel_id, bool join_to_send, Promise<Unit> &&promise) {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Supergroup not found"));
  }
  if (get_channel_type(c) == ChannelType::Broadcast || c->is_gigagroup) {
    return promise.set_error(Status::Error(400, "The method can be called only for ordinary supergroups"));
  }
  if (!get_channel_permissions(c).can_restrict_members()) {
    return promise.set_error(Status::Error(400, "Not enough rights"));
  }

  td_->create_handler<ToggleChannelJoinToSendQuery>(std::move(promise))->send(channel_id, join_to_send);
}

void ContactsManager::toggle_channel_join_request(ChannelId channel_id, bool join_request, Promise<Unit> &&promise) {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Supergroup not found"));
  }
  if (get_channel_type(c) == ChannelType::Broadcast || c->is_gigagroup) {
    return promise.set_error(Status::Error(400, "The method can be called only for ordinary supergroups"));
  }
  if (!get_channel_permissions(c).can_restrict_members()) {
    return promise.set_error(Status::Error(400, "Not enough rights"));
  }

  td_->create_handler<ToggleChannelJoinRequestQuery>(std::move(promise))->send(channel_id, join_request);
}

void ContactsManager::toggle_channel_is_all_history_available(ChannelId channel_id, bool is_all_history_available,
                                                              Promise<Unit> &&promise) {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Supergroup not found"));
  }
  if (!get_channel_permissions(c).can_change_info_and_settings()) {
    return promise.set_error(Status::Error(400, "Not enough rights to toggle all supergroup history availability"));
  }
  if (get_channel_type(c) != ChannelType::Megagroup) {
    return promise.set_error(Status::Error(400, "Message history can be hidden in supergroups only"));
  }
  if (c->has_linked_channel && !is_all_history_available) {
    return promise.set_error(Status::Error(400, "Message history can't be hidden in discussion supergroups"));
  }
  // it can be toggled in public chats, but will not affect them

  td_->create_handler<TogglePrehistoryHiddenQuery>(std::move(promise))->send(channel_id, is_all_history_available);
}

void ContactsManager::convert_channel_to_gigagroup(ChannelId channel_id, Promise<Unit> &&promise) {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Supergroup not found"));
  }
  if (!get_channel_permissions(c).is_creator()) {
    return promise.set_error(Status::Error(400, "Not enough rights to convert group to broadcast group"));
  }
  if (get_channel_type(c) != ChannelType::Megagroup) {
    return promise.set_error(Status::Error(400, "Chat must be a supergroup"));
  }

  remove_dialog_suggested_action(SuggestedAction{SuggestedAction::Type::ConvertToGigagroup, DialogId(channel_id)});

  td_->create_handler<ConvertToGigagroupQuery>(std::move(promise))->send(channel_id);
}

void ContactsManager::set_channel_description(ChannelId channel_id, const string &description,
                                              Promise<Unit> &&promise) {
  auto new_description = strip_empty_characters(description, MAX_DESCRIPTION_LENGTH);
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }
  if (!get_channel_permissions(c).can_change_info_and_settings()) {
    return promise.set_error(Status::Error(400, "Not enough rights to set chat description"));
  }

  td_->create_handler<EditChatAboutQuery>(std::move(promise))->send(DialogId(channel_id), new_description);
}

void ContactsManager::set_channel_discussion_group(DialogId dialog_id, DialogId discussion_dialog_id,
                                                   Promise<Unit> &&promise) {
  if (!dialog_id.is_valid() && !discussion_dialog_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Invalid chat identifiers specified"));
  }

  ChannelId broadcast_channel_id;
  telegram_api::object_ptr<telegram_api::InputChannel> broadcast_input_channel;
  if (dialog_id.is_valid()) {
    if (!td_->messages_manager_->have_dialog_force(dialog_id, "set_channel_discussion_group 1")) {
      return promise.set_error(Status::Error(400, "Chat not found"));
    }

    if (dialog_id.get_type() != DialogType::Channel) {
      return promise.set_error(Status::Error(400, "Chat is not a channel"));
    }

    broadcast_channel_id = dialog_id.get_channel_id();
    const Channel *c = get_channel(broadcast_channel_id);
    if (c == nullptr) {
      return promise.set_error(Status::Error(400, "Chat info not found"));
    }

    if (c->is_megagroup) {
      return promise.set_error(Status::Error(400, "Chat is not a channel"));
    }
    if (!c->status.is_administrator() || !c->status.can_change_info_and_settings()) {
      return promise.set_error(Status::Error(400, "Not enough rights in the channel"));
    }

    broadcast_input_channel = get_input_channel(broadcast_channel_id);
    CHECK(broadcast_input_channel != nullptr);
  } else {
    broadcast_input_channel = telegram_api::make_object<telegram_api::inputChannelEmpty>();
  }

  ChannelId group_channel_id;
  telegram_api::object_ptr<telegram_api::InputChannel> group_input_channel;
  if (discussion_dialog_id.is_valid()) {
    if (!td_->messages_manager_->have_dialog_force(discussion_dialog_id, "set_channel_discussion_group 2")) {
      return promise.set_error(Status::Error(400, "Discussion chat not found"));
    }
    if (discussion_dialog_id.get_type() != DialogType::Channel) {
      return promise.set_error(Status::Error(400, "Discussion chat is not a supergroup"));
    }

    group_channel_id = discussion_dialog_id.get_channel_id();
    const Channel *c = get_channel(group_channel_id);
    if (c == nullptr) {
      return promise.set_error(Status::Error(400, "Discussion chat info not found"));
    }

    if (!c->is_megagroup) {
      return promise.set_error(Status::Error(400, "Discussion chat is not a supergroup"));
    }
    if (!c->status.is_administrator() || !c->status.can_pin_messages()) {
      return promise.set_error(Status::Error(400, "Not enough rights in the supergroup"));
    }

    group_input_channel = get_input_channel(group_channel_id);
    CHECK(group_input_channel != nullptr);
  } else {
    group_input_channel = telegram_api::make_object<telegram_api::inputChannelEmpty>();
  }

  td_->create_handler<SetDiscussionGroupQuery>(std::move(promise))
      ->send(broadcast_channel_id, std::move(broadcast_input_channel), group_channel_id,
             std::move(group_input_channel));
}

void ContactsManager::set_channel_location(DialogId dialog_id, const DialogLocation &location,
                                           Promise<Unit> &&promise) {
  if (location.empty()) {
    return promise.set_error(Status::Error(400, "Invalid chat location specified"));
  }

  if (!dialog_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Invalid chat identifier specified"));
  }
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "set_channel_location")) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }

  if (dialog_id.get_type() != DialogType::Channel) {
    return promise.set_error(Status::Error(400, "Chat is not a supergroup"));
  }

  auto channel_id = dialog_id.get_channel_id();
  const Channel *c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }
  if (!c->is_megagroup) {
    return promise.set_error(Status::Error(400, "Chat is not a supergroup"));
  }
  if (!c->status.is_creator()) {
    return promise.set_error(Status::Error(400, "Not enough rights in the supergroup"));
  }

  td_->create_handler<EditLocationQuery>(std::move(promise))->send(channel_id, location);
}

void ContactsManager::set_channel_slow_mode_delay(DialogId dialog_id, int32 slow_mode_delay, Promise<Unit> &&promise) {
  vector<int32> allowed_slow_mode_delays{0, 10, 30, 60, 300, 900, 3600};
  if (!td::contains(allowed_slow_mode_delays, slow_mode_delay)) {
    return promise.set_error(Status::Error(400, "Invalid new value for slow mode delay"));
  }

  if (!dialog_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Invalid chat identifier specified"));
  }
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "set_channel_slow_mode_delay")) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }

  if (dialog_id.get_type() != DialogType::Channel) {
    return promise.set_error(Status::Error(400, "Chat is not a supergroup"));
  }

  auto channel_id = dialog_id.get_channel_id();
  const Channel *c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }
  if (!c->is_megagroup) {
    return promise.set_error(Status::Error(400, "Chat is not a supergroup"));
  }
  if (!get_channel_permissions(c).can_restrict_members()) {
    return promise.set_error(Status::Error(400, "Not enough rights in the supergroup"));
  }

  td_->create_handler<ToggleSlowModeQuery>(std::move(promise))->send(channel_id, slow_mode_delay);
}

void ContactsManager::get_channel_statistics_dc_id(DialogId dialog_id, bool for_full_statistics,
                                                   Promise<DcId> &&promise) {
  if (!dialog_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Invalid chat identifier specified"));
  }
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "get_channel_statistics_dc_id")) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }

  if (dialog_id.get_type() != DialogType::Channel) {
    return promise.set_error(Status::Error(400, "Chat is not a channel"));
  }

  auto channel_id = dialog_id.get_channel_id();
  const Channel *c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }

  auto channel_full = get_channel_full_force(channel_id, false, "get_channel_statistics_dc_id");
  if (channel_full == nullptr || !channel_full->stats_dc_id.is_exact() ||
      (for_full_statistics && !channel_full->can_view_statistics)) {
    auto query_promise = PromiseCreator::lambda([actor_id = actor_id(this), channel_id, for_full_statistics,
                                                 promise = std::move(promise)](Result<Unit> result) mutable {
      send_closure(actor_id, &ContactsManager::get_channel_statistics_dc_id_impl, channel_id, for_full_statistics,
                   std::move(promise));
    });
    send_get_channel_full_query(channel_full, channel_id, std::move(query_promise), "get_channel_statistics_dc_id");
    return;
  }

  promise.set_value(DcId(channel_full->stats_dc_id));
}

void ContactsManager::get_channel_statistics_dc_id_impl(ChannelId channel_id, bool for_full_statistics,
                                                        Promise<DcId> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  auto channel_full = get_channel_full(channel_id, false, "get_channel_statistics_dc_id_impl");
  if (channel_full == nullptr) {
    return promise.set_error(Status::Error(400, "Chat full info not found"));
  }

  if (!channel_full->stats_dc_id.is_exact() || (for_full_statistics && !channel_full->can_view_statistics)) {
    return promise.set_error(Status::Error(400, "Chat statistics is not available"));
  }

  promise.set_value(DcId(channel_full->stats_dc_id));
}

void ContactsManager::get_channel_statistics(DialogId dialog_id, bool is_dark,
                                             Promise<td_api::object_ptr<td_api::ChatStatistics>> &&promise) {
  auto dc_id_promise = PromiseCreator::lambda(
      [actor_id = actor_id(this), dialog_id, is_dark, promise = std::move(promise)](Result<DcId> r_dc_id) mutable {
        if (r_dc_id.is_error()) {
          return promise.set_error(r_dc_id.move_as_error());
        }
        send_closure(actor_id, &ContactsManager::send_get_channel_stats_query, r_dc_id.move_as_ok(),
                     dialog_id.get_channel_id(), is_dark, std::move(promise));
      });
  get_channel_statistics_dc_id(dialog_id, true, std::move(dc_id_promise));
}

void ContactsManager::send_get_channel_stats_query(DcId dc_id, ChannelId channel_id, bool is_dark,
                                                   Promise<td_api::object_ptr<td_api::ChatStatistics>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  const Channel *c = get_channel(channel_id);
  CHECK(c != nullptr);
  if (c->is_megagroup) {
    td_->create_handler<GetMegagroupStatsQuery>(std::move(promise))->send(channel_id, is_dark, dc_id);
  } else {
    td_->create_handler<GetBroadcastStatsQuery>(std::move(promise))->send(channel_id, is_dark, dc_id);
  }
}

bool ContactsManager::can_get_channel_message_statistics(DialogId dialog_id) const {
  if (dialog_id.get_type() != DialogType::Channel) {
    return false;
  }

  auto channel_id = dialog_id.get_channel_id();
  const Channel *c = get_channel(channel_id);
  if (c == nullptr || c->is_megagroup) {
    return false;
  }

  if (td_->auth_manager_->is_bot()) {
    return false;
  }

  auto channel_full = get_channel_full(channel_id);
  if (channel_full != nullptr) {
    return channel_full->stats_dc_id.is_exact();
  }

  return c->status.is_administrator();
}

void ContactsManager::get_channel_message_statistics(FullMessageId full_message_id, bool is_dark,
                                                     Promise<td_api::object_ptr<td_api::messageStatistics>> &&promise) {
  auto dc_id_promise = PromiseCreator::lambda([actor_id = actor_id(this), full_message_id, is_dark,
                                               promise = std::move(promise)](Result<DcId> r_dc_id) mutable {
    if (r_dc_id.is_error()) {
      return promise.set_error(r_dc_id.move_as_error());
    }
    send_closure(actor_id, &ContactsManager::send_get_channel_message_stats_query, r_dc_id.move_as_ok(),
                 full_message_id, is_dark, std::move(promise));
  });
  get_channel_statistics_dc_id(full_message_id.get_dialog_id(), false, std::move(dc_id_promise));
}

void ContactsManager::send_get_channel_message_stats_query(
    DcId dc_id, FullMessageId full_message_id, bool is_dark,
    Promise<td_api::object_ptr<td_api::messageStatistics>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  auto dialog_id = full_message_id.get_dialog_id();
  if (!td_->messages_manager_->have_message_force(full_message_id, "send_get_channel_message_stats_query")) {
    return promise.set_error(Status::Error(400, "Message not found"));
  }
  if (!td_->messages_manager_->can_get_message_statistics(full_message_id)) {
    return promise.set_error(Status::Error(400, "Message statistics is inaccessible"));
  }
  CHECK(dialog_id.get_type() == DialogType::Channel);
  td_->create_handler<GetMessageStatsQuery>(std::move(promise))
      ->send(dialog_id.get_channel_id(), full_message_id.get_message_id(), is_dark, dc_id);
}

void ContactsManager::load_statistics_graph(DialogId dialog_id, string token, int64 x,
                                            Promise<td_api::object_ptr<td_api::StatisticalGraph>> &&promise) {
  auto dc_id_promise = PromiseCreator::lambda([actor_id = actor_id(this), token = std::move(token), x,
                                               promise = std::move(promise)](Result<DcId> r_dc_id) mutable {
    if (r_dc_id.is_error()) {
      return promise.set_error(r_dc_id.move_as_error());
    }
    send_closure(actor_id, &ContactsManager::send_load_async_graph_query, r_dc_id.move_as_ok(), std::move(token), x,
                 std::move(promise));
  });
  get_channel_statistics_dc_id(dialog_id, false, std::move(dc_id_promise));
}

void ContactsManager::send_load_async_graph_query(DcId dc_id, string token, int64 x,
                                                  Promise<td_api::object_ptr<td_api::StatisticalGraph>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  td_->create_handler<LoadAsyncGraphQuery>(std::move(promise))->send(token, x, dc_id);
}

void ContactsManager::report_channel_spam(ChannelId channel_id, const vector<MessageId> &message_ids,
                                          Promise<Unit> &&promise) {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Supergroup not found"));
  }
  if (!c->is_megagroup) {
    return promise.set_error(Status::Error(400, "Spam can be reported only in supergroups"));
  }
  if (!c->status.is_administrator()) {
    return promise.set_error(Status::Error(400, "Spam can be reported only by chat administrators"));
  }

  FlatHashMap<DialogId, vector<MessageId>, DialogIdHash> server_message_ids;
  for (auto &message_id : message_ids) {
    if (message_id.is_valid_scheduled()) {
      return promise.set_error(Status::Error(400, "Can't report scheduled messages"));
    }

    if (!message_id.is_valid()) {
      return promise.set_error(Status::Error(400, "Message not found"));
    }

    if (!message_id.is_server()) {
      continue;
    }

    auto sender_dialog_id = td_->messages_manager_->get_dialog_message_sender({DialogId(channel_id), message_id});
    if (sender_dialog_id.is_valid() && sender_dialog_id != DialogId(get_my_id()) &&
        td_->messages_manager_->have_input_peer(sender_dialog_id, AccessRights::Know)) {
      server_message_ids[sender_dialog_id].push_back(message_id);
    }
  }
  if (server_message_ids.empty()) {
    return promise.set_value(Unit());
  }

  MultiPromiseActorSafe mpas{"ReportSupergroupSpamMultiPromiseActor"};
  mpas.add_promise(std::move(promise));
  auto lock_promise = mpas.get_promise();

  for (auto &it : server_message_ids) {
    td_->create_handler<ReportChannelSpamQuery>(mpas.get_promise())->send(channel_id, it.first, it.second);
  }

  lock_promise.set_value(Unit());
}

void ContactsManager::delete_chat(ChatId chat_id, Promise<Unit> &&promise) {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }
  if (!get_chat_status(c).is_creator()) {
    return promise.set_error(Status::Error(400, "Not enough rights to delete the chat"));
  }
  if (!c->is_active) {
    return promise.set_error(Status::Error(400, "Chat is already deactivated"));
  }

  td_->create_handler<DeleteChatQuery>(std::move(promise))->send(chat_id);
}

void ContactsManager::delete_channel(ChannelId channel_id, Promise<Unit> &&promise) {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }
  if (!get_channel_can_be_deleted(c)) {
    return promise.set_error(Status::Error(400, "The chat can't be deleted"));
  }

  td_->create_handler<DeleteChannelQuery>(std::move(promise))->send(channel_id);
}

void ContactsManager::delete_dialog(DialogId dialog_id, Promise<Unit> &&promise) {
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "delete_dialog")) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return td_->messages_manager_->delete_dialog_history(dialog_id, true, true, std::move(promise));
    case DialogType::Chat:
      return delete_chat(dialog_id.get_chat_id(), std::move(promise));
    case DialogType::Channel:
      return delete_channel(dialog_id.get_channel_id(), std::move(promise));
    case DialogType::SecretChat:
      send_closure(td_->secret_chats_manager_, &SecretChatsManager::cancel_chat, dialog_id.get_secret_chat_id(), true,
                   std::move(promise));
      return;
    default:
      UNREACHABLE();
  }
}

void ContactsManager::add_chat_participant(ChatId chat_id, UserId user_id, int32 forward_limit,
                                           Promise<Unit> &&promise) {
  const Chat *c = get_chat(chat_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }
  if (!c->is_active) {
    return promise.set_error(Status::Error(400, "Chat is deactivated"));
  }
  if (forward_limit < 0) {
    return promise.set_error(Status::Error(400, "Can't forward negative number of messages"));
  }
  if (user_id != get_my_id()) {
    if (!get_chat_permissions(c).can_invite_users()) {
      return promise.set_error(Status::Error(400, "Not enough rights to invite members to the group chat"));
    }
  } else if (c->status.is_banned()) {
    return promise.set_error(Status::Error(400, "User was kicked from the chat"));
  }
  // TODO upper bound on forward_limit

  auto r_input_user = get_input_user(user_id);
  if (r_input_user.is_error()) {
    return promise.set_error(r_input_user.move_as_error());
  }

  // TODO invoke after
  td_->create_handler<AddChatUserQuery>(std::move(promise))->send(chat_id, r_input_user.move_as_ok(), forward_limit);
}

void ContactsManager::add_channel_participant(ChannelId channel_id, UserId user_id,
                                              const DialogParticipantStatus &old_status, Promise<Unit> &&promise) {
  if (td_->auth_manager_->is_bot()) {
    return promise.set_error(Status::Error(400, "Bots can't add new chat members"));
  }

  const Channel *c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }
  auto r_input_user = get_input_user(user_id);
  if (r_input_user.is_error()) {
    return promise.set_error(r_input_user.move_as_error());
  }

  if (user_id == get_my_id()) {
    // join the channel
    if (get_channel_status(c).is_banned()) {
      return promise.set_error(Status::Error(400, "Can't return to kicked from chat"));
    }

    if (!get_channel_join_request(c)) {
      speculative_add_channel_user(channel_id, user_id, DialogParticipantStatus::Member(), c->status);
    }
    td_->create_handler<JoinChannelQuery>(std::move(promise))->send(channel_id);
    return;
  }

  if (!get_channel_permissions(c).can_invite_users()) {
    return promise.set_error(Status::Error(400, "Not enough rights to invite members to the supergroup chat"));
  }

  speculative_add_channel_user(channel_id, user_id, DialogParticipantStatus::Member(), old_status);
  vector<tl_object_ptr<telegram_api::InputUser>> input_users;
  input_users.push_back(r_input_user.move_as_ok());
  td_->create_handler<InviteToChannelQuery>(std::move(promise))->send(channel_id, std::move(input_users));
}

void ContactsManager::add_channel_participants(ChannelId channel_id, const vector<UserId> &user_ids,
                                               Promise<Unit> &&promise) {
  if (td_->auth_manager_->is_bot()) {
    return promise.set_error(Status::Error(400, "Bots can't add new chat members"));
  }

  const Channel *c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }

  if (!get_channel_permissions(c).can_invite_users()) {
    return promise.set_error(Status::Error(400, "Not enough rights to invite members to the supergroup chat"));
  }

  vector<tl_object_ptr<telegram_api::InputUser>> input_users;
  for (auto user_id : user_ids) {
    auto r_input_user = get_input_user(user_id);
    if (r_input_user.is_error()) {
      return promise.set_error(r_input_user.move_as_error());
    }

    if (user_id == get_my_id()) {
      // can't invite self
      continue;
    }
    input_users.push_back(r_input_user.move_as_ok());
  }

  if (input_users.empty()) {
    return promise.set_value(Unit());
  }

  td_->create_handler<InviteToChannelQuery>(std::move(promise))->send(channel_id, std::move(input_users));
}

void ContactsManager::set_channel_participant_status(ChannelId channel_id, DialogId participant_dialog_id,
                                                     td_api::object_ptr<td_api::ChatMemberStatus> &&chat_member_status,
                                                     Promise<Unit> &&promise) {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }
  auto status = get_dialog_participant_status(chat_member_status, get_channel_type(c));

  if (participant_dialog_id == DialogId(get_my_id())) {
    // fast path is needed, because get_channel_status may return Creator, while GetChannelParticipantQuery returning Left
    return set_channel_participant_status_impl(channel_id, participant_dialog_id, std::move(status),
                                               get_channel_status(c), std::move(promise));
  }
  if (participant_dialog_id.get_type() != DialogType::User) {
    if (status.is_administrator() || status.is_member() || status.is_restricted()) {
      return promise.set_error(Status::Error(400, "Other chats can be only banned or unbanned"));
    }
    // always pretend that old_status is different
    return restrict_channel_participant(
        channel_id, participant_dialog_id, std::move(status),
        status.is_banned() ? DialogParticipantStatus::Left() : DialogParticipantStatus::Banned(0), std::move(promise));
  }

  auto on_result_promise =
      PromiseCreator::lambda([actor_id = actor_id(this), channel_id, participant_dialog_id, status,
                              promise = std::move(promise)](Result<DialogParticipant> r_dialog_participant) mutable {
        // ResultHandlers are cleared before managers, so it is safe to capture this
        if (r_dialog_participant.is_error()) {
          return promise.set_error(r_dialog_participant.move_as_error());
        }

        send_closure(actor_id, &ContactsManager::set_channel_participant_status_impl, channel_id, participant_dialog_id,
                     std::move(status), r_dialog_participant.ok().status_, std::move(promise));
      });

  get_channel_participant(channel_id, participant_dialog_id, std::move(on_result_promise));
}

void ContactsManager::set_channel_participant_status_impl(ChannelId channel_id, DialogId participant_dialog_id,
                                                          DialogParticipantStatus status,
                                                          DialogParticipantStatus old_status, Promise<Unit> &&promise) {
  if (old_status == status && !old_status.is_creator()) {
    return promise.set_value(Unit());
  }

  LOG(INFO) << "Change status of " << participant_dialog_id << " in " << channel_id << " from " << old_status << " to "
            << status;
  bool need_add = false;
  bool need_promote = false;
  bool need_restrict = false;
  if (status.is_creator() || old_status.is_creator()) {
    if (!old_status.is_creator()) {
      return promise.set_error(Status::Error(400, "Can't add another owner to the chat"));
    }
    if (!status.is_creator()) {
      return promise.set_error(Status::Error(400, "Can't remove chat owner"));
    }
    auto user_id = get_my_id();
    if (participant_dialog_id != DialogId(user_id)) {
      return promise.set_error(Status::Error(400, "Not enough rights to edit chat owner rights"));
    }
    if (status.is_member() == old_status.is_member()) {
      // change rank and is_anonymous
      auto r_input_user = get_input_user(user_id);
      CHECK(r_input_user.is_ok());
      td_->create_handler<EditChannelAdminQuery>(std::move(promise))
          ->send(channel_id, user_id, r_input_user.move_as_ok(), status);
      return;
    }
    if (status.is_member()) {
      // creator not member -> creator member
      need_add = true;
    } else {
      // creator member -> creator not member
      need_restrict = true;
    }
  } else if (status.is_administrator()) {
    need_promote = true;
  } else if (!status.is_member() || status.is_restricted()) {
    if (status.is_member() && !old_status.is_member()) {
      // TODO there is no way in server API to invite someone and change restrictions
      // we need to first add user and change restrictions again after that
      // but if restrictions aren't changed, then adding is enough
      auto copy_old_status = old_status;
      copy_old_status.set_is_member(true);
      if (copy_old_status == status) {
        need_add = true;
      } else {
        need_restrict = true;
      }
    } else {
      need_restrict = true;
    }
  } else {
    // regular member
    if (old_status.is_administrator()) {
      need_promote = true;
    } else if (old_status.is_restricted() || old_status.is_banned()) {
      need_restrict = true;
    } else {
      CHECK(!old_status.is_member());
      need_add = true;
    }
  }

  if (need_promote) {
    if (participant_dialog_id.get_type() != DialogType::User) {
      return promise.set_error(Status::Error(400, "Can't promote chats to chat administrators"));
    }
    return promote_channel_participant(channel_id, participant_dialog_id.get_user_id(), status, old_status,
                                       std::move(promise));
  } else if (need_restrict) {
    return restrict_channel_participant(channel_id, participant_dialog_id, std::move(status), std::move(old_status),
                                        std::move(promise));
  } else {
    CHECK(need_add);
    if (participant_dialog_id.get_type() != DialogType::User) {
      return promise.set_error(Status::Error(400, "Can't add chats as chat members"));
    }
    return add_channel_participant(channel_id, participant_dialog_id.get_user_id(), old_status, std::move(promise));
  }
}

void ContactsManager::promote_channel_participant(ChannelId channel_id, UserId user_id,
                                                  const DialogParticipantStatus &status,
                                                  const DialogParticipantStatus &old_status, Promise<Unit> &&promise) {
  LOG(INFO) << "Promote " << user_id << " in " << channel_id << " from " << old_status << " to " << status;
  const Channel *c = get_channel(channel_id);
  CHECK(c != nullptr);

  if (user_id == get_my_id()) {
    if (status.is_administrator()) {
      return promise.set_error(Status::Error(400, "Can't promote self"));
    }
    CHECK(status.is_member());
    // allow to demote self. TODO is it allowed server-side?
  } else {
    if (!get_channel_permissions(c).can_promote_members()) {
      return promise.set_error(Status::Error(400, "Not enough rights"));
    }

    CHECK(!old_status.is_creator());
    CHECK(!status.is_creator());
  }

  TRY_RESULT_PROMISE(promise, input_user, get_input_user(user_id));

  speculative_add_channel_user(channel_id, user_id, status, old_status);
  td_->create_handler<EditChannelAdminQuery>(std::move(promise))
      ->send(channel_id, user_id, std::move(input_user), status);
}

void ContactsManager::set_chat_participant_status(ChatId chat_id, UserId user_id, DialogParticipantStatus status,
                                                  Promise<Unit> &&promise) {
  if (!status.is_member()) {
    return delete_chat_participant(chat_id, user_id, false, std::move(promise));
  }
  if (status.is_creator()) {
    return promise.set_error(Status::Error(400, "Can't change owner in basic group chats"));
  }
  if (status.is_restricted()) {
    return promise.set_error(Status::Error(400, "Can't restrict users in basic group chats"));
  }

  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }
  if (!c->is_active) {
    return promise.set_error(Status::Error(400, "Chat is deactivated"));
  }

  auto chat_full = get_chat_full(chat_id);
  if (chat_full == nullptr) {
    auto load_chat_full_promise =
        PromiseCreator::lambda([actor_id = actor_id(this), chat_id, user_id, status = std::move(status),
                                promise = std::move(promise)](Result<Unit> &&result) mutable {
          if (result.is_error()) {
            promise.set_error(result.move_as_error());
          } else {
            send_closure(actor_id, &ContactsManager::set_chat_participant_status, chat_id, user_id, status,
                         std::move(promise));
          }
        });
    return load_chat_full(chat_id, false, std::move(load_chat_full_promise), "set_chat_participant_status");
  }

  auto participant = get_chat_full_participant(chat_full, DialogId(user_id));
  if (participant == nullptr && !status.is_administrator()) {
    // the user isn't a member, but needs to be added
    return add_chat_participant(chat_id, user_id, 0, std::move(promise));
  }

  if (!get_chat_permissions(c).can_promote_members()) {
    return promise.set_error(Status::Error(400, "Need owner rights in the group chat"));
  }

  if (user_id == get_my_id()) {
    return promise.set_error(Status::Error(400, "Can't promote or demote self"));
  }

  if (participant == nullptr) {
    // the user must be added first
    CHECK(status.is_administrator());
    auto add_chat_participant_promise = PromiseCreator::lambda(
        [actor_id = actor_id(this), chat_id, user_id, promise = std::move(promise)](Result<Unit> &&result) mutable {
          if (result.is_error()) {
            promise.set_error(result.move_as_error());
          } else {
            send_closure(actor_id, &ContactsManager::send_edit_chat_admin_query, chat_id, user_id, true,
                         std::move(promise));
          }
        });
    return add_chat_participant(chat_id, user_id, 0, std::move(add_chat_participant_promise));
  }

  send_edit_chat_admin_query(chat_id, user_id, status.is_administrator(), std::move(promise));
}

void ContactsManager::send_edit_chat_admin_query(ChatId chat_id, UserId user_id, bool is_administrator,
                                                 Promise<Unit> &&promise) {
  auto r_input_user = get_input_user(user_id);
  if (r_input_user.is_error()) {
    return promise.set_error(r_input_user.move_as_error());
  }

  td_->create_handler<EditChatAdminQuery>(std::move(promise))
      ->send(chat_id, r_input_user.move_as_ok(), is_administrator);
}

void ContactsManager::can_transfer_ownership(Promise<CanTransferOwnershipResult> &&promise) {
  auto request_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<Unit> r_result) mutable {
    CHECK(r_result.is_error());

    auto error = r_result.move_as_error();
    CanTransferOwnershipResult result;
    if (error.message() == "PASSWORD_HASH_INVALID") {
      return promise.set_value(std::move(result));
    }
    if (error.message() == "PASSWORD_MISSING") {
      result.type = CanTransferOwnershipResult::Type::PasswordNeeded;
      return promise.set_value(std::move(result));
    }
    if (begins_with(error.message(), "PASSWORD_TOO_FRESH_")) {
      result.type = CanTransferOwnershipResult::Type::PasswordTooFresh;
      result.retry_after = to_integer<int32>(error.message().substr(Slice("PASSWORD_TOO_FRESH_").size()));
      if (result.retry_after < 0) {
        result.retry_after = 0;
      }
      return promise.set_value(std::move(result));
    }
    if (begins_with(error.message(), "SESSION_TOO_FRESH_")) {
      result.type = CanTransferOwnershipResult::Type::SessionTooFresh;
      result.retry_after = to_integer<int32>(error.message().substr(Slice("SESSION_TOO_FRESH_").size()));
      if (result.retry_after < 0) {
        result.retry_after = 0;
      }
      return promise.set_value(std::move(result));
    }
    promise.set_error(std::move(error));
  });

  td_->create_handler<CanEditChannelCreatorQuery>(std::move(request_promise))->send();
}

td_api::object_ptr<td_api::CanTransferOwnershipResult> ContactsManager::get_can_transfer_ownership_result_object(
    CanTransferOwnershipResult result) {
  switch (result.type) {
    case CanTransferOwnershipResult::Type::Ok:
      return td_api::make_object<td_api::canTransferOwnershipResultOk>();
    case CanTransferOwnershipResult::Type::PasswordNeeded:
      return td_api::make_object<td_api::canTransferOwnershipResultPasswordNeeded>();
    case CanTransferOwnershipResult::Type::PasswordTooFresh:
      return td_api::make_object<td_api::canTransferOwnershipResultPasswordTooFresh>(result.retry_after);
    case CanTransferOwnershipResult::Type::SessionTooFresh:
      return td_api::make_object<td_api::canTransferOwnershipResultSessionTooFresh>(result.retry_after);
    default:
      UNREACHABLE();
      return nullptr;
  }
}

void ContactsManager::transfer_dialog_ownership(DialogId dialog_id, UserId user_id, const string &password,
                                                Promise<Unit> &&promise) {
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "transfer_dialog_ownership")) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }
  if (!have_user_force(user_id)) {
    return promise.set_error(Status::Error(400, "User not found"));
  }
  if (is_user_bot(user_id)) {
    return promise.set_error(Status::Error(400, "User is a bot"));
  }
  if (is_user_deleted(user_id)) {
    return promise.set_error(Status::Error(400, "User is deleted"));
  }
  if (password.empty()) {
    return promise.set_error(Status::Error(400, "PASSWORD_HASH_INVALID"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::Chat:
    case DialogType::SecretChat:
      return promise.set_error(Status::Error(400, "Can't transfer chat ownership"));
    case DialogType::Channel:
      send_closure(
          td_->password_manager_, &PasswordManager::get_input_check_password_srp, password,
          PromiseCreator::lambda([actor_id = actor_id(this), channel_id = dialog_id.get_channel_id(), user_id,
                                  promise = std::move(promise)](
                                     Result<tl_object_ptr<telegram_api::InputCheckPasswordSRP>> result) mutable {
            if (result.is_error()) {
              return promise.set_error(result.move_as_error());
            }
            send_closure(actor_id, &ContactsManager::transfer_channel_ownership, channel_id, user_id,
                         result.move_as_ok(), std::move(promise));
          }));
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

void ContactsManager::transfer_channel_ownership(
    ChannelId channel_id, UserId user_id, tl_object_ptr<telegram_api::InputCheckPasswordSRP> input_check_password,
    Promise<Unit> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  td_->create_handler<EditChannelCreatorQuery>(std::move(promise))
      ->send(channel_id, user_id, std::move(input_check_password));
}

Status ContactsManager::can_manage_dialog_invite_links(DialogId dialog_id, bool creator_only) {
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "can_manage_dialog_invite_links")) {
    return Status::Error(400, "Chat not found");
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return Status::Error(400, "Can't invite members to a private chat");
    case DialogType::Chat: {
      const Chat *c = get_chat(dialog_id.get_chat_id());
      if (c == nullptr) {
        return Status::Error(400, "Chat info not found");
      }
      if (!c->is_active) {
        return Status::Error(400, "Chat is deactivated");
      }
      bool have_rights = creator_only ? c->status.is_creator() : c->status.can_manage_invite_links();
      if (!have_rights) {
        return Status::Error(400, "Not enough rights to manage chat invite link");
      }
      break;
    }
    case DialogType::Channel: {
      const Channel *c = get_channel(dialog_id.get_channel_id());
      if (c == nullptr) {
        return Status::Error(400, "Chat info not found");
      }
      bool have_rights = creator_only ? c->status.is_creator() : c->status.can_manage_invite_links();
      if (!have_rights) {
        return Status::Error(400, "Not enough rights to manage chat invite link");
      }
      break;
    }
    case DialogType::SecretChat:
      return Status::Error(400, "Can't invite members to a secret chat");
    case DialogType::None:
    default:
      UNREACHABLE();
  }
  return Status::OK();
}

void ContactsManager::export_dialog_invite_link(DialogId dialog_id, string title, int32 expire_date, int32 usage_limit,
                                                bool creates_join_request, bool is_permanent,
                                                Promise<td_api::object_ptr<td_api::chatInviteLink>> &&promise) {
  get_me(PromiseCreator::lambda([actor_id = actor_id(this), dialog_id, title = std::move(title), expire_date,
                                 usage_limit, creates_join_request, is_permanent,
                                 promise = std::move(promise)](Result<Unit> &&result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      send_closure(actor_id, &ContactsManager::export_dialog_invite_link_impl, dialog_id, std::move(title), expire_date,
                   usage_limit, creates_join_request, is_permanent, std::move(promise));
    }
  }));
}

void ContactsManager::export_dialog_invite_link_impl(DialogId dialog_id, string title, int32 expire_date,
                                                     int32 usage_limit, bool creates_join_request, bool is_permanent,
                                                     Promise<td_api::object_ptr<td_api::chatInviteLink>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());
  TRY_STATUS_PROMISE(promise, can_manage_dialog_invite_links(dialog_id));
  if (creates_join_request && usage_limit > 0) {
    return promise.set_error(
        Status::Error(400, "Member limit can't be specified for links requiring administrator approval"));
  }

  auto new_title = clean_name(std::move(title), MAX_INVITE_LINK_TITLE_LENGTH);
  td_->create_handler<ExportChatInviteQuery>(std::move(promise))
      ->send(dialog_id, new_title, expire_date, usage_limit, creates_join_request, is_permanent);
}

void ContactsManager::edit_dialog_invite_link(DialogId dialog_id, const string &invite_link, string title,
                                              int32 expire_date, int32 usage_limit, bool creates_join_request,
                                              Promise<td_api::object_ptr<td_api::chatInviteLink>> &&promise) {
  TRY_STATUS_PROMISE(promise, can_manage_dialog_invite_links(dialog_id));
  if (creates_join_request && usage_limit > 0) {
    return promise.set_error(
        Status::Error(400, "Member limit can't be specified for links requiring administrator approval"));
  }

  if (invite_link.empty()) {
    return promise.set_error(Status::Error(400, "Invite link must be non-empty"));
  }

  auto new_title = clean_name(std::move(title), MAX_INVITE_LINK_TITLE_LENGTH);
  td_->create_handler<EditChatInviteLinkQuery>(std::move(promise))
      ->send(dialog_id, invite_link, new_title, expire_date, usage_limit, creates_join_request);
}

void ContactsManager::get_dialog_invite_link(DialogId dialog_id, const string &invite_link,
                                             Promise<td_api::object_ptr<td_api::chatInviteLink>> &&promise) {
  TRY_STATUS_PROMISE(promise, can_manage_dialog_invite_links(dialog_id, false));

  if (invite_link.empty()) {
    return promise.set_error(Status::Error(400, "Invite link must be non-empty"));
  }

  td_->create_handler<GetExportedChatInviteQuery>(std::move(promise))->send(dialog_id, invite_link);
}

void ContactsManager::get_dialog_invite_link_counts(
    DialogId dialog_id, Promise<td_api::object_ptr<td_api::chatInviteLinkCounts>> &&promise) {
  TRY_STATUS_PROMISE(promise, can_manage_dialog_invite_links(dialog_id, true));

  td_->create_handler<GetChatAdminWithInvitesQuery>(std::move(promise))->send(dialog_id);
}

void ContactsManager::get_dialog_invite_links(DialogId dialog_id, UserId creator_user_id, bool is_revoked,
                                              int32 offset_date, const string &offset_invite_link, int32 limit,
                                              Promise<td_api::object_ptr<td_api::chatInviteLinks>> &&promise) {
  TRY_STATUS_PROMISE(promise, can_manage_dialog_invite_links(dialog_id, creator_user_id != get_my_id()));
  TRY_RESULT_PROMISE(promise, input_user, get_input_user(creator_user_id));

  if (limit <= 0) {
    return promise.set_error(Status::Error(400, "Parameter limit must be positive"));
  }

  td_->create_handler<GetExportedChatInvitesQuery>(std::move(promise))
      ->send(dialog_id, std::move(input_user), is_revoked, offset_date, offset_invite_link, limit);
}

void ContactsManager::get_dialog_invite_link_users(
    DialogId dialog_id, const string &invite_link, td_api::object_ptr<td_api::chatInviteLinkMember> offset_member,
    int32 limit, Promise<td_api::object_ptr<td_api::chatInviteLinkMembers>> &&promise) {
  TRY_STATUS_PROMISE(promise, can_manage_dialog_invite_links(dialog_id));

  if (limit <= 0) {
    return promise.set_error(Status::Error(400, "Parameter limit must be positive"));
  }

  if (invite_link.empty()) {
    return promise.set_error(Status::Error(400, "Invite link must be non-empty"));
  }

  UserId offset_user_id;
  int32 offset_date = 0;
  if (offset_member != nullptr) {
    offset_user_id = UserId(offset_member->user_id_);
    offset_date = offset_member->joined_chat_date_;
  }

  td_->create_handler<GetChatInviteImportersQuery>(std::move(promise))
      ->send(dialog_id, invite_link, offset_date, offset_user_id, limit);
}

void ContactsManager::get_dialog_join_requests(DialogId dialog_id, const string &invite_link, const string &query,
                                               td_api::object_ptr<td_api::chatJoinRequest> offset_request, int32 limit,
                                               Promise<td_api::object_ptr<td_api::chatJoinRequests>> &&promise) {
  TRY_STATUS_PROMISE(promise, can_manage_dialog_invite_links(dialog_id));

  if (limit <= 0) {
    return promise.set_error(Status::Error(400, "Parameter limit must be positive"));
  }

  UserId offset_user_id;
  int32 offset_date = 0;
  if (offset_request != nullptr) {
    offset_user_id = UserId(offset_request->user_id_);
    offset_date = offset_request->date_;
  }

  td_->create_handler<GetChatJoinRequestsQuery>(std::move(promise))
      ->send(dialog_id, invite_link, query, offset_date, offset_user_id, limit);
}

void ContactsManager::process_dialog_join_request(DialogId dialog_id, UserId user_id, bool approve,
                                                  Promise<Unit> &&promise) {
  TRY_STATUS_PROMISE(promise, can_manage_dialog_invite_links(dialog_id));
  td_->create_handler<HideChatJoinRequestQuery>(std::move(promise))->send(dialog_id, user_id, approve);
}

void ContactsManager::process_dialog_join_requests(DialogId dialog_id, const string &invite_link, bool approve,
                                                   Promise<Unit> &&promise) {
  TRY_STATUS_PROMISE(promise, can_manage_dialog_invite_links(dialog_id));
  td_->create_handler<HideAllChatJoinRequestsQuery>(std::move(promise))->send(dialog_id, invite_link, approve);
}

void ContactsManager::revoke_dialog_invite_link(DialogId dialog_id, const string &invite_link,
                                                Promise<td_api::object_ptr<td_api::chatInviteLinks>> &&promise) {
  TRY_STATUS_PROMISE(promise, can_manage_dialog_invite_links(dialog_id));

  if (invite_link.empty()) {
    return promise.set_error(Status::Error(400, "Invite link must be non-empty"));
  }

  td_->create_handler<RevokeChatInviteLinkQuery>(std::move(promise))->send(dialog_id, invite_link);
}

void ContactsManager::delete_revoked_dialog_invite_link(DialogId dialog_id, const string &invite_link,
                                                        Promise<Unit> &&promise) {
  TRY_STATUS_PROMISE(promise, can_manage_dialog_invite_links(dialog_id));

  if (invite_link.empty()) {
    return promise.set_error(Status::Error(400, "Invite link must be non-empty"));
  }

  td_->create_handler<DeleteExportedChatInviteQuery>(std::move(promise))->send(dialog_id, invite_link);
}

void ContactsManager::delete_all_revoked_dialog_invite_links(DialogId dialog_id, UserId creator_user_id,
                                                             Promise<Unit> &&promise) {
  TRY_STATUS_PROMISE(promise, can_manage_dialog_invite_links(dialog_id, creator_user_id != get_my_id()));
  TRY_RESULT_PROMISE(promise, input_user, get_input_user(creator_user_id));

  td_->create_handler<DeleteRevokedExportedChatInvitesQuery>(std::move(promise))
      ->send(dialog_id, std::move(input_user));
}

void ContactsManager::check_dialog_invite_link(const string &invite_link, bool force, Promise<Unit> &&promise) {
  auto it = invite_link_infos_.find(invite_link);
  if (it != invite_link_infos_.end()) {
    auto dialog_id = it->second->dialog_id;
    if (!force && dialog_id.get_type() == DialogType::Chat && !get_chat_is_active(dialog_id.get_chat_id())) {
      invite_link_infos_.erase(it);
    } else {
      return promise.set_value(Unit());
    }
  }

  if (!DialogInviteLink::is_valid_invite_link(invite_link)) {
    return promise.set_error(Status::Error(400, "Wrong invite link"));
  }

  CHECK(!invite_link.empty());
  td_->create_handler<CheckChatInviteQuery>(std::move(promise))->send(invite_link);
}

void ContactsManager::import_dialog_invite_link(const string &invite_link, Promise<DialogId> &&promise) {
  if (!DialogInviteLink::is_valid_invite_link(invite_link)) {
    return promise.set_error(Status::Error(400, "Wrong invite link"));
  }

  td_->create_handler<ImportChatInviteQuery>(std::move(promise))->send(invite_link);
}

void ContactsManager::delete_chat_participant(ChatId chat_id, UserId user_id, bool revoke_messages,
                                              Promise<Unit> &&promise) {
  const Chat *c = get_chat(chat_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }
  if (!c->is_active) {
    return promise.set_error(Status::Error(400, "Chat is deactivated"));
  }
  auto my_id = get_my_id();
  if (c->status.is_left()) {
    if (user_id == my_id) {
      if (revoke_messages) {
        return td_->messages_manager_->delete_dialog_history(DialogId(chat_id), true, false, std::move(promise));
      }
      return promise.set_value(Unit());
    } else {
      return promise.set_error(Status::Error(400, "Not in the chat"));
    }
  }
  if (user_id != my_id) {
    auto my_status = get_chat_permissions(c);
    if (!my_status.is_creator()) {  // creator can delete anyone
      auto participant = get_chat_participant(chat_id, user_id);
      if (participant != nullptr) {  // if have no information about participant, just send request to the server
        /*
        TODO
        if (c->everyone_is_administrator) {
          // if all are administrators, only invited by me participants can be deleted
          if (participant->inviter_user_id_ != my_id) {
            return promise.set_error(Status::Error(400, "Need to be inviter of a user to kick it from a basic group"));
          }
        } else {
          // otherwise, only creator can kick administrators
          if (participant->status_.is_administrator()) {
            return promise.set_error(
                Status::Error(400, "Only the creator of a basic group can kick group administrators"));
          }
          // regular users can be kicked by administrators and their inviters
          if (!my_status.is_administrator() && participant->inviter_user_id_ != my_id) {
            return promise.set_error(Status::Error(400, "Need to be inviter of a user to kick it from a basic group"));
          }
        }
        */
      }
    }
  }
  auto r_input_user = get_input_user(user_id);
  if (r_input_user.is_error()) {
    return promise.set_error(r_input_user.move_as_error());
  }

  // TODO invoke after
  td_->create_handler<DeleteChatUserQuery>(std::move(promise))
      ->send(chat_id, r_input_user.move_as_ok(), revoke_messages);
}

void ContactsManager::restrict_channel_participant(ChannelId channel_id, DialogId participant_dialog_id,
                                                   DialogParticipantStatus &&status,
                                                   DialogParticipantStatus &&old_status, Promise<Unit> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  LOG(INFO) << "Restrict " << participant_dialog_id << " in " << channel_id << " from " << old_status << " to "
            << status;
  const Channel *c = get_channel(channel_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Chat info not found"));
  }
  if (!c->status.is_member() && !c->status.is_creator()) {
    if (participant_dialog_id == DialogId(get_my_id())) {
      if (status.is_member()) {
        return promise.set_error(Status::Error(400, "Can't unrestrict self"));
      }
      return promise.set_value(Unit());
    } else {
      return promise.set_error(Status::Error(400, "Not in the chat"));
    }
  }
  auto input_peer = td_->messages_manager_->get_input_peer(participant_dialog_id, AccessRights::Know);
  if (input_peer == nullptr) {
    return promise.set_error(Status::Error(400, "Member not found"));
  }

  if (participant_dialog_id == DialogId(get_my_id())) {
    if (status.is_restricted() || status.is_banned()) {
      return promise.set_error(Status::Error(400, "Can't restrict self"));
    }
    if (status.is_member()) {
      return promise.set_error(Status::Error(400, "Can't unrestrict self"));
    }

    // leave the channel
    speculative_add_channel_user(channel_id, participant_dialog_id.get_user_id(), status, c->status);
    td_->create_handler<LeaveChannelQuery>(std::move(promise))->send(channel_id);
    return;
  }

  switch (participant_dialog_id.get_type()) {
    case DialogType::User:
      // ok;
      break;
    case DialogType::Channel:
      if (status.is_administrator() || status.is_member() || status.is_restricted()) {
        return promise.set_error(Status::Error(400, "Other chats can be only banned or unbanned"));
      }
      break;
    default:
      return promise.set_error(Status::Error(400, "Can't restrict the chat"));
  }

  CHECK(!old_status.is_creator());
  CHECK(!status.is_creator());

  if (!get_channel_permissions(c).can_restrict_members()) {
    return promise.set_error(Status::Error(400, "Not enough rights to restrict/unrestrict chat member"));
  }

  if (old_status.is_member() && !status.is_member() && !status.is_banned()) {
    // we can't make participant Left without kicking it first
    auto on_result_promise = PromiseCreator::lambda([actor_id = actor_id(this), channel_id, participant_dialog_id,
                                                     status = std::move(status),
                                                     promise = std::move(promise)](Result<> result) mutable {
      if (result.is_error()) {
        return promise.set_error(result.move_as_error());
      }

      create_actor<SleepActor>(
          "RestrictChannelParticipantSleepActor", 1.0,
          PromiseCreator::lambda([actor_id, channel_id, participant_dialog_id, status = std::move(status),
                                  promise = std::move(promise)](Result<> result) mutable {
            if (result.is_error()) {
              return promise.set_error(result.move_as_error());
            }

            send_closure(actor_id, &ContactsManager::restrict_channel_participant, channel_id, participant_dialog_id,
                         std::move(status), DialogParticipantStatus::Banned(0), std::move(promise));
          }))
          .release();
    });

    promise = std::move(on_result_promise);
    status = DialogParticipantStatus::Banned(G()->unix_time() + 60);
  }

  if (participant_dialog_id.get_type() == DialogType::User) {
    speculative_add_channel_user(channel_id, participant_dialog_id.get_user_id(), status, old_status);
  }
  td_->create_handler<EditChannelBannedQuery>(std::move(promise))
      ->send(channel_id, participant_dialog_id, std::move(input_peer), status);
}

void ContactsManager::on_set_channel_participant_status(ChannelId channel_id, DialogId participant_dialog_id,
                                                        DialogParticipantStatus status) {
  if (G()->close_flag() || participant_dialog_id == DialogId(get_my_id())) {
    return;
  }

  status.update_restrictions();
  if (have_channel_participant_cache(channel_id)) {
    update_channel_participant_status_cache(channel_id, participant_dialog_id, std::move(status));
  }
}

ChannelId ContactsManager::migrate_chat_to_megagroup(ChatId chat_id, Promise<Unit> &promise) {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    promise.set_error(Status::Error(400, "Chat info not found"));
    return ChannelId();
  }

  if (!c->status.is_creator()) {
    promise.set_error(Status::Error(400, "Need creator rights in the chat"));
    return ChannelId();
  }

  if (c->migrated_to_channel_id.is_valid()) {
    return c->migrated_to_channel_id;
  }

  td_->create_handler<MigrateChatQuery>(std::move(promise))->send(chat_id);
  return ChannelId();
}

vector<ChannelId> ContactsManager::get_channel_ids(vector<tl_object_ptr<telegram_api::Chat>> &&chats,
                                                   const char *source) {
  vector<ChannelId> channel_ids;
  for (auto &chat : chats) {
    auto channel_id = get_channel_id(chat);
    if (!channel_id.is_valid()) {
      LOG(ERROR) << "Receive invalid " << channel_id << " from " << source << " in " << to_string(chat);
      continue;
    }
    on_get_chat(std::move(chat), source);
    if (have_channel(channel_id)) {
      channel_ids.push_back(channel_id);
    }
  }
  return channel_ids;
}

vector<DialogId> ContactsManager::get_dialog_ids(vector<tl_object_ptr<telegram_api::Chat>> &&chats,
                                                 const char *source) {
  vector<DialogId> dialog_ids;
  for (auto &chat : chats) {
    auto channel_id = get_channel_id(chat);
    if (!channel_id.is_valid()) {
      auto chat_id = get_chat_id(chat);
      if (!chat_id.is_valid()) {
        LOG(ERROR) << "Receive invalid chat from " << source << " in " << to_string(chat);
      } else {
        dialog_ids.push_back(DialogId(chat_id));
      }
    } else {
      dialog_ids.push_back(DialogId(channel_id));
    }
    on_get_chat(std::move(chat), source);
  }
  return dialog_ids;
}

void ContactsManager::return_created_public_dialogs(Promise<td_api::object_ptr<td_api::chats>> &&promise,
                                                    const vector<ChannelId> &channel_ids) {
  if (!promise) {
    return;
  }

  auto total_count = narrow_cast<int32>(channel_ids.size());
  promise.set_value(td_api::make_object<td_api::chats>(
      total_count, transform(channel_ids, [](ChannelId channel_id) { return DialogId(channel_id).get(); })));
}

void ContactsManager::get_created_public_dialogs(PublicDialogType type,
                                                 Promise<td_api::object_ptr<td_api::chats>> &&promise,
                                                 bool from_binlog) {
  auto index = static_cast<int32>(type);
  if (created_public_channels_inited_[index]) {
    return return_created_public_dialogs(std::move(promise), created_public_channels_[index]);
  }

  if (get_created_public_channels_queries_[index].empty() && G()->parameters().use_chat_info_db) {
    auto pmc_key = PSTRING() << "public_channels" << index;
    auto str = G()->td_db()->get_binlog_pmc()->get(pmc_key);
    if (!str.empty()) {
      auto r_channel_ids = transform(full_split(Slice(str), ','), [](Slice str) -> Result<ChannelId> {
        TRY_RESULT(channel_id_int, to_integer_safe<int64>(str));
        ChannelId channel_id(channel_id_int);
        if (!channel_id.is_valid()) {
          return Status::Error("Have invalid channel ID");
        }
        return channel_id;
      });
      if (std::any_of(r_channel_ids.begin(), r_channel_ids.end(),
                      [](auto &r_channel_id) { return r_channel_id.is_error(); })) {
        LOG(ERROR) << "Can't parse " << str;
        G()->td_db()->get_binlog_pmc()->erase(pmc_key);
      } else {
        Dependencies dependencies;
        vector<ChannelId> channel_ids;
        for (auto &r_channel_id : r_channel_ids) {
          auto channel_id = r_channel_id.move_as_ok();
          dependencies.add_dialog_and_dependencies(DialogId(channel_id));
          channel_ids.push_back(channel_id);
        }
        if (!dependencies.resolve_force(td_, "get_created_public_dialogs")) {
          G()->td_db()->get_binlog_pmc()->erase(pmc_key);
        } else {
          created_public_channels_[index] = std::move(channel_ids);
          created_public_channels_inited_[index] = true;

          if (type == PublicDialogType::HasUsername) {
            update_created_public_broadcasts();
          }

          if (from_binlog) {
            return return_created_public_dialogs(std::move(promise), created_public_channels_[index]);
          }
        }
      }
    }
  }

  reload_created_public_dialogs(type, std::move(promise));
}

void ContactsManager::reload_created_public_dialogs(PublicDialogType type,
                                                    Promise<td_api::object_ptr<td_api::chats>> &&promise) {
  auto index = static_cast<int32>(type);
  get_created_public_channels_queries_[index].push_back(std::move(promise));
  if (get_created_public_channels_queries_[index].size() == 1) {
    auto query_promise = PromiseCreator::lambda([actor_id = actor_id(this), type](Result<Unit> &&result) {
      send_closure(actor_id, &ContactsManager::finish_get_created_public_dialogs, type, std::move(result));
    });
    td_->create_handler<GetCreatedPublicChannelsQuery>(std::move(query_promise))->send(type, false);
  }
}

void ContactsManager::finish_get_created_public_dialogs(PublicDialogType type, Result<Unit> &&result) {
  if (G()->close_flag()) {
    result = Global::request_aborted_error();
  }

  auto index = static_cast<int32>(type);
  auto promises = std::move(get_created_public_channels_queries_[index]);
  reset_to_empty(get_created_public_channels_queries_[index]);
  if (result.is_error()) {
    fail_promises(promises, result.move_as_error());
    return;
  }

  CHECK(created_public_channels_inited_[index]);
  for (auto &promise : promises) {
    return_created_public_dialogs(std::move(promise), created_public_channels_[index]);
  }
}

void ContactsManager::update_created_public_channels(Channel *c, ChannelId channel_id) {
  if (created_public_channels_inited_[0]) {
    bool was_changed = false;
    if (c->username.empty() || !c->status.is_creator()) {
      was_changed = td::remove(created_public_channels_[0], channel_id);
    } else {
      if (!td::contains(created_public_channels_[0], channel_id)) {
        created_public_channels_[0].push_back(channel_id);
        was_changed = true;
      }
    }
    if (was_changed) {
      if (!c->is_megagroup) {
        update_created_public_broadcasts();
      }

      save_created_public_channels(PublicDialogType::HasUsername);

      reload_created_public_dialogs(PublicDialogType::HasUsername, Promise<td_api::object_ptr<td_api::chats>>());
    }
  }
  if (created_public_channels_inited_[1]) {
    bool was_changed = false;
    if (!c->has_location || !c->status.is_creator()) {
      was_changed = td::remove(created_public_channels_[1], channel_id);
    } else {
      if (!td::contains(created_public_channels_[1], channel_id)) {
        created_public_channels_[1].push_back(channel_id);
        was_changed = true;
      }
    }
    if (was_changed) {
      save_created_public_channels(PublicDialogType::IsLocationBased);

      reload_created_public_dialogs(PublicDialogType::IsLocationBased, Promise<td_api::object_ptr<td_api::chats>>());
    }
  }
}

void ContactsManager::on_get_created_public_channels(PublicDialogType type,
                                                     vector<tl_object_ptr<telegram_api::Chat>> &&chats) {
  auto index = static_cast<int32>(type);
  auto channel_ids = get_channel_ids(std::move(chats), "on_get_created_public_channels");
  if (created_public_channels_inited_[index] && created_public_channels_[index] == channel_ids) {
    return;
  }
  for (auto channel_id : channel_ids) {
    td_->messages_manager_->force_create_dialog(DialogId(channel_id), "on_get_created_public_channels");
  }
  created_public_channels_[index] = std::move(channel_ids);
  created_public_channels_inited_[index] = true;

  if (type == PublicDialogType::HasUsername) {
    update_created_public_broadcasts();
  }

  save_created_public_channels(type);
}

void ContactsManager::save_created_public_channels(PublicDialogType type) {
  auto index = static_cast<int32>(type);
  CHECK(created_public_channels_inited_[index]);
  if (G()->parameters().use_chat_info_db) {
    G()->td_db()->get_binlog_pmc()->set(
        PSTRING() << "public_channels" << index,
        implode(
            transform(created_public_channels_[index], [](auto channel_id) { return PSTRING() << channel_id.get(); }),
            ','));
  }
}

void ContactsManager::update_created_public_broadcasts() {
  CHECK(created_public_channels_inited_[0]);
  vector<ChannelId> channel_ids;
  for (auto &channel_id : created_public_channels_[0]) {
    auto c = get_channel(channel_id);
    if (!c->is_megagroup) {
      channel_ids.push_back(channel_id);
    }
  }
  send_closure_later(G()->messages_manager(), &MessagesManager::on_update_created_public_broadcasts,
                     std::move(channel_ids));
}

void ContactsManager::check_created_public_dialogs_limit(PublicDialogType type, Promise<Unit> &&promise) {
  td_->create_handler<GetCreatedPublicChannelsQuery>(std::move(promise))->send(type, true);
}

vector<DialogId> ContactsManager::get_dialogs_for_discussion(Promise<Unit> &&promise) {
  if (dialogs_for_discussion_inited_) {
    promise.set_value(Unit());
    return transform(dialogs_for_discussion_, [&](DialogId dialog_id) {
      td_->messages_manager_->force_create_dialog(dialog_id, "get_dialogs_for_discussion");
      return dialog_id;
    });
  }

  td_->create_handler<GetGroupsForDiscussionQuery>(std::move(promise))->send();
  return {};
}

void ContactsManager::on_get_dialogs_for_discussion(vector<tl_object_ptr<telegram_api::Chat>> &&chats) {
  dialogs_for_discussion_inited_ = true;
  dialogs_for_discussion_ = get_dialog_ids(std::move(chats), "on_get_dialogs_for_discussion");
}

void ContactsManager::update_dialogs_for_discussion(DialogId dialog_id, bool is_suitable) {
  if (!dialogs_for_discussion_inited_) {
    return;
  }

  if (is_suitable) {
    if (!td::contains(dialogs_for_discussion_, dialog_id)) {
      LOG(DEBUG) << "Add " << dialog_id << " to list of suitable discussion chats";
      dialogs_for_discussion_.insert(dialogs_for_discussion_.begin(), dialog_id);
    }
  } else {
    if (td::remove(dialogs_for_discussion_, dialog_id)) {
      LOG(DEBUG) << "Remove " << dialog_id << " from list of suitable discussion chats";
    }
  }
}

vector<DialogId> ContactsManager::get_inactive_channels(Promise<Unit> &&promise) {
  if (inactive_channel_ids_inited_) {
    promise.set_value(Unit());
    return transform(inactive_channel_ids_, [&](ChannelId channel_id) { return DialogId(channel_id); });
  }

  td_->create_handler<GetInactiveChannelsQuery>(std::move(promise))->send();
  return {};
}

void ContactsManager::on_get_inactive_channels(vector<tl_object_ptr<telegram_api::Chat>> &&chats,
                                               Promise<Unit> &&promise) {
  auto channel_ids = get_channel_ids(std::move(chats), "on_get_inactive_channels");

  MultiPromiseActorSafe mpas{"GetInactiveChannelsMultiPromiseActor"};
  mpas.add_promise(PromiseCreator::lambda([actor_id = actor_id(this), channel_ids,
                                           promise = std::move(promise)](Unit) mutable {
    send_closure(actor_id, &ContactsManager::on_create_inactive_channels, std::move(channel_ids), std::move(promise));
  }));
  mpas.set_ignore_errors(true);
  auto lock_promise = mpas.get_promise();

  for (auto channel_id : channel_ids) {
    td_->messages_manager_->create_dialog(DialogId(channel_id), false, mpas.get_promise());
  }

  lock_promise.set_value(Unit());
}

void ContactsManager::on_create_inactive_channels(vector<ChannelId> &&channel_ids, Promise<Unit> &&promise) {
  inactive_channel_ids_inited_ = true;
  inactive_channel_ids_ = std::move(channel_ids);
  promise.set_value(Unit());
}

void ContactsManager::remove_inactive_channel(ChannelId channel_id) {
  if (inactive_channel_ids_inited_ && td::remove(inactive_channel_ids_, channel_id)) {
    LOG(DEBUG) << "Remove " << channel_id << " from list of inactive channels";
  }
}

void ContactsManager::remove_dialog_suggested_action(SuggestedAction action) {
  auto it = dialog_suggested_actions_.find(action.dialog_id_);
  if (it == dialog_suggested_actions_.end()) {
    return;
  }
  remove_suggested_action(it->second, action);
  if (it->second.empty()) {
    dialog_suggested_actions_.erase(it);
  }
}

void ContactsManager::dismiss_dialog_suggested_action(SuggestedAction action, Promise<Unit> &&promise) {
  auto dialog_id = action.dialog_id_;
  if (!td_->messages_manager_->have_dialog(dialog_id)) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }
  if (!td_->messages_manager_->have_input_peer(dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(400, "Can't access the chat"));
  }

  auto it = dialog_suggested_actions_.find(dialog_id);
  if (it == dialog_suggested_actions_.end() || !td::contains(it->second, action)) {
    return promise.set_value(Unit());
  }

  auto action_str = action.get_suggested_action_str();
  if (action_str.empty()) {
    return promise.set_value(Unit());
  }

  auto &queries = dismiss_suggested_action_queries_[dialog_id];
  queries.push_back(std::move(promise));
  if (queries.size() == 1) {
    auto query_promise = PromiseCreator::lambda([actor_id = actor_id(this), action](Result<Unit> &&result) {
      send_closure(actor_id, &ContactsManager::on_dismiss_suggested_action, action, std::move(result));
    });
    td_->create_handler<DismissSuggestionQuery>(std::move(query_promise))->send(std::move(action));
  }
}

void ContactsManager::on_dismiss_suggested_action(SuggestedAction action, Result<Unit> &&result) {
  auto it = dismiss_suggested_action_queries_.find(action.dialog_id_);
  CHECK(it != dismiss_suggested_action_queries_.end());
  auto promises = std::move(it->second);
  dismiss_suggested_action_queries_.erase(it);

  if (result.is_error()) {
    fail_promises(promises, result.move_as_error());
    return;
  }

  remove_dialog_suggested_action(action);

  set_promises(promises);
}

void ContactsManager::on_import_contacts_finished(int64 random_id, vector<UserId> imported_contact_user_ids,
                                                  vector<int32> unimported_contact_invites) {
  LOG(INFO) << "Contacts import with random_id " << random_id
            << " has finished: " << format::as_array(imported_contact_user_ids);
  if (random_id == 1) {
    // import from change_imported_contacts
    all_imported_contacts_ = std::move(next_all_imported_contacts_);
    next_all_imported_contacts_.clear();

    auto result_size = imported_contacts_unique_id_.size();
    auto unique_size = all_imported_contacts_.size();
    auto add_size = imported_contacts_pos_.size();

    imported_contact_user_ids_.resize(result_size);
    unimported_contact_invites_.resize(result_size);

    CHECK(imported_contact_user_ids.size() == add_size);
    CHECK(unimported_contact_invites.size() == add_size);
    CHECK(imported_contacts_unique_id_.size() == result_size);

    std::unordered_map<size_t, int32> unique_id_to_unimported_contact_invites;
    for (size_t i = 0; i < add_size; i++) {
      auto unique_id = imported_contacts_pos_[i];
      get_user_id_object(imported_contact_user_ids[i], "on_import_contacts_finished");  // to ensure updateUser
      all_imported_contacts_[unique_id].set_user_id(imported_contact_user_ids[i]);
      unique_id_to_unimported_contact_invites[unique_id] = unimported_contact_invites[i];
    }

    if (G()->parameters().use_chat_info_db) {
      G()->td_db()->get_binlog()->force_sync(PromiseCreator::lambda(
          [log_event = log_event_store(all_imported_contacts_).as_slice().str()](Result<> result) mutable {
            if (result.is_ok()) {
              LOG(INFO) << "Save imported contacts to database";
              G()->td_db()->get_sqlite_pmc()->set("user_imported_contacts", std::move(log_event), Auto());
            }
          }));
    }

    for (size_t i = 0; i < result_size; i++) {
      auto unique_id = imported_contacts_unique_id_[i];
      CHECK(unique_id < unique_size);
      imported_contact_user_ids_[i] = all_imported_contacts_[unique_id].get_user_id();
      auto it = unique_id_to_unimported_contact_invites.find(unique_id);
      if (it == unique_id_to_unimported_contact_invites.end()) {
        unimported_contact_invites_[i] = 0;
      } else {
        unimported_contact_invites_[i] = it->second;
      }
    }
    return;
  }

  auto it = imported_contacts_.find(random_id);
  CHECK(it != imported_contacts_.end());
  CHECK(it->second.first.empty());
  CHECK(it->second.second.empty());
  imported_contacts_[random_id] = {std::move(imported_contact_user_ids), std::move(unimported_contact_invites)};
}

void ContactsManager::on_deleted_contacts(const vector<UserId> &deleted_contact_user_ids) {
  LOG(INFO) << "Contacts deletion has finished for " << deleted_contact_user_ids;

  for (auto user_id : deleted_contact_user_ids) {
    auto u = get_user(user_id);
    CHECK(u != nullptr);
    if (!u->is_contact) {
      continue;
    }

    LOG(INFO) << "Drop contact with " << user_id;
    on_update_user_is_contact(u, user_id, false, false);
    CHECK(u->is_is_contact_changed);
    u->cache_version = 0;
    u->is_repaired = false;
    update_user(u, user_id);
    CHECK(!u->is_contact);
    CHECK(!contacts_hints_.has_key(user_id.get()));
  }
}

void ContactsManager::save_next_contacts_sync_date() {
  if (G()->close_flag()) {
    return;
  }
  if (!G()->parameters().use_chat_info_db) {
    return;
  }
  G()->td_db()->get_binlog_pmc()->set("next_contacts_sync_date", to_string(next_contacts_sync_date_));
}

void ContactsManager::on_get_contacts(tl_object_ptr<telegram_api::contacts_Contacts> &&new_contacts) {
  next_contacts_sync_date_ = G()->unix_time() + Random::fast(70000, 100000);

  CHECK(new_contacts != nullptr);
  if (new_contacts->get_id() == telegram_api::contacts_contactsNotModified::ID) {
    if (saved_contact_count_ == -1) {
      saved_contact_count_ = 0;
    }
    on_get_contacts_finished(contacts_hints_.size());
    td_->create_handler<GetContactsStatusesQuery>()->send();
    return;
  }

  auto contacts = move_tl_object_as<telegram_api::contacts_contacts>(new_contacts);
  FlatHashSet<UserId, UserIdHash> contact_user_ids;
  for (auto &user : contacts->users_) {
    auto user_id = get_user_id(user);
    if (!user_id.is_valid()) {
      LOG(ERROR) << "Receive invalid " << user_id;
      continue;
    }
    contact_user_ids.insert(user_id);
  }
  on_get_users(std::move(contacts->users_), "on_get_contacts");

  UserId my_id = get_my_id();
  users_.foreach([&](const UserId &user_id, unique_ptr<User> &user) {
    User *u = user.get();
    bool should_be_contact = contact_user_ids.count(user_id) == 1;
    if (u->is_contact != should_be_contact) {
      if (u->is_contact) {
        LOG(INFO) << "Drop contact with " << user_id;
        if (user_id != my_id) {
          LOG_CHECK(contacts_hints_.has_key(user_id.get()))
              << my_id << " " << user_id << " " << to_string(get_user_object(user_id, u));
        }
        on_update_user_is_contact(u, user_id, false, false);
        CHECK(u->is_is_contact_changed);
        u->cache_version = 0;
        u->is_repaired = false;
        update_user(u, user_id);
        CHECK(!u->is_contact);
        if (user_id != my_id) {
          CHECK(!contacts_hints_.has_key(user_id.get()));
        }
      } else {
        LOG(ERROR) << "Receive non-contact " << user_id << " in the list of contacts";
      }
    }
  });

  saved_contact_count_ = contacts->saved_count_;
  on_get_contacts_finished(std::numeric_limits<size_t>::max());
}

void ContactsManager::save_contacts_to_database() {
  if (!G()->parameters().use_chat_info_db || !are_contacts_loaded_) {
    return;
  }

  LOG(INFO) << "Schedule save contacts to database";
  vector<UserId> user_ids =
      transform(contacts_hints_.search_empty(100000).second, [](int64 key) { return UserId(key); });

  G()->td_db()->get_binlog_pmc()->set("saved_contact_count", to_string(saved_contact_count_));
  G()->td_db()->get_binlog()->force_sync(PromiseCreator::lambda([user_ids = std::move(user_ids)](Result<> result) {
    if (result.is_ok()) {
      LOG(INFO) << "Saved contacts to database";
      G()->td_db()->get_sqlite_pmc()->set(
          "user_contacts", log_event_store(user_ids).as_slice().str(), PromiseCreator::lambda([](Result<> result) {
            if (result.is_ok()) {
              send_closure(G()->contacts_manager(), &ContactsManager::save_next_contacts_sync_date);
            }
          }));
    }
  }));
}

void ContactsManager::on_get_contacts_failed(Status error) {
  CHECK(error.is_error());
  next_contacts_sync_date_ = G()->unix_time() + Random::fast(5, 10);
  fail_promises(load_contacts_queries_, std::move(error));
}

void ContactsManager::on_load_contacts_from_database(string value) {
  if (G()->close_flag()) {
    return;
  }
  if (value.empty()) {
    reload_contacts(true);
    return;
  }

  vector<UserId> user_ids;
  log_event_parse(user_ids, value).ensure();

  LOG(INFO) << "Successfully loaded " << user_ids.size() << " contacts from database";

  load_contact_users_multipromise_.add_promise(PromiseCreator::lambda(
      [actor_id = actor_id(this), expected_contact_count = user_ids.size()](Result<Unit> result) {
        if (result.is_ok()) {
          send_closure(actor_id, &ContactsManager::on_get_contacts_finished, expected_contact_count);
        }
      }));

  auto lock_promise = load_contact_users_multipromise_.get_promise();

  for (auto user_id : user_ids) {
    get_user(user_id, 3, load_contact_users_multipromise_.get_promise());
  }

  lock_promise.set_value(Unit());
}

void ContactsManager::on_get_contacts_finished(size_t expected_contact_count) {
  LOG(INFO) << "Finished to get " << contacts_hints_.size() << " contacts out of expected " << expected_contact_count;
  are_contacts_loaded_ = true;
  set_promises(load_contacts_queries_);
  if (expected_contact_count != contacts_hints_.size()) {
    save_contacts_to_database();
  }
}

void ContactsManager::on_get_contacts_statuses(vector<tl_object_ptr<telegram_api::contactStatus>> &&statuses) {
  auto my_user_id = get_my_id();
  for (auto &status : statuses) {
    UserId user_id(status->user_id_);
    if (user_id != my_user_id) {
      on_update_user_online(user_id, std::move(status->status_));
    }
  }
  save_next_contacts_sync_date();
}

void ContactsManager::on_update_online_status_privacy() {
  td_->create_handler<GetContactsStatusesQuery>()->send();
}

void ContactsManager::on_update_phone_number_privacy() {
  // all UserFull.need_phone_number_privacy_exception can be outdated now,
  // so mark all of them as expired
  users_full_.foreach([&](const UserId &user_id, unique_ptr<UserFull> &user_full) { user_full->expires_at = 0.0; });
}

void ContactsManager::invalidate_user_full(UserId user_id) {
  auto user_full = get_user_full_force(user_id);
  if (user_full != nullptr) {
    td_->messages_manager_->on_dialog_info_full_invalidated(DialogId(user_id));

    if (!user_full->is_expired()) {
      user_full->expires_at = 0.0;
      user_full->need_save_to_database = true;

      update_user_full(user_full, user_id, "invalidate_user_full");
    }
  }
}

UserId ContactsManager::get_user_id(const tl_object_ptr<telegram_api::User> &user) {
  CHECK(user != nullptr);
  switch (user->get_id()) {
    case telegram_api::userEmpty::ID:
      return UserId(static_cast<const telegram_api::userEmpty *>(user.get())->id_);
    case telegram_api::user::ID:
      return UserId(static_cast<const telegram_api::user *>(user.get())->id_);
    default:
      UNREACHABLE();
      return UserId();
  }
}

ChatId ContactsManager::get_chat_id(const tl_object_ptr<telegram_api::Chat> &chat) {
  CHECK(chat != nullptr);
  switch (chat->get_id()) {
    case telegram_api::chatEmpty::ID:
      return ChatId(static_cast<const telegram_api::chatEmpty *>(chat.get())->id_);
    case telegram_api::chat::ID:
      return ChatId(static_cast<const telegram_api::chat *>(chat.get())->id_);
    case telegram_api::chatForbidden::ID:
      return ChatId(static_cast<const telegram_api::chatForbidden *>(chat.get())->id_);
    default:
      return ChatId();
  }
}

ChannelId ContactsManager::get_channel_id(const tl_object_ptr<telegram_api::Chat> &chat) {
  CHECK(chat != nullptr);
  switch (chat->get_id()) {
    case telegram_api::channel::ID:
      return ChannelId(static_cast<const telegram_api::channel *>(chat.get())->id_);
    case telegram_api::channelForbidden::ID:
      return ChannelId(static_cast<const telegram_api::channelForbidden *>(chat.get())->id_);
    default:
      return ChannelId();
  }
}

DialogId ContactsManager::get_dialog_id(const tl_object_ptr<telegram_api::Chat> &chat) {
  auto channel_id = get_channel_id(chat);
  if (channel_id.is_valid()) {
    return DialogId(channel_id);
  }
  return DialogId(get_chat_id(chat));
}

void ContactsManager::on_get_user(tl_object_ptr<telegram_api::User> &&user_ptr, const char *source, bool is_me) {
  LOG(DEBUG) << "Receive from " << source << ' ' << to_string(user_ptr);
  int32 constructor_id = user_ptr->get_id();
  if (constructor_id == telegram_api::userEmpty::ID) {
    auto user = move_tl_object_as<telegram_api::userEmpty>(user_ptr);
    UserId user_id(user->id_);
    if (!user_id.is_valid()) {
      LOG(ERROR) << "Receive invalid " << user_id << " from " << source;
      return;
    }
    LOG(INFO) << "Receive empty " << user_id << " from " << source;

    User *u = get_user_force(user_id);
    if (u == nullptr && Slice(source) != Slice("GetUsersQuery")) {
      // userEmpty should be received only through getUsers for unexisting users
      LOG(ERROR) << "Have no information about " << user_id << ", but received userEmpty from " << source;
    }
    return;
  }

  CHECK(constructor_id == telegram_api::user::ID);
  auto user = move_tl_object_as<telegram_api::user>(user_ptr);
  UserId user_id(user->id_);
  if (!user_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << user_id;
    return;
  }
  int32 flags = user->flags_;
  LOG(INFO) << "Receive " << user_id << " with flags " << flags << " from " << source;
  if (is_me && (flags & USER_FLAG_IS_ME) == 0) {
    LOG(ERROR) << user_id << " doesn't have flag IS_ME, but must have it when received from " << source;
    flags |= USER_FLAG_IS_ME;
  }

  bool is_bot = (flags & USER_FLAG_IS_BOT) != 0;
  if (flags & USER_FLAG_IS_ME) {
    set_my_id(user_id);
    if (!is_bot) {
      td_->option_manager_->set_option_string("my_phone_number", user->phone_);
    }
  }

  bool have_access_hash = (flags & USER_FLAG_HAS_ACCESS_HASH) != 0;
  bool is_received = (flags & USER_FLAG_IS_INACCESSIBLE) == 0;
  bool is_contact = (flags & USER_FLAG_IS_CONTACT) != 0;

  if (!have_min_user(user_id)) {
    if (!is_received) {
      // we must preload received inaccessible users from database in order to not save
      // the min-user to the database and to not override access_hash and another info
      if (!have_user_force(user_id)) {
        LOG(INFO) << "Receive inaccessible " << user_id;
      }
    } else if (is_contact && !are_contacts_loaded_) {
      // preload contact users from database to know that is_contact didn't changed
      // and the list of contacts doesn't need to be saved to the database
      if (!have_user_force(user_id)) {
        LOG(INFO) << "Receive contact " << user_id << " for the first time";
      }
    }
  }

  User *u = add_user(user_id, "on_get_user");
  if (have_access_hash) {  // access_hash must be updated before photo
    auto access_hash = user->access_hash_;
    bool is_min_access_hash = !is_received && !((flags & USER_FLAG_HAS_PHONE_NUMBER) != 0 && user->phone_.empty());
    if (u->access_hash != access_hash && (!is_min_access_hash || u->is_min_access_hash || u->access_hash == -1)) {
      LOG(DEBUG) << "Access hash has changed for " << user_id << " from " << u->access_hash << "/"
                 << u->is_min_access_hash << " to " << access_hash << "/" << is_min_access_hash;
      u->access_hash = access_hash;
      u->is_min_access_hash = is_min_access_hash;
      u->need_save_to_database = true;
    }
  }
  if (is_received || !user->phone_.empty()) {
    on_update_user_phone_number(u, user_id, std::move(user->phone_));
  }
  if (is_received || u->need_apply_min_photo || !u->is_received) {
    on_update_user_photo(u, user_id, std::move(user->photo_), source);
  }
  if (is_received) {
    on_update_user_online(u, user_id, std::move(user->status_));

    auto is_mutual_contact = (flags & USER_FLAG_IS_MUTUAL_CONTACT) != 0;
    on_update_user_is_contact(u, user_id, is_contact, is_mutual_contact);
  }

  if (is_received || !u->is_received) {
    on_update_user_name(u, user_id, std::move(user->first_name_), std::move(user->last_name_),
                        std::move(user->username_));
  }
  on_update_user_emoji_status(u, user_id, EmojiStatus(std::move(user->emoji_status_)));

  bool is_verified = (flags & USER_FLAG_IS_VERIFIED) != 0;
  bool is_premium = (flags & USER_FLAG_IS_PREMIUM) != 0;
  bool is_support = (flags & USER_FLAG_IS_SUPPORT) != 0;
  bool is_deleted = (flags & USER_FLAG_IS_DELETED) != 0;
  bool can_join_groups = (flags & USER_FLAG_IS_PRIVATE_BOT) == 0;
  bool can_read_all_group_messages = (flags & USER_FLAG_IS_BOT_WITH_PRIVACY_DISABLED) != 0;
  bool can_be_added_to_attach_menu = (flags & USER_FLAG_IS_ATTACH_MENU_BOT) != 0;
  bool attach_menu_enabled = (flags & USER_FLAG_ATTACH_MENU_ENABLED) != 0;
  auto restriction_reasons = get_restriction_reasons(std::move(user->restriction_reason_));
  bool is_scam = (flags & USER_FLAG_IS_SCAM) != 0;
  bool is_inline_bot = (flags & USER_FLAG_IS_INLINE_BOT) != 0;
  string inline_query_placeholder = user->bot_inline_placeholder_;
  bool need_location_bot = (flags & USER_FLAG_NEED_LOCATION_BOT) != 0;
  bool has_bot_info_version = (flags & USER_FLAG_HAS_BOT_INFO_VERSION) != 0;
  bool need_apply_min_photo = (flags & USER_FLAG_NEED_APPLY_MIN_PHOTO) != 0;
  bool is_fake = (flags & USER_FLAG_IS_FAKE) != 0;

  LOG_IF(ERROR, !can_join_groups && !is_bot)
      << "Receive not bot " << user_id << " which can't join groups from " << source;
  LOG_IF(ERROR, can_read_all_group_messages && !is_bot)
      << "Receive not bot " << user_id << " which can read all group messages from " << source;
  LOG_IF(ERROR, can_be_added_to_attach_menu && !is_bot)
      << "Receive not bot " << user_id << " which can be added to attachment menu from " << source;
  LOG_IF(ERROR, is_inline_bot && !is_bot) << "Receive not bot " << user_id << " which is inline bot from " << source;
  LOG_IF(ERROR, need_location_bot && !is_inline_bot)
      << "Receive not inline bot " << user_id << " which needs user location from " << source;

  if (is_deleted) {
    // just in case
    is_verified = false;
    is_premium = false;
    is_support = false;
    is_bot = false;
    can_join_groups = false;
    can_read_all_group_messages = false;
    can_be_added_to_attach_menu = false;
    is_inline_bot = false;
    inline_query_placeholder = string();
    need_location_bot = false;
    has_bot_info_version = false;
    need_apply_min_photo = false;
  }

  LOG_IF(ERROR, has_bot_info_version && !is_bot)
      << "Receive not bot " << user_id << " which has bot info version from " << source;

  int32 bot_info_version = has_bot_info_version ? user->bot_info_version_ : -1;
  if (is_verified != u->is_verified || is_support != u->is_support || is_bot != u->is_bot ||
      can_join_groups != u->can_join_groups || can_read_all_group_messages != u->can_read_all_group_messages ||
      restriction_reasons != u->restriction_reasons || is_scam != u->is_scam || is_fake != u->is_fake ||
      is_inline_bot != u->is_inline_bot || inline_query_placeholder != u->inline_query_placeholder ||
      need_location_bot != u->need_location_bot || can_be_added_to_attach_menu != u->can_be_added_to_attach_menu ||
      attach_menu_enabled != u->attach_menu_enabled) {
    LOG_IF(ERROR, is_bot != u->is_bot && !is_deleted && !u->is_deleted && u->is_received)
        << "User.is_bot has changed for " << user_id << "/" << u->username << " from " << source << " from "
        << u->is_bot << " to " << is_bot;
    u->is_verified = is_verified;
    u->is_support = is_support;
    u->is_bot = is_bot;
    u->can_join_groups = can_join_groups;
    u->can_read_all_group_messages = can_read_all_group_messages;
    u->restriction_reasons = std::move(restriction_reasons);
    u->is_scam = is_scam;
    u->is_fake = is_fake;
    u->is_inline_bot = is_inline_bot;
    u->inline_query_placeholder = std::move(inline_query_placeholder);
    u->need_location_bot = need_location_bot;
    u->can_be_added_to_attach_menu = can_be_added_to_attach_menu;
    u->attach_menu_enabled = attach_menu_enabled;

    LOG(DEBUG) << "Info has changed for " << user_id;
    u->is_changed = true;
  }
  if (is_premium != u->is_premium) {
    u->is_premium = is_premium;
    u->is_changed = true;
  }

  if (u->bot_info_version != bot_info_version) {
    u->bot_info_version = bot_info_version;
    LOG(DEBUG) << "Bot info version has changed for " << user_id;
    u->need_save_to_database = true;
  }
  if (is_received && u->need_apply_min_photo != need_apply_min_photo) {
    u->need_apply_min_photo = need_apply_min_photo;
    u->need_save_to_database = true;
  }

  if (is_received && !u->is_received) {
    u->is_received = true;

    LOG(DEBUG) << "Receive " << user_id;
    u->is_changed = true;
  }

  if (is_deleted != u->is_deleted) {
    u->is_deleted = is_deleted;

    LOG(DEBUG) << "User.is_deleted has changed for " << user_id << " to " << u->is_deleted;
    u->is_is_deleted_changed = true;
    u->is_changed = true;
  }

  bool has_language_code = (flags & USER_FLAG_HAS_LANGUAGE_CODE) != 0;
  LOG_IF(ERROR, has_language_code && !td_->auth_manager_->is_bot())
      << "Receive language code for " << user_id << " from " << source;
  if (u->language_code != user->lang_code_ && !user->lang_code_.empty()) {
    u->language_code = user->lang_code_;

    LOG(DEBUG) << "Language code has changed for " << user_id << " to " << u->language_code;
    u->is_changed = true;
  }

  if (u->cache_version != User::CACHE_VERSION && u->is_received) {
    u->cache_version = User::CACHE_VERSION;
    u->need_save_to_database = true;
  }
  u->is_received_from_server = true;
  update_user(u, user_id);
}

class ContactsManager::UserLogEvent {
 public:
  UserId user_id;
  const User *u_in = nullptr;
  unique_ptr<User> u_out;

  UserLogEvent() = default;

  UserLogEvent(UserId user_id, const User *u) : user_id(user_id), u_in(u) {
  }

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(user_id, storer);
    td::store(*u_in, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(user_id, parser);
    td::parse(u_out, parser);
  }
};

void ContactsManager::save_user(User *u, UserId user_id, bool from_binlog) {
  if (!G()->parameters().use_chat_info_db) {
    return;
  }
  CHECK(u != nullptr);
  if (!u->is_saved || !u->is_status_saved) {  // TODO more effective handling of !u->is_status_saved
    if (!from_binlog) {
      auto log_event = UserLogEvent(user_id, u);
      auto storer = get_log_event_storer(log_event);
      if (u->log_event_id == 0) {
        u->log_event_id = binlog_add(G()->td_db()->get_binlog(), LogEvent::HandlerType::Users, storer);
      } else {
        binlog_rewrite(G()->td_db()->get_binlog(), u->log_event_id, LogEvent::HandlerType::Users, storer);
      }
    }

    save_user_to_database(u, user_id);
  }
}

void ContactsManager::on_binlog_user_event(BinlogEvent &&event) {
  if (!G()->parameters().use_chat_info_db) {
    binlog_erase(G()->td_db()->get_binlog(), event.id_);
    return;
  }

  UserLogEvent log_event;
  log_event_parse(log_event, event.data_).ensure();

  auto user_id = log_event.user_id;
  if (have_min_user(user_id) || !user_id.is_valid()) {
    LOG(ERROR) << "Skip adding already added " << user_id;
    binlog_erase(G()->td_db()->get_binlog(), event.id_);
    return;
  }

  LOG(INFO) << "Add " << user_id << " from binlog";
  users_.set(user_id, std::move(log_event.u_out));

  User *u = get_user(user_id);
  CHECK(u != nullptr);
  u->log_event_id = event.id_;

  update_user(u, user_id, true, false);
}

string ContactsManager::get_user_database_key(UserId user_id) {
  return PSTRING() << "us" << user_id.get();
}

string ContactsManager::get_user_database_value(const User *u) {
  return log_event_store(*u).as_slice().str();
}

void ContactsManager::save_user_to_database(User *u, UserId user_id) {
  CHECK(u != nullptr);
  if (u->is_being_saved) {
    return;
  }
  if (loaded_from_database_users_.count(user_id)) {
    save_user_to_database_impl(u, user_id, get_user_database_value(u));
    return;
  }
  if (load_user_from_database_queries_.count(user_id) != 0) {
    return;
  }

  load_user_from_database_impl(user_id, Auto());
}

void ContactsManager::save_user_to_database_impl(User *u, UserId user_id, string value) {
  CHECK(u != nullptr);
  CHECK(load_user_from_database_queries_.count(user_id) == 0);
  CHECK(!u->is_being_saved);
  u->is_being_saved = true;
  u->is_saved = true;
  u->is_status_saved = true;
  LOG(INFO) << "Trying to save to database " << user_id;
  G()->td_db()->get_sqlite_pmc()->set(
      get_user_database_key(user_id), std::move(value), PromiseCreator::lambda([user_id](Result<> result) {
        send_closure(G()->contacts_manager(), &ContactsManager::on_save_user_to_database, user_id, result.is_ok());
      }));
}

void ContactsManager::on_save_user_to_database(UserId user_id, bool success) {
  if (G()->close_flag()) {
    return;
  }

  User *u = get_user(user_id);
  CHECK(u != nullptr);
  LOG_CHECK(u->is_being_saved) << user_id << " " << u->is_saved << " " << u->is_status_saved << " "
                               << load_user_from_database_queries_.count(user_id) << " " << u->is_received << " "
                               << u->is_deleted << " " << u->is_bot << " " << u->need_save_to_database << " "
                               << u->is_changed << " " << u->is_status_changed << " " << u->is_name_changed << " "
                               << u->is_username_changed << " " << u->is_photo_changed << " "
                               << u->is_is_contact_changed << " " << u->is_is_deleted_changed;
  CHECK(load_user_from_database_queries_.count(user_id) == 0);
  u->is_being_saved = false;

  if (!success) {
    LOG(ERROR) << "Failed to save " << user_id << " to database";
    u->is_saved = false;
    u->is_status_saved = false;
  } else {
    LOG(INFO) << "Successfully saved " << user_id << " to database";
  }
  if (u->is_saved && u->is_status_saved) {
    if (u->log_event_id != 0) {
      binlog_erase(G()->td_db()->get_binlog(), u->log_event_id);
      u->log_event_id = 0;
    }
  } else {
    save_user(u, user_id, u->log_event_id != 0);
  }
}

void ContactsManager::load_user_from_database(User *u, UserId user_id, Promise<Unit> promise) {
  if (loaded_from_database_users_.count(user_id)) {
    promise.set_value(Unit());
    return;
  }

  CHECK(u == nullptr || !u->is_being_saved);
  load_user_from_database_impl(user_id, std::move(promise));
}

void ContactsManager::load_user_from_database_impl(UserId user_id, Promise<Unit> promise) {
  LOG(INFO) << "Load " << user_id << " from database";
  auto &load_user_queries = load_user_from_database_queries_[user_id];
  load_user_queries.push_back(std::move(promise));
  if (load_user_queries.size() == 1u) {
    G()->td_db()->get_sqlite_pmc()->get(get_user_database_key(user_id), PromiseCreator::lambda([user_id](string value) {
                                          send_closure(G()->contacts_manager(),
                                                       &ContactsManager::on_load_user_from_database, user_id,
                                                       std::move(value), false);
                                        }));
  }
}

void ContactsManager::on_load_user_from_database(UserId user_id, string value, bool force) {
  if (G()->close_flag() && !force) {
    // the user is in Binlog and will be saved after restart
    return;
  }

  CHECK(user_id.is_valid());
  if (!loaded_from_database_users_.insert(user_id).second) {
    return;
  }

  auto it = load_user_from_database_queries_.find(user_id);
  vector<Promise<Unit>> promises;
  if (it != load_user_from_database_queries_.end()) {
    promises = std::move(it->second);
    CHECK(!promises.empty());
    load_user_from_database_queries_.erase(it);
  }

  LOG(INFO) << "Successfully loaded " << user_id << " of size " << value.size() << " from database";
  //  G()->td_db()->get_sqlite_pmc()->erase(get_user_database_key(user_id), Auto());
  //  return;

  User *u = get_user(user_id);
  if (u == nullptr) {
    if (!value.empty()) {
      u = add_user(user_id, "on_load_user_from_database");

      log_event_parse(*u, value).ensure();

      u->is_saved = true;
      u->is_status_saved = true;
      update_user(u, user_id, true, true);
    }
  } else {
    CHECK(!u->is_saved);  // user can't be saved before load completes
    CHECK(!u->is_being_saved);
    auto new_value = get_user_database_value(u);
    if (value != new_value) {
      save_user_to_database_impl(u, user_id, std::move(new_value));
    } else if (u->log_event_id != 0) {
      binlog_erase(G()->td_db()->get_binlog(), u->log_event_id);
      u->log_event_id = 0;
    }
  }

  set_promises(promises);
}

bool ContactsManager::have_user_force(UserId user_id) {
  return get_user_force(user_id) != nullptr;
}

ContactsManager::User *ContactsManager::get_user_force(UserId user_id) {
  auto u = get_user_force_impl(user_id);
  if ((u == nullptr || !u->is_received) &&
      (user_id == get_service_notifications_user_id() || user_id == get_replies_bot_user_id() ||
       user_id == get_anonymous_bot_user_id() || user_id == get_channel_bot_user_id())) {
    int32 flags = USER_FLAG_HAS_ACCESS_HASH | USER_FLAG_HAS_FIRST_NAME | USER_FLAG_NEED_APPLY_MIN_PHOTO;
    int64 profile_photo_id = 0;
    int32 profile_photo_dc_id = 1;
    string first_name;
    string last_name;
    string username;
    string phone_number;
    int32 bot_info_version = 0;

    if (user_id == get_service_notifications_user_id()) {
      flags |= USER_FLAG_HAS_PHONE_NUMBER | USER_FLAG_IS_VERIFIED | USER_FLAG_IS_SUPPORT;
      first_name = "Telegram";
      if (G()->is_test_dc()) {
        flags |= USER_FLAG_HAS_LAST_NAME;
        last_name = "Notifications";
      }
      phone_number = "42777";
      profile_photo_id = 3337190045231023;
    } else if (user_id == get_replies_bot_user_id()) {
      flags |= USER_FLAG_HAS_USERNAME | USER_FLAG_IS_BOT;
      if (!G()->is_test_dc()) {
        flags |= USER_FLAG_IS_PRIVATE_BOT;
      }
      first_name = "Replies";
      username = "replies";
      bot_info_version = G()->is_test_dc() ? 1 : 3;
    } else if (user_id == get_anonymous_bot_user_id()) {
      flags |= USER_FLAG_HAS_USERNAME | USER_FLAG_IS_BOT;
      if (!G()->is_test_dc()) {
        flags |= USER_FLAG_IS_PRIVATE_BOT;
      }
      first_name = "Group";
      username = G()->is_test_dc() ? "izgroupbot" : "GroupAnonymousBot";
      bot_info_version = G()->is_test_dc() ? 1 : 3;
      profile_photo_id = 5159307831025969322;
    } else if (user_id == get_channel_bot_user_id()) {
      flags |= USER_FLAG_HAS_USERNAME | USER_FLAG_IS_BOT;
      if (!G()->is_test_dc()) {
        flags |= USER_FLAG_IS_PRIVATE_BOT;
      }
      first_name = G()->is_test_dc() ? "Channels" : "Channel";
      username = G()->is_test_dc() ? "channelsbot" : "Channel_Bot";
      bot_info_version = G()->is_test_dc() ? 1 : 4;
      profile_photo_id = 587627495930570665;
    }

    telegram_api::object_ptr<telegram_api::userProfilePhoto> profile_photo;
    if (!G()->is_test_dc() && profile_photo_id != 0) {
      profile_photo = telegram_api::make_object<telegram_api::userProfilePhoto>(0, false /*ignored*/, profile_photo_id,
                                                                                BufferSlice(), profile_photo_dc_id);
    }

    auto user = telegram_api::make_object<telegram_api::user>(
        flags, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/,
        false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/,
        false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/, false /*ignored*/,
        false /*ignored*/, false /*ignored*/, false /*ignored*/, user_id.get(), 1, first_name, string(), username,
        phone_number, std::move(profile_photo), nullptr, bot_info_version, Auto(), string(), string(), nullptr);
    on_get_user(std::move(user), "get_user_force");
    u = get_user(user_id);
    CHECK(u != nullptr && u->is_received);
  }
  return u;
}

ContactsManager::User *ContactsManager::get_user_force_impl(UserId user_id) {
  if (!user_id.is_valid()) {
    return nullptr;
  }

  User *u = get_user(user_id);
  if (u != nullptr) {
    return u;
  }
  if (!G()->parameters().use_chat_info_db) {
    return nullptr;
  }
  if (loaded_from_database_users_.count(user_id)) {
    return nullptr;
  }

  LOG(INFO) << "Trying to load " << user_id << " from database";
  on_load_user_from_database(user_id, G()->td_db()->get_sqlite_sync_pmc()->get(get_user_database_key(user_id)), true);
  return get_user(user_id);
}

class ContactsManager::ChatLogEvent {
 public:
  ChatId chat_id;
  const Chat *c_in = nullptr;
  unique_ptr<Chat> c_out;

  ChatLogEvent() = default;

  ChatLogEvent(ChatId chat_id, const Chat *c) : chat_id(chat_id), c_in(c) {
  }

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(chat_id, storer);
    td::store(*c_in, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(chat_id, parser);
    td::parse(c_out, parser);
  }
};

void ContactsManager::save_chat(Chat *c, ChatId chat_id, bool from_binlog) {
  if (!G()->parameters().use_chat_info_db) {
    return;
  }
  CHECK(c != nullptr);
  if (!c->is_saved) {
    if (!from_binlog) {
      auto log_event = ChatLogEvent(chat_id, c);
      auto storer = get_log_event_storer(log_event);
      if (c->log_event_id == 0) {
        c->log_event_id = binlog_add(G()->td_db()->get_binlog(), LogEvent::HandlerType::Chats, storer);
      } else {
        binlog_rewrite(G()->td_db()->get_binlog(), c->log_event_id, LogEvent::HandlerType::Chats, storer);
      }
    }

    save_chat_to_database(c, chat_id);
    return;
  }
}

void ContactsManager::on_binlog_chat_event(BinlogEvent &&event) {
  if (!G()->parameters().use_chat_info_db) {
    binlog_erase(G()->td_db()->get_binlog(), event.id_);
    return;
  }

  ChatLogEvent log_event;
  log_event_parse(log_event, event.data_).ensure();

  auto chat_id = log_event.chat_id;
  if (have_chat(chat_id) || !chat_id.is_valid()) {
    LOG(ERROR) << "Skip adding already added " << chat_id;
    binlog_erase(G()->td_db()->get_binlog(), event.id_);
    return;
  }

  LOG(INFO) << "Add " << chat_id << " from binlog";
  chats_.set(chat_id, std::move(log_event.c_out));

  Chat *c = get_chat(chat_id);
  CHECK(c != nullptr);
  c->log_event_id = event.id_;

  update_chat(c, chat_id, true, false);
}

string ContactsManager::get_chat_database_key(ChatId chat_id) {
  return PSTRING() << "gr" << chat_id.get();
}

string ContactsManager::get_chat_database_value(const Chat *c) {
  return log_event_store(*c).as_slice().str();
}

void ContactsManager::save_chat_to_database(Chat *c, ChatId chat_id) {
  CHECK(c != nullptr);
  if (c->is_being_saved) {
    return;
  }
  if (loaded_from_database_chats_.count(chat_id)) {
    save_chat_to_database_impl(c, chat_id, get_chat_database_value(c));
    return;
  }
  if (load_chat_from_database_queries_.count(chat_id) != 0) {
    return;
  }

  load_chat_from_database_impl(chat_id, Auto());
}

void ContactsManager::save_chat_to_database_impl(Chat *c, ChatId chat_id, string value) {
  CHECK(c != nullptr);
  CHECK(load_chat_from_database_queries_.count(chat_id) == 0);
  CHECK(!c->is_being_saved);
  c->is_being_saved = true;
  c->is_saved = true;
  LOG(INFO) << "Trying to save to database " << chat_id;
  G()->td_db()->get_sqlite_pmc()->set(
      get_chat_database_key(chat_id), std::move(value), PromiseCreator::lambda([chat_id](Result<> result) {
        send_closure(G()->contacts_manager(), &ContactsManager::on_save_chat_to_database, chat_id, result.is_ok());
      }));
}

void ContactsManager::on_save_chat_to_database(ChatId chat_id, bool success) {
  if (G()->close_flag()) {
    return;
  }

  Chat *c = get_chat(chat_id);
  CHECK(c != nullptr);
  CHECK(c->is_being_saved);
  CHECK(load_chat_from_database_queries_.count(chat_id) == 0);
  c->is_being_saved = false;

  if (!success) {
    LOG(ERROR) << "Failed to save " << chat_id << " to database";
    c->is_saved = false;
  } else {
    LOG(INFO) << "Successfully saved " << chat_id << " to database";
  }
  if (c->is_saved) {
    if (c->log_event_id != 0) {
      binlog_erase(G()->td_db()->get_binlog(), c->log_event_id);
      c->log_event_id = 0;
    }
  } else {
    save_chat(c, chat_id, c->log_event_id != 0);
  }
}

void ContactsManager::load_chat_from_database(Chat *c, ChatId chat_id, Promise<Unit> promise) {
  if (loaded_from_database_chats_.count(chat_id)) {
    promise.set_value(Unit());
    return;
  }

  CHECK(c == nullptr || !c->is_being_saved);
  load_chat_from_database_impl(chat_id, std::move(promise));
}

void ContactsManager::load_chat_from_database_impl(ChatId chat_id, Promise<Unit> promise) {
  LOG(INFO) << "Load " << chat_id << " from database";
  auto &load_chat_queries = load_chat_from_database_queries_[chat_id];
  load_chat_queries.push_back(std::move(promise));
  if (load_chat_queries.size() == 1u) {
    G()->td_db()->get_sqlite_pmc()->get(get_chat_database_key(chat_id), PromiseCreator::lambda([chat_id](string value) {
                                          send_closure(G()->contacts_manager(),
                                                       &ContactsManager::on_load_chat_from_database, chat_id,
                                                       std::move(value), false);
                                        }));
  }
}

void ContactsManager::on_load_chat_from_database(ChatId chat_id, string value, bool force) {
  if (G()->close_flag() && !force) {
    // the chat is in Binlog and will be saved after restart
    return;
  }

  CHECK(chat_id.is_valid());
  if (!loaded_from_database_chats_.insert(chat_id).second) {
    return;
  }

  auto it = load_chat_from_database_queries_.find(chat_id);
  vector<Promise<Unit>> promises;
  if (it != load_chat_from_database_queries_.end()) {
    promises = std::move(it->second);
    CHECK(!promises.empty());
    load_chat_from_database_queries_.erase(it);
  }

  LOG(INFO) << "Successfully loaded " << chat_id << " of size " << value.size() << " from database";
  //  G()->td_db()->get_sqlite_pmc()->erase(get_chat_database_key(chat_id), Auto());
  //  return;

  Chat *c = get_chat(chat_id);
  if (c == nullptr) {
    if (!value.empty()) {
      c = add_chat(chat_id);

      log_event_parse(*c, value).ensure();

      c->is_saved = true;
      update_chat(c, chat_id, true, true);
    }
  } else {
    CHECK(!c->is_saved);  // chat can't be saved before load completes
    CHECK(!c->is_being_saved);
    auto new_value = get_chat_database_value(c);
    if (value != new_value) {
      save_chat_to_database_impl(c, chat_id, std::move(new_value));
    } else if (c->log_event_id != 0) {
      binlog_erase(G()->td_db()->get_binlog(), c->log_event_id);
      c->log_event_id = 0;
    }
  }

  if (c != nullptr && c->migrated_to_channel_id.is_valid() && !have_channel_force(c->migrated_to_channel_id)) {
    LOG(ERROR) << "Can't find " << c->migrated_to_channel_id << " from " << chat_id;
  }

  set_promises(promises);
}

bool ContactsManager::have_chat_force(ChatId chat_id) {
  return get_chat_force(chat_id) != nullptr;
}

ContactsManager::Chat *ContactsManager::get_chat_force(ChatId chat_id) {
  if (!chat_id.is_valid()) {
    return nullptr;
  }

  Chat *c = get_chat(chat_id);
  if (c != nullptr) {
    if (c->migrated_to_channel_id.is_valid() && !have_channel_force(c->migrated_to_channel_id)) {
      LOG(ERROR) << "Can't find " << c->migrated_to_channel_id << " from " << chat_id;
    }

    return c;
  }
  if (!G()->parameters().use_chat_info_db) {
    return nullptr;
  }
  if (loaded_from_database_chats_.count(chat_id)) {
    return nullptr;
  }

  LOG(INFO) << "Trying to load " << chat_id << " from database";
  on_load_chat_from_database(chat_id, G()->td_db()->get_sqlite_sync_pmc()->get(get_chat_database_key(chat_id)), true);
  return get_chat(chat_id);
}

class ContactsManager::ChannelLogEvent {
 public:
  ChannelId channel_id;
  const Channel *c_in = nullptr;
  unique_ptr<Channel> c_out;

  ChannelLogEvent() = default;

  ChannelLogEvent(ChannelId channel_id, const Channel *c) : channel_id(channel_id), c_in(c) {
  }

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(channel_id, storer);
    td::store(*c_in, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(channel_id, parser);
    td::parse(c_out, parser);
  }
};

void ContactsManager::save_channel(Channel *c, ChannelId channel_id, bool from_binlog) {
  if (!G()->parameters().use_chat_info_db) {
    return;
  }
  CHECK(c != nullptr);
  if (!c->is_saved) {
    if (!from_binlog) {
      auto log_event = ChannelLogEvent(channel_id, c);
      auto storer = get_log_event_storer(log_event);
      if (c->log_event_id == 0) {
        c->log_event_id = binlog_add(G()->td_db()->get_binlog(), LogEvent::HandlerType::Channels, storer);
      } else {
        binlog_rewrite(G()->td_db()->get_binlog(), c->log_event_id, LogEvent::HandlerType::Channels, storer);
      }
    }

    save_channel_to_database(c, channel_id);
    return;
  }
}

void ContactsManager::on_binlog_channel_event(BinlogEvent &&event) {
  if (!G()->parameters().use_chat_info_db) {
    binlog_erase(G()->td_db()->get_binlog(), event.id_);
    return;
  }

  ChannelLogEvent log_event;
  log_event_parse(log_event, event.data_).ensure();

  auto channel_id = log_event.channel_id;
  if (have_channel(channel_id) || !channel_id.is_valid()) {
    LOG(ERROR) << "Skip adding already added " << channel_id;
    binlog_erase(G()->td_db()->get_binlog(), event.id_);
    return;
  }

  LOG(INFO) << "Add " << channel_id << " from binlog";
  channels_.set(channel_id, std::move(log_event.c_out));

  Channel *c = get_channel(channel_id);
  CHECK(c != nullptr);
  c->log_event_id = event.id_;

  update_channel(c, channel_id, true, false);
}

string ContactsManager::get_channel_database_key(ChannelId channel_id) {
  return PSTRING() << "ch" << channel_id.get();
}

string ContactsManager::get_channel_database_value(const Channel *c) {
  return log_event_store(*c).as_slice().str();
}

void ContactsManager::save_channel_to_database(Channel *c, ChannelId channel_id) {
  CHECK(c != nullptr);
  if (c->is_being_saved) {
    return;
  }
  if (loaded_from_database_channels_.count(channel_id)) {
    save_channel_to_database_impl(c, channel_id, get_channel_database_value(c));
    return;
  }
  if (load_channel_from_database_queries_.count(channel_id) != 0) {
    return;
  }

  load_channel_from_database_impl(channel_id, Auto());
}

void ContactsManager::save_channel_to_database_impl(Channel *c, ChannelId channel_id, string value) {
  CHECK(c != nullptr);
  CHECK(load_channel_from_database_queries_.count(channel_id) == 0);
  CHECK(!c->is_being_saved);
  c->is_being_saved = true;
  c->is_saved = true;
  LOG(INFO) << "Trying to save to database " << channel_id;
  G()->td_db()->get_sqlite_pmc()->set(
      get_channel_database_key(channel_id), std::move(value), PromiseCreator::lambda([channel_id](Result<> result) {
        send_closure(G()->contacts_manager(), &ContactsManager::on_save_channel_to_database, channel_id,
                     result.is_ok());
      }));
}

void ContactsManager::on_save_channel_to_database(ChannelId channel_id, bool success) {
  if (G()->close_flag()) {
    return;
  }

  Channel *c = get_channel(channel_id);
  CHECK(c != nullptr);
  CHECK(c->is_being_saved);
  CHECK(load_channel_from_database_queries_.count(channel_id) == 0);
  c->is_being_saved = false;

  if (!success) {
    LOG(ERROR) << "Failed to save " << channel_id << " to database";
    c->is_saved = false;
  } else {
    LOG(INFO) << "Successfully saved " << channel_id << " to database";
  }
  if (c->is_saved) {
    if (c->log_event_id != 0) {
      binlog_erase(G()->td_db()->get_binlog(), c->log_event_id);
      c->log_event_id = 0;
    }
  } else {
    save_channel(c, channel_id, c->log_event_id != 0);
  }
}

void ContactsManager::load_channel_from_database(Channel *c, ChannelId channel_id, Promise<Unit> promise) {
  if (loaded_from_database_channels_.count(channel_id)) {
    promise.set_value(Unit());
    return;
  }

  CHECK(c == nullptr || !c->is_being_saved);
  load_channel_from_database_impl(channel_id, std::move(promise));
}

void ContactsManager::load_channel_from_database_impl(ChannelId channel_id, Promise<Unit> promise) {
  LOG(INFO) << "Load " << channel_id << " from database";
  auto &load_channel_queries = load_channel_from_database_queries_[channel_id];
  load_channel_queries.push_back(std::move(promise));
  if (load_channel_queries.size() == 1u) {
    G()->td_db()->get_sqlite_pmc()->get(
        get_channel_database_key(channel_id), PromiseCreator::lambda([channel_id](string value) {
          send_closure(G()->contacts_manager(), &ContactsManager::on_load_channel_from_database, channel_id,
                       std::move(value), false);
        }));
  }
}

void ContactsManager::on_load_channel_from_database(ChannelId channel_id, string value, bool force) {
  if (G()->close_flag() && !force) {
    // the channel is in Binlog and will be saved after restart
    return;
  }

  CHECK(channel_id.is_valid());
  if (!loaded_from_database_channels_.insert(channel_id).second) {
    return;
  }

  auto it = load_channel_from_database_queries_.find(channel_id);
  vector<Promise<Unit>> promises;
  if (it != load_channel_from_database_queries_.end()) {
    promises = std::move(it->second);
    CHECK(!promises.empty());
    load_channel_from_database_queries_.erase(it);
  }

  LOG(INFO) << "Successfully loaded " << channel_id << " of size " << value.size() << " from database";
  //  G()->td_db()->get_sqlite_pmc()->erase(get_channel_database_key(channel_id), Auto());
  //  return;

  Channel *c = get_channel(channel_id);
  if (c == nullptr) {
    if (!value.empty()) {
      c = add_channel(channel_id, "on_load_channel_from_database");

      log_event_parse(*c, value).ensure();

      c->is_saved = true;
      update_channel(c, channel_id, true, true);
    }
  } else {
    CHECK(!c->is_saved);  // channel can't be saved before load completes
    CHECK(!c->is_being_saved);
    if (!value.empty()) {
      Channel temp_c;
      log_event_parse(temp_c, value).ensure();
      if (c->participant_count == 0 && temp_c.participant_count != 0) {
        c->participant_count = temp_c.participant_count;
        CHECK(c->is_update_supergroup_sent);
        send_closure(G()->td(), &Td::send_update,
                     make_tl_object<td_api::updateSupergroup>(get_supergroup_object(channel_id, c)));
      }

      c->status.update_restrictions();
      temp_c.status.update_restrictions();
      if (temp_c.status != c->status) {
        on_channel_status_changed(c, channel_id, temp_c.status, c->status);
        CHECK(!c->is_being_saved);
      }

      if (temp_c.username != c->username) {
        on_channel_username_changed(c, channel_id, temp_c.username, c->username);
        CHECK(!c->is_being_saved);
      }
    }
    auto new_value = get_channel_database_value(c);
    if (value != new_value) {
      save_channel_to_database_impl(c, channel_id, std::move(new_value));
    } else if (c->log_event_id != 0) {
      binlog_erase(G()->td_db()->get_binlog(), c->log_event_id);
      c->log_event_id = 0;
    }
  }

  set_promises(promises);
}

bool ContactsManager::have_channel_force(ChannelId channel_id) {
  return get_channel_force(channel_id) != nullptr;
}

ContactsManager::Channel *ContactsManager::get_channel_force(ChannelId channel_id) {
  if (!channel_id.is_valid()) {
    return nullptr;
  }

  Channel *c = get_channel(channel_id);
  if (c != nullptr) {
    return c;
  }
  if (!G()->parameters().use_chat_info_db) {
    return nullptr;
  }
  if (loaded_from_database_channels_.count(channel_id)) {
    return nullptr;
  }

  LOG(INFO) << "Trying to load " << channel_id << " from database";
  on_load_channel_from_database(channel_id,
                                G()->td_db()->get_sqlite_sync_pmc()->get(get_channel_database_key(channel_id)), true);
  return get_channel(channel_id);
}

class ContactsManager::SecretChatLogEvent {
 public:
  SecretChatId secret_chat_id;
  const SecretChat *c_in = nullptr;
  unique_ptr<SecretChat> c_out;

  SecretChatLogEvent() = default;

  SecretChatLogEvent(SecretChatId secret_chat_id, const SecretChat *c) : secret_chat_id(secret_chat_id), c_in(c) {
  }

  template <class StorerT>
  void store(StorerT &storer) const {
    td::store(secret_chat_id, storer);
    td::store(*c_in, storer);
  }

  template <class ParserT>
  void parse(ParserT &parser) {
    td::parse(secret_chat_id, parser);
    td::parse(c_out, parser);
  }
};

void ContactsManager::save_secret_chat(SecretChat *c, SecretChatId secret_chat_id, bool from_binlog) {
  if (!G()->parameters().use_chat_info_db) {
    return;
  }
  CHECK(c != nullptr);
  if (!c->is_saved) {
    if (!from_binlog) {
      auto log_event = SecretChatLogEvent(secret_chat_id, c);
      auto storer = get_log_event_storer(log_event);
      if (c->log_event_id == 0) {
        c->log_event_id = binlog_add(G()->td_db()->get_binlog(), LogEvent::HandlerType::SecretChatInfos, storer);
      } else {
        binlog_rewrite(G()->td_db()->get_binlog(), c->log_event_id, LogEvent::HandlerType::SecretChatInfos, storer);
      }
    }

    save_secret_chat_to_database(c, secret_chat_id);
    return;
  }
}

void ContactsManager::on_binlog_secret_chat_event(BinlogEvent &&event) {
  if (!G()->parameters().use_chat_info_db) {
    binlog_erase(G()->td_db()->get_binlog(), event.id_);
    return;
  }

  SecretChatLogEvent log_event;
  log_event_parse(log_event, event.data_).ensure();

  auto secret_chat_id = log_event.secret_chat_id;
  if (have_secret_chat(secret_chat_id) || !secret_chat_id.is_valid()) {
    LOG(ERROR) << "Skip adding already added " << secret_chat_id;
    binlog_erase(G()->td_db()->get_binlog(), event.id_);
    return;
  }

  LOG(INFO) << "Add " << secret_chat_id << " from binlog";
  secret_chats_.set(secret_chat_id, std::move(log_event.c_out));

  SecretChat *c = get_secret_chat(secret_chat_id);
  CHECK(c != nullptr);
  c->log_event_id = event.id_;

  update_secret_chat(c, secret_chat_id, true, false);
}

string ContactsManager::get_secret_chat_database_key(SecretChatId secret_chat_id) {
  return PSTRING() << "sc" << secret_chat_id.get();
}

string ContactsManager::get_secret_chat_database_value(const SecretChat *c) {
  return log_event_store(*c).as_slice().str();
}

void ContactsManager::save_secret_chat_to_database(SecretChat *c, SecretChatId secret_chat_id) {
  CHECK(c != nullptr);
  if (c->is_being_saved) {
    return;
  }
  if (loaded_from_database_secret_chats_.count(secret_chat_id)) {
    save_secret_chat_to_database_impl(c, secret_chat_id, get_secret_chat_database_value(c));
    return;
  }
  if (load_secret_chat_from_database_queries_.count(secret_chat_id) != 0) {
    return;
  }

  load_secret_chat_from_database_impl(secret_chat_id, Auto());
}

void ContactsManager::save_secret_chat_to_database_impl(SecretChat *c, SecretChatId secret_chat_id, string value) {
  CHECK(c != nullptr);
  CHECK(load_secret_chat_from_database_queries_.count(secret_chat_id) == 0);
  CHECK(!c->is_being_saved);
  c->is_being_saved = true;
  c->is_saved = true;
  LOG(INFO) << "Trying to save to database " << secret_chat_id;
  G()->td_db()->get_sqlite_pmc()->set(get_secret_chat_database_key(secret_chat_id), std::move(value),
                                      PromiseCreator::lambda([secret_chat_id](Result<> result) {
                                        send_closure(G()->contacts_manager(),
                                                     &ContactsManager::on_save_secret_chat_to_database, secret_chat_id,
                                                     result.is_ok());
                                      }));
}

void ContactsManager::on_save_secret_chat_to_database(SecretChatId secret_chat_id, bool success) {
  if (G()->close_flag()) {
    return;
  }

  SecretChat *c = get_secret_chat(secret_chat_id);
  CHECK(c != nullptr);
  CHECK(c->is_being_saved);
  CHECK(load_secret_chat_from_database_queries_.count(secret_chat_id) == 0);
  c->is_being_saved = false;

  if (!success) {
    LOG(ERROR) << "Failed to save " << secret_chat_id << " to database";
    c->is_saved = false;
  } else {
    LOG(INFO) << "Successfully saved " << secret_chat_id << " to database";
  }
  if (c->is_saved) {
    if (c->log_event_id != 0) {
      binlog_erase(G()->td_db()->get_binlog(), c->log_event_id);
      c->log_event_id = 0;
    }
  } else {
    save_secret_chat(c, secret_chat_id, c->log_event_id != 0);
  }
}

void ContactsManager::load_secret_chat_from_database(SecretChat *c, SecretChatId secret_chat_id,
                                                     Promise<Unit> promise) {
  if (loaded_from_database_secret_chats_.count(secret_chat_id)) {
    promise.set_value(Unit());
    return;
  }

  CHECK(c == nullptr || !c->is_being_saved);
  load_secret_chat_from_database_impl(secret_chat_id, std::move(promise));
}

void ContactsManager::load_secret_chat_from_database_impl(SecretChatId secret_chat_id, Promise<Unit> promise) {
  LOG(INFO) << "Load " << secret_chat_id << " from database";
  auto &load_secret_chat_queries = load_secret_chat_from_database_queries_[secret_chat_id];
  load_secret_chat_queries.push_back(std::move(promise));
  if (load_secret_chat_queries.size() == 1u) {
    G()->td_db()->get_sqlite_pmc()->get(
        get_secret_chat_database_key(secret_chat_id), PromiseCreator::lambda([secret_chat_id](string value) {
          send_closure(G()->contacts_manager(), &ContactsManager::on_load_secret_chat_from_database, secret_chat_id,
                       std::move(value), false);
        }));
  }
}

void ContactsManager::on_load_secret_chat_from_database(SecretChatId secret_chat_id, string value, bool force) {
  if (G()->close_flag() && !force) {
    // the secret chat is in Binlog and will be saved after restart
    return;
  }

  CHECK(secret_chat_id.is_valid());
  if (!loaded_from_database_secret_chats_.insert(secret_chat_id).second) {
    return;
  }

  auto it = load_secret_chat_from_database_queries_.find(secret_chat_id);
  vector<Promise<Unit>> promises;
  if (it != load_secret_chat_from_database_queries_.end()) {
    promises = std::move(it->second);
    CHECK(!promises.empty());
    load_secret_chat_from_database_queries_.erase(it);
  }

  LOG(INFO) << "Successfully loaded " << secret_chat_id << " of size " << value.size() << " from database";
  //  G()->td_db()->get_sqlite_pmc()->erase(get_secret_chat_database_key(secret_chat_id), Auto());
  //  return;

  SecretChat *c = get_secret_chat(secret_chat_id);
  if (c == nullptr) {
    if (!value.empty()) {
      c = add_secret_chat(secret_chat_id);

      log_event_parse(*c, value).ensure();

      c->is_saved = true;
      update_secret_chat(c, secret_chat_id, true, true);
    }
  } else {
    CHECK(!c->is_saved);  // secret chat can't be saved before load completes
    CHECK(!c->is_being_saved);
    auto new_value = get_secret_chat_database_value(c);
    if (value != new_value) {
      save_secret_chat_to_database_impl(c, secret_chat_id, std::move(new_value));
    } else if (c->log_event_id != 0) {
      binlog_erase(G()->td_db()->get_binlog(), c->log_event_id);
      c->log_event_id = 0;
    }
  }

  // TODO load users asynchronously
  if (c != nullptr && !have_user_force(c->user_id)) {
    LOG(ERROR) << "Can't find " << c->user_id << " from " << secret_chat_id;
  }

  set_promises(promises);
}

bool ContactsManager::have_secret_chat_force(SecretChatId secret_chat_id) {
  return get_secret_chat_force(secret_chat_id) != nullptr;
}

ContactsManager::SecretChat *ContactsManager::get_secret_chat_force(SecretChatId secret_chat_id) {
  if (!secret_chat_id.is_valid()) {
    return nullptr;
  }

  SecretChat *c = get_secret_chat(secret_chat_id);
  if (c != nullptr) {
    if (!have_user_force(c->user_id)) {
      LOG(ERROR) << "Can't find " << c->user_id << " from " << secret_chat_id;
    }
    return c;
  }
  if (!G()->parameters().use_chat_info_db) {
    return nullptr;
  }
  if (loaded_from_database_secret_chats_.count(secret_chat_id)) {
    return nullptr;
  }

  LOG(INFO) << "Trying to load " << secret_chat_id << " from database";
  on_load_secret_chat_from_database(
      secret_chat_id, G()->td_db()->get_sqlite_sync_pmc()->get(get_secret_chat_database_key(secret_chat_id)), true);
  return get_secret_chat(secret_chat_id);
}

void ContactsManager::save_user_full(const UserFull *user_full, UserId user_id) {
  if (!G()->parameters().use_chat_info_db) {
    return;
  }

  LOG(INFO) << "Trying to save to database full " << user_id;
  CHECK(user_full != nullptr);
  G()->td_db()->get_sqlite_pmc()->set(get_user_full_database_key(user_id), get_user_full_database_value(user_full),
                                      Auto());
}

string ContactsManager::get_user_full_database_key(UserId user_id) {
  return PSTRING() << "usf" << user_id.get();
}

string ContactsManager::get_user_full_database_value(const UserFull *user_full) {
  return log_event_store(*user_full).as_slice().str();
}

void ContactsManager::on_load_user_full_from_database(UserId user_id, string value) {
  LOG(INFO) << "Successfully loaded full " << user_id << " of size " << value.size() << " from database";
  //  G()->td_db()->get_sqlite_pmc()->erase(get_user_full_database_key(user_id), Auto());
  //  return;

  if (get_user_full(user_id) != nullptr || value.empty()) {
    return;
  }

  UserFull *user_full = add_user_full(user_id);
  auto status = log_event_parse(*user_full, value);
  if (status.is_error()) {
    // can't happen unless database is broken
    LOG(ERROR) << "Repair broken full " << user_id << ' ' << format::as_hex_dump<4>(Slice(value));

    // just clean all known data about the user and pretend that there was nothing in the database
    users_full_.erase(user_id);
    G()->td_db()->get_sqlite_pmc()->erase(get_user_full_database_key(user_id), Auto());
    return;
  }

  Dependencies dependencies;
  dependencies.add(user_id);
  if (!dependencies.resolve_force(td_, "on_load_user_full_from_database")) {
    users_full_.erase(user_id);
    G()->td_db()->get_sqlite_pmc()->erase(get_user_full_database_key(user_id), Auto());
    return;
  }

  if (user_full->need_phone_number_privacy_exception && is_user_contact(user_id)) {
    user_full->need_phone_number_privacy_exception = false;
  }

  User *u = get_user(user_id);
  CHECK(u != nullptr);
  if (u->photo.id != user_full->photo.id.get()) {
    user_full->photo = Photo();
    if (u->photo.id > 0) {
      user_full->expires_at = 0.0;
    }
  }
  if (!user_full->photo.is_empty()) {
    register_user_photo(u, user_id, user_full->photo);
  }

  td_->group_call_manager_->on_update_dialog_about(DialogId(user_id), user_full->about, false);

  user_full->is_update_user_full_sent = true;
  update_user_full(user_full, user_id, "on_load_user_full_from_database", true);

  if (is_user_deleted(user_id)) {
    drop_user_full(user_id);
  } else if (user_full->expires_at == 0.0) {
    load_user_full(user_id, true, Auto(), "on_load_user_full_from_database");
  }
}

ContactsManager::UserFull *ContactsManager::get_user_full_force(UserId user_id) {
  if (!have_user_force(user_id)) {
    return nullptr;
  }

  UserFull *user_full = get_user_full(user_id);
  if (user_full != nullptr) {
    return user_full;
  }
  if (!G()->parameters().use_chat_info_db) {
    return nullptr;
  }
  if (!unavailable_user_fulls_.insert(user_id).second) {
    return nullptr;
  }

  LOG(INFO) << "Trying to load full " << user_id << " from database";
  on_load_user_full_from_database(user_id,
                                  G()->td_db()->get_sqlite_sync_pmc()->get(get_user_full_database_key(user_id)));
  return get_user_full(user_id);
}

void ContactsManager::save_chat_full(const ChatFull *chat_full, ChatId chat_id) {
  if (!G()->parameters().use_chat_info_db) {
    return;
  }

  LOG(INFO) << "Trying to save to database full " << chat_id;
  CHECK(chat_full != nullptr);
  G()->td_db()->get_sqlite_pmc()->set(get_chat_full_database_key(chat_id), get_chat_full_database_value(chat_full),
                                      Auto());
}

string ContactsManager::get_chat_full_database_key(ChatId chat_id) {
  return PSTRING() << "grf" << chat_id.get();
}

string ContactsManager::get_chat_full_database_value(const ChatFull *chat_full) {
  return log_event_store(*chat_full).as_slice().str();
}

void ContactsManager::on_load_chat_full_from_database(ChatId chat_id, string value) {
  LOG(INFO) << "Successfully loaded full " << chat_id << " of size " << value.size() << " from database";
  //  G()->td_db()->get_sqlite_pmc()->erase(get_chat_full_database_key(chat_id), Auto());
  //  return;

  if (get_chat_full(chat_id) != nullptr || value.empty()) {
    return;
  }

  ChatFull *chat_full = add_chat_full(chat_id);
  auto status = log_event_parse(*chat_full, value);
  if (status.is_error()) {
    // can't happen unless database is broken
    LOG(ERROR) << "Repair broken full " << chat_id << ' ' << format::as_hex_dump<4>(Slice(value));

    // just clean all known data about the chat and pretend that there was nothing in the database
    chats_full_.erase(chat_id);
    G()->td_db()->get_sqlite_pmc()->erase(get_chat_full_database_key(chat_id), Auto());
    return;
  }

  Dependencies dependencies;
  dependencies.add(chat_id);
  dependencies.add(chat_full->creator_user_id);
  for (auto &participant : chat_full->participants) {
    dependencies.add_message_sender_dependencies(participant.dialog_id_);
    dependencies.add(participant.inviter_user_id_);
  }
  dependencies.add(chat_full->invite_link.get_creator_user_id());
  if (!dependencies.resolve_force(td_, "on_load_chat_full_from_database")) {
    chats_full_.erase(chat_id);
    G()->td_db()->get_sqlite_pmc()->erase(get_chat_full_database_key(chat_id), Auto());
    return;
  }

  Chat *c = get_chat(chat_id);
  CHECK(c != nullptr);

  bool need_invite_link = c->is_active && c->status.can_manage_invite_links();
  bool have_invite_link = chat_full->invite_link.is_valid();
  if (need_invite_link != have_invite_link) {
    if (need_invite_link) {
      // ignore ChatFull without invite link
      chats_full_.erase(chat_id);
      return;
    } else {
      chat_full->invite_link = DialogInviteLink();
    }
  }

  if (!is_same_dialog_photo(td_->file_manager_.get(), DialogId(chat_id), chat_full->photo, c->photo)) {
    chat_full->photo = Photo();
    if (c->photo.small_file_id.is_valid()) {
      reload_chat_full(chat_id, Auto());
    }
  }

  auto photo = std::move(chat_full->photo);
  chat_full->photo = Photo();
  on_update_chat_full_photo(chat_full, chat_id, std::move(photo));

  td_->group_call_manager_->on_update_dialog_about(DialogId(chat_id), chat_full->description, false);

  chat_full->is_update_chat_full_sent = true;
  update_chat_full(chat_full, chat_id, "on_load_chat_full_from_database", true);
}

ContactsManager::ChatFull *ContactsManager::get_chat_full_force(ChatId chat_id, const char *source) {
  if (!have_chat_force(chat_id)) {
    return nullptr;
  }

  ChatFull *chat_full = get_chat_full(chat_id);
  if (chat_full != nullptr) {
    return chat_full;
  }
  if (!G()->parameters().use_chat_info_db) {
    return nullptr;
  }
  if (!unavailable_chat_fulls_.insert(chat_id).second) {
    return nullptr;
  }

  LOG(INFO) << "Trying to load full " << chat_id << " from database from " << source;
  on_load_chat_full_from_database(chat_id,
                                  G()->td_db()->get_sqlite_sync_pmc()->get(get_chat_full_database_key(chat_id)));
  return get_chat_full(chat_id);
}

void ContactsManager::save_channel_full(const ChannelFull *channel_full, ChannelId channel_id) {
  if (!G()->parameters().use_chat_info_db) {
    return;
  }

  LOG(INFO) << "Trying to save to database full " << channel_id;
  CHECK(channel_full != nullptr);
  G()->td_db()->get_sqlite_pmc()->set(get_channel_full_database_key(channel_id),
                                      get_channel_full_database_value(channel_full), Auto());
}

string ContactsManager::get_channel_full_database_key(ChannelId channel_id) {
  return PSTRING() << "chf" << channel_id.get();
}

string ContactsManager::get_channel_full_database_value(const ChannelFull *channel_full) {
  return log_event_store(*channel_full).as_slice().str();
}

void ContactsManager::on_load_channel_full_from_database(ChannelId channel_id, string value, const char *source) {
  LOG(INFO) << "Successfully loaded full " << channel_id << " of size " << value.size() << " from database from "
            << source;
  //  G()->td_db()->get_sqlite_pmc()->erase(get_channel_full_database_key(channel_id), Auto());
  //  return;

  if (get_channel_full(channel_id, true, "on_load_channel_full_from_database") != nullptr || value.empty()) {
    return;
  }

  ChannelFull *channel_full = add_channel_full(channel_id);
  auto status = log_event_parse(*channel_full, value);
  if (status.is_error()) {
    // can't happen unless database is broken
    LOG(ERROR) << "Repair broken full " << channel_id << ' ' << format::as_hex_dump<4>(Slice(value));

    // just clean all known data about the channel and pretend that there was nothing in the database
    channels_full_.erase(channel_id);
    G()->td_db()->get_sqlite_pmc()->erase(get_channel_full_database_key(channel_id), Auto());
    return;
  }

  Dependencies dependencies;
  dependencies.add(channel_id);
  // must not depend on the linked_dialog_id itself, because message database can be disabled
  // the Dialog will be forcely created in update_channel_full
  dependencies.add_dialog_dependencies(DialogId(channel_full->linked_channel_id));
  dependencies.add(channel_full->migrated_from_chat_id);
  for (auto bot_user_id : channel_full->bot_user_ids) {
    dependencies.add(bot_user_id);
  }
  dependencies.add(channel_full->invite_link.get_creator_user_id());
  if (!dependencies.resolve_force(td_, source)) {
    channels_full_.erase(channel_id);
    G()->td_db()->get_sqlite_pmc()->erase(get_channel_full_database_key(channel_id), Auto());
    return;
  }

  Channel *c = get_channel(channel_id);
  CHECK(c != nullptr);

  bool need_invite_link = c->status.can_manage_invite_links();
  bool have_invite_link = channel_full->invite_link.is_valid();
  if (need_invite_link != have_invite_link) {
    if (need_invite_link) {
      // ignore ChannelFull without invite link
      channels_full_.erase(channel_id);
      return;
    } else {
      channel_full->invite_link = DialogInviteLink();
    }
  }

  if (!is_same_dialog_photo(td_->file_manager_.get(), DialogId(channel_id), channel_full->photo, c->photo)) {
    channel_full->photo = Photo();
    if (c->photo.small_file_id.is_valid()) {
      channel_full->expires_at = 0.0;
    }
  }
  auto photo = std::move(channel_full->photo);
  channel_full->photo = Photo();
  on_update_channel_full_photo(channel_full, channel_id, std::move(photo));

  if (channel_full->participant_count < channel_full->administrator_count) {
    channel_full->participant_count = channel_full->administrator_count;
  }
  if (c->participant_count != 0 && c->participant_count != channel_full->participant_count) {
    channel_full->participant_count = c->participant_count;

    if (channel_full->participant_count < channel_full->administrator_count) {
      channel_full->participant_count = channel_full->administrator_count;
      channel_full->expires_at = 0.0;

      c->participant_count = channel_full->participant_count;
      c->is_changed = true;
    }
  }
  if (c->can_be_deleted != channel_full->can_be_deleted) {
    c->can_be_deleted = channel_full->can_be_deleted;
    c->need_save_to_database = true;
  }

  if (invalidated_channels_full_.erase(channel_id) > 0 ||
      (!c->is_slow_mode_enabled && channel_full->slow_mode_delay != 0)) {
    do_invalidate_channel_full(channel_full, channel_id, !c->is_slow_mode_enabled);
  }

  td_->group_call_manager_->on_update_dialog_about(DialogId(channel_id), channel_full->description, false);

  send_closure_later(G()->messages_manager(), &MessagesManager::on_dialog_bots_updated, DialogId(channel_id),
                     channel_full->bot_user_ids, true);

  update_channel(c, channel_id);

  channel_full->is_update_channel_full_sent = true;
  update_channel_full(channel_full, channel_id, "on_load_channel_full_from_database", true);

  if (channel_full->expires_at == 0.0) {
    load_channel_full(channel_id, true, Auto(), "on_load_channel_full_from_database");
  }
}

ContactsManager::ChannelFull *ContactsManager::get_channel_full_force(ChannelId channel_id, bool only_local,
                                                                      const char *source) {
  if (!have_channel_force(channel_id)) {
    return nullptr;
  }

  ChannelFull *channel_full = get_channel_full(channel_id, only_local, source);
  if (channel_full != nullptr) {
    return channel_full;
  }
  if (!G()->parameters().use_chat_info_db) {
    return nullptr;
  }
  if (!unavailable_channel_fulls_.insert(channel_id).second) {
    return nullptr;
  }

  LOG(INFO) << "Trying to load full " << channel_id << " from database from " << source;
  on_load_channel_full_from_database(
      channel_id, G()->td_db()->get_sqlite_sync_pmc()->get(get_channel_full_database_key(channel_id)), source);
  return get_channel_full(channel_id, only_local, source);
}

void ContactsManager::for_each_secret_chat_with_user(UserId user_id, const std::function<void(SecretChatId)> &f) {
  auto it = secret_chats_with_user_.find(user_id);
  if (it != secret_chats_with_user_.end()) {
    for (auto secret_chat_id : it->second) {
      f(secret_chat_id);
    }
  }
}

void ContactsManager::update_user(User *u, UserId user_id, bool from_binlog, bool from_database) {
  CHECK(u != nullptr);
  if (user_id == get_my_id()) {
    if (td_->option_manager_->get_option_boolean("is_premium") != u->is_premium) {
      td_->option_manager_->set_option_boolean("is_premium", u->is_premium);
      send_closure(td_->config_manager_, &ConfigManager::request_config, true);
      td_->stickers_manager_->reload_top_reactions();
    }
  }
  if (u->is_name_changed || u->is_username_changed || u->is_is_contact_changed) {
    update_contacts_hints(u, user_id, from_database);
    u->is_username_changed = false;
  }
  if (u->is_is_contact_changed) {
    td_->messages_manager_->on_dialog_user_is_contact_updated(DialogId(user_id), u->is_contact);
    if (is_user_contact(u, user_id, false)) {
      auto user_full = get_user_full(user_id);
      if (user_full != nullptr && user_full->need_phone_number_privacy_exception) {
        on_update_user_full_need_phone_number_privacy_exception(user_full, user_id, false);
        update_user_full(user_full, user_id, "update_user");
      }
    }
    u->is_is_contact_changed = false;
  }
  if (u->is_is_deleted_changed) {
    td_->messages_manager_->on_dialog_user_is_deleted_updated(DialogId(user_id), u->is_deleted);
    if (u->is_deleted) {
      auto user_full = get_user_full(user_id);  // must not load user_full from database before sending updateUser
      if (user_full != nullptr) {
        drop_user_full(user_id);
      }
    }
    u->is_is_deleted_changed = false;
  }
  if (u->is_name_changed) {
    auto messages_manager = td_->messages_manager_.get();
    messages_manager->on_dialog_title_updated(DialogId(user_id));
    for_each_secret_chat_with_user(user_id, [messages_manager](SecretChatId secret_chat_id) {
      messages_manager->on_dialog_title_updated(DialogId(secret_chat_id));
    });
    u->is_name_changed = false;
  }
  if (u->is_photo_changed) {
    auto messages_manager = td_->messages_manager_.get();
    messages_manager->on_dialog_photo_updated(DialogId(user_id));
    for_each_secret_chat_with_user(user_id, [messages_manager](SecretChatId secret_chat_id) {
      messages_manager->on_dialog_photo_updated(DialogId(secret_chat_id));
    });
    u->is_photo_changed = false;
  }
  if (u->is_phone_number_changed) {
    if (!u->phone_number.empty() && !td_->auth_manager_->is_bot()) {
      resolved_phone_numbers_[u->phone_number] = user_id;
    }
    u->is_phone_number_changed = false;
  }
  if (u->is_status_changed && user_id != get_my_id()) {
    auto left_time = get_user_was_online(u, user_id) - G()->server_time_cached();
    if (left_time >= 0 && left_time < 30 * 86400) {
      left_time += 2.0;  // to guarantee expiration
      LOG(DEBUG) << "Set online timeout for " << user_id << " in " << left_time;
      user_online_timeout_.set_timeout_in(user_id.get(), left_time);
    } else {
      LOG(DEBUG) << "Cancel online timeout for " << user_id;
      user_online_timeout_.cancel_timeout(user_id.get());
    }
  }
  if (!td_->auth_manager_->is_bot()) {
    if (u->restriction_reasons.empty()) {
      restricted_user_ids_.erase(user_id);
    } else {
      restricted_user_ids_.insert(user_id);
    }
  }

  auto unix_time = G()->unix_time();
  auto effective_custom_emoji_id = u->emoji_status.get_effective_custom_emoji_id(u->is_premium, unix_time);
  if (effective_custom_emoji_id != u->last_sent_emoji_status) {
    u->last_sent_emoji_status = effective_custom_emoji_id;
    u->is_changed = true;
  } else {
    u->need_save_to_database = true;
  }
  if (u->last_sent_emoji_status != 0) {
    auto until_date = u->emoji_status.get_until_date();
    auto left_time = until_date - unix_time;
    if (left_time >= 0 && left_time < 30 * 86400) {
      LOG(DEBUG) << "Set emoji status timeout for " << user_id << " in " << left_time;
      user_emoji_status_timeout_.set_timeout_in(user_id.get(), left_time);
    } else {
      user_emoji_status_timeout_.cancel_timeout(user_id.get());
    }
  } else {
    user_emoji_status_timeout_.cancel_timeout(user_id.get());
  }

  if (u->is_deleted) {
    td_->inline_queries_manager_->remove_recent_inline_bot(user_id, Promise<>());
  }

  LOG(DEBUG) << "Update " << user_id << ": need_save_to_database = " << u->need_save_to_database
             << ", is_changed = " << u->is_changed << ", is_status_changed = " << u->is_status_changed
             << ", from_binlog = " << from_binlog << ", from_database = " << from_database;
  u->need_save_to_database |= u->is_changed;
  if (u->need_save_to_database) {
    if (!from_database) {
      u->is_saved = false;
    }
    u->need_save_to_database = false;
  }
  if (u->is_changed) {
    send_closure(G()->td(), &Td::send_update, make_tl_object<td_api::updateUser>(get_user_object(user_id, u)));
    u->is_changed = false;
    u->is_status_changed = false;
    u->is_update_user_sent = true;
  }
  if (u->is_status_changed) {
    if (!from_database) {
      u->is_status_saved = false;
    }
    CHECK(u->is_update_user_sent);
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateUserStatus>(user_id.get(), get_user_status_object(user_id, u)));
    u->is_status_changed = false;
  }
  if (u->is_online_status_changed) {
    update_user_online_member_count(u);
    u->is_online_status_changed = false;
  }

  if (!from_database) {
    save_user(u, user_id, from_binlog);
  }

  if (u->cache_version != User::CACHE_VERSION && !u->is_repaired && have_input_peer_user(u, AccessRights::Read) &&
      !G()->close_flag()) {
    u->is_repaired = true;

    LOG(INFO) << "Repairing cache of " << user_id;
    reload_user(user_id, Promise<Unit>());
  }
}

void ContactsManager::update_chat(Chat *c, ChatId chat_id, bool from_binlog, bool from_database) {
  CHECK(c != nullptr);
  bool need_update_chat_full = false;
  if (c->is_photo_changed) {
    td_->messages_manager_->on_dialog_photo_updated(DialogId(chat_id));
    c->is_photo_changed = false;

    auto chat_full = get_chat_full(chat_id);  // must not load ChatFull
    if (chat_full != nullptr &&
        !is_same_dialog_photo(td_->file_manager_.get(), DialogId(chat_id), chat_full->photo, c->photo)) {
      on_update_chat_full_photo(chat_full, chat_id, Photo());
      need_update_chat_full = true;
      if (c->photo.small_file_id.is_valid()) {
        reload_chat_full(chat_id, Auto());
      }
    }
  }
  if (c->is_title_changed) {
    td_->messages_manager_->on_dialog_title_updated(DialogId(chat_id));
    c->is_title_changed = false;
  }
  if (c->is_default_permissions_changed) {
    td_->messages_manager_->on_dialog_default_permissions_updated(DialogId(chat_id));
    c->is_default_permissions_changed = false;
  }
  if (c->is_is_active_changed) {
    update_dialogs_for_discussion(DialogId(chat_id), c->is_active && c->status.is_creator());
    c->is_is_active_changed = false;
  }
  if (c->is_status_changed) {
    if (!c->status.can_manage_invite_links()) {
      td_->messages_manager_->drop_dialog_pending_join_requests(DialogId(chat_id));
    }
    c->is_status_changed = false;
  }
  if (c->is_noforwards_changed) {
    td_->messages_manager_->on_dialog_has_protected_content_updated(DialogId(chat_id));
    c->is_noforwards_changed = false;
  }

  LOG(DEBUG) << "Update " << chat_id << ": need_save_to_database = " << c->need_save_to_database
             << ", is_changed = " << c->is_changed;
  c->need_save_to_database |= c->is_changed;
  if (c->need_save_to_database) {
    if (!from_database) {
      c->is_saved = false;
    }
    c->need_save_to_database = false;
  }
  if (c->is_changed) {
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateBasicGroup>(get_basic_group_object(chat_id, c)));
    c->is_changed = false;
    c->is_update_basic_group_sent = true;
  }

  if (!from_database) {
    save_chat(c, chat_id, from_binlog);
  }

  if (c->cache_version != Chat::CACHE_VERSION && !c->is_repaired && have_input_peer_chat(c, AccessRights::Read) &&
      !G()->close_flag()) {
    c->is_repaired = true;

    LOG(INFO) << "Repairing cache of " << chat_id;
    reload_chat(chat_id, Promise<Unit>());
  }

  if (need_update_chat_full) {
    auto chat_full = get_chat_full(chat_id);
    CHECK(chat_full != nullptr);
    update_chat_full(chat_full, chat_id, "drop_chat_photos");
  }
}

void ContactsManager::update_channel(Channel *c, ChannelId channel_id, bool from_binlog, bool from_database) {
  CHECK(c != nullptr);
  bool need_update_channel_full = false;
  if (c->is_photo_changed) {
    td_->messages_manager_->on_dialog_photo_updated(DialogId(channel_id));
    c->is_photo_changed = false;

    auto channel_full = get_channel_full(channel_id, true, "update_channel");
    if (channel_full != nullptr &&
        !is_same_dialog_photo(td_->file_manager_.get(), DialogId(channel_id), channel_full->photo, c->photo)) {
      on_update_channel_full_photo(channel_full, channel_id, Photo());
      need_update_channel_full = true;
      if (c->photo.small_file_id.is_valid()) {
        if (channel_full->expires_at > 0.0) {
          channel_full->expires_at = 0.0;
          channel_full->need_save_to_database = true;
        }
        send_get_channel_full_query(channel_full, channel_id, Auto(), "update_channel");
      }
    }
  }
  if (c->is_title_changed) {
    td_->messages_manager_->on_dialog_title_updated(DialogId(channel_id));
    c->is_title_changed = false;
  }
  if (c->is_status_changed) {
    c->status.update_restrictions();
    auto until_date = c->status.get_until_date();
    int32 left_time = 0;
    if (until_date > 0) {
      left_time = until_date - G()->unix_time_cached() + 1;
      CHECK(left_time > 0);
    }
    if (left_time > 0 && left_time < 366 * 86400) {
      channel_unban_timeout_.set_timeout_in(channel_id.get(), left_time);
    } else {
      channel_unban_timeout_.cancel_timeout(channel_id.get());
    }

    if (c->is_megagroup) {
      update_dialogs_for_discussion(DialogId(channel_id), c->status.is_administrator() && c->status.can_pin_messages());
    }
    if (!c->status.is_member()) {
      remove_inactive_channel(channel_id);
    }
    if (!c->status.can_manage_invite_links()) {
      td_->messages_manager_->drop_dialog_pending_join_requests(DialogId(channel_id));
    }
    c->is_status_changed = false;
  }
  if (c->is_username_changed) {
    if (c->status.is_creator()) {
      update_created_public_channels(c, channel_id);
    }
    c->is_username_changed = false;
  }
  if (c->is_default_permissions_changed) {
    td_->messages_manager_->on_dialog_default_permissions_updated(DialogId(channel_id));
    if (c->default_permissions !=
        RestrictedRights(false, false, false, false, false, false, false, false, false, false, false)) {
      remove_dialog_suggested_action(SuggestedAction{SuggestedAction::Type::ConvertToGigagroup, DialogId(channel_id)});
    }
    c->is_default_permissions_changed = false;
  }
  if (c->is_has_location_changed) {
    if (c->status.is_creator()) {
      update_created_public_channels(c, channel_id);
    }
    c->is_has_location_changed = false;
  }
  if (c->is_creator_changed) {
    update_created_public_channels(c, channel_id);
    c->is_creator_changed = false;
  }
  if (c->is_noforwards_changed) {
    td_->messages_manager_->on_dialog_has_protected_content_updated(DialogId(channel_id));
    c->is_noforwards_changed = false;
  }

  if (!td_->auth_manager_->is_bot()) {
    if (c->restriction_reasons.empty()) {
      restricted_channel_ids_.erase(channel_id);
    } else {
      restricted_channel_ids_.insert(channel_id);
    }
  }

  LOG(DEBUG) << "Update " << channel_id << ": need_save_to_database = " << c->need_save_to_database
             << ", is_changed = " << c->is_changed;
  c->need_save_to_database |= c->is_changed;
  if (c->need_save_to_database) {
    if (!from_database) {
      c->is_saved = false;
    }
    c->need_save_to_database = false;
  }
  if (c->is_changed) {
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateSupergroup>(get_supergroup_object(channel_id, c)));
    c->is_changed = false;
    c->is_update_supergroup_sent = true;
  }

  if (!from_database) {
    save_channel(c, channel_id, from_binlog);
  }

  bool have_read_access = have_input_peer_channel(c, channel_id, AccessRights::Read);
  bool is_member = c->status.is_member();
  if (c->had_read_access && !have_read_access) {
    send_closure_later(G()->messages_manager(), &MessagesManager::on_dialog_deleted, DialogId(channel_id),
                       Promise<Unit>());
  } else if (!from_database && c->was_member != is_member) {
    DialogId dialog_id(channel_id);
    send_closure_later(G()->messages_manager(), &MessagesManager::force_create_dialog, dialog_id, "update channel",
                       true, true);
  }
  c->had_read_access = have_read_access;
  c->was_member = is_member;

  if (c->cache_version != Channel::CACHE_VERSION && !c->is_repaired &&
      have_input_peer_channel(c, channel_id, AccessRights::Read) && !G()->close_flag()) {
    c->is_repaired = true;

    LOG(INFO) << "Repairing cache of " << channel_id;
    reload_channel(channel_id, Promise<Unit>());
  }

  if (need_update_channel_full) {
    auto channel_full = get_channel_full(channel_id, true, "update_channel");
    CHECK(channel_full != nullptr);
    update_channel_full(channel_full, channel_id, "update_channel");
  }
}

void ContactsManager::update_secret_chat(SecretChat *c, SecretChatId secret_chat_id, bool from_binlog,
                                         bool from_database) {
  CHECK(c != nullptr);
  LOG(DEBUG) << "Update " << secret_chat_id << ": need_save_to_database = " << c->need_save_to_database
             << ", is_changed = " << c->is_changed;
  c->need_save_to_database |= c->is_changed;
  if (c->need_save_to_database) {
    if (!from_database) {
      c->is_saved = false;
    }
    c->need_save_to_database = false;

    DialogId dialog_id(secret_chat_id);
    send_closure_later(G()->messages_manager(), &MessagesManager::force_create_dialog, dialog_id, "update secret chat",
                       true, true);
    if (c->is_state_changed) {
      send_closure_later(G()->messages_manager(), &MessagesManager::on_update_secret_chat_state, secret_chat_id,
                         c->state);
      c->is_state_changed = false;
    }
    if (c->is_ttl_changed) {
      send_closure_later(G()->messages_manager(), &MessagesManager::on_update_dialog_message_ttl,
                         DialogId(secret_chat_id), MessageTtl(c->ttl));
      c->is_ttl_changed = false;
    }
  }
  if (c->is_changed) {
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateSecretChat>(get_secret_chat_object(secret_chat_id, c)));
    c->is_changed = false;
  }

  if (!from_database) {
    save_secret_chat(c, secret_chat_id, from_binlog);
  }
}

void ContactsManager::update_user_full(UserFull *user_full, UserId user_id, const char *source, bool from_database) {
  CHECK(user_full != nullptr);
  unavailable_user_fulls_.erase(user_id);  // don't needed anymore
  if (user_full->is_common_chat_count_changed) {
    td_->messages_manager_->drop_common_dialogs_cache(user_id);
    user_full->is_common_chat_count_changed = false;
  }
  if (user_full->are_files_changed) {
    auto file_ids = photo_get_file_ids(user_full->description_photo);
    if (user_full->description_animation_file_id.is_valid()) {
      file_ids.push_back(user_full->description_animation_file_id);
    }
    if (user_full->registered_file_ids != file_ids) {
      auto &file_source_id = user_full->file_source_id;
      if (!file_source_id.is_valid()) {
        file_source_id = user_full_file_source_ids_.get(user_id);
        if (file_source_id.is_valid()) {
          VLOG(file_references) << "Move " << file_source_id << " inside of " << user_id;
          user_full_file_source_ids_.erase(user_id);
        } else {
          VLOG(file_references) << "Need to create new file source for full " << user_id;
          file_source_id = td_->file_reference_manager_->create_user_full_file_source(user_id);
        }
      }

      td_->file_manager_->change_files_source(file_source_id, user_full->registered_file_ids, file_ids);
      user_full->registered_file_ids = std::move(file_ids);
    }
  }

  user_full->need_send_update |= user_full->is_changed;
  user_full->need_save_to_database |= user_full->is_changed;
  user_full->is_changed = false;
  if (user_full->need_send_update || user_full->need_save_to_database) {
    LOG(INFO) << "Update full " << user_id << " from " << source;
  }
  if (user_full->need_send_update) {
    {
      auto u = get_user(user_id);
      CHECK(u == nullptr || u->is_update_user_sent);
    }
    if (!user_full->is_update_user_full_sent) {
      LOG(ERROR) << "Send partial updateUserFullInfo for " << user_id << " from " << source;
      user_full->is_update_user_full_sent = true;
    }
    send_closure(G()->td(), &Td::send_update,
                 make_tl_object<td_api::updateUserFullInfo>(get_user_id_object(user_id, "updateUserFullInfo"),
                                                            get_user_full_info_object(user_id, user_full)));
    user_full->need_send_update = false;
  }
  if (user_full->need_save_to_database) {
    if (!from_database) {
      save_user_full(user_full, user_id);
    }
    user_full->need_save_to_database = false;
  }
}

void ContactsManager::update_chat_full(ChatFull *chat_full, ChatId chat_id, const char *source, bool from_database) {
  CHECK(chat_full != nullptr);
  unavailable_chat_fulls_.erase(chat_id);  // don't needed anymore

  chat_full->need_send_update |= chat_full->is_changed;
  chat_full->need_save_to_database |= chat_full->is_changed;
  chat_full->is_changed = false;
  if (chat_full->need_send_update || chat_full->need_save_to_database) {
    LOG(INFO) << "Update full " << chat_id << " from " << source;
  }
  if (chat_full->need_send_update) {
    vector<DialogAdministrator> administrators;
    vector<UserId> bot_user_ids;
    for (const auto &participant : chat_full->participants) {
      if (participant.status_.is_administrator() && participant.dialog_id_.get_type() == DialogType::User) {
        administrators.emplace_back(participant.dialog_id_.get_user_id(), participant.status_.get_rank(),
                                    participant.status_.is_creator());
      }
      if (participant.dialog_id_.get_type() == DialogType::User) {
        auto user_id = participant.dialog_id_.get_user_id();
        if (is_user_bot(user_id)) {
          bot_user_ids.push_back(user_id);
        }
      }
    }
    td::remove_if(chat_full->bot_commands, [&bot_user_ids](const BotCommands &commands) {
      return !td::contains(bot_user_ids, commands.get_bot_user_id());
    });

    on_update_dialog_administrators(DialogId(chat_id), std::move(administrators), chat_full->version != -1,
                                    from_database);
    send_closure_later(G()->messages_manager(), &MessagesManager::on_dialog_bots_updated, DialogId(chat_id),
                       std::move(bot_user_ids), from_database);

    {
      Chat *c = get_chat(chat_id);
      CHECK(c == nullptr || c->is_update_basic_group_sent);
    }
    if (!chat_full->is_update_chat_full_sent) {
      LOG(ERROR) << "Send partial updateBasicGroupFullInfo for " << chat_id << " from " << source;
      chat_full->is_update_chat_full_sent = true;
    }
    send_closure(
        G()->td(), &Td::send_update,
        make_tl_object<td_api::updateBasicGroupFullInfo>(get_basic_group_id_object(chat_id, "update_chat_full"),
                                                         get_basic_group_full_info_object(chat_full)));
    chat_full->need_send_update = false;
  }
  if (chat_full->need_save_to_database) {
    if (!from_database) {
      save_chat_full(chat_full, chat_id);
    }
    chat_full->need_save_to_database = false;
  }
}

void ContactsManager::update_channel_full(ChannelFull *channel_full, ChannelId channel_id, const char *source,
                                          bool from_database) {
  CHECK(channel_full != nullptr);
  unavailable_channel_fulls_.erase(channel_id);  // don't needed anymore

  CHECK(channel_full->participant_count >= channel_full->administrator_count);

  if (channel_full->is_slow_mode_next_send_date_changed) {
    auto now = G()->server_time();
    if (channel_full->slow_mode_next_send_date > now + 3601) {
      channel_full->slow_mode_next_send_date = static_cast<int32>(now) + 3601;
    }
    if (channel_full->slow_mode_next_send_date <= now) {
      channel_full->slow_mode_next_send_date = 0;
    }
    if (channel_full->slow_mode_next_send_date == 0) {
      slow_mode_delay_timeout_.cancel_timeout(channel_id.get());
    } else {
      slow_mode_delay_timeout_.set_timeout_in(channel_id.get(), channel_full->slow_mode_next_send_date - now + 0.002);
    }
    channel_full->is_slow_mode_next_send_date_changed = false;
  }

  if (channel_full->need_save_to_database) {
    channel_full->is_changed |= td::remove_if(
        channel_full->bot_commands, [bot_user_ids = &channel_full->bot_user_ids](const BotCommands &commands) {
          return !td::contains(*bot_user_ids, commands.get_bot_user_id());
        });
  }

  channel_full->need_send_update |= channel_full->is_changed;
  channel_full->need_save_to_database |= channel_full->is_changed;
  channel_full->is_changed = false;
  if (channel_full->need_send_update || channel_full->need_save_to_database) {
    LOG(INFO) << "Update full " << channel_id << " from " << source;
  }
  if (channel_full->need_send_update) {
    if (channel_full->linked_channel_id.is_valid()) {
      td_->messages_manager_->force_create_dialog(DialogId(channel_full->linked_channel_id), "update_channel_full",
                                                  true);
    }

    {
      Channel *c = get_channel(channel_id);
      CHECK(c == nullptr || c->is_update_supergroup_sent);
    }
    if (!channel_full->is_update_channel_full_sent) {
      LOG(ERROR) << "Send partial updateSupergroupFullInfo for " << channel_id << " from " << source;
      channel_full->is_update_channel_full_sent = true;
    }
    send_closure(
        G()->td(), &Td::send_update,
        make_tl_object<td_api::updateSupergroupFullInfo>(get_supergroup_id_object(channel_id, "update_channel_full"),
                                                         get_supergroup_full_info_object(channel_full, channel_id)));
    channel_full->need_send_update = false;
  }
  if (channel_full->need_save_to_database) {
    if (!from_database) {
      save_channel_full(channel_full, channel_id);
    }
    channel_full->need_save_to_database = false;
  }
}

void ContactsManager::on_get_users(vector<tl_object_ptr<telegram_api::User>> &&users, const char *source) {
  for (auto &user : users) {
    on_get_user(std::move(user), source);
  }
}

void ContactsManager::on_get_user_full(tl_object_ptr<telegram_api::userFull> &&user) {
  LOG(INFO) << "Receive " << to_string(user);

  UserId user_id(user->id_);
  User *u = get_user(user_id);
  if (u == nullptr) {
    LOG(ERROR) << "Failed to find " << user_id;
    return;
  }

  apply_pending_user_photo(u, user_id);

  td_->messages_manager_->on_update_dialog_notify_settings(DialogId(user_id), std::move(user->notify_settings_),
                                                           "on_get_user_full");

  td_->messages_manager_->on_update_dialog_theme_name(DialogId(user_id), std::move(user->theme_emoticon_));

  {
    MessageId pinned_message_id;
    if ((user->flags_ & USER_FULL_FLAG_HAS_PINNED_MESSAGE) != 0) {
      pinned_message_id = MessageId(ServerMessageId(user->pinned_msg_id_));
    }
    td_->messages_manager_->on_update_dialog_last_pinned_message_id(DialogId(user_id), pinned_message_id);
  }
  {
    FolderId folder_id;
    if ((user->flags_ & USER_FULL_FLAG_HAS_FOLDER_ID) != 0) {
      folder_id = FolderId(user->folder_id_);
    }
    td_->messages_manager_->on_update_dialog_folder_id(DialogId(user_id), folder_id);
  }
  td_->messages_manager_->on_update_dialog_has_scheduled_server_messages(
      DialogId(user_id), (user->flags_ & USER_FULL_FLAG_HAS_SCHEDULED_MESSAGES) != 0);
  {
    MessageTtl message_ttl;
    if ((user->flags_ & USER_FULL_FLAG_HAS_MESSAGE_TTL) != 0) {
      message_ttl = MessageTtl(user->ttl_period_);
    }
    td_->messages_manager_->on_update_dialog_message_ttl(DialogId(user_id), message_ttl);
  }

  UserFull *user_full = add_user_full(user_id);
  user_full->expires_at = Time::now() + USER_FULL_EXPIRE_TIME;

  {
    bool is_blocked = (user->flags_ & USER_FULL_FLAG_IS_BLOCKED) != 0;
    on_update_user_full_is_blocked(user_full, user_id, is_blocked);
    td_->messages_manager_->on_update_dialog_is_blocked(DialogId(user_id), is_blocked);
  }

  on_update_user_full_common_chat_count(user_full, user_id, user->common_chats_count_);
  on_update_user_full_need_phone_number_privacy_exception(user_full, user_id,
                                                          user->settings_->need_contacts_exception_);

  bool can_pin_messages = user->can_pin_message_;
  if (user_full->can_pin_messages != can_pin_messages) {
    user_full->can_pin_messages = can_pin_messages;
    user_full->is_changed = true;
  }

  bool can_be_called = user->phone_calls_available_ && !user->phone_calls_private_;
  bool supports_video_calls = user->video_calls_available_ && !user->phone_calls_private_;
  bool has_private_calls = user->phone_calls_private_;
  bool voice_messages_forbidden = u->is_premium ? user->voice_messages_forbidden_ : false;
  auto premium_gift_options = get_premium_gift_options(std::move(user->premium_gifts_));
  AdministratorRights group_administrator_rights(user->bot_group_admin_rights_, ChannelType::Megagroup);
  AdministratorRights broadcast_administrator_rights(user->bot_broadcast_admin_rights_, ChannelType::Broadcast);
  if (user_full->can_be_called != can_be_called || user_full->supports_video_calls != supports_video_calls ||
      user_full->has_private_calls != has_private_calls ||
      user_full->group_administrator_rights != group_administrator_rights ||
      user_full->broadcast_administrator_rights != broadcast_administrator_rights ||
      user_full->premium_gift_options != premium_gift_options ||
      user_full->voice_messages_forbidden != voice_messages_forbidden) {
    user_full->can_be_called = can_be_called;
    user_full->supports_video_calls = supports_video_calls;
    user_full->has_private_calls = has_private_calls;
    user_full->group_administrator_rights = group_administrator_rights;
    user_full->broadcast_administrator_rights = broadcast_administrator_rights;
    user_full->premium_gift_options = std::move(premium_gift_options);
    user_full->voice_messages_forbidden = voice_messages_forbidden;

    user_full->is_changed = true;
  }
  if (user_full->private_forward_name != user->private_forward_name_) {
    if (user_full->private_forward_name.empty() != user->private_forward_name_.empty()) {
      user_full->is_changed = true;
    }
    user_full->private_forward_name = std::move(user->private_forward_name_);
    user_full->need_save_to_database = true;
  }
  if (user_full->about != user->about_) {
    user_full->about = std::move(user->about_);
    user_full->is_changed = true;
    td_->group_call_manager_->on_update_dialog_about(DialogId(user_id), user_full->about, true);
  }
  string description;
  Photo description_photo;
  FileId description_animation_file_id;
  if (user->bot_info_ != nullptr && !td_->auth_manager_->is_bot()) {
    description = std::move(user->bot_info_->description_);
    description_photo =
        get_photo(td_->file_manager_.get(), std::move(user->bot_info_->description_photo_), DialogId(user_id));
    auto document = std::move(user->bot_info_->description_document_);
    if (document != nullptr) {
      int32 document_id = document->get_id();
      if (document_id == telegram_api::document::ID) {
        auto parsed_document = td_->documents_manager_->on_get_document(
            move_tl_object_as<telegram_api::document>(document), DialogId(user_id));
        if (parsed_document.type == Document::Type::Animation) {
          description_animation_file_id = parsed_document.file_id;
        } else {
          LOG(ERROR) << "Receive non-animation document in bot description";
        }
      }
    }

    on_update_user_full_commands(user_full, user_id, std::move(user->bot_info_->commands_));
    on_update_user_full_menu_button(user_full, user_id, std::move(user->bot_info_->menu_button_));
  }
  if (user_full->description != description) {
    user_full->description = std::move(description);
    user_full->is_changed = true;
  }
  if (user_full->description_photo != description_photo ||
      user_full->description_animation_file_id != description_animation_file_id) {
    user_full->description_photo = std::move(description_photo);
    user_full->description_animation_file_id = description_animation_file_id;
    user_full->are_files_changed = true;
    user_full->is_changed = true;
  }

  auto photo = get_photo(td_->file_manager_.get(), std::move(user->profile_photo_), DialogId(user_id));
  // do_update_user_photo should be a no-op if server sent consistent data
  do_update_user_photo(u, user_id, as_profile_photo(td_->file_manager_.get(), user_id, u->access_hash, photo), false,
                       "on_get_user_full");
  if (photo != user_full->photo) {
    user_full->photo = std::move(photo);
    user_full->is_changed = true;
  }
  if (user_full->photo.is_empty()) {
    drop_user_photos(user_id, true, false, "on_get_user_full");
  } else {
    register_user_photo(u, user_id, user_full->photo);
  }

  if (u->is_changed) {
    LOG(WARNING) << "Receive inconsistent chatPhoto and chatPhotoInfo for " << user_id;
    update_user(u, user_id);
  }

  user_full->is_update_user_full_sent = true;
  update_user_full(user_full, user_id, "on_get_user_full");

  // update peer settings after UserFull is created and updated to not update twice need_phone_number_privacy_exception
  td_->messages_manager_->on_get_peer_settings(DialogId(user_id), std::move(user->settings_));
}

void ContactsManager::on_get_user_photos(UserId user_id, int32 offset, int32 limit, int32 total_count,
                                         vector<tl_object_ptr<telegram_api::Photo>> photos) {
  auto photo_count = narrow_cast<int32>(photos.size());
  int32 min_total_count = (offset >= 0 && photo_count > 0 ? offset : 0) + photo_count;
  if (total_count < min_total_count) {
    LOG(ERROR) << "Receive wrong photos total_count " << total_count << " for user " << user_id << ": receive "
               << photo_count << " photos with offset " << offset;
    total_count = min_total_count;
  }
  LOG_IF(ERROR, limit < photo_count) << "Requested not more than " << limit << " photos, but " << photo_count
                                     << " received";

  User *u = get_user(user_id);
  if (u == nullptr) {
    LOG(ERROR) << "Can't find " << user_id;
    return;
  }

  if (offset == -1) {
    // from reload_user_profile_photo
    CHECK(limit == 1);
    for (auto &photo_ptr : photos) {
      if (photo_ptr->get_id() == telegram_api::photo::ID) {
        auto server_photo = telegram_api::move_object_as<telegram_api::photo>(photo_ptr);
        if (server_photo->id_ == u->photo.id) {
          auto profile_photo = convert_photo_to_profile_photo(server_photo);
          if (profile_photo) {
            LOG_IF(ERROR, u->access_hash == -1) << "Receive profile photo of " << user_id << " without access hash";
            get_profile_photo(td_->file_manager_.get(), user_id, u->access_hash, std::move(profile_photo));
          } else {
            LOG(ERROR) << "Failed to get profile photo from " << to_string(server_photo);
          }
        }

        auto photo = get_photo(td_->file_manager_.get(), std::move(server_photo), DialogId(user_id));
        register_user_photo(u, user_id, photo);
      }
    }
    return;
  }

  LOG(INFO) << "Receive " << photo_count << " photos of " << user_id << " out of " << total_count << " with offset "
            << offset << " and limit " << limit;
  UserPhotos *user_photos = &user_photos_[user_id];
  user_photos->count = total_count;
  CHECK(user_photos->getting_now);
  user_photos->getting_now = false;

  if (user_photos->offset == -1) {
    user_photos->offset = 0;
    CHECK(user_photos->photos.empty());
  }

  if (offset != narrow_cast<int32>(user_photos->photos.size()) + user_photos->offset) {
    LOG(INFO) << "Inappropriate offset to append " << user_id << " profile photos to cache: offset = " << offset
              << ", current_offset = " << user_photos->offset << ", photo_count = " << user_photos->photos.size();
    user_photos->photos.clear();
    user_photos->offset = offset;
  }

  for (auto &photo : photos) {
    auto user_photo = get_photo(td_->file_manager_.get(), std::move(photo), DialogId(user_id));
    if (user_photo.is_empty()) {
      LOG(ERROR) << "Receive empty profile photo in getUserPhotos request for " << user_id << " with offset " << offset
                 << " and limit " << limit << ". Receive " << photo_count << " photos out of " << total_count
                 << " photos";
      user_photos->count--;
      CHECK(user_photos->count >= 0);
      continue;
    }

    user_photos->photos.push_back(std::move(user_photo));
    register_user_photo(u, user_id, user_photos->photos.back());
  }
  if (user_photos->offset > user_photos->count) {
    user_photos->offset = user_photos->count;
    user_photos->photos.clear();
  }

  auto known_photo_count = narrow_cast<int32>(user_photos->photos.size());
  if (user_photos->offset + known_photo_count > user_photos->count) {
    user_photos->photos.resize(user_photos->count - user_photos->offset);
  }
}

void ContactsManager::on_get_chat(tl_object_ptr<telegram_api::Chat> &&chat, const char *source) {
  LOG(DEBUG) << "Receive from " << source << ' ' << to_string(chat);
  downcast_call(*chat, [this, source](auto &c) { this->on_chat_update(c, source); });
}

void ContactsManager::on_get_chats(vector<tl_object_ptr<telegram_api::Chat>> &&chats, const char *source) {
  for (auto &chat : chats) {
    auto constuctor_id = chat->get_id();
    if (constuctor_id == telegram_api::channel::ID || constuctor_id == telegram_api::channelForbidden::ID) {
      // apply info about megagroups before corresponding chats
      on_get_chat(std::move(chat), source);
      chat = nullptr;
    }
  }
  for (auto &chat : chats) {
    if (chat != nullptr) {
      on_get_chat(std::move(chat), source);
      chat = nullptr;
    }
  }
}

vector<BotCommands> ContactsManager::get_bot_commands(vector<tl_object_ptr<telegram_api::botInfo>> &&bot_infos,
                                                      const vector<DialogParticipant> *participants) {
  vector<BotCommands> result;
  if (td_->auth_manager_->is_bot()) {
    return result;
  }
  for (auto &bot_info : bot_infos) {
    if (bot_info->commands_.empty()) {
      continue;
    }

    auto user_id = UserId(bot_info->user_id_);
    if (!have_user_force(user_id)) {
      LOG(ERROR) << "Receive unknown " << user_id;
      continue;
    }
    if (!is_user_bot(user_id)) {
      if (!is_user_deleted(user_id)) {
        LOG(ERROR) << "Receive non-bot " << user_id;
      }
      continue;
    }
    if (participants != nullptr) {
      bool is_participant = false;
      for (auto &participant : *participants) {
        if (participant.dialog_id_ == DialogId(user_id)) {
          is_participant = true;
          break;
        }
      }
      if (!is_participant) {
        LOG(ERROR) << "Skip commands of non-member bot " << user_id;
        continue;
      }
    }
    result.emplace_back(user_id, std::move(bot_info->commands_));
  }
  return result;
}

void ContactsManager::on_get_chat_full(tl_object_ptr<telegram_api::ChatFull> &&chat_full_ptr, Promise<Unit> &&promise) {
  LOG(INFO) << "Receive " << to_string(chat_full_ptr);
  if (chat_full_ptr->get_id() == telegram_api::chatFull::ID) {
    auto chat = move_tl_object_as<telegram_api::chatFull>(chat_full_ptr);
    ChatId chat_id(chat->id_);
    if (!chat_id.is_valid()) {
      LOG(ERROR) << "Receive invalid " << chat_id;
      return promise.set_value(Unit());
    }

    Chat *c = get_chat(chat_id);
    if (c == nullptr) {
      LOG(ERROR) << "Can't find " << chat_id;
      return promise.set_value(Unit());
    }
    {
      MessageId pinned_message_id;
      if ((chat->flags_ & CHAT_FULL_FLAG_HAS_PINNED_MESSAGE) != 0) {
        pinned_message_id = MessageId(ServerMessageId(chat->pinned_msg_id_));
      }
      if (c->version >= c->pinned_message_version) {
        LOG(INFO) << "Receive pinned " << pinned_message_id << " in " << chat_id << " with version " << c->version
                  << ". Current version is " << c->pinned_message_version;
        td_->messages_manager_->on_update_dialog_last_pinned_message_id(DialogId(chat_id), pinned_message_id);
        if (c->version > c->pinned_message_version) {
          c->pinned_message_version = c->version;
          c->need_save_to_database = true;
          update_chat(c, chat_id);
        }
      }
    }
    {
      FolderId folder_id;
      if ((chat->flags_ & CHAT_FULL_FLAG_HAS_FOLDER_ID) != 0) {
        folder_id = FolderId(chat->folder_id_);
      }
      td_->messages_manager_->on_update_dialog_folder_id(DialogId(chat_id), folder_id);
    }
    td_->messages_manager_->on_update_dialog_has_scheduled_server_messages(
        DialogId(chat_id), (chat->flags_ & CHAT_FULL_FLAG_HAS_SCHEDULED_MESSAGES) != 0);
    {
      InputGroupCallId input_group_call_id;
      if (chat->call_ != nullptr) {
        input_group_call_id = InputGroupCallId(chat->call_);
      }
      td_->messages_manager_->on_update_dialog_group_call_id(DialogId(chat_id), input_group_call_id);
    }
    {
      DialogId default_join_group_call_as_dialog_id;
      if (chat->groupcall_default_join_as_ != nullptr) {
        default_join_group_call_as_dialog_id = DialogId(chat->groupcall_default_join_as_);
      }
      // use send closure later to not create synchronously default_join_group_call_as_dialog_id
      send_closure_later(G()->messages_manager(),
                         &MessagesManager::on_update_dialog_default_join_group_call_as_dialog_id, DialogId(chat_id),
                         default_join_group_call_as_dialog_id, false);
    }
    {
      MessageTtl message_ttl;
      if ((chat->flags_ & CHAT_FULL_FLAG_HAS_MESSAGE_TTL) != 0) {
        message_ttl = MessageTtl(chat->ttl_period_);
      }
      td_->messages_manager_->on_update_dialog_message_ttl(DialogId(chat_id), message_ttl);
    }

    ChatFull *chat_full = add_chat_full(chat_id);
    on_update_chat_full_invite_link(chat_full, std::move(chat->exported_invite_));
    auto photo = get_photo(td_->file_manager_.get(), std::move(chat->chat_photo_), DialogId(chat_id));
    // on_update_chat_photo should be a no-op if server sent consistent data
    on_update_chat_photo(c, chat_id, as_dialog_photo(td_->file_manager_.get(), DialogId(chat_id), 0, photo), false);
    on_update_chat_full_photo(chat_full, chat_id, std::move(photo));
    if (chat_full->description != chat->about_) {
      chat_full->description = std::move(chat->about_);
      chat_full->is_changed = true;
      td_->group_call_manager_->on_update_dialog_about(DialogId(chat_id), chat_full->description, true);
    }
    if (chat_full->can_set_username != chat->can_set_username_) {
      chat_full->can_set_username = chat->can_set_username_;
      chat_full->is_changed = true;
    }

    on_get_chat_participants(std::move(chat->participants_), false);
    td_->messages_manager_->on_update_dialog_notify_settings(DialogId(chat_id), std::move(chat->notify_settings_),
                                                             "on_get_chat_full");

    td_->messages_manager_->on_update_dialog_available_reactions(DialogId(chat_id),
                                                                 std::move(chat->available_reactions_));

    td_->messages_manager_->on_update_dialog_theme_name(DialogId(chat_id), std::move(chat->theme_emoticon_));

    td_->messages_manager_->on_update_dialog_pending_join_requests(DialogId(chat_id), chat->requests_pending_,
                                                                   std::move(chat->recent_requesters_));

    auto bot_commands = get_bot_commands(std::move(chat->bot_info_), &chat_full->participants);
    if (chat_full->bot_commands != bot_commands) {
      chat_full->bot_commands = std::move(bot_commands);
      chat_full->is_changed = true;
    }

    if (c->is_changed) {
      LOG(WARNING) << "Receive inconsistent chatPhoto and chatPhotoInfo for " << chat_id;
      update_chat(c, chat_id);
    }

    chat_full->is_update_chat_full_sent = true;
    update_chat_full(chat_full, chat_id, "on_get_chat_full");
  } else {
    CHECK(chat_full_ptr->get_id() == telegram_api::channelFull::ID);
    auto channel = move_tl_object_as<telegram_api::channelFull>(chat_full_ptr);
    ChannelId channel_id(channel->id_);
    if (!channel_id.is_valid()) {
      LOG(ERROR) << "Receive invalid " << channel_id;
      return promise.set_value(Unit());
    }

    invalidated_channels_full_.erase(channel_id);

    if (!G()->close_flag()) {
      auto channel_full = get_channel_full(channel_id, true, "on_get_channel_full");
      if (channel_full != nullptr) {
        if (channel_full->repair_request_version != 0 &&
            channel_full->repair_request_version < channel_full->speculative_version) {
          LOG(INFO) << "Receive ChannelFull with request version " << channel_full->repair_request_version
                    << ", but current speculative version is " << channel_full->speculative_version;

          channel_full->repair_request_version = channel_full->speculative_version;

          auto input_channel = get_input_channel(channel_id);
          CHECK(input_channel != nullptr);
          td_->create_handler<GetFullChannelQuery>(std::move(promise))->send(channel_id, std::move(input_channel));
          return;
        }
        channel_full->repair_request_version = 0;
      }
    }

    td_->messages_manager_->on_update_dialog_notify_settings(DialogId(channel_id), std::move(channel->notify_settings_),
                                                             "on_get_channel_full");

    td_->messages_manager_->on_update_dialog_available_reactions(DialogId(channel_id),
                                                                 std::move(channel->available_reactions_));

    td_->messages_manager_->on_update_dialog_theme_name(DialogId(channel_id), std::move(channel->theme_emoticon_));

    td_->messages_manager_->on_update_dialog_pending_join_requests(DialogId(channel_id), channel->requests_pending_,
                                                                   std::move(channel->recent_requesters_));

    {
      MessageTtl message_ttl;
      if ((channel->flags_ & CHANNEL_FULL_FLAG_HAS_MESSAGE_TTL) != 0) {
        message_ttl = MessageTtl(channel->ttl_period_);
      }
      td_->messages_manager_->on_update_dialog_message_ttl(DialogId(channel_id), message_ttl);
    }

    auto c = get_channel(channel_id);
    if (c == nullptr) {
      LOG(ERROR) << channel_id << " not found";
      return promise.set_value(Unit());
    }

    ChannelFull *channel_full = add_channel_full(channel_id);

    bool have_participant_count = (channel->flags_ & CHANNEL_FULL_FLAG_HAS_PARTICIPANT_COUNT) != 0;
    auto participant_count = have_participant_count ? channel->participants_count_ : channel_full->participant_count;
    auto administrator_count = 0;
    if ((channel->flags_ & CHANNEL_FULL_FLAG_HAS_ADMINISTRATOR_COUNT) != 0) {
      administrator_count = channel->admins_count_;
    } else if (c->is_megagroup || c->status.is_administrator()) {
      // in megagroups and administered channels don't drop known number of administrators
      administrator_count = channel_full->administrator_count;
    }
    if (participant_count < administrator_count) {
      participant_count = administrator_count;
    }
    auto restricted_count = (channel->flags_ & CHANNEL_FULL_FLAG_HAS_BANNED_COUNT) != 0 ? channel->banned_count_ : 0;
    auto banned_count = (channel->flags_ & CHANNEL_FULL_FLAG_HAS_BANNED_COUNT) != 0 ? channel->kicked_count_ : 0;
    auto can_get_participants = (channel->flags_ & CHANNEL_FULL_FLAG_CAN_GET_PARTICIPANTS) != 0;
    auto can_set_username = (channel->flags_ & CHANNEL_FULL_FLAG_CAN_SET_USERNAME) != 0;
    auto can_set_sticker_set = (channel->flags_ & CHANNEL_FULL_FLAG_CAN_SET_STICKER_SET) != 0;
    auto can_set_location = (channel->flags_ & CHANNEL_FULL_FLAG_CAN_SET_LOCATION) != 0;
    auto is_all_history_available = (channel->flags_ & CHANNEL_FULL_FLAG_IS_ALL_HISTORY_HIDDEN) == 0;
    auto can_view_statistics = (channel->flags_ & CHANNEL_FULL_FLAG_CAN_VIEW_STATISTICS) != 0;
    StickerSetId sticker_set_id;
    if (channel->stickerset_ != nullptr) {
      sticker_set_id =
          td_->stickers_manager_->on_get_sticker_set(std::move(channel->stickerset_), true, "on_get_channel_full");
    }
    DcId stats_dc_id;
    if ((channel->flags_ & CHANNEL_FULL_FLAG_HAS_STATISTICS_DC_ID) != 0) {
      stats_dc_id = DcId::create(channel->stats_dc_);
    }
    if (!stats_dc_id.is_exact() && can_view_statistics) {
      LOG(ERROR) << "Receive can_view_statistics == true, but invalid statistics DC ID in " << channel_id;
      can_view_statistics = false;
    }

    channel_full->repair_request_version = 0;
    channel_full->expires_at = Time::now() + CHANNEL_FULL_EXPIRE_TIME;
    if (channel_full->participant_count != participant_count ||
        channel_full->administrator_count != administrator_count ||
        channel_full->restricted_count != restricted_count || channel_full->banned_count != banned_count ||
        channel_full->can_get_participants != can_get_participants ||
        channel_full->can_set_username != can_set_username ||
        channel_full->can_set_sticker_set != can_set_sticker_set ||
        channel_full->can_set_location != can_set_location ||
        channel_full->can_view_statistics != can_view_statistics || channel_full->stats_dc_id != stats_dc_id ||
        channel_full->sticker_set_id != sticker_set_id ||
        channel_full->is_all_history_available != is_all_history_available) {
      channel_full->participant_count = participant_count;
      channel_full->administrator_count = administrator_count;
      channel_full->restricted_count = restricted_count;
      channel_full->banned_count = banned_count;
      channel_full->can_get_participants = can_get_participants;
      channel_full->can_set_username = can_set_username;
      channel_full->can_set_sticker_set = can_set_sticker_set;
      channel_full->can_set_location = can_set_location;
      channel_full->can_view_statistics = can_view_statistics;
      channel_full->stats_dc_id = stats_dc_id;
      channel_full->is_all_history_available = is_all_history_available;
      channel_full->sticker_set_id = sticker_set_id;

      channel_full->is_changed = true;
    }
    if (channel_full->description != channel->about_) {
      channel_full->description = std::move(channel->about_);
      channel_full->is_changed = true;
      td_->group_call_manager_->on_update_dialog_about(DialogId(channel_id), channel_full->description, true);
    }

    if (have_participant_count && c->participant_count != participant_count) {
      c->participant_count = participant_count;
      c->is_changed = true;
      update_channel(c, channel_id);
    }
    if (!channel_full->is_can_view_statistics_inited) {
      channel_full->is_can_view_statistics_inited = true;
      channel_full->need_save_to_database = true;
    }

    auto photo = get_photo(td_->file_manager_.get(), std::move(channel->chat_photo_), DialogId(channel_id));
    // on_update_channel_photo should be a no-op if server sent consistent data
    on_update_channel_photo(
        c, channel_id, as_dialog_photo(td_->file_manager_.get(), DialogId(channel_id), c->access_hash, photo), false);
    on_update_channel_full_photo(channel_full, channel_id, std::move(photo));

    td_->messages_manager_->on_read_channel_outbox(channel_id,
                                                   MessageId(ServerMessageId(channel->read_outbox_max_id_)));
    if ((channel->flags_ & CHANNEL_FULL_FLAG_HAS_AVAILABLE_MIN_MESSAGE_ID) != 0) {
      td_->messages_manager_->on_update_channel_max_unavailable_message_id(
          channel_id, MessageId(ServerMessageId(channel->available_min_id_)));
    }
    td_->messages_manager_->on_read_channel_inbox(channel_id, MessageId(ServerMessageId(channel->read_inbox_max_id_)),
                                                  channel->unread_count_, channel->pts_, "ChannelFull");

    on_update_channel_full_invite_link(channel_full, std::move(channel->exported_invite_));

    {
      auto is_blocked = (channel->flags_ & CHANNEL_FULL_FLAG_IS_BLOCKED) != 0;
      td_->messages_manager_->on_update_dialog_is_blocked(DialogId(channel_id), is_blocked);
    }
    {
      MessageId pinned_message_id;
      if ((channel->flags_ & CHANNEL_FULL_FLAG_HAS_PINNED_MESSAGE) != 0) {
        pinned_message_id = MessageId(ServerMessageId(channel->pinned_msg_id_));
      }
      td_->messages_manager_->on_update_dialog_last_pinned_message_id(DialogId(channel_id), pinned_message_id);
    }
    {
      FolderId folder_id;
      if ((channel->flags_ & CHANNEL_FULL_FLAG_HAS_FOLDER_ID) != 0) {
        folder_id = FolderId(channel->folder_id_);
      }
      td_->messages_manager_->on_update_dialog_folder_id(DialogId(channel_id), folder_id);
    }
    td_->messages_manager_->on_update_dialog_has_scheduled_server_messages(
        DialogId(channel_id), (channel->flags_ & CHANNEL_FULL_FLAG_HAS_SCHEDULED_MESSAGES) != 0);
    {
      InputGroupCallId input_group_call_id;
      if (channel->call_ != nullptr) {
        input_group_call_id = InputGroupCallId(channel->call_);
      }
      td_->messages_manager_->on_update_dialog_group_call_id(DialogId(channel_id), input_group_call_id);
    }
    {
      DialogId default_join_group_call_as_dialog_id;
      if (channel->groupcall_default_join_as_ != nullptr) {
        default_join_group_call_as_dialog_id = DialogId(channel->groupcall_default_join_as_);
      }
      // use send closure later to not create synchronously default_join_group_call_as_dialog_id
      send_closure_later(G()->messages_manager(),
                         &MessagesManager::on_update_dialog_default_join_group_call_as_dialog_id, DialogId(channel_id),
                         default_join_group_call_as_dialog_id, false);
    }
    {
      DialogId default_send_message_as_dialog_id;
      if (channel->default_send_as_ != nullptr) {
        default_send_message_as_dialog_id = DialogId(channel->default_send_as_);
      }
      // use send closure later to not create synchronously default_send_message_as_dialog_id
      send_closure_later(G()->messages_manager(), &MessagesManager::on_update_dialog_default_send_message_as_dialog_id,
                         DialogId(channel_id), default_send_message_as_dialog_id, false);
    }

    if (participant_count >= 190) {
      int32 online_member_count = 0;
      if ((channel->flags_ & CHANNEL_FULL_FLAG_HAS_ONLINE_MEMBER_COUNT) != 0) {
        online_member_count = channel->online_count_;
      }
      td_->messages_manager_->on_update_dialog_online_member_count(DialogId(channel_id), online_member_count, true);
    }

    vector<UserId> bot_user_ids;
    for (const auto &bot_info : channel->bot_info_) {
      UserId user_id(bot_info->user_id_);
      if (!is_user_bot(user_id)) {
        continue;
      }

      bot_user_ids.push_back(user_id);
    }
    on_update_channel_full_bot_user_ids(channel_full, channel_id, std::move(bot_user_ids));

    auto bot_commands = get_bot_commands(std::move(channel->bot_info_), nullptr);
    if (channel_full->bot_commands != bot_commands) {
      channel_full->bot_commands = std::move(bot_commands);
      channel_full->is_changed = true;
    }

    ChannelId linked_channel_id;
    if ((channel->flags_ & CHANNEL_FULL_FLAG_HAS_LINKED_CHANNEL_ID) != 0) {
      linked_channel_id = ChannelId(channel->linked_chat_id_);
      auto linked_channel = get_channel_force(linked_channel_id);
      if (linked_channel == nullptr || c->is_megagroup == linked_channel->is_megagroup ||
          channel_id == linked_channel_id) {
        LOG(ERROR) << "Failed to add a link between " << channel_id << " and " << linked_channel_id;
        linked_channel_id = ChannelId();
      }
    }
    on_update_channel_full_linked_channel_id(channel_full, channel_id, linked_channel_id);

    on_update_channel_full_location(channel_full, channel_id, DialogLocation(std::move(channel->location_)));

    if (c->is_megagroup) {
      int32 slow_mode_delay = 0;
      int32 slow_mode_next_send_date = 0;
      if ((channel->flags_ & CHANNEL_FULL_FLAG_HAS_SLOW_MODE_DELAY) != 0) {
        slow_mode_delay = channel->slowmode_seconds_;
      }
      if ((channel->flags_ & CHANNEL_FULL_FLAG_HAS_SLOW_MODE_NEXT_SEND_DATE) != 0) {
        slow_mode_next_send_date = channel->slowmode_next_send_date_;
      }
      on_update_channel_full_slow_mode_delay(channel_full, channel_id, slow_mode_delay, slow_mode_next_send_date);
    }
    if (channel_full->can_be_deleted != channel->can_delete_channel_) {
      channel_full->can_be_deleted = channel->can_delete_channel_;
      channel_full->need_save_to_database = true;
    }
    if (c->can_be_deleted != channel_full->can_be_deleted) {
      c->can_be_deleted = channel_full->can_be_deleted;
      c->need_save_to_database = true;
    }

    ChatId migrated_from_chat_id;
    MessageId migrated_from_max_message_id;

    if ((channel->flags_ & CHANNEL_FULL_FLAG_MIGRATED_FROM) != 0) {
      migrated_from_chat_id = ChatId(channel->migrated_from_chat_id_);
      migrated_from_max_message_id = MessageId(ServerMessageId(channel->migrated_from_max_id_));
    }

    if (channel_full->migrated_from_chat_id != migrated_from_chat_id ||
        channel_full->migrated_from_max_message_id != migrated_from_max_message_id) {
      channel_full->migrated_from_chat_id = migrated_from_chat_id;
      channel_full->migrated_from_max_message_id = migrated_from_max_message_id;
      channel_full->is_changed = true;
    }

    if (c->is_changed) {
      LOG(WARNING) << "Receive inconsistent chatPhoto and chatPhotoInfo for " << channel_id;
      update_channel(c, channel_id);
    }

    channel_full->is_update_channel_full_sent = true;
    update_channel_full(channel_full, channel_id, "on_get_channel_full");

    if (linked_channel_id.is_valid()) {
      auto linked_channel_full = get_channel_full_force(linked_channel_id, true, "on_get_channel_full");
      on_update_channel_full_linked_channel_id(linked_channel_full, linked_channel_id, channel_id);
      if (linked_channel_full != nullptr) {
        update_channel_full(linked_channel_full, linked_channel_id, "on_get_channel_full 2");
      }
    }

    if (dismiss_suggested_action_queries_.count(DialogId(channel_id)) == 0) {
      auto it = dialog_suggested_actions_.find(DialogId(channel_id));
      if (it != dialog_suggested_actions_.end() || !channel->pending_suggestions_.empty()) {
        vector<SuggestedAction> suggested_actions;
        for (auto &action_str : channel->pending_suggestions_) {
          SuggestedAction suggested_action(action_str, DialogId(channel_id));
          if (!suggested_action.is_empty()) {
            if (suggested_action == SuggestedAction{SuggestedAction::Type::ConvertToGigagroup, DialogId(channel_id)} &&
                (c->is_gigagroup || c->default_permissions != RestrictedRights(false, false, false, false, false, false,
                                                                               false, false, false, false, false))) {
              LOG(INFO) << "Skip ConvertToGigagroup suggested action";
            } else {
              suggested_actions.push_back(suggested_action);
            }
          }
        }
        if (it == dialog_suggested_actions_.end()) {
          it = dialog_suggested_actions_.emplace(DialogId(channel_id), vector<SuggestedAction>()).first;
        }
        update_suggested_actions(it->second, std::move(suggested_actions));
        if (it->second.empty()) {
          dialog_suggested_actions_.erase(it);
        }
      }
    }
  }
  promise.set_value(Unit());
}

void ContactsManager::on_get_chat_full_failed(ChatId chat_id) {
  if (G()->close_flag()) {
    return;
  }

  LOG(INFO) << "Failed to get full " << chat_id;
}

void ContactsManager::on_get_channel_full_failed(ChannelId channel_id) {
  if (G()->close_flag()) {
    return;
  }

  LOG(INFO) << "Failed to get full " << channel_id;
  auto channel_full = get_channel_full(channel_id, true, "on_get_channel_full");
  if (channel_full != nullptr) {
    channel_full->repair_request_version = 0;
  }
}

bool ContactsManager::is_update_about_username_change_received(UserId user_id) const {
  const User *u = get_user(user_id);
  if (u != nullptr) {
    return u->is_contact;
  } else {
    return false;
  }
}

void ContactsManager::on_update_user_name(UserId user_id, string &&first_name, string &&last_name, string &&username) {
  if (!user_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << user_id;
    return;
  }

  User *u = get_user_force(user_id);
  if (u != nullptr) {
    on_update_user_name(u, user_id, std::move(first_name), std::move(last_name), std::move(username));
    update_user(u, user_id);
  } else {
    LOG(INFO) << "Ignore update user name about unknown " << user_id;
  }
}

void ContactsManager::on_update_user_name(User *u, UserId user_id, string &&first_name, string &&last_name,
                                          string &&username) {
  if (first_name.empty() && last_name.empty()) {
    first_name = u->phone_number;
  }
  if (u->first_name != first_name || u->last_name != last_name) {
    u->first_name = std::move(first_name);
    u->last_name = std::move(last_name);
    u->is_name_changed = true;
    LOG(DEBUG) << "Name has changed for " << user_id;
    u->is_changed = true;
  }
  td_->messages_manager_->on_dialog_username_updated(DialogId(user_id), u->username, username);
  if (u->username != username) {
    u->username = std::move(username);
    u->is_username_changed = true;
    LOG(DEBUG) << "Username has changed for " << user_id;
    u->is_changed = true;
  }
}

void ContactsManager::on_update_user_phone_number(UserId user_id, string &&phone_number) {
  if (!user_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << user_id;
    return;
  }

  User *u = get_user_force(user_id);
  if (u != nullptr) {
    on_update_user_phone_number(u, user_id, std::move(phone_number));
    update_user(u, user_id);
  } else {
    LOG(INFO) << "Ignore update user phone number about unknown " << user_id;
  }
}

void ContactsManager::on_update_user_phone_number(User *u, UserId user_id, string &&phone_number) {
  clean_phone_number(phone_number);
  if (u->phone_number != phone_number) {
    if (!u->phone_number.empty()) {
      auto it = resolved_phone_numbers_.find(u->phone_number);
      if (it != resolved_phone_numbers_.end() && it->second == user_id) {
        resolved_phone_numbers_.erase(it);
      }
    }

    u->phone_number = std::move(phone_number);
    u->is_phone_number_changed = true;
    LOG(DEBUG) << "Phone number has changed for " << user_id;
    u->is_changed = true;
  }
}

void ContactsManager::on_update_user_photo(UserId user_id, tl_object_ptr<telegram_api::UserProfilePhoto> &&photo_ptr) {
  if (!user_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << user_id;
    return;
  }

  User *u = get_user_force(user_id);
  if (u != nullptr) {
    on_update_user_photo(u, user_id, std::move(photo_ptr), "on_update_user_photo");
    update_user(u, user_id);
  } else {
    LOG(INFO) << "Ignore update user photo about unknown " << user_id;
  }
}

void ContactsManager::on_update_user_photo(User *u, UserId user_id,
                                           tl_object_ptr<telegram_api::UserProfilePhoto> &&photo, const char *source) {
  if (td_->auth_manager_->is_bot() && !G()->parameters().use_file_db && !u->is_photo_inited) {
    if (photo != nullptr && photo->get_id() == telegram_api::userProfilePhoto::ID) {
      auto *profile_photo = static_cast<telegram_api::userProfilePhoto *>(photo.get());
      if ((profile_photo->flags_ & telegram_api::userProfilePhoto::STRIPPED_THUMB_MASK) != 0) {
        profile_photo->flags_ -= telegram_api::userProfilePhoto::STRIPPED_THUMB_MASK;
        profile_photo->stripped_thumb_ = BufferSlice();
      }
    }
    auto &old_photo = pending_user_photos_[user_id];
    if (!LOG_IS_STRIPPED(ERROR) && to_string(old_photo) == to_string(photo)) {
      return;
    }

    bool is_empty = photo == nullptr || photo->get_id() == telegram_api::userProfilePhotoEmpty::ID;
    old_photo = std::move(photo);

    drop_user_photos(user_id, is_empty, true, "on_update_user_photo");
    return;
  }

  do_update_user_photo(u, user_id, std::move(photo), source);
}

void ContactsManager::do_update_user_photo(User *u, UserId user_id,
                                           tl_object_ptr<telegram_api::UserProfilePhoto> &&photo, const char *source) {
  ProfilePhoto new_photo = get_profile_photo(td_->file_manager_.get(), user_id, u->access_hash, std::move(photo));
  if (td_->auth_manager_->is_bot()) {
    new_photo.minithumbnail.clear();
  }
  do_update_user_photo(u, user_id, std::move(new_photo), true, source);
}

void ContactsManager::do_update_user_photo(User *u, UserId user_id, ProfilePhoto &&new_photo,
                                           bool invalidate_photo_cache, const char *source) {
  u->is_photo_inited = true;
  if (need_update_profile_photo(u->photo, new_photo)) {
    LOG_IF(ERROR, u->access_hash == -1 && new_photo.small_file_id.is_valid())
        << "Update profile photo of " << user_id << " without access hash from " << source;
    u->photo = new_photo;
    u->is_photo_changed = true;
    LOG(DEBUG) << "Photo has changed for " << user_id;
    u->is_changed = true;

    if (invalidate_photo_cache) {
      drop_user_photos(user_id, !u->photo.small_file_id.is_valid(), true, "do_update_user_photo");
    }
  }
}

void ContactsManager::register_user_photo(User *u, UserId user_id, const Photo &photo) {
  auto photo_file_ids = photo_get_file_ids(photo);
  if (photo.is_empty() || photo_file_ids.empty()) {
    return;
  }
  auto first_file_id = photo_file_ids[0];
  auto file_type = td_->file_manager_->get_file_view(first_file_id).get_type();
  if (file_type == FileType::ProfilePhoto) {
    return;
  }
  CHECK(file_type == FileType::Photo);
  CHECK(u != nullptr);
  auto photo_id = photo.id.get();
  if (photo_id != 0 && u->photo_ids.emplace(photo_id).second) {
    VLOG(file_references) << "Register photo " << photo_id << " of " << user_id;
    if (user_id == get_my_id()) {
      my_photo_file_id_[photo_id] = first_file_id;
    }
    auto file_source_id = user_profile_photo_file_source_ids_.get(std::make_pair(user_id, photo_id));
    if (file_source_id.is_valid()) {
      VLOG(file_references) << "Move " << file_source_id << " inside of " << user_id;
      user_profile_photo_file_source_ids_.erase(std::make_pair(user_id, photo_id));
    } else {
      VLOG(file_references) << "Need to create new file source for photo " << photo_id << " of " << user_id;
      file_source_id = td_->file_reference_manager_->create_user_photo_file_source(user_id, photo_id);
    }
    for (auto &file_id : photo_file_ids) {
      td_->file_manager_->add_file_source(file_id, file_source_id);
    }
  }
}

void ContactsManager::on_update_user_emoji_status(UserId user_id,
                                                  tl_object_ptr<telegram_api::EmojiStatus> &&emoji_status) {
  if (!user_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << user_id;
    return;
  }

  User *u = get_user_force(user_id);
  if (u != nullptr) {
    on_update_user_emoji_status(u, user_id, EmojiStatus(std::move(emoji_status)));
    update_user(u, user_id);
  } else {
    LOG(INFO) << "Ignore update user emoji status about unknown " << user_id;
  }
}

void ContactsManager::on_update_user_emoji_status(User *u, UserId user_id, EmojiStatus emoji_status) {
  if (u->emoji_status != emoji_status) {
    LOG(DEBUG) << "Change emoji status of " << user_id << " from " << u->emoji_status << " to " << emoji_status;
    u->emoji_status = emoji_status;
  }
}

void ContactsManager::on_update_user_is_contact(User *u, UserId user_id, bool is_contact, bool is_mutual_contact) {
  UserId my_id = get_my_id();
  if (user_id == my_id) {
    is_mutual_contact = is_contact;
  }
  if (!is_contact && is_mutual_contact) {
    LOG(ERROR) << "Receive is_mutual_contact == true for non-contact " << user_id;
    is_mutual_contact = false;
  }

  if (u->is_contact != is_contact || u->is_mutual_contact != is_mutual_contact) {
    LOG(DEBUG) << "Update " << user_id << " is_contact from (" << u->is_contact << ", " << u->is_mutual_contact
               << ") to (" << is_contact << ", " << is_mutual_contact << ")";
    if (u->is_contact != is_contact) {
      u->is_is_contact_changed = true;
    }
    u->is_contact = is_contact;
    u->is_mutual_contact = is_mutual_contact;
    u->is_changed = true;
  }
}

void ContactsManager::on_update_user_online(UserId user_id, tl_object_ptr<telegram_api::UserStatus> &&status) {
  if (!user_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << user_id;
    return;
  }

  User *u = get_user_force(user_id);
  if (u != nullptr) {
    if (u->is_bot) {
      LOG(ERROR) << "Receive updateUserStatus about bot " << user_id;
      return;
    }
    on_update_user_online(u, user_id, std::move(status));
    update_user(u, user_id);

    if (user_id == get_my_id() &&
        was_online_remote_ != u->was_online) {  // only update was_online_remote_ from updateUserStatus
      was_online_remote_ = u->was_online;
      VLOG(notifications) << "Set was_online_remote to " << was_online_remote_;
      G()->td_db()->get_binlog_pmc()->set("my_was_online_remote", to_string(was_online_remote_));
    }
  } else {
    LOG(INFO) << "Ignore update user online about unknown " << user_id;
  }
}

void ContactsManager::on_update_user_online(User *u, UserId user_id, tl_object_ptr<telegram_api::UserStatus> &&status) {
  int32 id = status == nullptr ? telegram_api::userStatusEmpty::ID : status->get_id();
  int32 new_online;
  bool is_offline = false;
  if (id == telegram_api::userStatusOnline::ID) {
    int32 now = G()->unix_time();

    auto st = move_tl_object_as<telegram_api::userStatusOnline>(status);
    new_online = st->expires_;
    LOG_IF(ERROR, new_online < now - 86400)
        << "Receive userStatusOnline expired more than one day in past " << new_online;
  } else if (id == telegram_api::userStatusOffline::ID) {
    int32 now = G()->unix_time();

    auto st = move_tl_object_as<telegram_api::userStatusOffline>(status);
    new_online = st->was_online_;
    if (new_online >= now) {
      LOG_IF(ERROR, new_online > now + 10)
          << "Receive userStatusOffline but was online points to future time " << new_online << ", now is " << now;
      new_online = now - 1;
    }
    is_offline = true;
  } else if (id == telegram_api::userStatusRecently::ID) {
    new_online = -1;
  } else if (id == telegram_api::userStatusLastWeek::ID) {
    new_online = -2;
    is_offline = true;
  } else if (id == telegram_api::userStatusLastMonth::ID) {
    new_online = -3;
    is_offline = true;
  } else {
    CHECK(id == telegram_api::userStatusEmpty::ID);
    new_online = 0;
  }

  if (new_online != u->was_online) {
    LOG(DEBUG) << "Update " << user_id << " online from " << u->was_online << " to " << new_online;
    bool old_is_online = u->was_online > G()->unix_time_cached();
    bool new_is_online = new_online > G()->unix_time_cached();
    u->was_online = new_online;
    u->is_status_changed = true;
    if (u->was_online > 0) {
      u->local_was_online = 0;
    }

    if (user_id == get_my_id()) {
      if (my_was_online_local_ != 0 || old_is_online != new_is_online) {
        my_was_online_local_ = 0;
        u->is_online_status_changed = true;
      }
      if (is_offline) {
        td_->on_online_updated(false, false);
      }
    } else if (old_is_online != new_is_online) {
      u->is_online_status_changed = true;
    }
  }
}

void ContactsManager::on_update_user_local_was_online(UserId user_id, int32 local_was_online) {
  CHECK(user_id.is_valid());

  User *u = get_user_force(user_id);
  if (u == nullptr) {
    return;
  }

  on_update_user_local_was_online(u, user_id, local_was_online);
  update_user(u, user_id);
}

void ContactsManager::on_update_user_local_was_online(User *u, UserId user_id, int32 local_was_online) {
  CHECK(u != nullptr);
  if (u->is_deleted || u->is_bot || u->is_support || user_id == get_my_id()) {
    return;
  }
  if (u->was_online > G()->unix_time_cached()) {
    // if user is currently online, ignore local online
    return;
  }

  // bring users online for 30 seconds
  local_was_online += 30;
  if (local_was_online < G()->unix_time_cached() + 2 || local_was_online <= u->local_was_online ||
      local_was_online <= u->was_online) {
    return;
  }

  LOG(DEBUG) << "Update " << user_id << " local online from " << u->local_was_online << " to " << local_was_online;
  bool old_is_online = u->local_was_online > G()->unix_time_cached();
  u->local_was_online = local_was_online;
  u->is_status_changed = true;

  if (!old_is_online) {
    u->is_online_status_changed = true;
  }
}

void ContactsManager::on_update_user_is_blocked(UserId user_id, bool is_blocked) {
  if (!user_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << user_id;
    return;
  }

  UserFull *user_full = get_user_full_force(user_id);
  if (user_full == nullptr || user_full->is_blocked == is_blocked) {
    return;
  }
  on_update_user_full_is_blocked(user_full, user_id, is_blocked);
  update_user_full(user_full, user_id, "on_update_user_is_blocked");
}

void ContactsManager::on_update_user_full_is_blocked(UserFull *user_full, UserId user_id, bool is_blocked) {
  CHECK(user_full != nullptr);
  if (user_full->is_blocked != is_blocked) {
    LOG(INFO) << "Receive update user full is blocked with " << user_id << " and is_blocked = " << is_blocked;
    user_full->is_blocked = is_blocked;
    user_full->is_changed = true;
  }
}

void ContactsManager::on_update_user_common_chat_count(UserId user_id, int32 common_chat_count) {
  LOG(INFO) << "Receive " << common_chat_count << " common chat count with " << user_id;
  if (!user_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << user_id;
    return;
  }

  UserFull *user_full = get_user_full_force(user_id);
  if (user_full == nullptr) {
    return;
  }
  on_update_user_full_common_chat_count(user_full, user_id, common_chat_count);
  update_user_full(user_full, user_id, "on_update_user_common_chat_count");
}

void ContactsManager::on_update_user_full_common_chat_count(UserFull *user_full, UserId user_id,
                                                            int32 common_chat_count) {
  CHECK(user_full != nullptr);
  if (common_chat_count < 0) {
    LOG(ERROR) << "Receive " << common_chat_count << " as common group count with " << user_id;
    common_chat_count = 0;
  }
  if (user_full->common_chat_count != common_chat_count) {
    user_full->common_chat_count = common_chat_count;
    user_full->is_common_chat_count_changed = true;
    user_full->is_changed = true;
  }
}

void ContactsManager::on_update_user_full_commands(UserFull *user_full, UserId user_id,
                                                   vector<tl_object_ptr<telegram_api::botCommand>> &&bot_commands) {
  CHECK(user_full != nullptr);
  auto commands = transform(std::move(bot_commands), [](tl_object_ptr<telegram_api::botCommand> &&bot_command) {
    return BotCommand(std::move(bot_command));
  });
  if (user_full->commands != commands) {
    user_full->commands = std::move(commands);
    user_full->is_changed = true;
  }
}

void ContactsManager::on_update_user_full_menu_button(UserFull *user_full, UserId user_id,
                                                      tl_object_ptr<telegram_api::BotMenuButton> &&bot_menu_button) {
  CHECK(user_full != nullptr);
  auto new_button = get_bot_menu_button(std::move(bot_menu_button));
  bool is_changed;
  if (user_full->menu_button == nullptr) {
    is_changed = (new_button != nullptr);
  } else {
    is_changed = (new_button == nullptr || *user_full->menu_button != *new_button);
  }
  if (is_changed) {
    user_full->menu_button = std::move(new_button);
    user_full->is_changed = true;
  }
}

void ContactsManager::on_update_user_need_phone_number_privacy_exception(UserId user_id,
                                                                         bool need_phone_number_privacy_exception) {
  LOG(INFO) << "Receive " << need_phone_number_privacy_exception << " need phone number privacy exception with "
            << user_id;
  if (!user_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << user_id;
    return;
  }

  UserFull *user_full = get_user_full_force(user_id);
  if (user_full == nullptr) {
    return;
  }
  on_update_user_full_need_phone_number_privacy_exception(user_full, user_id, need_phone_number_privacy_exception);
  update_user_full(user_full, user_id, "on_update_user_need_phone_number_privacy_exception");
}

void ContactsManager::on_update_user_full_need_phone_number_privacy_exception(
    UserFull *user_full, UserId user_id, bool need_phone_number_privacy_exception) const {
  CHECK(user_full != nullptr);
  if (need_phone_number_privacy_exception) {
    const User *u = get_user(user_id);
    if (u == nullptr || u->is_contact || user_id == get_my_id()) {
      need_phone_number_privacy_exception = false;
    }
  }
  if (user_full->need_phone_number_privacy_exception != need_phone_number_privacy_exception) {
    user_full->need_phone_number_privacy_exception = need_phone_number_privacy_exception;
    user_full->is_changed = true;
  }
}

void ContactsManager::on_ignored_restriction_reasons_changed() {
  restricted_user_ids_.foreach([&](const UserId &user_id) {
    send_closure(G()->td(), &Td::send_update,
                 td_api::make_object<td_api::updateUser>(get_user_object(user_id, get_user(user_id))));
  });
  restricted_channel_ids_.foreach([&](const ChannelId &channel_id) {
    send_closure(
        G()->td(), &Td::send_update,
        td_api::make_object<td_api::updateSupergroup>(get_supergroup_object(channel_id, get_channel(channel_id))));
  });
}

void ContactsManager::on_set_profile_photo(tl_object_ptr<telegram_api::photos_photo> &&photo, int64 old_photo_id) {
  LOG(INFO) << "Changed profile photo to " << to_string(photo);

  UserId my_user_id = get_my_id();

  if (old_photo_id != 0) {
    delete_profile_photo_from_cache(my_user_id, old_photo_id, false);
  }

  add_profile_photo_to_cache(my_user_id,
                             get_photo(td_->file_manager_.get(), std::move(photo->photo_), DialogId(my_user_id)));

  User *u = get_user(my_user_id);
  if (u != nullptr) {
    update_user(u, my_user_id);
  }
  auto *user_full = get_user_full(my_user_id);
  if (user_full != nullptr) {
    update_user_full(user_full, my_user_id, "on_set_profile_photo");
  }

  // if cache was correctly updated, this should produce no updates
  on_get_users(std::move(photo->users_), "on_set_profile_photo");
}

void ContactsManager::on_delete_profile_photo(int64 profile_photo_id, Promise<Unit> promise) {
  UserId my_user_id = get_my_id();

  bool need_reget_user = delete_profile_photo_from_cache(my_user_id, profile_photo_id, true);
  if (need_reget_user && !G()->close_flag()) {
    return reload_user(my_user_id, std::move(promise));
  }

  promise.set_value(Unit());
}

void ContactsManager::add_profile_photo_to_cache(UserId user_id, Photo &&photo) {
  if (photo.is_empty()) {
    return;
  }

  // we have subsequence of user photos in user_photos_
  // ProfilePhoto in User and Photo in UserFull

  User *u = get_user_force(user_id);
  if (u == nullptr) {
    return;
  }

  // update photo list
  auto it = user_photos_.find(user_id);
  if (it != user_photos_.end() && it->second.count != -1) {
    auto user_photos = &it->second;
    if (user_photos->offset == 0) {
      if (user_photos->photos.empty() || user_photos->photos[0].id.get() != photo.id.get()) {
        user_photos->photos.insert(user_photos->photos.begin(), photo);
        user_photos->count++;
        register_user_photo(u, user_id, user_photos->photos[0]);
      }
    } else {
      user_photos->count++;
      user_photos->offset++;
    }
  }

  // update Photo in UserFull
  auto user_full = get_user_full_force(user_id);
  if (user_full != nullptr) {
    if (user_full->photo != photo) {
      user_full->photo = photo;
      user_full->is_changed = true;
      register_user_photo(u, user_id, photo);
    }
    update_user_full(user_full, user_id, "add_profile_photo_to_cache");
  }

  // update ProfilePhoto in User
  do_update_user_photo(u, user_id, as_profile_photo(td_->file_manager_.get(), user_id, u->access_hash, photo), false,
                       "add_profile_photo_to_cache");
  update_user(u, user_id);
}

bool ContactsManager::delete_profile_photo_from_cache(UserId user_id, int64 profile_photo_id, bool send_updates) {
  CHECK(profile_photo_id != 0);

  // we have subsequence of user photos in user_photos_
  // ProfilePhoto in User and Photo in UserFull

  User *u = get_user_force(user_id);
  bool is_main_photo_deleted = u != nullptr && u->photo.id == profile_photo_id;

  // update photo list
  auto it = user_photos_.find(user_id);
  if (it != user_photos_.end() && it->second.count > 0) {
    auto user_photos = &it->second;
    auto old_size = user_photos->photos.size();
    if (td::remove_if(user_photos->photos,
                      [profile_photo_id](const auto &photo) { return photo.id.get() == profile_photo_id; })) {
      auto removed_photos = old_size - user_photos->photos.size();
      CHECK(removed_photos > 0);
      LOG_IF(ERROR, removed_photos != 1) << "Had " << removed_photos << " photos with ID " << profile_photo_id;
      user_photos->count -= narrow_cast<int32>(removed_photos);
      // offset was not changed
      CHECK(user_photos->count >= 0);
    } else {
      // failed to find photo to remove from cache
      // don't know how to adjust user_photos->offset, so drop photos cache
      LOG(INFO) << "Drop photos of " << user_id;
      user_photos->photos.clear();
      user_photos->count = -1;
      user_photos->offset = -1;
    }
  }

  // update Photo in UserFull
  auto user_full = get_user_full_force(user_id);
  if (user_full != nullptr && !user_full->photo.is_empty() &&
      (is_main_photo_deleted || user_full->photo.id.get() == profile_photo_id)) {
    if (it != user_photos_.end() && it->second.count != -1 && it->second.offset == 0 && !it->second.photos.empty()) {
      // found exact new photo
      if (it->second.photos[0] != user_full->photo) {
        user_full->photo = it->second.photos[0];
        user_full->is_changed = true;
      }
    } else {
      // repair UserFull photo
      user_full->expires_at = 0.0;
      user_full->photo = Photo();
      user_full->is_changed = true;

      load_user_full(user_id, true, Auto(), "delete_profile_photo_from_cache");
    }
    if (send_updates) {
      update_user_full(user_full, user_id, "delete_profile_photo_from_cache");
    }
  }

  // update ProfilePhoto in User
  if (is_main_photo_deleted) {
    bool need_reget_user = false;
    if (it != user_photos_.end() && it->second.count != -1 && it->second.offset == 0 && !it->second.photos.empty()) {
      // found exact new photo
      do_update_user_photo(u, user_id,
                           as_profile_photo(td_->file_manager_.get(), user_id, u->access_hash, it->second.photos[0]),
                           false, "delete_profile_photo_from_cache");
    } else {
      do_update_user_photo(u, user_id, ProfilePhoto(), false, "delete_profile_photo_from_cache 2");
      need_reget_user = it == user_photos_.end() || it->second.count != 0;
    }
    if (send_updates) {
      update_user(u, user_id);
    }
    return need_reget_user;
  }

  return false;
}

void ContactsManager::drop_user_photos(UserId user_id, bool is_empty, bool drop_user_full_photo, const char *source) {
  auto it = user_photos_.find(user_id);
  if (it != user_photos_.end()) {
    auto user_photos = &it->second;
    int32 new_count = is_empty ? 0 : -1;
    if (user_photos->count == new_count) {
      CHECK(user_photos->photos.empty());
      CHECK(user_photos->offset == user_photos->count);
    } else {
      LOG(INFO) << "Drop photos of " << user_id << " to " << (is_empty ? "empty" : "unknown") << " from " << source;
      user_photos->photos.clear();
      user_photos->count = new_count;
      user_photos->offset = user_photos->count;
    }
  }

  if (drop_user_full_photo) {
    auto user_full = get_user_full(user_id);  // must not load UserFull
    if (user_full != nullptr) {
      if (!user_full->photo.is_empty()) {
        user_full->photo = Photo();
        user_full->is_changed = true;
      }
      if (!is_empty) {
        if (user_full->expires_at > 0.0) {
          user_full->expires_at = 0.0;
          user_full->need_save_to_database = true;
        }
        reload_user_full(user_id, Auto());
      }
      update_user_full(user_full, user_id, "drop_user_photos");
    }
  }
}

void ContactsManager::drop_user_full(UserId user_id) {
  auto user_full = get_user_full_force(user_id);

  drop_user_photos(user_id, false, false, "drop_user_full");

  if (user_full == nullptr) {
    return;
  }

  user_full->expires_at = 0.0;

  user_full->photo = Photo();
  user_full->is_blocked = false;
  user_full->can_be_called = false;
  user_full->supports_video_calls = false;
  user_full->has_private_calls = false;
  user_full->need_phone_number_privacy_exception = false;
  user_full->about = string();
  user_full->description = string();
  user_full->description_photo = Photo();
  user_full->description_animation_file_id = FileId();
  user_full->menu_button = nullptr;
  user_full->commands.clear();
  user_full->common_chat_count = 0;
  user_full->private_forward_name.clear();
  user_full->group_administrator_rights = {};
  user_full->broadcast_administrator_rights = {};
  user_full->premium_gift_options.clear();
  user_full->voice_messages_forbidden = false;
  user_full->are_files_changed = true;
  user_full->is_changed = true;

  update_user_full(user_full, user_id, "drop_user_full");
  td_->group_call_manager_->on_update_dialog_about(DialogId(user_id), user_full->about, true);
}

void ContactsManager::update_user_online_member_count(User *u) {
  if (u->online_member_dialogs.empty()) {
    return;
  }

  auto now = G()->unix_time_cached();
  vector<DialogId> expired_dialog_ids;
  for (const auto &it : u->online_member_dialogs) {
    auto dialog_id = it.first;
    auto time = it.second;
    if (time < now - MessagesManager::ONLINE_MEMBER_COUNT_CACHE_EXPIRE_TIME) {
      expired_dialog_ids.push_back(dialog_id);
      continue;
    }

    switch (dialog_id.get_type()) {
      case DialogType::Chat: {
        auto chat_id = dialog_id.get_chat_id();
        auto chat_full = get_chat_full(chat_id);
        CHECK(chat_full != nullptr);
        update_chat_online_member_count(chat_full, chat_id, false);
        break;
      }
      case DialogType::Channel: {
        auto channel_id = dialog_id.get_channel_id();
        update_channel_online_member_count(channel_id, false);
        break;
      }
      case DialogType::User:
      case DialogType::SecretChat:
      case DialogType::None:
        UNREACHABLE();
        break;
    }
  }
  for (auto &dialog_id : expired_dialog_ids) {
    u->online_member_dialogs.erase(dialog_id);
    if (dialog_id.get_type() == DialogType::Channel) {
      cached_channel_participants_.erase(dialog_id.get_channel_id());
    }
  }
}

void ContactsManager::update_chat_online_member_count(const ChatFull *chat_full, ChatId chat_id, bool is_from_server) {
  update_dialog_online_member_count(chat_full->participants, DialogId(chat_id), is_from_server);
}

void ContactsManager::update_channel_online_member_count(ChannelId channel_id, bool is_from_server) {
  if (!is_megagroup_channel(channel_id)) {
    return;
  }

  auto it = cached_channel_participants_.find(channel_id);
  if (it == cached_channel_participants_.end()) {
    return;
  }
  update_dialog_online_member_count(it->second, DialogId(channel_id), is_from_server);
}

void ContactsManager::update_dialog_online_member_count(const vector<DialogParticipant> &participants,
                                                        DialogId dialog_id, bool is_from_server) {
  if (td_->auth_manager_->is_bot()) {
    return;
  }
  CHECK(dialog_id.is_valid());

  int32 online_member_count = 0;
  int32 time = G()->unix_time();
  for (const auto &participant : participants) {
    if (participant.dialog_id_.get_type() != DialogType::User) {
      continue;
    }
    auto user_id = participant.dialog_id_.get_user_id();
    auto u = get_user(user_id);
    if (u != nullptr && !u->is_deleted && !u->is_bot) {
      if (get_user_was_online(u, user_id) > time) {
        online_member_count++;
      }
      if (is_from_server) {
        u->online_member_dialogs[dialog_id] = time;
      }
    }
  }
  td_->messages_manager_->on_update_dialog_online_member_count(dialog_id, online_member_count, is_from_server);
}

void ContactsManager::on_get_chat_participants(tl_object_ptr<telegram_api::ChatParticipants> &&participants_ptr,
                                               bool from_update) {
  switch (participants_ptr->get_id()) {
    case telegram_api::chatParticipantsForbidden::ID: {
      auto participants = move_tl_object_as<telegram_api::chatParticipantsForbidden>(participants_ptr);
      ChatId chat_id(participants->chat_id_);
      if (!chat_id.is_valid()) {
        LOG(ERROR) << "Receive invalid " << chat_id;
        return;
      }

      if (!have_chat_force(chat_id)) {
        LOG(ERROR) << chat_id << " not found";
        return;
      }

      if (from_update) {
        drop_chat_full(chat_id);
      }
      break;
    }
    case telegram_api::chatParticipants::ID: {
      auto participants = move_tl_object_as<telegram_api::chatParticipants>(participants_ptr);
      ChatId chat_id(participants->chat_id_);
      if (!chat_id.is_valid()) {
        LOG(ERROR) << "Receive invalid " << chat_id;
        return;
      }

      const Chat *c = get_chat_force(chat_id);
      if (c == nullptr) {
        LOG(ERROR) << chat_id << " not found";
        return;
      }

      ChatFull *chat_full = get_chat_full_force(chat_id, "telegram_api::chatParticipants");
      if (chat_full == nullptr) {
        LOG(INFO) << "Ignore update of members for unknown full " << chat_id;
        return;
      }

      UserId new_creator_user_id;
      vector<DialogParticipant> new_participants;
      new_participants.reserve(participants->participants_.size());

      for (auto &participant_ptr : participants->participants_) {
        DialogParticipant dialog_participant(std::move(participant_ptr), c->date, c->status.is_creator());
        if (!dialog_participant.is_valid()) {
          LOG(ERROR) << "Receive invalid " << dialog_participant;
          continue;
        }

        LOG_IF(ERROR, !td_->messages_manager_->have_dialog_info(dialog_participant.dialog_id_))
            << "Have no information about " << dialog_participant.dialog_id_ << " as a member of " << chat_id;
        LOG_IF(ERROR, !have_user(dialog_participant.inviter_user_id_))
            << "Have no information about " << dialog_participant.inviter_user_id_ << " as a member of " << chat_id;
        if (dialog_participant.joined_date_ < c->date) {
          LOG_IF(ERROR, dialog_participant.joined_date_ < c->date - 30 && c->date >= 1486000000)
              << "Wrong join date = " << dialog_participant.joined_date_ << " for " << dialog_participant.dialog_id_
              << ", " << chat_id << " was created at " << c->date;
          dialog_participant.joined_date_ = c->date;
        }
        if (dialog_participant.status_.is_creator() && dialog_participant.dialog_id_.get_type() == DialogType::User) {
          new_creator_user_id = dialog_participant.dialog_id_.get_user_id();
        }
        new_participants.push_back(std::move(dialog_participant));
      }

      if (chat_full->creator_user_id != new_creator_user_id) {
        if (new_creator_user_id.is_valid() && chat_full->creator_user_id.is_valid()) {
          LOG(ERROR) << "Group creator has changed from " << chat_full->creator_user_id << " to " << new_creator_user_id
                     << " in " << chat_id;
        }
        chat_full->creator_user_id = new_creator_user_id;
        chat_full->is_changed = true;
      }

      on_update_chat_full_participants(chat_full, chat_id, std::move(new_participants), participants->version_,
                                       from_update);
      if (from_update) {
        update_chat_full(chat_full, chat_id, "on_get_chat_participants");
      }
      break;
    }
    default:
      UNREACHABLE();
  }
}

const DialogParticipant *ContactsManager::get_chat_participant(ChatId chat_id, UserId user_id) const {
  auto chat_full = get_chat_full(chat_id);
  if (chat_full == nullptr) {
    return nullptr;
  }
  return get_chat_full_participant(chat_full, DialogId(user_id));
}

const DialogParticipant *ContactsManager::get_chat_full_participant(const ChatFull *chat_full, DialogId dialog_id) {
  for (const auto &dialog_participant : chat_full->participants) {
    if (dialog_participant.dialog_id_ == dialog_id) {
      return &dialog_participant;
    }
  }
  return nullptr;
}

tl_object_ptr<td_api::chatMember> ContactsManager::get_chat_member_object(
    const DialogParticipant &dialog_participant) const {
  DialogId dialog_id = dialog_participant.dialog_id_;
  UserId participant_user_id;
  if (dialog_id.get_type() == DialogType::User) {
    participant_user_id = dialog_id.get_user_id();
  } else {
    td_->messages_manager_->force_create_dialog(dialog_id, "get_chat_member_object", true);
  }
  return td_api::make_object<td_api::chatMember>(
      get_message_sender_object_const(td_, dialog_id, "get_chat_member_object"),
      get_user_id_object(dialog_participant.inviter_user_id_, "chatMember.inviter_user_id"),
      dialog_participant.joined_date_, dialog_participant.status_.get_chat_member_status_object());
}

bool ContactsManager::on_get_channel_error(ChannelId channel_id, const Status &status, const string &source) {
  LOG(INFO) << "Receive " << status << " in " << channel_id << " from " << source;
  if (status.message() == CSlice("BOT_METHOD_INVALID")) {
    LOG(ERROR) << "Receive BOT_METHOD_INVALID from " << source;
    return true;
  }
  if (G()->is_expected_error(status)) {
    return true;
  }
  if (status.message() == "CHANNEL_PRIVATE" || status.message() == "CHANNEL_PUBLIC_GROUP_NA") {
    if (!channel_id.is_valid()) {
      LOG(ERROR) << "Receive " << status.message() << " in invalid " << channel_id << " from " << source;
      return false;
    }

    auto c = get_channel(channel_id);
    if (c == nullptr) {
      if (source == "GetChannelDifferenceQuery" || (td_->auth_manager_->is_bot() && source == "GetChannelsQuery")) {
        // get channel difference after restart
        // get channel from server by its identifier
        return true;
      }
      LOG(ERROR) << "Receive " << status.message() << " in not found " << channel_id << " from " << source;
      return false;
    }

    auto debug_channel_object = oneline(to_string(get_supergroup_object(channel_id, c)));
    if (c->status.is_member()) {
      LOG(INFO) << "Emulate leaving " << channel_id;
      // TODO we also may try to write to a public channel
      int32 flags = 0;
      if (c->is_megagroup) {
        flags |= CHANNEL_FLAG_IS_MEGAGROUP;
      } else {
        flags |= CHANNEL_FLAG_IS_BROADCAST;
      }
      telegram_api::channelForbidden update(flags, false /*ignored*/, false /*ignored*/, channel_id.get(),
                                            c->access_hash, c->title, 0);
      on_chat_update(update, "CHANNEL_PRIVATE");
    } else if (!c->status.is_banned()) {
      if (!c->username.empty()) {
        LOG(INFO) << "Drop username of " << channel_id;
        on_update_channel_username(c, channel_id, "");
      }

      on_update_channel_has_location(c, channel_id, false);

      on_update_channel_linked_channel_id(channel_id, ChannelId());

      update_channel(c, channel_id);

      remove_dialog_access_by_invite_link(DialogId(channel_id));
    }
    invalidate_channel_full(channel_id, !c->is_slow_mode_enabled);
    LOG_IF(ERROR, have_input_peer_channel(c, channel_id, AccessRights::Read))
        << "Have read access to channel after receiving CHANNEL_PRIVATE. Channel state: "
        << oneline(to_string(get_supergroup_object(channel_id, c)))
        << ". Previous channel state: " << debug_channel_object;

    return true;
  }
  return false;
}

bool ContactsManager::is_user_contact(UserId user_id, bool is_mutual) const {
  return is_user_contact(get_user(user_id), user_id, is_mutual);
}

bool ContactsManager::is_user_contact(const User *u, UserId user_id, bool is_mutual) const {
  return u != nullptr && (is_mutual ? u->is_mutual_contact : u->is_contact) && user_id != get_my_id();
}

void ContactsManager::on_get_channel_participants(
    ChannelId channel_id, ChannelParticipantFilter &&filter, int32 offset, int32 limit, string additional_query,
    int32 additional_limit, tl_object_ptr<telegram_api::channels_channelParticipants> &&channel_participants,
    Promise<DialogParticipants> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  on_get_users(std::move(channel_participants->users_), "on_get_channel_participants");
  on_get_chats(std::move(channel_participants->chats_), "on_get_channel_participants");
  int32 total_count = channel_participants->count_;
  auto participants = std::move(channel_participants->participants_);
  LOG(INFO) << "Receive " << participants.size() << " " << filter << " members in " << channel_id;

  bool is_full = offset == 0 && static_cast<int32>(participants.size()) < limit && total_count < limit;

  auto channel_type = get_channel_type(channel_id);
  vector<DialogParticipant> result;
  for (auto &participant_ptr : participants) {
    auto debug_participant = to_string(participant_ptr);
    result.emplace_back(std::move(participant_ptr), channel_type);
    const auto &participant = result.back();
    UserId participant_user_id;
    if (participant.dialog_id_.get_type() == DialogType::User) {
      participant_user_id = participant.dialog_id_.get_user_id();
    }
    if (!participant.is_valid() || (filter.is_bots() && !is_user_bot(participant_user_id)) ||
        (filter.is_administrators() && !participant.status_.is_administrator()) ||
        ((filter.is_recent() || filter.is_contacts() || filter.is_search()) && !participant.status_.is_member()) ||
        (filter.is_contacts() && !is_user_contact(participant_user_id)) ||
        (filter.is_restricted() && !participant.status_.is_restricted()) ||
        (filter.is_banned() && !participant.status_.is_banned())) {
      bool skip_error = ((filter.is_administrators() || filter.is_bots()) && is_user_deleted(participant_user_id)) ||
                        (filter.is_contacts() && participant_user_id == get_my_id());
      if (!skip_error) {
        LOG(ERROR) << "Receive " << participant << ", while searching for " << filter << " in " << channel_id
                   << " with offset " << offset << " and limit " << limit << ": " << oneline(debug_participant);
      }
      result.pop_back();
      total_count--;
    }
  }

  if (total_count < narrow_cast<int32>(result.size())) {
    LOG(ERROR) << "Receive total_count = " << total_count << ", but have at least " << result.size() << " " << filter
               << " members in " << channel_id;
    total_count = static_cast<int32>(result.size());
  } else if (is_full && total_count > static_cast<int32>(result.size())) {
    LOG(ERROR) << "Fix total number of " << filter << " members from " << total_count << " to " << result.size()
               << " in " << channel_id;
    total_count = static_cast<int32>(result.size());
  }

  const auto max_participant_count = is_megagroup_channel(channel_id) ? 975 : 195;
  auto participant_count =
      filter.is_recent() && total_count != 0 && total_count < max_participant_count ? total_count : -1;
  int32 administrator_count = filter.is_administrators() ? total_count : -1;
  if (is_full && (filter.is_administrators() || filter.is_bots() || filter.is_recent())) {
    vector<DialogAdministrator> administrators;
    vector<UserId> bot_user_ids;
    {
      if (filter.is_recent()) {
        for (const auto &participant : result) {
          if (participant.dialog_id_.get_type() == DialogType::User) {
            auto participant_user_id = participant.dialog_id_.get_user_id();
            if (participant.status_.is_administrator()) {
              administrators.emplace_back(participant_user_id, participant.status_.get_rank(),
                                          participant.status_.is_creator());
            }
            if (is_user_bot(participant_user_id)) {
              bot_user_ids.push_back(participant_user_id);
            }
          }
        }
        administrator_count = narrow_cast<int32>(administrators.size());

        if (is_megagroup_channel(channel_id) && !td_->auth_manager_->is_bot()) {
          cached_channel_participants_[channel_id] = result;
          update_channel_online_member_count(channel_id, true);
        }
      } else if (filter.is_administrators()) {
        for (const auto &participant : result) {
          if (participant.dialog_id_.get_type() == DialogType::User) {
            administrators.emplace_back(participant.dialog_id_.get_user_id(), participant.status_.get_rank(),
                                        participant.status_.is_creator());
          }
        }
      } else if (filter.is_bots()) {
        bot_user_ids = transform(result, [](const DialogParticipant &participant) {
          CHECK(participant.dialog_id_.get_type() == DialogType::User);
          return participant.dialog_id_.get_user_id();
        });
      }
    }
    if (filter.is_administrators() || filter.is_recent()) {
      on_update_dialog_administrators(DialogId(channel_id), std::move(administrators), true, false);
    }
    if (filter.is_bots() || filter.is_recent()) {
      on_update_channel_bot_user_ids(channel_id, std::move(bot_user_ids));
    }
  }
  if (have_channel_participant_cache(channel_id)) {
    for (const auto &participant : result) {
      add_channel_participant_to_cache(channel_id, participant, false);
    }
  }

  if (participant_count != -1 || administrator_count != -1) {
    auto channel_full = get_channel_full_force(channel_id, true, "on_get_channel_participants_success");
    if (channel_full != nullptr) {
      if (administrator_count == -1) {
        administrator_count = channel_full->administrator_count;
      }
      if (participant_count == -1) {
        participant_count = channel_full->participant_count;
      }
      if (participant_count < administrator_count) {
        participant_count = administrator_count;
      }
      if (channel_full->participant_count != participant_count) {
        channel_full->participant_count = participant_count;
        channel_full->is_changed = true;
      }
      if (channel_full->administrator_count != administrator_count) {
        channel_full->administrator_count = administrator_count;
        channel_full->is_changed = true;
      }
      update_channel_full(channel_full, channel_id, "on_get_channel_participants");
    }
    if (participant_count != -1) {
      auto c = get_channel(channel_id);
      if (c != nullptr && c->participant_count != participant_count) {
        c->participant_count = participant_count;
        c->is_changed = true;
        update_channel(c, channel_id);
      }
    }
  }

  if (!additional_query.empty()) {
    auto dialog_ids = transform(result, [](const DialogParticipant &participant) { return participant.dialog_id_; });
    std::pair<int32, vector<DialogId>> result_dialog_ids =
        search_among_dialogs(dialog_ids, additional_query, additional_limit);

    total_count = result_dialog_ids.first;
    FlatHashSet<DialogId, DialogIdHash> result_dialog_ids_set;
    for (auto result_dialog_id : result_dialog_ids.second) {
      CHECK(result_dialog_id.is_valid());
      result_dialog_ids_set.insert(result_dialog_id);
    }
    auto all_participants = std::move(result);
    result.clear();
    for (auto &participant : all_participants) {
      if (result_dialog_ids_set.count(participant.dialog_id_)) {
        result_dialog_ids_set.erase(participant.dialog_id_);
        result.push_back(std::move(participant));
      }
    }
  }

  promise.set_value(DialogParticipants{total_count, std::move(result)});
}

bool ContactsManager::have_channel_participant_cache(ChannelId channel_id) const {
  if (!td_->auth_manager_->is_bot()) {
    return false;
  }
  auto c = get_channel(channel_id);
  return c != nullptr && c->status.is_administrator();
}

void ContactsManager::add_channel_participant_to_cache(ChannelId channel_id,
                                                       const DialogParticipant &dialog_participant,
                                                       bool allow_replace) {
  CHECK(channel_id.is_valid());
  CHECK(dialog_participant.is_valid());
  auto &participants = channel_participants_[channel_id];
  if (participants.participants_.empty()) {
    channel_participant_cache_timeout_.set_timeout_in(channel_id.get(), CHANNEL_PARTICIPANT_CACHE_TIME);
  }
  auto &participant_info = participants.participants_[dialog_participant.dialog_id_];
  if (participant_info.last_access_date_ > 0 && !allow_replace) {
    return;
  }
  participant_info.participant_ = dialog_participant;
  participant_info.last_access_date_ = G()->unix_time();
}

void ContactsManager::update_channel_participant_status_cache(ChannelId channel_id, DialogId participant_dialog_id,
                                                              DialogParticipantStatus &&dialog_participant_status) {
  CHECK(channel_id.is_valid());
  CHECK(participant_dialog_id.is_valid());
  auto channel_participants_it = channel_participants_.find(channel_id);
  if (channel_participants_it == channel_participants_.end()) {
    return;
  }
  auto &participants = channel_participants_it->second;
  auto it = participants.participants_.find(participant_dialog_id);
  if (it == participants.participants_.end()) {
    return;
  }
  auto &participant_info = it->second;
  LOG(INFO) << "Update cached status of " << participant_dialog_id << " in " << channel_id << " from "
            << participant_info.participant_.status_ << " to " << dialog_participant_status;
  participant_info.participant_.status_ = std::move(dialog_participant_status);
  participant_info.last_access_date_ = G()->unix_time();
}

const DialogParticipant *ContactsManager::get_channel_participant_from_cache(ChannelId channel_id,
                                                                             DialogId participant_dialog_id) {
  auto channel_participants_it = channel_participants_.find(channel_id);
  if (channel_participants_it == channel_participants_.end()) {
    return nullptr;
  }

  auto &participants = channel_participants_it->second.participants_;
  CHECK(!participants.empty());
  auto it = participants.find(participant_dialog_id);
  if (it != participants.end()) {
    it->second.participant_.status_.update_restrictions();
    it->second.last_access_date_ = G()->unix_time();
    return &it->second.participant_;
  }
  return nullptr;
}

bool ContactsManager::speculative_add_count(int32 &count, int32 delta_count, int32 min_count) {
  auto new_count = count + delta_count;
  if (new_count < min_count) {
    new_count = min_count;
  }
  if (new_count == count) {
    return false;
  }

  count = new_count;
  return true;
}

void ContactsManager::speculative_add_channel_participants(ChannelId channel_id, const vector<UserId> &added_user_ids,
                                                           UserId inviter_user_id, int32 date, bool by_me) {
  auto it = cached_channel_participants_.find(channel_id);
  auto channel_full = get_channel_full_force(channel_id, true, "speculative_add_channel_participants");
  bool is_participants_cache_changed = false;

  int32 delta_participant_count = 0;
  for (auto user_id : added_user_ids) {
    if (!user_id.is_valid()) {
      continue;
    }

    delta_participant_count++;

    if (it != cached_channel_participants_.end()) {
      auto &participants = it->second;
      bool is_found = false;
      for (auto &participant : participants) {
        if (participant.dialog_id_ == DialogId(user_id)) {
          is_found = true;
          break;
        }
      }
      if (!is_found) {
        is_participants_cache_changed = true;
        participants.emplace_back(DialogId(user_id), inviter_user_id, date, DialogParticipantStatus::Member());
      }
    }

    if (channel_full != nullptr && is_user_bot(user_id) && !td::contains(channel_full->bot_user_ids, user_id)) {
      channel_full->bot_user_ids.push_back(user_id);
      channel_full->need_save_to_database = true;
      reload_channel_full(channel_id, Promise<Unit>(), "speculative_add_channel_participants");

      send_closure_later(G()->messages_manager(), &MessagesManager::on_dialog_bots_updated, DialogId(channel_id),
                         channel_full->bot_user_ids, false);
    }
  }
  if (is_participants_cache_changed) {
    update_channel_online_member_count(channel_id, false);
  }
  if (channel_full != nullptr) {
    update_channel_full(channel_full, channel_id, "speculative_add_channel_participants");
  }
  if (delta_participant_count == 0) {
    return;
  }

  speculative_add_channel_participant_count(channel_id, delta_participant_count, by_me);
}

void ContactsManager::speculative_delete_channel_participant(ChannelId channel_id, UserId deleted_user_id, bool by_me) {
  if (!deleted_user_id.is_valid()) {
    return;
  }

  auto it = cached_channel_participants_.find(channel_id);
  if (it != cached_channel_participants_.end()) {
    auto &participants = it->second;
    for (size_t i = 0; i < participants.size(); i++) {
      if (participants[i].dialog_id_ == DialogId(deleted_user_id)) {
        participants.erase(participants.begin() + i);
        update_channel_online_member_count(channel_id, false);
        break;
      }
    }
  }

  if (is_user_bot(deleted_user_id)) {
    auto channel_full = get_channel_full_force(channel_id, true, "speculative_delete_channel_participant");
    if (channel_full != nullptr && td::remove(channel_full->bot_user_ids, deleted_user_id)) {
      channel_full->need_save_to_database = true;
      update_channel_full(channel_full, channel_id, "speculative_delete_channel_participant");

      send_closure_later(G()->messages_manager(), &MessagesManager::on_dialog_bots_updated, DialogId(channel_id),
                         channel_full->bot_user_ids, false);
    }
  }

  speculative_add_channel_participant_count(channel_id, -1, by_me);
}

void ContactsManager::speculative_add_channel_participant_count(ChannelId channel_id, int32 delta_participant_count,
                                                                bool by_me) {
  if (by_me) {
    // Currently ignore all changes made by the current user, because they may be already counted
    invalidate_channel_full(channel_id, false);  // just in case
    return;
  }

  auto channel_full = get_channel_full_force(channel_id, true, "speculative_add_channel_participant_count");
  auto min_count = channel_full == nullptr ? 0 : channel_full->administrator_count;

  auto c = get_channel_force(channel_id);
  if (c != nullptr && c->participant_count != 0 &&
      speculative_add_count(c->participant_count, delta_participant_count, min_count)) {
    c->is_changed = true;
    update_channel(c, channel_id);
  }

  if (channel_full == nullptr) {
    return;
  }

  channel_full->is_changed |=
      speculative_add_count(channel_full->participant_count, delta_participant_count, min_count);

  if (channel_full->is_changed) {
    channel_full->speculative_version++;
  }

  update_channel_full(channel_full, channel_id, "speculative_add_channel_participant_count");
}

void ContactsManager::speculative_add_channel_user(ChannelId channel_id, UserId user_id,
                                                   const DialogParticipantStatus &new_status,
                                                   const DialogParticipantStatus &old_status) {
  auto c = get_channel_force(channel_id);
  // channel full must be loaded before c->participant_count is updated, because on_load_channel_full_from_database
  // must copy the initial c->participant_count before it is speculatibely updated
  auto channel_full = get_channel_full_force(channel_id, true, "speculative_add_channel_user");
  int32 min_count = 0;
  if (channel_full != nullptr) {
    channel_full->is_changed |= speculative_add_count(channel_full->administrator_count,
                                                      new_status.is_administrator() - old_status.is_administrator());
    min_count = channel_full->administrator_count;
  }

  if (c != nullptr && c->participant_count != 0 &&
      speculative_add_count(c->participant_count, new_status.is_member() - old_status.is_member(), min_count)) {
    c->is_changed = true;
    update_channel(c, channel_id);
  }

  if (new_status.is_administrator() != old_status.is_administrator() ||
      new_status.get_rank() != old_status.get_rank()) {
    DialogId dialog_id(channel_id);
    auto administrators_it = dialog_administrators_.find(dialog_id);
    if (administrators_it != dialog_administrators_.end()) {
      auto administrators = administrators_it->second;
      if (new_status.is_administrator()) {
        bool is_found = false;
        for (auto &administrator : administrators) {
          if (administrator.get_user_id() == user_id) {
            is_found = true;
            if (administrator.get_rank() != new_status.get_rank() ||
                administrator.is_creator() != new_status.is_creator()) {
              administrator = DialogAdministrator(user_id, new_status.get_rank(), new_status.is_creator());
              on_update_dialog_administrators(dialog_id, std::move(administrators), true, false);
            }
            break;
          }
        }
        if (!is_found) {
          administrators.emplace_back(user_id, new_status.get_rank(), new_status.is_creator());
          on_update_dialog_administrators(dialog_id, std::move(administrators), true, false);
        }
      } else {
        size_t i = 0;
        while (i != administrators.size() && administrators[i].get_user_id() != user_id) {
          i++;
        }
        if (i != administrators.size()) {
          administrators.erase(administrators.begin() + i);
          on_update_dialog_administrators(dialog_id, std::move(administrators), true, false);
        }
      }
    }
  }

  auto it = cached_channel_participants_.find(channel_id);
  if (it != cached_channel_participants_.end()) {
    auto &participants = it->second;
    bool is_found = false;
    for (size_t i = 0; i < participants.size(); i++) {
      if (participants[i].dialog_id_ == DialogId(user_id)) {
        if (!new_status.is_member()) {
          participants.erase(participants.begin() + i);
          update_channel_online_member_count(channel_id, false);
        } else {
          participants[i].status_ = new_status;
        }
        is_found = true;
        break;
      }
    }
    if (!is_found && new_status.is_member()) {
      participants.emplace_back(DialogId(user_id), get_my_id(), G()->unix_time(), new_status);
      update_channel_online_member_count(channel_id, false);
    }
  }

  if (channel_full == nullptr) {
    return;
  }

  channel_full->is_changed |= speculative_add_count(channel_full->participant_count,
                                                    new_status.is_member() - old_status.is_member(), min_count);
  channel_full->is_changed |=
      speculative_add_count(channel_full->restricted_count, new_status.is_restricted() - old_status.is_restricted());
  channel_full->is_changed |=
      speculative_add_count(channel_full->banned_count, new_status.is_banned() - old_status.is_banned());

  if (channel_full->is_changed) {
    channel_full->speculative_version++;
  }

  if (new_status.is_member() != old_status.is_member() && is_user_bot(user_id)) {
    if (new_status.is_member()) {
      if (!td::contains(channel_full->bot_user_ids, user_id)) {
        channel_full->bot_user_ids.push_back(user_id);
        channel_full->need_save_to_database = true;
        reload_channel_full(channel_id, Promise<Unit>(), "speculative_add_channel_user");

        send_closure_later(G()->messages_manager(), &MessagesManager::on_dialog_bots_updated, DialogId(channel_id),
                           channel_full->bot_user_ids, false);
      }
    } else {
      if (td::remove(channel_full->bot_user_ids, user_id)) {
        channel_full->need_save_to_database = true;

        send_closure_later(G()->messages_manager(), &MessagesManager::on_dialog_bots_updated, DialogId(channel_id),
                           channel_full->bot_user_ids, false);
      }
    }
  }

  update_channel_full(channel_full, channel_id, "speculative_add_channel_user");
}

void ContactsManager::invalidate_channel_full(ChannelId channel_id, bool need_drop_slow_mode_delay) {
  LOG(INFO) << "Invalidate supergroup full for " << channel_id;
  auto channel_full = get_channel_full(channel_id, true, "invalidate_channel_full");  // must not load ChannelFull
  if (channel_full != nullptr) {
    do_invalidate_channel_full(channel_full, channel_id, need_drop_slow_mode_delay);
    update_channel_full(channel_full, channel_id, "invalidate_channel_full");
  } else if (channel_id.is_valid()) {
    invalidated_channels_full_.insert(channel_id);
  }
}

void ContactsManager::do_invalidate_channel_full(ChannelFull *channel_full, ChannelId channel_id,
                                                 bool need_drop_slow_mode_delay) {
  CHECK(channel_full != nullptr);
  td_->messages_manager_->on_dialog_info_full_invalidated(DialogId(channel_id));
  if (channel_full->expires_at >= Time::now()) {
    channel_full->expires_at = 0.0;
    channel_full->need_save_to_database = true;
  }
  if (need_drop_slow_mode_delay && channel_full->slow_mode_delay != 0) {
    channel_full->slow_mode_delay = 0;
    channel_full->slow_mode_next_send_date = 0;
    channel_full->is_slow_mode_next_send_date_changed = true;
    channel_full->is_changed = true;
  }
}

void ContactsManager::on_update_chat_full_photo(ChatFull *chat_full, ChatId chat_id, Photo photo) {
  CHECK(chat_full != nullptr);
  if (photo != chat_full->photo) {
    chat_full->photo = std::move(photo);
    chat_full->is_changed = true;
  }

  auto photo_file_ids = photo_get_file_ids(chat_full->photo);
  if (chat_full->registered_photo_file_ids == photo_file_ids) {
    return;
  }

  auto &file_source_id = chat_full->file_source_id;
  if (!file_source_id.is_valid()) {
    file_source_id = chat_full_file_source_ids_.get(chat_id);
    if (file_source_id.is_valid()) {
      VLOG(file_references) << "Move " << file_source_id << " inside of " << chat_id;
      chat_full_file_source_ids_.erase(chat_id);
    } else {
      VLOG(file_references) << "Need to create new file source for full " << chat_id;
      file_source_id = td_->file_reference_manager_->create_chat_full_file_source(chat_id);
    }
  }

  td_->file_manager_->change_files_source(file_source_id, chat_full->registered_photo_file_ids, photo_file_ids);
  chat_full->registered_photo_file_ids = std::move(photo_file_ids);
}

void ContactsManager::on_update_channel_full_photo(ChannelFull *channel_full, ChannelId channel_id, Photo photo) {
  CHECK(channel_full != nullptr);
  if (photo != channel_full->photo) {
    channel_full->photo = std::move(photo);
    channel_full->is_changed = true;
  }

  auto photo_file_ids = photo_get_file_ids(channel_full->photo);
  if (channel_full->registered_photo_file_ids == photo_file_ids) {
    return;
  }

  auto &file_source_id = channel_full->file_source_id;
  if (!file_source_id.is_valid()) {
    file_source_id = channel_full_file_source_ids_.get(channel_id);
    if (file_source_id.is_valid()) {
      VLOG(file_references) << "Move " << file_source_id << " inside of " << channel_id;
      channel_full_file_source_ids_.erase(channel_id);
    } else {
      VLOG(file_references) << "Need to create new file source for full " << channel_id;
      file_source_id = td_->file_reference_manager_->create_channel_full_file_source(channel_id);
    }
  }

  td_->file_manager_->change_files_source(file_source_id, channel_full->registered_photo_file_ids, photo_file_ids);
  channel_full->registered_photo_file_ids = std::move(photo_file_ids);
}

void ContactsManager::on_get_permanent_dialog_invite_link(DialogId dialog_id, const DialogInviteLink &invite_link) {
  switch (dialog_id.get_type()) {
    case DialogType::Chat: {
      auto chat_id = dialog_id.get_chat_id();
      auto chat_full = get_chat_full_force(chat_id, "on_get_permanent_dialog_invite_link");
      if (chat_full != nullptr && update_permanent_invite_link(chat_full->invite_link, invite_link)) {
        chat_full->is_changed = true;
        update_chat_full(chat_full, chat_id, "on_get_permanent_dialog_invite_link");
      }
      break;
    }
    case DialogType::Channel: {
      auto channel_id = dialog_id.get_channel_id();
      auto channel_full = get_channel_full_force(channel_id, true, "on_get_permanent_dialog_invite_link");
      if (channel_full != nullptr && update_permanent_invite_link(channel_full->invite_link, invite_link)) {
        channel_full->is_changed = true;
        update_channel_full(channel_full, channel_id, "on_get_permanent_dialog_invite_link");
      }
      break;
    }
    case DialogType::User:
    case DialogType::SecretChat:
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

void ContactsManager::on_update_chat_full_invite_link(ChatFull *chat_full,
                                                      tl_object_ptr<telegram_api::ExportedChatInvite> &&invite_link) {
  CHECK(chat_full != nullptr);
  if (update_permanent_invite_link(chat_full->invite_link, DialogInviteLink(std::move(invite_link), "ChatFull"))) {
    chat_full->is_changed = true;
  }
}

void ContactsManager::on_update_channel_full_invite_link(
    ChannelFull *channel_full, tl_object_ptr<telegram_api::ExportedChatInvite> &&invite_link) {
  CHECK(channel_full != nullptr);
  if (update_permanent_invite_link(channel_full->invite_link,
                                   DialogInviteLink(std::move(invite_link), "ChannelFull"))) {
    channel_full->is_changed = true;
  }
}

void ContactsManager::remove_linked_channel_id(ChannelId channel_id) {
  if (!channel_id.is_valid()) {
    return;
  }

  auto linked_channel_id = linked_channel_ids_.get(channel_id);
  if (linked_channel_id.is_valid()) {
    linked_channel_ids_.erase(channel_id);
    linked_channel_ids_.erase(linked_channel_id);
  }
}

ChannelId ContactsManager::get_linked_channel_id(ChannelId channel_id) const {
  auto channel_full = get_channel_full(channel_id);
  if (channel_full != nullptr) {
    return channel_full->linked_channel_id;
  }

  return linked_channel_ids_.get(channel_id);
}

void ContactsManager::on_update_channel_full_linked_channel_id(ChannelFull *channel_full, ChannelId channel_id,
                                                               ChannelId linked_channel_id) {
  auto old_linked_channel_id = get_linked_channel_id(channel_id);
  LOG(INFO) << "Uplate linked channel in " << channel_id << " from " << old_linked_channel_id << " to "
            << linked_channel_id;

  if (channel_full != nullptr && channel_full->linked_channel_id != linked_channel_id &&
      channel_full->linked_channel_id.is_valid()) {
    get_channel_force(channel_full->linked_channel_id);
    get_channel_full_force(channel_full->linked_channel_id, true, "on_update_channel_full_linked_channel_id 0");
  }
  auto old_linked_linked_channel_id = get_linked_channel_id(linked_channel_id);

  remove_linked_channel_id(channel_id);
  remove_linked_channel_id(linked_channel_id);
  if (channel_id.is_valid() && linked_channel_id.is_valid()) {
    linked_channel_ids_.set(channel_id, linked_channel_id);
    linked_channel_ids_.set(linked_channel_id, channel_id);
  }

  if (channel_full != nullptr && channel_full->linked_channel_id != linked_channel_id) {
    if (channel_full->linked_channel_id.is_valid()) {
      // remove link from a previously linked channel_full
      auto linked_channel = get_channel_force(channel_full->linked_channel_id);
      if (linked_channel != nullptr && linked_channel->has_linked_channel) {
        linked_channel->has_linked_channel = false;
        linked_channel->is_changed = true;
        update_channel(linked_channel, channel_full->linked_channel_id);
        reload_channel(channel_full->linked_channel_id, Auto());
      }
      auto linked_channel_full =
          get_channel_full_force(channel_full->linked_channel_id, true, "on_update_channel_full_linked_channel_id 1");
      if (linked_channel_full != nullptr && linked_channel_full->linked_channel_id == channel_id) {
        linked_channel_full->linked_channel_id = ChannelId();
        linked_channel_full->is_changed = true;
        update_channel_full(linked_channel_full, channel_full->linked_channel_id,
                            "on_update_channel_full_linked_channel_id 3");
      }
    }

    channel_full->linked_channel_id = linked_channel_id;
    channel_full->is_changed = true;

    if (channel_full->linked_channel_id.is_valid()) {
      // add link from a newly linked channel_full
      auto linked_channel = get_channel_force(channel_full->linked_channel_id);
      if (linked_channel != nullptr && !linked_channel->has_linked_channel) {
        linked_channel->has_linked_channel = true;
        linked_channel->is_changed = true;
        update_channel(linked_channel, channel_full->linked_channel_id);
        reload_channel(channel_full->linked_channel_id, Auto());
      }
      auto linked_channel_full =
          get_channel_full_force(channel_full->linked_channel_id, true, "on_update_channel_full_linked_channel_id 2");
      if (linked_channel_full != nullptr && linked_channel_full->linked_channel_id != channel_id) {
        linked_channel_full->linked_channel_id = channel_id;
        linked_channel_full->is_changed = true;
        update_channel_full(linked_channel_full, channel_full->linked_channel_id,
                            "on_update_channel_full_linked_channel_id 4");
      }
    }
  }

  Channel *c = get_channel(channel_id);
  CHECK(c != nullptr);
  if (linked_channel_id.is_valid() != c->has_linked_channel) {
    c->has_linked_channel = linked_channel_id.is_valid();
    c->is_changed = true;
    update_channel(c, channel_id);
  }

  if (old_linked_channel_id != linked_channel_id) {
    // must be called after the linked channel is changed
    td_->messages_manager_->on_dialog_linked_channel_updated(DialogId(channel_id), old_linked_channel_id,
                                                             linked_channel_id);
  }

  if (linked_channel_id.is_valid()) {
    auto new_linked_linked_channel_id = get_linked_channel_id(linked_channel_id);
    LOG(INFO) << "Uplate linked channel in " << linked_channel_id << " from " << old_linked_linked_channel_id << " to "
              << new_linked_linked_channel_id;
    if (old_linked_linked_channel_id != new_linked_linked_channel_id) {
      // must be called after the linked channel is changed
      td_->messages_manager_->on_dialog_linked_channel_updated(
          DialogId(linked_channel_id), old_linked_linked_channel_id, new_linked_linked_channel_id);
    }
  }
}

void ContactsManager::on_update_channel_full_location(ChannelFull *channel_full, ChannelId channel_id,
                                                      const DialogLocation &location) {
  if (channel_full->location != location) {
    channel_full->location = location;
    channel_full->is_changed = true;
  }

  Channel *c = get_channel(channel_id);
  CHECK(c != nullptr);
  on_update_channel_has_location(c, channel_id, !location.empty());
  update_channel(c, channel_id);
}

void ContactsManager::on_update_channel_full_slow_mode_delay(ChannelFull *channel_full, ChannelId channel_id,
                                                             int32 slow_mode_delay, int32 slow_mode_next_send_date) {
  if (slow_mode_delay < 0) {
    LOG(ERROR) << "Receive slow mode delay " << slow_mode_delay << " in " << channel_id;
    slow_mode_delay = 0;
  }

  if (channel_full->slow_mode_delay != slow_mode_delay) {
    channel_full->slow_mode_delay = slow_mode_delay;
    channel_full->is_changed = true;
  }
  on_update_channel_full_slow_mode_next_send_date(channel_full, slow_mode_next_send_date);

  Channel *c = get_channel(channel_id);
  CHECK(c != nullptr);
  bool is_slow_mode_enabled = slow_mode_delay != 0;
  if (is_slow_mode_enabled != c->is_slow_mode_enabled) {
    c->is_slow_mode_enabled = is_slow_mode_enabled;
    c->is_changed = true;
    update_channel(c, channel_id);
  }
}

void ContactsManager::on_update_channel_full_slow_mode_next_send_date(ChannelFull *channel_full,
                                                                      int32 slow_mode_next_send_date) {
  if (slow_mode_next_send_date < 0) {
    LOG(ERROR) << "Receive slow mode next send date " << slow_mode_next_send_date;
    slow_mode_next_send_date = 0;
  }
  if (channel_full->slow_mode_delay == 0 && slow_mode_next_send_date > 0) {
    LOG(ERROR) << "Slow mode is disabled, but next send date is " << slow_mode_next_send_date;
    slow_mode_next_send_date = 0;
  }

  if (slow_mode_next_send_date != 0) {
    auto now = G()->unix_time();
    if (slow_mode_next_send_date <= now) {
      slow_mode_next_send_date = 0;
    }
    if (slow_mode_next_send_date > now + 3601) {
      slow_mode_next_send_date = now + 3601;
    }
  }
  if (channel_full->slow_mode_next_send_date != slow_mode_next_send_date) {
    channel_full->slow_mode_next_send_date = slow_mode_next_send_date;
    channel_full->is_slow_mode_next_send_date_changed = true;
    channel_full->is_changed = true;
  }
}

void ContactsManager::on_get_dialog_invite_link_info(const string &invite_link,
                                                     tl_object_ptr<telegram_api::ChatInvite> &&chat_invite_ptr,
                                                     Promise<Unit> &&promise) {
  CHECK(chat_invite_ptr != nullptr);
  switch (chat_invite_ptr->get_id()) {
    case telegram_api::chatInviteAlready::ID:
    case telegram_api::chatInvitePeek::ID: {
      telegram_api::object_ptr<telegram_api::Chat> chat = nullptr;
      int32 accessible_before = 0;
      if (chat_invite_ptr->get_id() == telegram_api::chatInviteAlready::ID) {
        auto chat_invite_already = move_tl_object_as<telegram_api::chatInviteAlready>(chat_invite_ptr);
        chat = std::move(chat_invite_already->chat_);
      } else {
        auto chat_invite_peek = move_tl_object_as<telegram_api::chatInvitePeek>(chat_invite_ptr);
        chat = std::move(chat_invite_peek->chat_);
        accessible_before = chat_invite_peek->expires_;
      }
      auto chat_id = get_chat_id(chat);
      if (chat_id != ChatId() && !chat_id.is_valid()) {
        LOG(ERROR) << "Receive invalid " << chat_id;
        chat_id = ChatId();
      }
      auto channel_id = get_channel_id(chat);
      if (channel_id != ChannelId() && !channel_id.is_valid()) {
        LOG(ERROR) << "Receive invalid " << channel_id;
        channel_id = ChannelId();
      }
      if (accessible_before != 0 && (!channel_id.is_valid() || accessible_before < 0)) {
        LOG(ERROR) << "Receive expires = " << accessible_before << " for invite link " << invite_link << " to "
                   << to_string(chat);
        accessible_before = 0;
      }
      on_get_chat(std::move(chat), "chatInviteAlready");

      CHECK(chat_id == ChatId() || channel_id == ChannelId());

      // the access is already expired, reget the info
      if (accessible_before != 0 && accessible_before <= G()->unix_time() + 1) {
        td_->create_handler<CheckChatInviteQuery>(std::move(promise))->send(invite_link);
        return;
      }

      DialogId dialog_id = chat_id.is_valid() ? DialogId(chat_id) : DialogId(channel_id);
      auto &invite_link_info = invite_link_infos_[invite_link];
      if (invite_link_info == nullptr) {
        invite_link_info = make_unique<InviteLinkInfo>();
      }
      invite_link_info->dialog_id = dialog_id;
      if (accessible_before != 0 && dialog_id.is_valid()) {
        auto &access = dialog_access_by_invite_link_[dialog_id];
        access.invite_links.insert(invite_link);
        if (access.accessible_before < accessible_before) {
          access.accessible_before = accessible_before;

          auto expires_in = accessible_before - G()->unix_time() - 1;
          invite_link_info_expire_timeout_.set_timeout_in(dialog_id.get(), expires_in);
        }
      }
      break;
    }
    case telegram_api::chatInvite::ID: {
      auto chat_invite = move_tl_object_as<telegram_api::chatInvite>(chat_invite_ptr);
      vector<UserId> participant_user_ids;
      for (auto &user : chat_invite->participants_) {
        auto user_id = get_user_id(user);
        if (!user_id.is_valid()) {
          LOG(ERROR) << "Receive invalid " << user_id;
          continue;
        }

        on_get_user(std::move(user), "chatInvite");
        participant_user_ids.push_back(user_id);
      }

      auto &invite_link_info = invite_link_infos_[invite_link];
      if (invite_link_info == nullptr) {
        invite_link_info = make_unique<InviteLinkInfo>();
      }
      invite_link_info->dialog_id = DialogId();
      invite_link_info->title = chat_invite->title_;
      invite_link_info->photo = get_photo(td_->file_manager_.get(), std::move(chat_invite->photo_), DialogId());
      invite_link_info->description = std::move(chat_invite->about_);
      invite_link_info->participant_count = chat_invite->participants_count_;
      invite_link_info->participant_user_ids = std::move(participant_user_ids);
      invite_link_info->creates_join_request = std::move(chat_invite->request_needed_);
      invite_link_info->is_chat = (chat_invite->flags_ & CHAT_INVITE_FLAG_IS_CHANNEL) == 0;
      invite_link_info->is_channel = (chat_invite->flags_ & CHAT_INVITE_FLAG_IS_CHANNEL) != 0;

      bool is_broadcast = (chat_invite->flags_ & CHAT_INVITE_FLAG_IS_BROADCAST) != 0;
      bool is_public = (chat_invite->flags_ & CHAT_INVITE_FLAG_IS_PUBLIC) != 0;
      bool is_megagroup = (chat_invite->flags_ & CHAT_INVITE_FLAG_IS_MEGAGROUP) != 0;

      if (!invite_link_info->is_channel) {
        if (is_broadcast || is_public || is_megagroup) {
          LOG(ERROR) << "Receive wrong chat invite: " << to_string(chat_invite);
          is_public = is_megagroup = false;
        }
      } else {
        LOG_IF(ERROR, is_broadcast == is_megagroup) << "Receive wrong chat invite: " << to_string(chat_invite);
      }

      invite_link_info->is_public = is_public;
      invite_link_info->is_megagroup = is_megagroup;
      break;
    }
    default:
      UNREACHABLE();
  }
  promise.set_value(Unit());
}

void ContactsManager::remove_dialog_access_by_invite_link(DialogId dialog_id) {
  auto access_it = dialog_access_by_invite_link_.find(dialog_id);
  if (access_it == dialog_access_by_invite_link_.end()) {
    return;
  }

  for (auto &invite_link : access_it->second.invite_links) {
    invalidate_invite_link_info(invite_link);
  }
  dialog_access_by_invite_link_.erase(access_it);

  invite_link_info_expire_timeout_.cancel_timeout(dialog_id.get());
}

bool ContactsManager::update_permanent_invite_link(DialogInviteLink &invite_link, DialogInviteLink new_invite_link) {
  if (new_invite_link != invite_link) {
    if (invite_link.is_valid() && invite_link.get_invite_link() != new_invite_link.get_invite_link()) {
      // old link was invalidated
      invite_link_infos_.erase(invite_link.get_invite_link());
    }

    invite_link = std::move(new_invite_link);
    return true;
  }
  return false;
}

void ContactsManager::invalidate_invite_link_info(const string &invite_link) {
  LOG(INFO) << "Invalidate info about invite link " << invite_link;
  invite_link_infos_.erase(invite_link);
}

void ContactsManager::repair_chat_participants(ChatId chat_id) {
  send_get_chat_full_query(chat_id, Auto(), "repair_chat_participants");
}

void ContactsManager::on_update_chat_add_user(ChatId chat_id, UserId inviter_user_id, UserId user_id, int32 date,
                                              int32 version) {
  if (!chat_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << chat_id;
    return;
  }
  if (!have_user(user_id)) {
    LOG(ERROR) << "Can't find " << user_id;
    return;
  }
  if (!have_user(inviter_user_id)) {
    LOG(ERROR) << "Can't find " << inviter_user_id;
    return;
  }
  LOG(INFO) << "Receive updateChatParticipantAdd to " << chat_id << " with " << user_id << " invited by "
            << inviter_user_id << " at " << date << " with version " << version;

  ChatFull *chat_full = get_chat_full_force(chat_id, "on_update_chat_add_user");
  if (chat_full == nullptr) {
    LOG(INFO) << "Ignoring update about members of " << chat_id;
    return;
  }
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    LOG(ERROR) << "Receive updateChatParticipantAdd for unknown " << chat_id << ". Couldn't apply it";
    repair_chat_participants(chat_id);
    return;
  }
  if (c->status.is_left()) {
    // possible if updates come out of order
    LOG(WARNING) << "Receive updateChatParticipantAdd for left " << chat_id << ". Couldn't apply it";

    repair_chat_participants(chat_id);  // just in case
    return;
  }
  if (on_update_chat_full_participants_short(chat_full, chat_id, version)) {
    for (auto &participant : chat_full->participants) {
      if (participant.dialog_id_ == DialogId(user_id)) {
        if (participant.inviter_user_id_ != inviter_user_id) {
          LOG(ERROR) << user_id << " was readded to " << chat_id << " by " << inviter_user_id
                     << ", previously invited by " << participant.inviter_user_id_;
          participant.inviter_user_id_ = inviter_user_id;
          participant.joined_date_ = date;
          repair_chat_participants(chat_id);
        } else {
          // Possible if update comes twice
          LOG(INFO) << user_id << " was readded to " << chat_id;
        }
        return;
      }
    }
    chat_full->participants.push_back(DialogParticipant{DialogId(user_id), inviter_user_id, date,
                                                        user_id == chat_full->creator_user_id
                                                            ? DialogParticipantStatus::Creator(true, false, string())
                                                            : DialogParticipantStatus::Member()});
    update_chat_online_member_count(chat_full, chat_id, false);
    chat_full->is_changed = true;
    update_chat_full(chat_full, chat_id, "on_update_chat_add_user");

    // Chat is already updated
    if (chat_full->version == c->version &&
        narrow_cast<int32>(chat_full->participants.size()) != c->participant_count) {
      LOG(ERROR) << "Number of members in " << chat_id << " with version " << c->version << " is "
                 << c->participant_count << " but there are " << chat_full->participants.size()
                 << " members in the ChatFull";
      repair_chat_participants(chat_id);
    }
  }
}

void ContactsManager::on_update_chat_edit_administrator(ChatId chat_id, UserId user_id, bool is_administrator,
                                                        int32 version) {
  if (!chat_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << chat_id;
    return;
  }
  if (!have_user(user_id)) {
    LOG(ERROR) << "Can't find " << user_id;
    return;
  }
  LOG(INFO) << "Receive updateChatParticipantAdmin in " << chat_id << " with " << user_id << ", administrator rights "
            << (is_administrator ? "enabled" : "disabled") << " with version " << version;

  auto c = get_chat_force(chat_id);
  if (c == nullptr) {
    LOG(INFO) << "Ignoring update about members of unknown " << chat_id;
    return;
  }

  if (c->status.is_left()) {
    // possible if updates come out of order
    LOG(WARNING) << "Receive updateChatParticipantAdmin for left " << chat_id << ". Couldn't apply it";

    repair_chat_participants(chat_id);  // just in case
    return;
  }
  if (version <= -1) {
    LOG(ERROR) << "Receive wrong version " << version << " for " << chat_id;
    return;
  }
  CHECK(c->version >= 0);

  auto status = is_administrator ? DialogParticipantStatus::GroupAdministrator(c->status.is_creator())
                                 : DialogParticipantStatus::Member();
  if (version > c->version) {
    if (version != c->version + 1) {
      LOG(INFO) << "Administrators of " << chat_id << " with version " << c->version
                << " has changed, but new version is " << version;
      repair_chat_participants(chat_id);
      return;
    }

    c->version = version;
    c->need_save_to_database = true;
    if (user_id == get_my_id() && !c->status.is_creator()) {
      // if chat with version was already received, then the update is already processed
      // so we need to call on_update_chat_status only if version > c->version
      on_update_chat_status(c, chat_id, status);
    }
    update_chat(c, chat_id);
  }

  ChatFull *chat_full = get_chat_full_force(chat_id, "on_update_chat_edit_administrator");
  if (chat_full != nullptr) {
    if (chat_full->version + 1 == version) {
      for (auto &participant : chat_full->participants) {
        if (participant.dialog_id_ == DialogId(user_id)) {
          participant.status_ = std::move(status);
          chat_full->is_changed = true;
          update_chat_full(chat_full, chat_id, "on_update_chat_edit_administrator");
          return;
        }
      }
    }

    // can't find chat member or version have increased too much
    repair_chat_participants(chat_id);
  }
}

void ContactsManager::on_update_chat_delete_user(ChatId chat_id, UserId user_id, int32 version) {
  if (!chat_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << chat_id;
    return;
  }
  if (!have_user(user_id)) {
    LOG(ERROR) << "Can't find " << user_id;
    return;
  }
  LOG(INFO) << "Receive updateChatParticipantDelete from " << chat_id << " with " << user_id << " and version "
            << version;

  ChatFull *chat_full = get_chat_full_force(chat_id, "on_update_chat_delete_user");
  if (chat_full == nullptr) {
    LOG(INFO) << "Ignoring update about members of " << chat_id;
    return;
  }
  const Chat *c = get_chat_force(chat_id);
  if (c == nullptr) {
    LOG(ERROR) << "Receive updateChatParticipantDelete for unknown " << chat_id;
    repair_chat_participants(chat_id);
    return;
  }
  if (user_id == get_my_id()) {
    LOG_IF(WARNING, c->status.is_member()) << "User was removed from " << chat_id
                                           << " but it is not left the group. Possible if updates comes out of order";
    return;
  }
  if (c->status.is_left()) {
    // possible if updates come out of order
    LOG(INFO) << "Receive updateChatParticipantDelete for left " << chat_id;

    repair_chat_participants(chat_id);
    return;
  }
  if (on_update_chat_full_participants_short(chat_full, chat_id, version)) {
    for (size_t i = 0; i < chat_full->participants.size(); i++) {
      if (chat_full->participants[i].dialog_id_ == DialogId(user_id)) {
        chat_full->participants[i] = chat_full->participants.back();
        chat_full->participants.resize(chat_full->participants.size() - 1);
        chat_full->is_changed = true;
        update_chat_online_member_count(chat_full, chat_id, false);
        update_chat_full(chat_full, chat_id, "on_update_chat_delete_user");

        if (static_cast<int32>(chat_full->participants.size()) != c->participant_count) {
          repair_chat_participants(chat_id);
        }
        return;
      }
    }
    LOG(ERROR) << "Can't find basic group member " << user_id << " in " << chat_id << " to be removed";
    repair_chat_participants(chat_id);
  }
}

void ContactsManager::on_update_chat_status(Chat *c, ChatId chat_id, DialogParticipantStatus status) {
  if (c->status != status) {
    LOG(INFO) << "Update " << chat_id << " status from " << c->status << " to " << status;
    bool need_reload_group_call = c->status.can_manage_calls() != status.can_manage_calls();
    bool need_drop_invite_link = c->status.can_manage_invite_links() && !status.can_manage_invite_links();

    c->status = std::move(status);
    c->is_status_changed = true;

    if (c->status.is_left()) {
      c->participant_count = 0;
      c->version = -1;
      c->default_permissions_version = -1;
      c->pinned_message_version = -1;

      drop_chat_full(chat_id);
    } else if (need_drop_invite_link) {
      ChatFull *chat_full = get_chat_full_force(chat_id, "on_update_chat_status");
      if (chat_full != nullptr) {
        on_update_chat_full_invite_link(chat_full, nullptr);
        update_chat_full(chat_full, chat_id, "on_update_chat_status");
      }
    }
    if (need_reload_group_call) {
      send_closure_later(G()->messages_manager(), &MessagesManager::on_update_dialog_group_call_rights,
                         DialogId(chat_id));
    }

    c->is_changed = true;
  }
}

void ContactsManager::on_update_chat_default_permissions(ChatId chat_id, RestrictedRights default_permissions,
                                                         int32 version) {
  if (!chat_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << chat_id;
    return;
  }
  auto c = get_chat_force(chat_id);
  if (c == nullptr) {
    LOG(INFO) << "Ignoring update about unknown " << chat_id;
    return;
  }

  LOG(INFO) << "Receive updateChatDefaultBannedRights in " << chat_id << " with " << default_permissions
            << " and version " << version << ". Current version is " << c->version;

  if (c->status.is_left()) {
    // possible if updates come out of order
    LOG(WARNING) << "Receive updateChatDefaultBannedRights for left " << chat_id << ". Couldn't apply it";

    repair_chat_participants(chat_id);  // just in case
    return;
  }
  if (version <= -1) {
    LOG(ERROR) << "Receive wrong version " << version << " for " << chat_id;
    return;
  }
  CHECK(c->version >= 0);

  if (version > c->version) {
    // this should be unreachable, because version and default permissions must be already updated from
    // the chat object in on_chat_update
    if (version != c->version + 1) {
      LOG(INFO) << "Default permissions of " << chat_id << " with version " << c->version
                << " has changed, but new version is " << version;
      repair_chat_participants(chat_id);
      return;
    }

    LOG_IF(ERROR, default_permissions == c->default_permissions)
        << "Receive updateChatDefaultBannedRights in " << chat_id << " with version " << version
        << " and default_permissions = " << default_permissions
        << ", but default_permissions are not changed. Current version is " << c->version;
    c->version = version;
    c->need_save_to_database = true;
    on_update_chat_default_permissions(c, chat_id, default_permissions, version);
    update_chat(c, chat_id);
  }
}

void ContactsManager::on_update_chat_default_permissions(Chat *c, ChatId chat_id, RestrictedRights default_permissions,
                                                         int32 version) {
  if (c->default_permissions != default_permissions && version >= c->default_permissions_version) {
    LOG(INFO) << "Update " << chat_id << " default permissions from " << c->default_permissions << " to "
              << default_permissions << " and version from " << c->default_permissions_version << " to " << version;
    c->default_permissions = default_permissions;
    c->default_permissions_version = version;
    c->is_default_permissions_changed = true;
    c->need_save_to_database = true;
  }
}

void ContactsManager::on_update_chat_noforwards(Chat *c, ChatId chat_id, bool noforwards) {
  if (c->noforwards != noforwards) {
    LOG(INFO) << "Update " << chat_id << " has_protected_content from " << c->noforwards << " to " << noforwards;
    c->noforwards = noforwards;
    c->is_noforwards_changed = true;
    c->need_save_to_database = true;
  }
}

void ContactsManager::on_update_chat_pinned_message(ChatId chat_id, MessageId pinned_message_id, int32 version) {
  if (!chat_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << chat_id;
    return;
  }
  auto c = get_chat_force(chat_id);
  if (c == nullptr) {
    LOG(INFO) << "Ignoring update about unknown " << chat_id;
    return;
  }

  LOG(INFO) << "Receive updateChatPinnedMessage in " << chat_id << " with " << pinned_message_id << " and version "
            << version << ". Current version is " << c->version << "/" << c->pinned_message_version;

  if (c->status.is_left()) {
    // possible if updates come out of order
    repair_chat_participants(chat_id);  // just in case
    return;
  }
  if (version <= -1) {
    LOG(ERROR) << "Receive wrong version " << version << " for " << chat_id;
    return;
  }
  CHECK(c->version >= 0);

  if (version >= c->pinned_message_version) {
    if (version != c->version + 1 && version != c->version) {
      LOG(INFO) << "Pinned message of " << chat_id << " with version " << c->version
                << " has changed, but new version is " << version;
      repair_chat_participants(chat_id);
    } else if (version == c->version + 1) {
      c->version = version;
      c->need_save_to_database = true;
    }
    td_->messages_manager_->on_update_dialog_last_pinned_message_id(DialogId(chat_id), pinned_message_id);
    if (version > c->pinned_message_version) {
      LOG(INFO) << "Change pinned message version of " << chat_id << " from " << c->pinned_message_version << " to "
                << version;
      c->pinned_message_version = version;
      c->need_save_to_database = true;
    }
    update_chat(c, chat_id);
  }
}

void ContactsManager::on_update_chat_participant_count(Chat *c, ChatId chat_id, int32 participant_count, int32 version,
                                                       const string &debug_str) {
  if (version <= -1) {
    LOG(ERROR) << "Receive wrong version " << version << " in " << chat_id << debug_str;
    return;
  }

  if (version < c->version) {
    // some outdated data
    LOG(INFO) << "Receive number of members in " << chat_id << " with version " << version << debug_str
              << ", but current version is " << c->version;
    return;
  }

  if (c->participant_count != participant_count) {
    if (version == c->version && participant_count != 0) {
      // version is not changed when deleted user is removed from the chat
      LOG_IF(ERROR, c->participant_count != participant_count + 1)
          << "Number of members in " << chat_id << " has changed from " << c->participant_count << " to "
          << participant_count << ", but version " << c->version << " remains unchanged" << debug_str;
      repair_chat_participants(chat_id);
    }

    c->participant_count = participant_count;
    c->version = version;
    c->is_changed = true;
    return;
  }

  if (version > c->version) {
    c->version = version;
    c->need_save_to_database = true;
  }
}

void ContactsManager::on_update_chat_photo(Chat *c, ChatId chat_id,
                                           tl_object_ptr<telegram_api::ChatPhoto> &&chat_photo_ptr) {
  on_update_chat_photo(
      c, chat_id, get_dialog_photo(td_->file_manager_.get(), DialogId(chat_id), 0, std::move(chat_photo_ptr)), true);
}

void ContactsManager::on_update_chat_photo(Chat *c, ChatId chat_id, DialogPhoto &&photo, bool invalidate_photo_cache) {
  if (td_->auth_manager_->is_bot()) {
    photo.minithumbnail.clear();
  }

  if (need_update_dialog_photo(c->photo, photo)) {
    c->photo = std::move(photo);
    c->is_photo_changed = true;
    c->need_save_to_database = true;

    if (invalidate_photo_cache) {
      auto chat_full = get_chat_full(chat_id);  // must not load ChatFull
      if (chat_full != nullptr) {
        if (!chat_full->photo.is_empty()) {
          chat_full->photo = Photo();
          chat_full->is_changed = true;
        }
        if (c->photo.small_file_id.is_valid()) {
          reload_chat_full(chat_id, Auto());
        }
        update_chat_full(chat_full, chat_id, "on_update_chat_photo");
      }
    }
  }
}

void ContactsManager::on_update_chat_title(Chat *c, ChatId chat_id, string &&title) {
  if (c->title != title) {
    c->title = std::move(title);
    c->is_title_changed = true;
    c->need_save_to_database = true;
  }
}

void ContactsManager::on_update_chat_active(Chat *c, ChatId chat_id, bool is_active) {
  if (c->is_active != is_active) {
    c->is_active = is_active;
    c->is_is_active_changed = true;
    c->is_changed = true;
  }
}

void ContactsManager::on_update_chat_migrated_to_channel_id(Chat *c, ChatId chat_id, ChannelId migrated_to_channel_id) {
  if (c->migrated_to_channel_id != migrated_to_channel_id && migrated_to_channel_id.is_valid()) {
    LOG_IF(ERROR, c->migrated_to_channel_id.is_valid())
        << "Upgraded supergroup ID for " << chat_id << " has changed from " << c->migrated_to_channel_id << " to "
        << migrated_to_channel_id;
    c->migrated_to_channel_id = migrated_to_channel_id;
    c->is_changed = true;
  }
}

void ContactsManager::on_update_chat_description(ChatId chat_id, string &&description) {
  if (!chat_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << chat_id;
    return;
  }

  auto chat_full = get_chat_full_force(chat_id, "on_update_chat_description");
  if (chat_full == nullptr) {
    return;
  }
  if (chat_full->description != description) {
    chat_full->description = std::move(description);
    chat_full->is_changed = true;
    update_chat_full(chat_full, chat_id, "on_update_chat_description");
    td_->group_call_manager_->on_update_dialog_about(DialogId(chat_id), chat_full->description, true);
  }
}

bool ContactsManager::on_update_chat_full_participants_short(ChatFull *chat_full, ChatId chat_id, int32 version) {
  if (version <= -1) {
    LOG(ERROR) << "Receive wrong version " << version << " for " << chat_id;
    return false;
  }
  if (chat_full->version == -1) {
    // chat members are unknown, nothing to update
    return false;
  }

  if (chat_full->version + 1 == version) {
    chat_full->version = version;
    return true;
  }

  LOG(INFO) << "Number of members in " << chat_id << " with version " << chat_full->version
            << " has changed, but new version is " << version;
  repair_chat_participants(chat_id);
  return false;
}

void ContactsManager::on_update_chat_full_participants(ChatFull *chat_full, ChatId chat_id,
                                                       vector<DialogParticipant> participants, int32 version,
                                                       bool from_update) {
  if (version <= -1) {
    LOG(ERROR) << "Receive members with wrong version " << version << " in " << chat_id;
    return;
  }

  if (version < chat_full->version) {
    // some outdated data
    LOG(WARNING) << "Receive members of " << chat_id << " with version " << version << " but current version is "
                 << chat_full->version;
    return;
  }

  if ((chat_full->participants.size() != participants.size() && version == chat_full->version) ||
      (from_update && version != chat_full->version + 1)) {
    LOG(INFO) << "Members of " << chat_id << " has changed";
    // this is possible in very rare situations
    repair_chat_participants(chat_id);
  }

  chat_full->participants = std::move(participants);
  chat_full->version = version;
  chat_full->is_changed = true;
  update_chat_online_member_count(chat_full, chat_id, true);
}

void ContactsManager::drop_chat_full(ChatId chat_id) {
  ChatFull *chat_full = get_chat_full_force(chat_id, "drop_chat_full");
  if (chat_full == nullptr) {
    return;
  }

  LOG(INFO) << "Drop basicGroupFullInfo of " << chat_id;
  on_update_chat_full_photo(chat_full, chat_id, Photo());
  // chat_full->creator_user_id = UserId();
  chat_full->participants.clear();
  chat_full->bot_commands.clear();
  chat_full->version = -1;
  on_update_chat_full_invite_link(chat_full, nullptr);
  update_chat_online_member_count(chat_full, chat_id, true);
  chat_full->is_changed = true;
  update_chat_full(chat_full, chat_id, "drop_chat_full");
}

void ContactsManager::on_update_channel_photo(Channel *c, ChannelId channel_id,
                                              tl_object_ptr<telegram_api::ChatPhoto> &&chat_photo_ptr) {
  on_update_channel_photo(
      c, channel_id,
      get_dialog_photo(td_->file_manager_.get(), DialogId(channel_id), c->access_hash, std::move(chat_photo_ptr)),
      true);
}

void ContactsManager::on_update_channel_photo(Channel *c, ChannelId channel_id, DialogPhoto &&photo,
                                              bool invalidate_photo_cache) {
  if (td_->auth_manager_->is_bot()) {
    photo.minithumbnail.clear();
  }

  if (need_update_dialog_photo(c->photo, photo)) {
    c->photo = std::move(photo);
    c->is_photo_changed = true;
    c->need_save_to_database = true;

    if (invalidate_photo_cache) {
      auto channel_full = get_channel_full(channel_id, true, "on_update_channel_photo");  // must not load ChannelFull
      if (channel_full != nullptr) {
        if (!channel_full->photo.is_empty()) {
          channel_full->photo = Photo();
          channel_full->is_changed = true;
        }
        if (c->photo.small_file_id.is_valid()) {
          if (channel_full->expires_at > 0.0) {
            channel_full->expires_at = 0.0;
            channel_full->need_save_to_database = true;
          }
          reload_channel_full(channel_id, Auto(), "on_update_channel_photo");
        }
        update_channel_full(channel_full, channel_id, "on_update_channel_photo");
      }
    }
  }
}

void ContactsManager::on_update_channel_title(Channel *c, ChannelId channel_id, string &&title) {
  if (c->title != title) {
    c->title = std::move(title);
    c->is_title_changed = true;
    c->need_save_to_database = true;
  }
}

void ContactsManager::on_update_channel_status(Channel *c, ChannelId channel_id, DialogParticipantStatus &&status) {
  if (c->status != status) {
    LOG(INFO) << "Update " << channel_id << " status from " << c->status << " to " << status;
    if (c->is_update_supergroup_sent) {
      on_channel_status_changed(c, channel_id, c->status, status);
    }
    c->status = status;
    c->is_status_changed = true;
    c->is_changed = true;
  }
}

void ContactsManager::on_channel_status_changed(Channel *c, ChannelId channel_id,
                                                const DialogParticipantStatus &old_status,
                                                const DialogParticipantStatus &new_status) {
  CHECK(c->is_update_supergroup_sent);
  bool have_channel_full = get_channel_full(channel_id) != nullptr;

  bool need_reload_group_call = old_status.can_manage_calls() != new_status.can_manage_calls();
  if (old_status.can_manage_invite_links() && !new_status.can_manage_invite_links()) {
    auto channel_full = get_channel_full(channel_id, true, "on_channel_status_changed");
    if (channel_full != nullptr) {  // otherwise invite_link will be dropped when the channel is loaded
      on_update_channel_full_invite_link(channel_full, nullptr);
      do_invalidate_channel_full(channel_full, channel_id, !c->is_slow_mode_enabled);
      update_channel_full(channel_full, channel_id, "on_channel_status_changed");
    }
  } else {
    invalidate_channel_full(channel_id, !c->is_slow_mode_enabled);
  }

  if (old_status.is_creator() != new_status.is_creator()) {
    c->is_creator_changed = true;

    send_get_channel_full_query(nullptr, channel_id, Auto(), "update channel owner");
    reload_dialog_administrators(DialogId(channel_id), {}, Auto());
    remove_dialog_suggested_action(SuggestedAction{SuggestedAction::Type::ConvertToGigagroup, DialogId(channel_id)});
  }

  if (old_status.is_member() != new_status.is_member() || new_status.is_banned()) {
    remove_dialog_access_by_invite_link(DialogId(channel_id));

    if (new_status.is_member() || new_status.is_creator()) {
      reload_channel_full(channel_id,
                          PromiseCreator::lambda([channel_id](Unit) { LOG(INFO) << "Reloaded full " << channel_id; }),
                          "on_channel_status_changed");
    }
  }
  if (need_reload_group_call) {
    send_closure_later(G()->messages_manager(), &MessagesManager::on_update_dialog_group_call_rights,
                       DialogId(channel_id));
  }
  if (td_->auth_manager_->is_bot() && old_status.is_administrator() && !new_status.is_administrator()) {
    channel_participants_.erase(channel_id);
  }
  if (td_->auth_manager_->is_bot() && old_status.is_member() && !new_status.is_member() &&
      !G()->parameters().use_message_db) {
    send_closure_later(G()->messages_manager(), &MessagesManager::on_dialog_deleted, DialogId(channel_id),
                       Promise<Unit>());
  }

  // must not load ChannelFull, because must not change the Channel
  CHECK(have_channel_full == (get_channel_full(channel_id) != nullptr));
}

void ContactsManager::on_update_channel_default_permissions(Channel *c, ChannelId channel_id,
                                                            RestrictedRights default_permissions) {
  if (c->default_permissions != default_permissions) {
    LOG(INFO) << "Update " << channel_id << " default permissions from " << c->default_permissions << " to "
              << default_permissions;
    c->default_permissions = default_permissions;
    c->is_default_permissions_changed = true;
    c->need_save_to_database = true;
  }
}

void ContactsManager::on_update_channel_has_location(Channel *c, ChannelId channel_id, bool has_location) {
  if (c->has_location != has_location) {
    LOG(INFO) << "Update " << channel_id << " has_location from " << c->has_location << " to " << has_location;
    c->has_location = has_location;
    c->is_has_location_changed = true;
    c->is_changed = true;
  }
}

void ContactsManager::on_update_channel_noforwards(Channel *c, ChannelId channel_id, bool noforwards) {
  if (c->noforwards != noforwards) {
    LOG(INFO) << "Update " << channel_id << " has_protected_content from " << c->noforwards << " to " << noforwards;
    c->noforwards = noforwards;
    c->is_noforwards_changed = true;
    c->need_save_to_database = true;
  }
}

void ContactsManager::on_update_channel_username(ChannelId channel_id, string &&username) {
  if (!channel_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << channel_id;
    return;
  }

  Channel *c = get_channel_force(channel_id);
  if (c != nullptr) {
    on_update_channel_username(c, channel_id, std::move(username));
    update_channel(c, channel_id);
  } else {
    LOG(INFO) << "Ignore update channel username about unknown " << channel_id;
  }
}

void ContactsManager::on_update_channel_username(Channel *c, ChannelId channel_id, string &&username) {
  td_->messages_manager_->on_dialog_username_updated(DialogId(channel_id), c->username, username);
  if (c->username != username) {
    if (c->is_update_supergroup_sent) {
      on_channel_username_changed(c, channel_id, c->username, username);
    }

    c->username = std::move(username);
    c->is_username_changed = true;
    c->is_changed = true;
  }
}

void ContactsManager::on_channel_username_changed(const Channel *c, ChannelId channel_id, const string &old_username,
                                                  const string &new_username) {
  bool have_channel_full = get_channel_full(channel_id) != nullptr;
  if (old_username.empty() || new_username.empty()) {
    // moving channel from private to public can change availability of chat members
    invalidate_channel_full(channel_id, !c->is_slow_mode_enabled);
  }

  // must not load ChannelFull, because must not change the Channel
  CHECK(have_channel_full == (get_channel_full(channel_id) != nullptr));
}

void ContactsManager::on_update_channel_description(ChannelId channel_id, string &&description) {
  CHECK(channel_id.is_valid());
  auto channel_full = get_channel_full_force(channel_id, true, "on_update_channel_description");
  if (channel_full == nullptr) {
    return;
  }
  if (channel_full->description != description) {
    channel_full->description = std::move(description);
    channel_full->is_changed = true;
    update_channel_full(channel_full, channel_id, "on_update_channel_description");
    td_->group_call_manager_->on_update_dialog_about(DialogId(channel_id), channel_full->description, true);
  }
}

void ContactsManager::on_update_channel_sticker_set(ChannelId channel_id, StickerSetId sticker_set_id) {
  CHECK(channel_id.is_valid());
  auto channel_full = get_channel_full_force(channel_id, true, "on_update_channel_sticker_set");
  if (channel_full == nullptr) {
    return;
  }
  if (channel_full->sticker_set_id != sticker_set_id) {
    channel_full->sticker_set_id = sticker_set_id;
    channel_full->is_changed = true;
    update_channel_full(channel_full, channel_id, "on_update_channel_sticker_set");
  }
}

void ContactsManager::on_update_channel_linked_channel_id(ChannelId channel_id, ChannelId group_channel_id) {
  if (channel_id.is_valid()) {
    auto channel_full = get_channel_full_force(channel_id, true, "on_update_channel_linked_channel_id 1");
    on_update_channel_full_linked_channel_id(channel_full, channel_id, group_channel_id);
    if (channel_full != nullptr) {
      update_channel_full(channel_full, channel_id, "on_update_channel_linked_channel_id 3");
    }
  }
  if (group_channel_id.is_valid()) {
    auto channel_full = get_channel_full_force(group_channel_id, true, "on_update_channel_linked_channel_id 2");
    on_update_channel_full_linked_channel_id(channel_full, group_channel_id, channel_id);
    if (channel_full != nullptr) {
      update_channel_full(channel_full, group_channel_id, "on_update_channel_linked_channel_id 4");
    }
  }
}

void ContactsManager::on_update_channel_location(ChannelId channel_id, const DialogLocation &location) {
  auto channel_full = get_channel_full_force(channel_id, true, "on_update_channel_location");
  if (channel_full != nullptr) {
    on_update_channel_full_location(channel_full, channel_id, location);
    update_channel_full(channel_full, channel_id, "on_update_channel_location");
  }
}

void ContactsManager::on_update_channel_slow_mode_delay(ChannelId channel_id, int32 slow_mode_delay,
                                                        Promise<Unit> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  auto channel_full = get_channel_full_force(channel_id, true, "on_update_channel_slow_mode_delay");
  if (channel_full != nullptr) {
    on_update_channel_full_slow_mode_delay(channel_full, channel_id, slow_mode_delay, 0);
    update_channel_full(channel_full, channel_id, "on_update_channel_slow_mode_delay");
  }
  promise.set_value(Unit());
}

void ContactsManager::on_update_channel_slow_mode_next_send_date(ChannelId channel_id, int32 slow_mode_next_send_date) {
  auto channel_full = get_channel_full_force(channel_id, true, "on_update_channel_slow_mode_next_send_date");
  if (channel_full != nullptr) {
    on_update_channel_full_slow_mode_next_send_date(channel_full, slow_mode_next_send_date);
    update_channel_full(channel_full, channel_id, "on_update_channel_slow_mode_next_send_date");
  }
}

void ContactsManager::on_update_channel_bot_user_ids(ChannelId channel_id, vector<UserId> &&bot_user_ids) {
  CHECK(channel_id.is_valid());
  if (!have_channel(channel_id)) {
    LOG(ERROR) << channel_id << " not found";
    return;
  }

  auto channel_full = get_channel_full_force(channel_id, true, "on_update_channel_bot_user_ids");
  if (channel_full == nullptr) {
    send_closure_later(G()->messages_manager(), &MessagesManager::on_dialog_bots_updated, DialogId(channel_id),
                       std::move(bot_user_ids), false);
    return;
  }
  on_update_channel_full_bot_user_ids(channel_full, channel_id, std::move(bot_user_ids));
  update_channel_full(channel_full, channel_id, "on_update_channel_bot_user_ids");
}

void ContactsManager::on_update_channel_full_bot_user_ids(ChannelFull *channel_full, ChannelId channel_id,
                                                          vector<UserId> &&bot_user_ids) {
  CHECK(channel_full != nullptr);
  if (channel_full->bot_user_ids != bot_user_ids) {
    send_closure_later(G()->messages_manager(), &MessagesManager::on_dialog_bots_updated, DialogId(channel_id),
                       bot_user_ids, false);
    channel_full->bot_user_ids = std::move(bot_user_ids);
    channel_full->need_save_to_database = true;
  }
}

void ContactsManager::on_update_channel_is_all_history_available(ChannelId channel_id, bool is_all_history_available,
                                                                 Promise<Unit> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());
  CHECK(channel_id.is_valid());
  auto channel_full = get_channel_full_force(channel_id, true, "on_update_channel_is_all_history_available");
  if (channel_full != nullptr && channel_full->is_all_history_available != is_all_history_available) {
    channel_full->is_all_history_available = is_all_history_available;
    channel_full->is_changed = true;
    update_channel_full(channel_full, channel_id, "on_update_channel_is_all_history_available");
  }
  promise.set_value(Unit());
}

void ContactsManager::on_update_channel_default_permissions(ChannelId channel_id,
                                                            RestrictedRights default_permissions) {
  if (!channel_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << channel_id;
    return;
  }

  Channel *c = get_channel_force(channel_id);
  if (c != nullptr) {
    on_update_channel_default_permissions(c, channel_id, std::move(default_permissions));
    update_channel(c, channel_id);
  } else {
    LOG(INFO) << "Ignore update channel default permissions about unknown " << channel_id;
  }
}

void ContactsManager::send_update_chat_member(DialogId dialog_id, UserId agent_user_id, int32 date,
                                              const DialogInviteLink &invite_link,
                                              const DialogParticipant &old_dialog_participant,
                                              const DialogParticipant &new_dialog_participant) {
  CHECK(td_->auth_manager_->is_bot());
  td_->messages_manager_->force_create_dialog(dialog_id, "send_update_chat_member", true);
  send_closure(G()->td(), &Td::send_update,
               td_api::make_object<td_api::updateChatMember>(
                   dialog_id.get(), get_user_id_object(agent_user_id, "send_update_chat_member"), date,
                   invite_link.get_chat_invite_link_object(this), get_chat_member_object(old_dialog_participant),
                   get_chat_member_object(new_dialog_participant)));
}

void ContactsManager::on_update_bot_stopped(UserId user_id, int32 date, bool is_stopped) {
  if (!td_->auth_manager_->is_bot()) {
    LOG(ERROR) << "Receive updateBotStopped by non-bot";
    return;
  }
  if (date <= 0 || !have_user_force(user_id)) {
    LOG(ERROR) << "Receive invalid updateBotStopped by " << user_id << " at " << date;
    return;
  }

  DialogParticipant old_dialog_participant(DialogId(get_my_id()), user_id, date, DialogParticipantStatus::Banned(0));
  DialogParticipant new_dialog_participant(DialogId(get_my_id()), user_id, date, DialogParticipantStatus::Member());
  if (is_stopped) {
    std::swap(old_dialog_participant.status_, new_dialog_participant.status_);
  }

  send_update_chat_member(DialogId(user_id), user_id, date, DialogInviteLink(), old_dialog_participant,
                          new_dialog_participant);
}

void ContactsManager::on_update_chat_participant(ChatId chat_id, UserId user_id, int32 date,
                                                 DialogInviteLink invite_link,
                                                 tl_object_ptr<telegram_api::ChatParticipant> old_participant,
                                                 tl_object_ptr<telegram_api::ChatParticipant> new_participant) {
  if (!td_->auth_manager_->is_bot()) {
    LOG(ERROR) << "Receive updateChatParticipant by non-bot";
    return;
  }
  if (!chat_id.is_valid() || !user_id.is_valid() || date <= 0 ||
      (old_participant == nullptr && new_participant == nullptr)) {
    LOG(ERROR) << "Receive invalid updateChatParticipant in " << chat_id << " by " << user_id << " at " << date << ": "
               << to_string(old_participant) << " -> " << to_string(new_participant);
    return;
  }

  const Chat *c = get_chat(chat_id);
  if (c == nullptr) {
    LOG(ERROR) << "Receive updateChatParticipant in unknown " << chat_id;
    return;
  }

  DialogParticipant old_dialog_participant;
  DialogParticipant new_dialog_participant;
  if (old_participant != nullptr) {
    old_dialog_participant = DialogParticipant(std::move(old_participant), c->date, c->status.is_creator());
    if (new_participant == nullptr) {
      new_dialog_participant = DialogParticipant::left(old_dialog_participant.dialog_id_);
    } else {
      new_dialog_participant = DialogParticipant(std::move(new_participant), c->date, c->status.is_creator());
    }
  } else {
    new_dialog_participant = DialogParticipant(std::move(new_participant), c->date, c->status.is_creator());
    old_dialog_participant = DialogParticipant::left(new_dialog_participant.dialog_id_);
  }
  if (old_dialog_participant.dialog_id_ != new_dialog_participant.dialog_id_ || !old_dialog_participant.is_valid() ||
      !new_dialog_participant.is_valid()) {
    LOG(ERROR) << "Receive wrong updateChatParticipant: " << old_dialog_participant << " -> " << new_dialog_participant;
    return;
  }
  if (new_dialog_participant.dialog_id_ == DialogId(get_my_id()) &&
      new_dialog_participant.status_ != get_chat_status(chat_id) && false) {
    LOG(ERROR) << "Have status " << get_chat_status(chat_id) << " after receiving updateChatParticipant in " << chat_id
               << " by " << user_id << " at " << date << " from " << old_dialog_participant << " to "
               << new_dialog_participant;
  }

  send_update_chat_member(DialogId(chat_id), user_id, date, invite_link, old_dialog_participant,
                          new_dialog_participant);
}

void ContactsManager::on_update_channel_participant(ChannelId channel_id, UserId user_id, int32 date,
                                                    DialogInviteLink invite_link,
                                                    tl_object_ptr<telegram_api::ChannelParticipant> old_participant,
                                                    tl_object_ptr<telegram_api::ChannelParticipant> new_participant) {
  if (!td_->auth_manager_->is_bot()) {
    LOG(ERROR) << "Receive updateChannelParticipant by non-bot";
    return;
  }
  if (!channel_id.is_valid() || !user_id.is_valid() || date <= 0 ||
      (old_participant == nullptr && new_participant == nullptr)) {
    LOG(ERROR) << "Receive invalid updateChannelParticipant in " << channel_id << " by " << user_id << " at " << date
               << ": " << to_string(old_participant) << " -> " << to_string(new_participant);
    return;
  }

  DialogParticipant old_dialog_participant;
  DialogParticipant new_dialog_participant;
  auto channel_type = get_channel_type(channel_id);
  if (old_participant != nullptr) {
    old_dialog_participant = DialogParticipant(std::move(old_participant), channel_type);
    if (new_participant == nullptr) {
      new_dialog_participant = DialogParticipant::left(old_dialog_participant.dialog_id_);
    } else {
      new_dialog_participant = DialogParticipant(std::move(new_participant), channel_type);
    }
  } else {
    new_dialog_participant = DialogParticipant(std::move(new_participant), channel_type);
    old_dialog_participant = DialogParticipant::left(new_dialog_participant.dialog_id_);
  }
  if (old_dialog_participant.dialog_id_ != new_dialog_participant.dialog_id_ || !old_dialog_participant.is_valid() ||
      !new_dialog_participant.is_valid()) {
    LOG(ERROR) << "Receive wrong updateChannelParticipant: " << old_dialog_participant << " -> "
               << new_dialog_participant;
    return;
  }
  if (new_dialog_participant.status_.is_administrator() && user_id == get_my_id() &&
      !new_dialog_participant.status_.can_be_edited()) {
    LOG(ERROR) << "Fix wrong can_be_edited in " << new_dialog_participant << " from " << channel_id << " changed from "
               << old_dialog_participant;
    new_dialog_participant.status_.toggle_can_be_edited();
  }

  if (old_dialog_participant.dialog_id_ == DialogId(get_my_id()) && old_dialog_participant.status_.is_administrator() &&
      !new_dialog_participant.status_.is_administrator()) {
    channel_participants_.erase(channel_id);
  } else if (have_channel_participant_cache(channel_id)) {
    add_channel_participant_to_cache(channel_id, new_dialog_participant, true);
  }
  if (new_dialog_participant.dialog_id_ == DialogId(get_my_id()) &&
      new_dialog_participant.status_ != get_channel_status(channel_id) && false) {
    LOG(ERROR) << "Have status " << get_channel_status(channel_id) << " after receiving updateChannelParticipant in "
               << channel_id << " by " << user_id << " at " << date << " from " << old_dialog_participant << " to "
               << new_dialog_participant;
  }

  send_update_chat_member(DialogId(channel_id), user_id, date, invite_link, old_dialog_participant,
                          new_dialog_participant);
}

void ContactsManager::on_update_chat_invite_requester(DialogId dialog_id, UserId user_id, string about, int32 date,
                                                      DialogInviteLink invite_link) {
  if (!td_->auth_manager_->is_bot() || date <= 0 || !have_user_force(user_id) ||
      !td_->messages_manager_->have_dialog_info_force(dialog_id)) {
    LOG(ERROR) << "Receive invalid updateBotChatInviteRequester by " << user_id << " in " << dialog_id << " at "
               << date;
    return;
  }
  td_->messages_manager_->force_create_dialog(dialog_id, "on_update_chat_invite_requester", true);

  send_closure(G()->td(), &Td::send_update,
               td_api::make_object<td_api::updateNewChatJoinRequest>(
                   dialog_id.get(),
                   td_api::make_object<td_api::chatJoinRequest>(
                       get_user_id_object(user_id, "on_update_chat_invite_requester"), date, about),
                   invite_link.get_chat_invite_link_object(this)));
}

void ContactsManager::update_contacts_hints(const User *u, UserId user_id, bool from_database) {
  bool is_contact = is_user_contact(u, user_id, false);
  if (td_->auth_manager_->is_bot()) {
    LOG_IF(ERROR, is_contact) << "Bot has " << user_id << " in the contacts list";
    return;
  }

  int64 key = user_id.get();
  string old_value = contacts_hints_.key_to_string(key);
  string new_value = is_contact ? (PSTRING() << u->first_name << ' ' << u->last_name << ' ' << u->username) : string();

  if (new_value != old_value) {
    if (is_contact) {
      contacts_hints_.add(key, new_value);
    } else {
      contacts_hints_.remove(key);
    }
  }

  if (G()->parameters().use_chat_info_db) {
    // update contacts database
    if (!are_contacts_loaded_) {
      if (!from_database && load_contacts_queries_.empty() && is_contact && u->is_is_contact_changed) {
        search_contacts("", std::numeric_limits<int32>::max(), Auto());
      }
    } else {
      if (old_value.empty() == is_contact) {
        save_contacts_to_database();
      }
    }
  }
}

bool ContactsManager::have_user(UserId user_id) const {
  auto u = get_user(user_id);
  return u != nullptr && u->is_received;
}

bool ContactsManager::have_min_user(UserId user_id) const {
  return users_.count(user_id) > 0;
}

bool ContactsManager::is_user_premium(UserId user_id) const {
  auto u = get_user(user_id);
  return u != nullptr && u->is_premium;
}

bool ContactsManager::is_user_deleted(UserId user_id) const {
  auto u = get_user(user_id);
  return u == nullptr || u->is_deleted;
}

bool ContactsManager::is_user_support(UserId user_id) const {
  auto u = get_user(user_id);
  return u != nullptr && !u->is_deleted && u->is_support;
}

bool ContactsManager::is_user_bot(UserId user_id) const {
  auto u = get_user(user_id);
  return u != nullptr && !u->is_deleted && u->is_bot;
}

Result<ContactsManager::BotData> ContactsManager::get_bot_data(UserId user_id) const {
  auto u = get_user(user_id);
  if (u == nullptr) {
    return Status::Error(400, "Bot not found");
  }
  if (!u->is_bot) {
    return Status::Error(400, "User is not a bot");
  }
  if (u->is_deleted) {
    return Status::Error(400, "Bot is deleted");
  }
  if (!u->is_received) {
    return Status::Error(400, "Bot is inaccessible");
  }

  BotData bot_data;
  bot_data.username = u->username;
  bot_data.can_join_groups = u->can_join_groups;
  bot_data.can_read_all_group_messages = u->can_read_all_group_messages;
  bot_data.is_inline = u->is_inline_bot;
  bot_data.need_location = u->need_location_bot;
  bot_data.can_be_added_to_attach_menu = u->can_be_added_to_attach_menu;
  return bot_data;
}

bool ContactsManager::is_user_online(UserId user_id, int32 tolerance) const {
  int32 was_online = get_user_was_online(get_user(user_id), user_id);
  return was_online > G()->unix_time() - tolerance;
}

bool ContactsManager::is_user_status_exact(UserId user_id) const {
  auto u = get_user(user_id);
  return u != nullptr && !u->is_deleted && !u->is_bot && u->was_online > 0;
}

bool ContactsManager::can_report_user(UserId user_id) const {
  auto u = get_user(user_id);
  return u != nullptr && !u->is_deleted && !u->is_support && (u->is_bot || all_users_nearby_.count(user_id) != 0);
}

const ContactsManager::User *ContactsManager::get_user(UserId user_id) const {
  return users_.get_pointer(user_id);
}

ContactsManager::User *ContactsManager::get_user(UserId user_id) {
  return users_.get_pointer(user_id);
}

bool ContactsManager::is_dialog_info_received_from_server(DialogId dialog_id) const {
  switch (dialog_id.get_type()) {
    case DialogType::User: {
      auto u = get_user(dialog_id.get_user_id());
      return u != nullptr && u->is_received_from_server;
    }
    case DialogType::Chat: {
      auto c = get_chat(dialog_id.get_chat_id());
      return c != nullptr && c->is_received_from_server;
    }
    case DialogType::Channel: {
      auto c = get_channel(dialog_id.get_channel_id());
      return c != nullptr && c->is_received_from_server;
    }
    default:
      return false;
  }
}

void ContactsManager::reload_dialog_info(DialogId dialog_id, Promise<Unit> &&promise) {
  switch (dialog_id.get_type()) {
    case DialogType::User:
      return reload_user(dialog_id.get_user_id(), std::move(promise));
    case DialogType::Chat:
      return reload_chat(dialog_id.get_chat_id(), std::move(promise));
    case DialogType::Channel:
      return reload_channel(dialog_id.get_channel_id(), std::move(promise));
    default:
      return promise.set_error(Status::Error("Invalid dialog ID to reload"));
  }
}

void ContactsManager::send_get_me_query(Td *td, Promise<Unit> &&promise) {
  vector<tl_object_ptr<telegram_api::InputUser>> users;
  users.push_back(make_tl_object<telegram_api::inputUserSelf>());
  td->create_handler<GetUsersQuery>(std::move(promise))->send(std::move(users));
}

UserId ContactsManager::get_me(Promise<Unit> &&promise) {
  auto my_id = get_my_id();
  if (!have_user_force(my_id)) {
    send_get_me_query(td_, std::move(promise));
    return UserId();
  }

  promise.set_value(Unit());
  return my_id;
}

bool ContactsManager::get_user(UserId user_id, int left_tries, Promise<Unit> &&promise) {
  if (!user_id.is_valid()) {
    promise.set_error(Status::Error(400, "Invalid user identifier"));
    return false;
  }

  if (user_id == get_service_notifications_user_id() || user_id == get_replies_bot_user_id() ||
      user_id == get_anonymous_bot_user_id() || user_id == get_channel_bot_user_id()) {
    get_user_force(user_id);
  }

  if (td_->auth_manager_->is_bot() ? !have_user(user_id) : !have_min_user(user_id)) {
    // TODO UserLoader
    if (left_tries > 2 && G()->parameters().use_chat_info_db) {
      send_closure_later(actor_id(this), &ContactsManager::load_user_from_database, nullptr, user_id,
                         std::move(promise));
      return false;
    }
    auto r_input_user = get_input_user(user_id);
    if (left_tries == 1 || r_input_user.is_error()) {
      if (r_input_user.is_error()) {
        promise.set_error(r_input_user.move_as_error());
      } else {
        promise.set_error(Status::Error(400, "User not found"));
      }
      return false;
    }

    vector<tl_object_ptr<telegram_api::InputUser>> users;
    users.push_back(r_input_user.move_as_ok());
    td_->create_handler<GetUsersQuery>(std::move(promise))->send(std::move(users));
    return false;
  }

  promise.set_value(Unit());
  return true;
}

ContactsManager::User *ContactsManager::add_user(UserId user_id, const char *source) {
  CHECK(user_id.is_valid());
  auto &user_ptr = users_[user_id];
  if (user_ptr == nullptr) {
    user_ptr = make_unique<User>();
  }
  return user_ptr.get();
}

const ContactsManager::UserFull *ContactsManager::get_user_full(UserId user_id) const {
  return users_full_.get_pointer(user_id);
}

ContactsManager::UserFull *ContactsManager::get_user_full(UserId user_id) {
  return users_full_.get_pointer(user_id);
}

ContactsManager::UserFull *ContactsManager::add_user_full(UserId user_id) {
  CHECK(user_id.is_valid());
  auto &user_full_ptr = users_full_[user_id];
  if (user_full_ptr == nullptr) {
    user_full_ptr = make_unique<UserFull>();
  }
  return user_full_ptr.get();
}

void ContactsManager::reload_user(UserId user_id, Promise<Unit> &&promise) {
  if (!user_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Invalid user identifier"));
  }

  have_user_force(user_id);
  auto r_input_user = get_input_user(user_id);
  if (r_input_user.is_error()) {
    return promise.set_error(r_input_user.move_as_error());
  }

  // there is no much reason to combine different requests into one request
  vector<tl_object_ptr<telegram_api::InputUser>> users;
  users.push_back(r_input_user.move_as_ok());
  td_->create_handler<GetUsersQuery>(std::move(promise))->send(std::move(users));
}

void ContactsManager::load_user_full(UserId user_id, bool force, Promise<Unit> &&promise, const char *source) {
  auto u = get_user(user_id);
  if (u == nullptr) {
    return promise.set_error(Status::Error(400, "User not found"));
  }

  auto user_full = get_user_full_force(user_id);
  if (user_full == nullptr) {
    auto r_input_user = get_input_user(user_id);
    if (r_input_user.is_error()) {
      return promise.set_error(r_input_user.move_as_error());
    }

    return send_get_user_full_query(user_id, r_input_user.move_as_ok(), std::move(promise), source);
  }
  if (user_full->is_expired()) {
    auto r_input_user = get_input_user(user_id);
    CHECK(r_input_user.is_ok());
    if (td_->auth_manager_->is_bot() && !force) {
      return send_get_user_full_query(user_id, r_input_user.move_as_ok(), std::move(promise), "load expired user_full");
    }

    send_get_user_full_query(user_id, r_input_user.move_as_ok(), Auto(), "load expired user_full");
  }

  promise.set_value(Unit());
}

void ContactsManager::reload_user_full(UserId user_id, Promise<Unit> &&promise) {
  TRY_RESULT_PROMISE(promise, input_user, get_input_user(user_id));
  send_get_user_full_query(user_id, std::move(input_user), std::move(promise), "reload_user_full");
}

void ContactsManager::send_get_user_full_query(UserId user_id, tl_object_ptr<telegram_api::InputUser> &&input_user,
                                               Promise<Unit> &&promise, const char *source) {
  LOG(INFO) << "Get full " << user_id << " from " << source;
  if (!user_id.is_valid()) {
    return promise.set_error(Status::Error(500, "Invalid user_id"));
  }
  auto send_query =
      PromiseCreator::lambda([td = td_, input_user = std::move(input_user)](Result<Promise<Unit>> &&promise) mutable {
        if (promise.is_ok() && !G()->close_flag()) {
          td->create_handler<GetFullUserQuery>(promise.move_as_ok())->send(std::move(input_user));
        }
      });
  get_user_full_queries_.add_query(user_id.get(), std::move(send_query), std::move(promise));
}

std::pair<int32, vector<const Photo *>> ContactsManager::get_user_profile_photos(UserId user_id, int32 offset,
                                                                                 int32 limit, Promise<Unit> &&promise) {
  std::pair<int32, vector<const Photo *>> result;
  result.first = -1;

  if (offset < 0) {
    promise.set_error(Status::Error(400, "Parameter offset must be non-negative"));
    return result;
  }
  if (limit <= 0) {
    promise.set_error(Status::Error(400, "Parameter limit must be positive"));
    return result;
  }
  if (limit > MAX_GET_PROFILE_PHOTOS) {
    limit = MAX_GET_PROFILE_PHOTOS;
  }

  auto r_input_user = get_input_user(user_id);
  if (r_input_user.is_error()) {
    promise.set_error(r_input_user.move_as_error());
    return result;
  }

  apply_pending_user_photo(get_user(user_id), user_id);

  auto user_photos = &user_photos_[user_id];
  if (user_photos->getting_now) {
    promise.set_error(Status::Error(400, "Request for new profile photos has already been sent"));
    return result;
  }

  if (user_photos->count != -1) {  // know photo count
    CHECK(user_photos->offset != -1);
    result.first = user_photos->count;

    if (offset >= user_photos->count) {
      // offset if too big
      promise.set_value(Unit());
      return result;
    }

    if (limit > user_photos->count - offset) {
      limit = user_photos->count - offset;
    }

    int32 cache_begin = user_photos->offset;
    int32 cache_end = cache_begin + narrow_cast<int32>(user_photos->photos.size());
    if (cache_begin <= offset && offset + limit <= cache_end) {
      // answer query from cache
      for (int i = 0; i < limit; i++) {
        result.second.push_back(&user_photos->photos[i + offset - cache_begin]);
      }
      promise.set_value(Unit());
      return result;
    }

    if (cache_begin <= offset && offset < cache_end) {
      // adjust offset to the end of cache
      limit = offset + limit - cache_end;
      offset = cache_end;
    }
  }

  user_photos->getting_now = true;

  if (limit < MAX_GET_PROFILE_PHOTOS / 5) {
    limit = MAX_GET_PROFILE_PHOTOS / 5;  // make limit reasonable
  }

  td_->create_handler<GetUserPhotosQuery>(std::move(promise))
      ->send(user_id, r_input_user.move_as_ok(), offset, limit, 0);
  return result;
}

void ContactsManager::reload_user_profile_photo(UserId user_id, int64 photo_id, Promise<Unit> &&promise) {
  get_user_force(user_id);
  auto r_input_user = get_input_user(user_id);
  if (r_input_user.is_error()) {
    return promise.set_error(r_input_user.move_as_error());
  }

  // this request will be needed only to download the photo,
  // so there is no reason to combine different requests for a photo into one request
  td_->create_handler<GetUserPhotosQuery>(std::move(promise))
      ->send(user_id, r_input_user.move_as_ok(), -1, 1, photo_id);
}

FileSourceId ContactsManager::get_user_profile_photo_file_source_id(UserId user_id, int64 photo_id) {
  if (!user_id.is_valid()) {
    return FileSourceId();
  }

  auto u = get_user(user_id);
  if (u != nullptr && u->photo_ids.count(photo_id) != 0) {
    VLOG(file_references) << "Don't need to create file source for photo " << photo_id << " of " << user_id;
    // photo was already added, source ID was registered and shouldn't be needed
    return FileSourceId();
  }

  auto &source_id = user_profile_photo_file_source_ids_[std::make_pair(user_id, photo_id)];
  if (!source_id.is_valid()) {
    source_id = td_->file_reference_manager_->create_user_photo_file_source(user_id, photo_id);
  }
  VLOG(file_references) << "Return " << source_id << " for photo " << photo_id << " of " << user_id;
  return source_id;
}

FileSourceId ContactsManager::get_user_full_file_source_id(UserId user_id) {
  if (!user_id.is_valid()) {
    return FileSourceId();
  }

  if (get_user_full(user_id) != nullptr) {
    VLOG(file_references) << "Don't need to create file source for full " << user_id;
    // user full was already added, source ID was registered and shouldn't be needed
    return FileSourceId();
  }

  auto &source_id = user_full_file_source_ids_[user_id];
  if (!source_id.is_valid()) {
    source_id = td_->file_reference_manager_->create_user_full_file_source(user_id);
  }
  VLOG(file_references) << "Return " << source_id << " for full " << user_id;
  return source_id;
}

FileSourceId ContactsManager::get_chat_full_file_source_id(ChatId chat_id) {
  if (!chat_id.is_valid()) {
    return FileSourceId();
  }

  if (get_chat_full(chat_id) != nullptr) {
    VLOG(file_references) << "Don't need to create file source for full " << chat_id;
    // chat full was already added, source ID was registered and shouldn't be needed
    return FileSourceId();
  }

  auto &source_id = chat_full_file_source_ids_[chat_id];
  if (!source_id.is_valid()) {
    source_id = td_->file_reference_manager_->create_chat_full_file_source(chat_id);
  }
  VLOG(file_references) << "Return " << source_id << " for full " << chat_id;
  return source_id;
}

FileSourceId ContactsManager::get_channel_full_file_source_id(ChannelId channel_id) {
  if (!channel_id.is_valid()) {
    return FileSourceId();
  }

  if (get_channel_full(channel_id) != nullptr) {
    VLOG(file_references) << "Don't need to create file source for full " << channel_id;
    // channel full was already added, source ID was registered and shouldn't be needed
    return FileSourceId();
  }

  auto &source_id = channel_full_file_source_ids_[channel_id];
  if (!source_id.is_valid()) {
    source_id = td_->file_reference_manager_->create_channel_full_file_source(channel_id);
  }
  VLOG(file_references) << "Return " << source_id << " for full " << channel_id;
  return source_id;
}

bool ContactsManager::have_chat(ChatId chat_id) const {
  return chats_.count(chat_id) > 0;
}

const ContactsManager::Chat *ContactsManager::get_chat(ChatId chat_id) const {
  return chats_.get_pointer(chat_id);
}

ContactsManager::Chat *ContactsManager::get_chat(ChatId chat_id) {
  return chats_.get_pointer(chat_id);
}

ContactsManager::Chat *ContactsManager::add_chat(ChatId chat_id) {
  CHECK(chat_id.is_valid());
  auto &chat_ptr = chats_[chat_id];
  if (chat_ptr == nullptr) {
    chat_ptr = make_unique<Chat>();
  }
  return chat_ptr.get();
}

bool ContactsManager::get_chat(ChatId chat_id, int left_tries, Promise<Unit> &&promise) {
  if (!chat_id.is_valid()) {
    promise.set_error(Status::Error(400, "Invalid basic group identifier"));
    return false;
  }

  if (!have_chat(chat_id)) {
    if (left_tries > 2 && G()->parameters().use_chat_info_db) {
      send_closure_later(actor_id(this), &ContactsManager::load_chat_from_database, nullptr, chat_id,
                         std::move(promise));
      return false;
    }

    if (left_tries > 1) {
      td_->create_handler<GetChatsQuery>(std::move(promise))->send(vector<int64>{chat_id.get()});
      return false;
    }

    promise.set_error(Status::Error(400, "Group not found"));
    return false;
  }

  promise.set_value(Unit());
  return true;
}

void ContactsManager::reload_chat(ChatId chat_id, Promise<Unit> &&promise) {
  if (!chat_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Invalid basic group identifier"));
  }

  // there is no much reason to combine different requests into one request
  td_->create_handler<GetChatsQuery>(std::move(promise))->send(vector<int64>{chat_id.get()});
}

const ContactsManager::ChatFull *ContactsManager::get_chat_full(ChatId chat_id) const {
  return chats_full_.get_pointer(chat_id);
}

ContactsManager::ChatFull *ContactsManager::get_chat_full(ChatId chat_id) {
  return chats_full_.get_pointer(chat_id);
}

ContactsManager::ChatFull *ContactsManager::add_chat_full(ChatId chat_id) {
  CHECK(chat_id.is_valid());
  auto &chat_full_ptr = chats_full_[chat_id];
  if (chat_full_ptr == nullptr) {
    chat_full_ptr = make_unique<ChatFull>();
  }
  return chat_full_ptr.get();
}

bool ContactsManager::is_chat_full_outdated(const ChatFull *chat_full, const Chat *c, ChatId chat_id,
                                            bool only_participants) const {
  CHECK(c != nullptr);
  CHECK(chat_full != nullptr);
  if (!c->is_active && chat_full->version == -1) {
    return false;
  }

  if (chat_full->version != c->version) {
    LOG(INFO) << "Have outdated ChatFull " << chat_id << " with current version " << chat_full->version
              << " and chat version " << c->version;
    return true;
  }

  if (!only_participants && c->is_active && c->status.can_manage_invite_links() && !chat_full->invite_link.is_valid()) {
    LOG(INFO) << "Have outdated invite link in " << chat_id;
    return true;
  }

  if (!only_participants &&
      !is_same_dialog_photo(td_->file_manager_.get(), DialogId(chat_id), chat_full->photo, c->photo)) {
    LOG(INFO) << "Have outdated chat photo in " << chat_id;
    return true;
  }

  LOG(DEBUG) << "Full " << chat_id << " is up-to-date with version " << chat_full->version << " and photos " << c->photo
             << '/' << chat_full->photo;
  return false;
}

void ContactsManager::load_chat_full(ChatId chat_id, bool force, Promise<Unit> &&promise, const char *source) {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Group not found"));
  }

  auto chat_full = get_chat_full_force(chat_id, source);
  if (chat_full == nullptr) {
    LOG(INFO) << "Full " << chat_id << " not found";
    return send_get_chat_full_query(chat_id, std::move(promise), source);
  }

  if (is_chat_full_outdated(chat_full, c, chat_id, false)) {
    LOG(INFO) << "Have outdated full " << chat_id;
    if (td_->auth_manager_->is_bot() && !force) {
      return send_get_chat_full_query(chat_id, std::move(promise), source);
    }

    send_get_chat_full_query(chat_id, Auto(), source);
  }

  promise.set_value(Unit());
}

void ContactsManager::reload_chat_full(ChatId chat_id, Promise<Unit> &&promise) {
  send_get_chat_full_query(chat_id, std::move(promise), "reload_chat_full");
}

void ContactsManager::send_get_chat_full_query(ChatId chat_id, Promise<Unit> &&promise, const char *source) {
  LOG(INFO) << "Get full " << chat_id << " from " << source;
  if (!chat_id.is_valid()) {
    return promise.set_error(Status::Error(500, "Invalid chat_id"));
  }
  auto send_query = PromiseCreator::lambda([td = td_, chat_id](Result<Promise<Unit>> &&promise) {
    if (promise.is_ok() && !G()->close_flag()) {
      td->create_handler<GetFullChatQuery>(promise.move_as_ok())->send(chat_id);
    }
  });

  get_chat_full_queries_.add_query(DialogId(chat_id).get(), std::move(send_query), std::move(promise));
}

int32 ContactsManager::get_chat_date(ChatId chat_id) const {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return 0;
  }
  return c->date;
}

int32 ContactsManager::get_chat_participant_count(ChatId chat_id) const {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return 0;
  }
  return c->participant_count;
}

bool ContactsManager::get_chat_is_active(ChatId chat_id) const {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return false;
  }
  return c->is_active;
}

ChannelId ContactsManager::get_chat_migrated_to_channel_id(ChatId chat_id) const {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return ChannelId();
  }
  return c->migrated_to_channel_id;
}

DialogParticipantStatus ContactsManager::get_chat_status(ChatId chat_id) const {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return DialogParticipantStatus::Banned(0);
  }
  return get_chat_status(c);
}

DialogParticipantStatus ContactsManager::get_chat_status(const Chat *c) {
  if (!c->is_active) {
    return DialogParticipantStatus::Banned(0);
  }
  return c->status;
}

DialogParticipantStatus ContactsManager::get_chat_permissions(ChatId chat_id) const {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return DialogParticipantStatus::Banned(0);
  }
  return get_chat_permissions(c);
}

DialogParticipantStatus ContactsManager::get_chat_permissions(const Chat *c) const {
  if (!c->is_active) {
    return DialogParticipantStatus::Banned(0);
  }
  return c->status.apply_restrictions(c->default_permissions, td_->auth_manager_->is_bot());
}

bool ContactsManager::is_appointed_chat_administrator(ChatId chat_id) const {
  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return false;
  }
  return c->status.is_administrator();
}

bool ContactsManager::is_channel_public(ChannelId channel_id) const {
  return is_channel_public(get_channel(channel_id));
}

bool ContactsManager::is_channel_public(const Channel *c) {
  return c != nullptr && (!c->username.empty() || c->has_location);
}

ChannelType ContactsManager::get_channel_type(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    auto min_channel = get_min_channel(channel_id);
    if (min_channel != nullptr) {
      return min_channel->is_megagroup_ ? ChannelType::Megagroup : ChannelType::Broadcast;
    }
    return ChannelType::Unknown;
  }
  return get_channel_type(c);
}

ChannelType ContactsManager::get_channel_type(const Channel *c) {
  if (c->is_megagroup) {
    return ChannelType::Megagroup;
  }
  return ChannelType::Broadcast;
}

bool ContactsManager::is_broadcast_channel(ChannelId channel_id) const {
  return get_channel_type(channel_id) == ChannelType::Broadcast;
}

bool ContactsManager::is_megagroup_channel(ChannelId channel_id) const {
  return get_channel_type(channel_id) == ChannelType::Megagroup;
}

int32 ContactsManager::get_channel_date(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return 0;
  }
  return c->date;
}

DialogParticipantStatus ContactsManager::get_channel_status(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return DialogParticipantStatus::Banned(0);
  }
  return get_channel_status(c);
}

DialogParticipantStatus ContactsManager::get_channel_status(const Channel *c) {
  c->status.update_restrictions();
  return c->status;
}

DialogParticipantStatus ContactsManager::get_channel_permissions(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return DialogParticipantStatus::Banned(0);
  }
  return get_channel_permissions(c);
}

DialogParticipantStatus ContactsManager::get_channel_permissions(const Channel *c) const {
  c->status.update_restrictions();
  if (!c->is_megagroup) {
    // there is no restrictions in broadcast channels
    return c->status;
  }
  return c->status.apply_restrictions(c->default_permissions, td_->auth_manager_->is_bot());
}

int32 ContactsManager::get_channel_participant_count(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return 0;
  }
  return c->participant_count;
}

bool ContactsManager::get_channel_sign_messages(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return false;
  }
  return get_channel_sign_messages(c);
}

bool ContactsManager::get_channel_sign_messages(const Channel *c) {
  return c->sign_messages;
}

bool ContactsManager::get_channel_has_linked_channel(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return false;
  }
  return get_channel_has_linked_channel(c);
}

bool ContactsManager::get_channel_has_linked_channel(const Channel *c) {
  return c->has_linked_channel;
}

bool ContactsManager::get_channel_can_be_deleted(ChannelId channel_id) const {
  auto c = get_channel(channel_id);
  if (c == nullptr) {
    return false;
  }
  return get_channel_can_be_deleted(c);
}

bool ContactsManager::get_channel_can_be_deleted(const Channel *c) {
  return c->can_be_deleted;
}

bool ContactsManager::get_channel_join_to_send(const Channel *c) {
  return c->join_to_send || !c->is_megagroup || !c->has_linked_channel;
}

bool ContactsManager::get_channel_join_request(const Channel *c) {
  return c->join_request && c->is_megagroup && (is_channel_public(c) || c->has_linked_channel);
}

ChannelId ContactsManager::get_channel_linked_channel_id(ChannelId channel_id) {
  auto channel_full = get_channel_full_const(channel_id);
  if (channel_full == nullptr) {
    channel_full = get_channel_full_force(channel_id, true, "get_channel_linked_channel_id");
    if (channel_full == nullptr) {
      return ChannelId();
    }
  }
  return channel_full->linked_channel_id;
}

int32 ContactsManager::get_channel_slow_mode_delay(ChannelId channel_id) {
  auto channel_full = get_channel_full_const(channel_id);
  if (channel_full == nullptr) {
    channel_full = get_channel_full_force(channel_id, true, "get_channel_slow_mode_delay");
    if (channel_full == nullptr) {
      return 0;
    }
  }
  return channel_full->slow_mode_delay;
}

bool ContactsManager::have_channel(ChannelId channel_id) const {
  return channels_.count(channel_id) > 0;
}

bool ContactsManager::have_min_channel(ChannelId channel_id) const {
  return min_channels_.count(channel_id) > 0;
}

const MinChannel *ContactsManager::get_min_channel(ChannelId channel_id) const {
  return min_channels_.get_pointer(channel_id);
}

void ContactsManager::add_min_channel(ChannelId channel_id, const MinChannel &min_channel) {
  if (have_channel(channel_id) || have_min_channel(channel_id) || !channel_id.is_valid()) {
    return;
  }
  min_channels_.set(channel_id, td::make_unique<MinChannel>(min_channel));
}

const ContactsManager::Channel *ContactsManager::get_channel(ChannelId channel_id) const {
  return channels_.get_pointer(channel_id);
}

ContactsManager::Channel *ContactsManager::get_channel(ChannelId channel_id) {
  return channels_.get_pointer(channel_id);
}

ContactsManager::Channel *ContactsManager::add_channel(ChannelId channel_id, const char *source) {
  CHECK(channel_id.is_valid());
  auto &channel_ptr = channels_[channel_id];
  if (channel_ptr == nullptr) {
    channel_ptr = make_unique<Channel>();
    min_channels_.erase(channel_id);
  }
  return channel_ptr.get();
}

bool ContactsManager::get_channel(ChannelId channel_id, int left_tries, Promise<Unit> &&promise) {
  if (!channel_id.is_valid()) {
    promise.set_error(Status::Error(400, "Invalid supergroup identifier"));
    return false;
  }

  if (!have_channel(channel_id)) {
    if (left_tries > 2 && G()->parameters().use_chat_info_db) {
      send_closure_later(actor_id(this), &ContactsManager::load_channel_from_database, nullptr, channel_id,
                         std::move(promise));
      return false;
    }

    if (left_tries > 1 && td_->auth_manager_->is_bot()) {
      td_->create_handler<GetChannelsQuery>(std::move(promise))->send(get_input_channel(channel_id));
      return false;
    }

    promise.set_error(Status::Error(400, "Supergroup not found"));
    return false;
  }

  promise.set_value(Unit());
  return true;
}

void ContactsManager::reload_channel(ChannelId channel_id, Promise<Unit> &&promise) {
  if (!channel_id.is_valid()) {
    return promise.set_error(Status::Error(400, "Invalid supergroup identifier"));
  }

  have_channel_force(channel_id);
  auto input_channel = get_input_channel(channel_id);
  if (input_channel == nullptr) {
    input_channel = make_tl_object<telegram_api::inputChannel>(channel_id.get(), 0);
  }

  // there is no much reason to combine different requests into one request
  // requests with 0 access_hash must not be merged
  td_->create_handler<GetChannelsQuery>(std::move(promise))->send(std::move(input_channel));
}

const ContactsManager::ChannelFull *ContactsManager::get_channel_full_const(ChannelId channel_id) const {
  return channels_full_.get_pointer(channel_id);
}

const ContactsManager::ChannelFull *ContactsManager::get_channel_full(ChannelId channel_id) const {
  return channels_full_.get_pointer(channel_id);
}

ContactsManager::ChannelFull *ContactsManager::get_channel_full(ChannelId channel_id, bool only_local,
                                                                const char *source) {
  auto channel_full = channels_full_.get_pointer(channel_id);
  if (channel_full == nullptr) {
    return nullptr;
  }

  if (!only_local && channel_full->is_expired() && !td_->auth_manager_->is_bot()) {
    send_get_channel_full_query(channel_full, channel_id, Auto(), source);
  }

  return channel_full;
}

ContactsManager::ChannelFull *ContactsManager::add_channel_full(ChannelId channel_id) {
  CHECK(channel_id.is_valid());
  auto &channel_full_ptr = channels_full_[channel_id];
  if (channel_full_ptr == nullptr) {
    channel_full_ptr = make_unique<ChannelFull>();
  }
  return channel_full_ptr.get();
}

void ContactsManager::load_channel_full(ChannelId channel_id, bool force, Promise<Unit> &&promise, const char *source) {
  auto channel_full = get_channel_full_force(channel_id, true, source);
  if (channel_full == nullptr) {
    return send_get_channel_full_query(channel_full, channel_id, std::move(promise), source);
  }
  if (channel_full->is_expired()) {
    if (td_->auth_manager_->is_bot() && !force) {
      return send_get_channel_full_query(channel_full, channel_id, std::move(promise), "load expired channel_full");
    }

    Promise<Unit> new_promise;
    if (promise) {
      new_promise = PromiseCreator::lambda([channel_id](Result<Unit> result) {
        if (result.is_error()) {
          LOG(INFO) << "Failed to reload expired " << channel_id << ": " << result.error();
        } else {
          LOG(INFO) << "Reloaded expired " << channel_id;
        }
      });
    }
    send_get_channel_full_query(channel_full, channel_id, std::move(new_promise), "load expired channel_full");
  }

  promise.set_value(Unit());
}

void ContactsManager::reload_channel_full(ChannelId channel_id, Promise<Unit> &&promise, const char *source) {
  send_get_channel_full_query(get_channel_full(channel_id, true, "reload_channel_full"), channel_id, std::move(promise),
                              source);
}

void ContactsManager::send_get_channel_full_query(ChannelFull *channel_full, ChannelId channel_id,
                                                  Promise<Unit> &&promise, const char *source) {
  auto input_channel = get_input_channel(channel_id);
  if (input_channel == nullptr) {
    return promise.set_error(Status::Error(400, "Supergroup not found"));
  }

  if (!have_input_peer_channel(channel_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(400, "Can't access the chat"));
  }

  if (channel_full != nullptr) {
    if (!promise) {
      if (channel_full->repair_request_version != 0) {
        LOG(INFO) << "Skip get full " << channel_id << " request from " << source;
        return;
      }
      channel_full->repair_request_version = channel_full->speculative_version;
    } else {
      channel_full->repair_request_version = std::numeric_limits<uint32>::max();
    }
  }

  LOG(INFO) << "Get full " << channel_id << " from " << source;
  auto send_query = PromiseCreator::lambda(
      [td = td_, channel_id, input_channel = std::move(input_channel)](Result<Promise<Unit>> &&promise) mutable {
        if (promise.is_ok() && !G()->close_flag()) {
          td->create_handler<GetFullChannelQuery>(promise.move_as_ok())->send(channel_id, std::move(input_channel));
        }
      });
  get_chat_full_queries_.add_query(DialogId(channel_id).get(), std::move(send_query), std::move(promise));
}

bool ContactsManager::have_secret_chat(SecretChatId secret_chat_id) const {
  return secret_chats_.count(secret_chat_id) > 0;
}

ContactsManager::SecretChat *ContactsManager::add_secret_chat(SecretChatId secret_chat_id) {
  CHECK(secret_chat_id.is_valid());
  auto &secret_chat_ptr = secret_chats_[secret_chat_id];
  if (secret_chat_ptr == nullptr) {
    secret_chat_ptr = make_unique<SecretChat>();
  }
  return secret_chat_ptr.get();
}

const ContactsManager::SecretChat *ContactsManager::get_secret_chat(SecretChatId secret_chat_id) const {
  return secret_chats_.get_pointer(secret_chat_id);
}

ContactsManager::SecretChat *ContactsManager::get_secret_chat(SecretChatId secret_chat_id) {
  return secret_chats_.get_pointer(secret_chat_id);
}

bool ContactsManager::get_secret_chat(SecretChatId secret_chat_id, bool force, Promise<Unit> &&promise) {
  if (!secret_chat_id.is_valid()) {
    promise.set_error(Status::Error(400, "Invalid secret chat identifier"));
    return false;
  }

  if (!have_secret_chat(secret_chat_id)) {
    if (!force && G()->parameters().use_chat_info_db) {
      send_closure_later(actor_id(this), &ContactsManager::load_secret_chat_from_database, nullptr, secret_chat_id,
                         std::move(promise));
      return false;
    }

    promise.set_error(Status::Error(400, "Secret chat not found"));
    return false;
  }

  promise.set_value(Unit());
  return true;
}

void ContactsManager::on_update_secret_chat(SecretChatId secret_chat_id, int64 access_hash, UserId user_id,
                                            SecretChatState state, bool is_outbound, int32 ttl, int32 date,
                                            string key_hash, int32 layer, FolderId initial_folder_id) {
  LOG(INFO) << "Update " << secret_chat_id << " with " << user_id << " and access_hash " << access_hash;
  auto *secret_chat = add_secret_chat(secret_chat_id);
  if (access_hash != secret_chat->access_hash) {
    secret_chat->access_hash = access_hash;
    secret_chat->need_save_to_database = true;
  }
  if (user_id.is_valid() && user_id != secret_chat->user_id) {
    if (secret_chat->user_id.is_valid()) {
      LOG(ERROR) << "Secret chat user has changed from " << secret_chat->user_id << " to " << user_id;
      auto &old_secret_chat_ids = secret_chats_with_user_[secret_chat->user_id];
      td::remove(old_secret_chat_ids, secret_chat_id);
    }
    secret_chat->user_id = user_id;
    secret_chats_with_user_[secret_chat->user_id].push_back(secret_chat_id);
    secret_chat->is_changed = true;
  }
  if (state != SecretChatState::Unknown && state != secret_chat->state) {
    secret_chat->state = state;
    secret_chat->is_changed = true;
    secret_chat->is_state_changed = true;
  }
  if (is_outbound != secret_chat->is_outbound) {
    secret_chat->is_outbound = is_outbound;
    secret_chat->is_changed = true;
  }

  if (ttl != -1 && ttl != secret_chat->ttl) {
    secret_chat->ttl = ttl;
    secret_chat->need_save_to_database = true;
    secret_chat->is_ttl_changed = true;
  }
  if (date != 0 && date != secret_chat->date) {
    secret_chat->date = date;
    secret_chat->need_save_to_database = true;
  }
  if (!key_hash.empty() && key_hash != secret_chat->key_hash) {
    secret_chat->key_hash = std::move(key_hash);
    secret_chat->is_changed = true;
  }
  if (layer != 0 && layer != secret_chat->layer) {
    secret_chat->layer = layer;
    secret_chat->is_changed = true;
  }
  if (initial_folder_id != FolderId() && initial_folder_id != secret_chat->initial_folder_id) {
    secret_chat->initial_folder_id = initial_folder_id;
    secret_chat->is_changed = true;
  }

  update_secret_chat(secret_chat, secret_chat_id);
}

std::pair<int32, vector<DialogId>> ContactsManager::search_among_dialogs(const vector<DialogId> &dialog_ids,
                                                                         const string &query, int32 limit) const {
  Hints hints;  // TODO cache Hints

  for (auto dialog_id : dialog_ids) {
    int64 rating = 0;
    if (dialog_id.get_type() == DialogType::User) {
      auto user_id = dialog_id.get_user_id();
      auto u = get_user(user_id);
      if (u == nullptr) {
        continue;
      }
      if (query.empty()) {
        hints.add(dialog_id.get(), Slice(" "));
      } else {
        hints.add(dialog_id.get(), PSLICE() << u->first_name << ' ' << u->last_name << ' ' << u->username);
      }
      rating = -get_user_was_online(u, user_id);
    } else {
      if (!td_->messages_manager_->have_dialog_info(dialog_id)) {
        continue;
      }
      if (query.empty()) {
        hints.add(dialog_id.get(), Slice(" "));
      } else {
        hints.add(dialog_id.get(), td_->messages_manager_->get_dialog_title(dialog_id));
      }
    }
    hints.set_rating(dialog_id.get(), rating);
  }

  auto result = hints.search(query, limit, true);
  return {narrow_cast<int32>(result.first), transform(result.second, [](int64 key) { return DialogId(key); })};
}

void ContactsManager::add_dialog_participant(DialogId dialog_id, UserId user_id, int32 forward_limit,
                                             Promise<Unit> &&promise) {
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "add_dialog_participant")) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return promise.set_error(Status::Error(400, "Can't add members to a private chat"));
    case DialogType::Chat:
      return add_chat_participant(dialog_id.get_chat_id(), user_id, forward_limit, std::move(promise));
    case DialogType::Channel:
      return add_channel_participant(dialog_id.get_channel_id(), user_id, DialogParticipantStatus::Left(),
                                     std::move(promise));
    case DialogType::SecretChat:
      return promise.set_error(Status::Error(400, "Can't add members to a secret chat"));
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

void ContactsManager::add_dialog_participants(DialogId dialog_id, const vector<UserId> &user_ids,
                                              Promise<Unit> &&promise) {
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "add_dialog_participants")) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return promise.set_error(Status::Error(400, "Can't add members to a private chat"));
    case DialogType::Chat:
      return promise.set_error(Status::Error(400, "Can't add many members at once to a basic group chat"));
    case DialogType::Channel:
      return add_channel_participants(dialog_id.get_channel_id(), user_ids, std::move(promise));
    case DialogType::SecretChat:
      return promise.set_error(Status::Error(400, "Can't add members to a secret chat"));
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

void ContactsManager::set_dialog_participant_status(DialogId dialog_id, DialogId participant_dialog_id,
                                                    td_api::object_ptr<td_api::ChatMemberStatus> &&chat_member_status,
                                                    Promise<Unit> &&promise) {
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "set_dialog_participant_status")) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return promise.set_error(Status::Error(400, "Chat member status can't be changed in private chats"));
    case DialogType::Chat: {
      auto status = get_dialog_participant_status(chat_member_status, ChannelType::Unknown);
      if (participant_dialog_id.get_type() != DialogType::User) {
        if (status == DialogParticipantStatus::Left()) {
          return promise.set_value(Unit());
        } else {
          return promise.set_error(Status::Error(400, "Chats can't be members of basic groups"));
        }
      }
      return set_chat_participant_status(dialog_id.get_chat_id(), participant_dialog_id.get_user_id(), status,
                                         std::move(promise));
    }
    case DialogType::Channel:
      return set_channel_participant_status(dialog_id.get_channel_id(), participant_dialog_id,
                                            std::move(chat_member_status), std::move(promise));
    case DialogType::SecretChat:
      return promise.set_error(Status::Error(400, "Chat member status can't be changed in secret chats"));
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

void ContactsManager::ban_dialog_participant(DialogId dialog_id, DialogId participant_dialog_id,
                                             int32 banned_until_date, bool revoke_messages, Promise<Unit> &&promise) {
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "ban_dialog_participant")) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      return promise.set_error(Status::Error(400, "Can't ban members in private chats"));
    case DialogType::Chat:
      if (participant_dialog_id.get_type() != DialogType::User) {
        return promise.set_error(Status::Error(400, "Can't ban chats in basic groups"));
      }
      return delete_chat_participant(dialog_id.get_chat_id(), participant_dialog_id.get_user_id(), revoke_messages,
                                     std::move(promise));
    case DialogType::Channel:
      // must use td_api::chatMemberStatusBanned to properly fix banned_until_date
      return set_channel_participant_status(dialog_id.get_channel_id(), participant_dialog_id,
                                            td_api::make_object<td_api::chatMemberStatusBanned>(banned_until_date),
                                            std::move(promise));
    case DialogType::SecretChat:
      return promise.set_error(Status::Error(400, "Can't ban members in secret chats"));
    case DialogType::None:
    default:
      UNREACHABLE();
  }
}

void ContactsManager::get_dialog_participant(DialogId dialog_id, DialogId participant_dialog_id,
                                             Promise<td_api::object_ptr<td_api::chatMember>> &&promise) {
  auto new_promise = PromiseCreator::lambda(
      [actor_id = actor_id(this), promise = std::move(promise)](Result<DialogParticipant> &&result) mutable {
        TRY_RESULT_PROMISE(promise, dialog_participant, std::move(result));
        send_closure(actor_id, &ContactsManager::finish_get_dialog_participant, std::move(dialog_participant),
                     std::move(promise));
      });
  do_get_dialog_participant(dialog_id, participant_dialog_id, std::move(new_promise));
}

void ContactsManager::finish_get_dialog_participant(DialogParticipant &&dialog_participant,
                                                    Promise<td_api::object_ptr<td_api::chatMember>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  auto participant_dialog_id = dialog_participant.dialog_id_;
  bool is_user = participant_dialog_id.get_type() == DialogType::User;
  if ((is_user && !have_user(participant_dialog_id.get_user_id())) ||
      (!is_user && !td_->messages_manager_->have_dialog(participant_dialog_id))) {
    return promise.set_error(Status::Error(400, "Member not found"));
  }

  promise.set_value(get_chat_member_object(dialog_participant));
}

void ContactsManager::do_get_dialog_participant(DialogId dialog_id, DialogId participant_dialog_id,
                                                Promise<DialogParticipant> &&promise) {
  LOG(INFO) << "Receive GetChatMember request to get " << participant_dialog_id << " in " << dialog_id;
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "do_get_dialog_participant")) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User: {
      auto my_user_id = get_my_id();
      auto peer_user_id = dialog_id.get_user_id();
      if (participant_dialog_id == DialogId(my_user_id)) {
        return promise.set_value(DialogParticipant::private_member(my_user_id, peer_user_id));
      }
      if (participant_dialog_id == dialog_id) {
        return promise.set_value(DialogParticipant::private_member(peer_user_id, my_user_id));
      }

      return promise.set_error(Status::Error(400, "Member not found"));
    }
    case DialogType::Chat:
      if (participant_dialog_id.get_type() != DialogType::User) {
        return promise.set_value(DialogParticipant::left(participant_dialog_id));
      }
      return get_chat_participant(dialog_id.get_chat_id(), participant_dialog_id.get_user_id(), std::move(promise));
    case DialogType::Channel:
      return get_channel_participant(dialog_id.get_channel_id(), participant_dialog_id, std::move(promise));
    case DialogType::SecretChat: {
      auto my_user_id = get_my_id();
      auto peer_user_id = get_secret_chat_user_id(dialog_id.get_secret_chat_id());
      if (participant_dialog_id == DialogId(my_user_id)) {
        return promise.set_value(DialogParticipant::private_member(my_user_id, peer_user_id));
      }
      if (peer_user_id.is_valid() && participant_dialog_id == DialogId(peer_user_id)) {
        return promise.set_value(DialogParticipant::private_member(peer_user_id, my_user_id));
      }

      return promise.set_error(Status::Error(400, "Member not found"));
    }
    case DialogType::None:
    default:
      UNREACHABLE();
      return promise.set_error(Status::Error(500, "Wrong chat type"));
  }
}

DialogParticipants ContactsManager::search_private_chat_participants(UserId my_user_id, UserId peer_user_id,
                                                                     const string &query, int32 limit,
                                                                     DialogParticipantFilter filter) const {
  vector<DialogId> dialog_ids;
  if (filter.is_dialog_participant_suitable(td_, DialogParticipant::private_member(my_user_id, peer_user_id))) {
    dialog_ids.push_back(DialogId(my_user_id));
  }
  if (peer_user_id.is_valid() && peer_user_id != my_user_id &&
      filter.is_dialog_participant_suitable(td_, DialogParticipant::private_member(peer_user_id, my_user_id))) {
    dialog_ids.push_back(DialogId(peer_user_id));
  }

  auto result = search_among_dialogs(dialog_ids, query, limit);
  return {result.first, transform(result.second, [&](DialogId dialog_id) {
            auto user_id = dialog_id.get_user_id();
            return DialogParticipant::private_member(user_id, user_id == my_user_id ? peer_user_id : my_user_id);
          })};
}

void ContactsManager::search_dialog_participants(DialogId dialog_id, const string &query, int32 limit,
                                                 DialogParticipantFilter filter,
                                                 Promise<DialogParticipants> &&promise) {
  LOG(INFO) << "Receive searchChatMembers request to search for \"" << query << "\" in " << dialog_id << " with filter "
            << filter;
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "search_dialog_participants")) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }
  if (limit < 0) {
    return promise.set_error(Status::Error(400, "Parameter limit must be non-negative"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
      promise.set_value(search_private_chat_participants(get_my_id(), dialog_id.get_user_id(), query, limit, filter));
      return;
    case DialogType::Chat:
      return search_chat_participants(dialog_id.get_chat_id(), query, limit, filter, std::move(promise));
    case DialogType::Channel: {
      auto channel_id = dialog_id.get_channel_id();
      if (filter.has_query()) {
        return get_channel_participants(channel_id, filter.get_supergroup_members_filter_object(query), string(), 0,
                                        limit, 0, std::move(promise));
      } else {
        return get_channel_participants(channel_id, filter.get_supergroup_members_filter_object(string()), query, 0,
                                        100, limit, std::move(promise));
      }
    }
    case DialogType::SecretChat: {
      auto peer_user_id = get_secret_chat_user_id(dialog_id.get_secret_chat_id());
      promise.set_value(search_private_chat_participants(get_my_id(), peer_user_id, query, limit, filter));
      return;
    }
    case DialogType::None:
    default:
      UNREACHABLE();
      promise.set_error(Status::Error(500, "Wrong chat type"));
  }
}

void ContactsManager::get_chat_participant(ChatId chat_id, UserId user_id, Promise<DialogParticipant> &&promise) {
  LOG(INFO) << "Trying to get " << user_id << " as member of " << chat_id;

  auto c = get_chat(chat_id);
  if (c == nullptr) {
    return promise.set_error(Status::Error(400, "Group not found"));
  }

  auto chat_full = get_chat_full_force(chat_id, "get_chat_participant");
  if (chat_full == nullptr || (td_->auth_manager_->is_bot() && is_chat_full_outdated(chat_full, c, chat_id, true))) {
    auto query_promise = PromiseCreator::lambda(
        [actor_id = actor_id(this), chat_id, user_id, promise = std::move(promise)](Result<Unit> &&result) mutable {
          TRY_STATUS_PROMISE(promise, std::move(result));
          send_closure(actor_id, &ContactsManager::finish_get_chat_participant, chat_id, user_id, std::move(promise));
        });
    send_get_chat_full_query(chat_id, std::move(query_promise), "get_chat_participant");
    return;
  }

  if (is_chat_full_outdated(chat_full, c, chat_id, true)) {
    send_get_chat_full_query(chat_id, Auto(), "get_chat_participant lazy");
  }

  finish_get_chat_participant(chat_id, user_id, std::move(promise));
}

void ContactsManager::finish_get_chat_participant(ChatId chat_id, UserId user_id,
                                                  Promise<DialogParticipant> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  const auto *participant = get_chat_participant(chat_id, user_id);
  if (participant == nullptr) {
    return promise.set_value(DialogParticipant::left(DialogId(user_id)));
  }

  promise.set_value(DialogParticipant(*participant));
}

void ContactsManager::search_chat_participants(ChatId chat_id, const string &query, int32 limit,
                                               DialogParticipantFilter filter, Promise<DialogParticipants> &&promise) {
  if (limit < 0) {
    return promise.set_error(Status::Error(400, "Parameter limit must be non-negative"));
  }

  auto load_chat_full_promise = PromiseCreator::lambda([actor_id = actor_id(this), chat_id, query, limit, filter,
                                                        promise = std::move(promise)](Result<Unit> &&result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      send_closure(actor_id, &ContactsManager::do_search_chat_participants, chat_id, query, limit, filter,
                   std::move(promise));
    }
  });
  load_chat_full(chat_id, false, std::move(load_chat_full_promise), "search_chat_participants");
}

void ContactsManager::do_search_chat_participants(ChatId chat_id, const string &query, int32 limit,
                                                  DialogParticipantFilter filter,
                                                  Promise<DialogParticipants> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  auto chat_full = get_chat_full(chat_id);
  if (chat_full == nullptr) {
    return promise.set_error(Status::Error(500, "Can't find basic group full info"));
  }

  vector<DialogId> dialog_ids;
  for (const auto &participant : chat_full->participants) {
    if (filter.is_dialog_participant_suitable(td_, participant)) {
      dialog_ids.push_back(participant.dialog_id_);
    }
  }

  int32 total_count;
  std::tie(total_count, dialog_ids) = search_among_dialogs(dialog_ids, query, limit);
  promise.set_value(DialogParticipants{total_count, transform(dialog_ids, [chat_full](DialogId dialog_id) {
                                         return *ContactsManager::get_chat_full_participant(chat_full, dialog_id);
                                       })});
}

void ContactsManager::get_channel_participant(ChannelId channel_id, DialogId participant_dialog_id,
                                              Promise<DialogParticipant> &&promise) {
  LOG(INFO) << "Trying to get " << participant_dialog_id << " as member of " << channel_id;

  auto input_peer = td_->messages_manager_->get_input_peer(participant_dialog_id, AccessRights::Know);
  if (input_peer == nullptr) {
    return promise.set_error(Status::Error(400, "Member not found"));
  }

  if (have_channel_participant_cache(channel_id)) {
    auto *participant = get_channel_participant_from_cache(channel_id, participant_dialog_id);
    if (participant != nullptr) {
      return promise.set_value(DialogParticipant{*participant});
    }
  }

  auto on_result_promise = PromiseCreator::lambda([actor_id = actor_id(this), channel_id, promise = std::move(promise)](
                                                      Result<DialogParticipant> r_dialog_participant) mutable {
    TRY_RESULT_PROMISE(promise, dialog_participant, std::move(r_dialog_participant));
    send_closure(actor_id, &ContactsManager::finish_get_channel_participant, channel_id, std::move(dialog_participant),
                 std::move(promise));
  });

  td_->create_handler<GetChannelParticipantQuery>(std::move(on_result_promise))
      ->send(channel_id, participant_dialog_id, std::move(input_peer));
}

void ContactsManager::finish_get_channel_participant(ChannelId channel_id, DialogParticipant &&dialog_participant,
                                                     Promise<DialogParticipant> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  CHECK(dialog_participant.is_valid());  // checked in GetChannelParticipantQuery

  LOG(INFO) << "Receive " << dialog_participant.dialog_id_ << " as a member of a channel " << channel_id;

  dialog_participant.status_.update_restrictions();
  if (have_channel_participant_cache(channel_id)) {
    add_channel_participant_to_cache(channel_id, dialog_participant, false);
  }
  promise.set_value(std::move(dialog_participant));
}

void ContactsManager::get_channel_participants(ChannelId channel_id,
                                               tl_object_ptr<td_api::SupergroupMembersFilter> &&filter,
                                               string additional_query, int32 offset, int32 limit,
                                               int32 additional_limit, Promise<DialogParticipants> &&promise) {
  if (limit <= 0) {
    return promise.set_error(Status::Error(400, "Parameter limit must be positive"));
  }
  if (limit > MAX_GET_CHANNEL_PARTICIPANTS) {
    limit = MAX_GET_CHANNEL_PARTICIPANTS;
  }

  if (offset < 0) {
    return promise.set_error(Status::Error(400, "Parameter offset must be non-negative"));
  }

  auto channel_full = get_channel_full_force(channel_id, true, "get_channel_participants");
  if (channel_full != nullptr && !channel_full->is_expired() && !channel_full->can_get_participants) {
    return promise.set_error(Status::Error(400, "Member list is inaccessible"));
  }

  ChannelParticipantFilter participant_filter(filter);
  auto get_channel_participants_promise = PromiseCreator::lambda(
      [actor_id = actor_id(this), channel_id, filter = participant_filter,
       additional_query = std::move(additional_query), offset, limit, additional_limit, promise = std::move(promise)](
          Result<tl_object_ptr<telegram_api::channels_channelParticipants>> &&result) mutable {
        if (result.is_error()) {
          promise.set_error(result.move_as_error());
        } else {
          send_closure(actor_id, &ContactsManager::on_get_channel_participants, channel_id, std::move(filter), offset,
                       limit, std::move(additional_query), additional_limit, result.move_as_ok(), std::move(promise));
        }
      });
  td_->create_handler<GetChannelParticipantsQuery>(std::move(get_channel_participants_promise))
      ->send(channel_id, participant_filter, offset, limit);
}

td_api::object_ptr<td_api::chatAdministrators> ContactsManager::get_chat_administrators_object(
    const vector<DialogAdministrator> &dialog_administrators) {
  auto administrator_objects = transform(dialog_administrators, [this](const DialogAdministrator &administrator) {
    return administrator.get_chat_administrator_object(this);
  });
  return td_api::make_object<td_api::chatAdministrators>(std::move(administrator_objects));
}

void ContactsManager::get_dialog_administrators(DialogId dialog_id,
                                                Promise<td_api::object_ptr<td_api::chatAdministrators>> &&promise) {
  if (!td_->messages_manager_->have_dialog_force(dialog_id, "get_dialog_administrators")) {
    return promise.set_error(Status::Error(400, "Chat not found"));
  }

  switch (dialog_id.get_type()) {
    case DialogType::User:
    case DialogType::SecretChat:
      return promise.set_value(td_api::make_object<td_api::chatAdministrators>());
    case DialogType::Chat:
    case DialogType::Channel:
      break;
    case DialogType::None:
    default:
      UNREACHABLE();
      return;
  }

  auto it = dialog_administrators_.find(dialog_id);
  if (it != dialog_administrators_.end()) {
    reload_dialog_administrators(dialog_id, it->second, Auto());  // update administrators cache
    return promise.set_value(get_chat_administrators_object(it->second));
  }

  if (G()->parameters().use_chat_info_db) {
    LOG(INFO) << "Load administrators of " << dialog_id << " from database";
    G()->td_db()->get_sqlite_pmc()->get(get_dialog_administrators_database_key(dialog_id),
                                        PromiseCreator::lambda([actor_id = actor_id(this), dialog_id,
                                                                promise = std::move(promise)](string value) mutable {
                                          send_closure(actor_id,
                                                       &ContactsManager::on_load_dialog_administrators_from_database,
                                                       dialog_id, std::move(value), std::move(promise));
                                        }));
    return;
  }

  reload_dialog_administrators(dialog_id, {}, std::move(promise));
}

string ContactsManager::get_dialog_administrators_database_key(DialogId dialog_id) {
  return PSTRING() << "adm" << (-dialog_id.get());
}

void ContactsManager::on_load_dialog_administrators_from_database(
    DialogId dialog_id, string value, Promise<td_api::object_ptr<td_api::chatAdministrators>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  if (value.empty()) {
    return reload_dialog_administrators(dialog_id, {}, std::move(promise));
  }

  vector<DialogAdministrator> administrators;
  log_event_parse(administrators, value).ensure();

  LOG(INFO) << "Successfully loaded " << administrators.size() << " administrators in " << dialog_id
            << " from database";

  MultiPromiseActorSafe load_users_multipromise{"LoadUsersMultiPromiseActor"};
  load_users_multipromise.add_promise(
      PromiseCreator::lambda([actor_id = actor_id(this), dialog_id, administrators,
                              promise = std::move(promise)](Result<Unit> result) mutable {
        send_closure(actor_id, &ContactsManager::on_load_administrator_users_finished, dialog_id,
                     std::move(administrators), std::move(result), std::move(promise));
      }));

  auto lock_promise = load_users_multipromise.get_promise();

  for (auto &administrator : administrators) {
    get_user(administrator.get_user_id(), 3, load_users_multipromise.get_promise());
  }

  lock_promise.set_value(Unit());
}

void ContactsManager::on_load_administrator_users_finished(
    DialogId dialog_id, vector<DialogAdministrator> administrators, Result<> result,
    Promise<td_api::object_ptr<td_api::chatAdministrators>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  if (result.is_error()) {
    return reload_dialog_administrators(dialog_id, {}, std::move(promise));
  }

  auto it = dialog_administrators_.emplace(dialog_id, std::move(administrators)).first;
  reload_dialog_administrators(dialog_id, it->second, Auto());  // update administrators cache
  promise.set_value(get_chat_administrators_object(it->second));
}

void ContactsManager::on_update_channel_administrator_count(ChannelId channel_id, int32 administrator_count) {
  auto channel_full = get_channel_full_force(channel_id, true, "on_update_channel_administrator_count");
  if (channel_full != nullptr && channel_full->administrator_count != administrator_count) {
    channel_full->administrator_count = administrator_count;
    channel_full->is_changed = true;

    if (channel_full->participant_count < channel_full->administrator_count) {
      channel_full->participant_count = channel_full->administrator_count;

      auto c = get_channel(channel_id);
      if (c != nullptr && c->participant_count != channel_full->participant_count) {
        c->participant_count = channel_full->participant_count;
        c->is_changed = true;
        update_channel(c, channel_id);
      }
    }

    update_channel_full(channel_full, channel_id, "on_update_channel_administrator_count");
  }
}

void ContactsManager::on_update_dialog_administrators(DialogId dialog_id, vector<DialogAdministrator> &&administrators,
                                                      bool have_access, bool from_database) {
  LOG(INFO) << "Update administrators in " << dialog_id << " to " << format::as_array(administrators);
  if (have_access) {
    CHECK(dialog_id.is_valid());
    std::sort(administrators.begin(), administrators.end(),
              [](const DialogAdministrator &lhs, const DialogAdministrator &rhs) {
                return lhs.get_user_id().get() < rhs.get_user_id().get();
              });

    auto it = dialog_administrators_.find(dialog_id);
    if (it != dialog_administrators_.end()) {
      if (it->second == administrators) {
        return;
      }
      it->second = std::move(administrators);
    } else {
      it = dialog_administrators_.emplace(dialog_id, std::move(administrators)).first;
    }

    if (G()->parameters().use_chat_info_db && !from_database) {
      LOG(INFO) << "Save administrators of " << dialog_id << " to database";
      G()->td_db()->get_sqlite_pmc()->set(get_dialog_administrators_database_key(dialog_id),
                                          log_event_store(it->second).as_slice().str(), Auto());
    }
  } else {
    dialog_administrators_.erase(dialog_id);
    if (G()->parameters().use_chat_info_db) {
      G()->td_db()->get_sqlite_pmc()->erase(get_dialog_administrators_database_key(dialog_id), Auto());
    }
  }
}

void ContactsManager::reload_dialog_administrators(DialogId dialog_id,
                                                   const vector<DialogAdministrator> &dialog_administrators,
                                                   Promise<td_api::object_ptr<td_api::chatAdministrators>> &&promise) {
  auto dialog_type = dialog_id.get_type();
  if (dialog_type == DialogType::Chat && !get_chat_permissions(dialog_id.get_chat_id()).is_member()) {
    return promise.set_value(td_api::make_object<td_api::chatAdministrators>());
  }
  auto query_promise = PromiseCreator::lambda(
      [actor_id = actor_id(this), dialog_id, promise = std::move(promise)](Result<Unit> &&result) mutable {
        if (promise) {
          if (result.is_ok()) {
            send_closure(actor_id, &ContactsManager::on_reload_dialog_administrators, dialog_id, std::move(promise));
          } else {
            promise.set_error(result.move_as_error());
          }
        }
      });
  switch (dialog_type) {
    case DialogType::Chat:
      load_chat_full(dialog_id.get_chat_id(), false, std::move(query_promise), "reload_dialog_administrators");
      break;
    case DialogType::Channel: {
      auto hash = get_vector_hash(transform(dialog_administrators, [](const DialogAdministrator &administrator) {
        return static_cast<uint64>(administrator.get_user_id().get());
      }));
      td_->create_handler<GetChannelAdministratorsQuery>(std::move(query_promise))
          ->send(dialog_id.get_channel_id(), hash);
      break;
    }
    default:
      UNREACHABLE();
  }
}

void ContactsManager::on_reload_dialog_administrators(
    DialogId dialog_id, Promise<td_api::object_ptr<td_api::chatAdministrators>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  auto it = dialog_administrators_.find(dialog_id);
  if (it != dialog_administrators_.end()) {
    return promise.set_value(get_chat_administrators_object(it->second));
  }

  LOG(ERROR) << "Failed to load administrators in " << dialog_id;
  promise.set_error(Status::Error(500, "Failed to find chat administrators"));
}

void ContactsManager::on_chat_update(telegram_api::chatEmpty &chat, const char *source) {
  ChatId chat_id(chat.id_);
  if (!chat_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << chat_id << " from " << source;
    return;
  }

  if (!have_chat(chat_id)) {
    LOG(ERROR) << "Have no information about " << chat_id << " but received chatEmpty from " << source;
  }
}

void ContactsManager::on_chat_update(telegram_api::chat &chat, const char *source) {
  auto debug_str = PSTRING() << " from " << source << " in " << oneline(to_string(chat));
  ChatId chat_id(chat.id_);
  if (!chat_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << chat_id << debug_str;
    return;
  }

  DialogParticipantStatus status = [&] {
    bool is_creator = 0 != (chat.flags_ & CHAT_FLAG_USER_IS_CREATOR);
    bool has_left = 0 != (chat.flags_ & CHAT_FLAG_USER_HAS_LEFT);
    if (is_creator) {
      return DialogParticipantStatus::Creator(!has_left, false, string());
    } else if (chat.admin_rights_ != nullptr) {
      return DialogParticipantStatus(false, std::move(chat.admin_rights_), string(), ChannelType::Unknown);
    } else if (has_left) {
      return DialogParticipantStatus::Left();
    } else {
      return DialogParticipantStatus::Member();
    }
  }();

  bool is_active = 0 == (chat.flags_ & CHAT_FLAG_IS_DEACTIVATED);

  ChannelId migrated_to_channel_id;
  if (chat.flags_ & CHAT_FLAG_WAS_MIGRATED) {
    switch (chat.migrated_to_->get_id()) {
      case telegram_api::inputChannelEmpty::ID: {
        LOG(ERROR) << "Receive empty upgraded to supergroup for " << chat_id << debug_str;
        break;
      }
      case telegram_api::inputChannel::ID: {
        auto input_channel = move_tl_object_as<telegram_api::inputChannel>(chat.migrated_to_);
        migrated_to_channel_id = ChannelId(input_channel->channel_id_);
        if (!have_channel_force(migrated_to_channel_id)) {
          if (!migrated_to_channel_id.is_valid()) {
            LOG(ERROR) << "Receive invalid " << migrated_to_channel_id << debug_str;
          } else {
            // temporarily create the channel
            Channel *c = add_channel(migrated_to_channel_id, "on_chat_update");
            c->access_hash = input_channel->access_hash_;
            c->title = chat.title_;
            c->status = DialogParticipantStatus::Left();
            c->is_megagroup = true;

            // we definitely need to call update_channel, because client should know about every added channel
            update_channel(c, migrated_to_channel_id);

            // get info about the channel
            td_->create_handler<GetChannelsQuery>(Promise<>())->send(std::move(input_channel));
          }
        }
        break;
      }
      default:
        UNREACHABLE();
    }
  }

  Chat *c = get_chat_force(chat_id);  // to load versions
  if (c == nullptr) {
    c = add_chat(chat_id);
  }
  on_update_chat_title(c, chat_id, std::move(chat.title_));
  if (!status.is_left()) {
    on_update_chat_participant_count(c, chat_id, chat.participants_count_, chat.version_, debug_str);
  } else {
    chat.photo_ = nullptr;
  }
  if (c->date != chat.date_) {
    LOG_IF(ERROR, c->date != 0) << "Chat creation date has changed from " << c->date << " to " << chat.date_
                                << debug_str;
    c->date = chat.date_;
    c->need_save_to_database = true;
  }
  on_update_chat_status(c, chat_id, std::move(status));
  on_update_chat_default_permissions(c, chat_id, RestrictedRights(chat.default_banned_rights_), chat.version_);
  on_update_chat_photo(c, chat_id, std::move(chat.photo_));
  on_update_chat_active(c, chat_id, is_active);
  on_update_chat_noforwards(c, chat_id, chat.noforwards_);
  on_update_chat_migrated_to_channel_id(c, chat_id, migrated_to_channel_id);
  LOG_IF(INFO, !is_active && !migrated_to_channel_id.is_valid()) << chat_id << " is deactivated" << debug_str;
  if (c->cache_version != Chat::CACHE_VERSION) {
    c->cache_version = Chat::CACHE_VERSION;
    c->need_save_to_database = true;
  }
  c->is_received_from_server = true;
  update_chat(c, chat_id);

  bool has_active_group_call = (chat.flags_ & CHAT_FLAG_HAS_ACTIVE_GROUP_CALL) != 0;
  bool is_group_call_empty = (chat.flags_ & CHAT_FLAG_IS_GROUP_CALL_NON_EMPTY) == 0;
  td_->messages_manager_->on_update_dialog_group_call(DialogId(chat_id), has_active_group_call, is_group_call_empty,
                                                      "receive chat");
}

void ContactsManager::on_chat_update(telegram_api::chatForbidden &chat, const char *source) {
  ChatId chat_id(chat.id_);
  if (!chat_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << chat_id << " from " << source;
    return;
  }

  bool is_uninited = get_chat_force(chat_id) == nullptr;
  Chat *c = add_chat(chat_id);
  on_update_chat_title(c, chat_id, std::move(chat.title_));
  // chat participant count will be updated in on_update_chat_status
  on_update_chat_photo(c, chat_id, nullptr);
  if (c->date != 0) {
    c->date = 0;  // removed in 38-th layer
    c->need_save_to_database = true;
  }
  on_update_chat_status(c, chat_id, DialogParticipantStatus::Banned(0));
  if (is_uninited) {
    on_update_chat_active(c, chat_id, true);
    on_update_chat_migrated_to_channel_id(c, chat_id, ChannelId());
  } else {
    // leave active and migrated to as is
  }
  if (c->cache_version != Chat::CACHE_VERSION) {
    c->cache_version = Chat::CACHE_VERSION;
    c->need_save_to_database = true;
  }
  c->is_received_from_server = true;
  update_chat(c, chat_id);
}

void ContactsManager::on_chat_update(telegram_api::channel &channel, const char *source) {
  ChannelId channel_id(channel.id_);
  if (!channel_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << channel_id << " from " << source << ": " << to_string(channel);
    return;
  }

  if (channel.flags_ == 0 && channel.access_hash_ == 0 && channel.title_.empty()) {
    Channel *c = get_channel_force(channel_id);
    LOG(ERROR) << "Receive empty " << to_string(channel) << " from " << source << ", have "
               << to_string(get_supergroup_object(channel_id, c));
    if (c == nullptr && !have_min_channel(channel_id)) {
      min_channels_.set(channel_id, td::make_unique<MinChannel>());
    }
    return;
  }

  bool is_min = (channel.flags_ & CHANNEL_FLAG_IS_MIN) != 0;
  bool has_access_hash = (channel.flags_ & CHANNEL_FLAG_HAS_ACCESS_HASH) != 0;
  auto access_hash = has_access_hash ? channel.access_hash_ : 0;

  bool has_linked_channel = (channel.flags_ & CHANNEL_FLAG_HAS_LINKED_CHAT) != 0;
  bool sign_messages = (channel.flags_ & CHANNEL_FLAG_SIGN_MESSAGES) != 0;
  bool join_to_send = (channel.flags_ & CHANNEL_FLAG_JOIN_TO_SEND) != 0;
  bool join_request = (channel.flags_ & CHANNEL_FLAG_JOIN_REQUEST) != 0;
  bool is_slow_mode_enabled = (channel.flags_ & CHANNEL_FLAG_IS_SLOW_MODE_ENABLED) != 0;
  bool is_megagroup = (channel.flags_ & CHANNEL_FLAG_IS_MEGAGROUP) != 0;
  bool is_verified = (channel.flags_ & CHANNEL_FLAG_IS_VERIFIED) != 0;
  auto restriction_reasons = get_restriction_reasons(std::move(channel.restriction_reason_));
  bool is_scam = (channel.flags_ & CHANNEL_FLAG_IS_SCAM) != 0;
  bool is_fake = (channel.flags_ & CHANNEL_FLAG_IS_FAKE) != 0;
  bool is_gigagroup = (channel.flags_ & CHANNEL_FLAG_IS_GIGAGROUP) != 0;
  bool have_participant_count = (channel.flags_ & CHANNEL_FLAG_HAS_PARTICIPANT_COUNT) != 0;
  int32 participant_count = have_participant_count ? channel.participants_count_ : 0;

  if (have_participant_count) {
    auto channel_full = get_channel_full_const(channel_id);
    if (channel_full != nullptr && channel_full->administrator_count > participant_count) {
      participant_count = channel_full->administrator_count;
    }
  }

  {
    bool is_broadcast = (channel.flags_ & CHANNEL_FLAG_IS_BROADCAST) != 0;
    LOG_IF(ERROR, is_broadcast == is_megagroup)
        << "Receive wrong channel flag is_broadcast == is_megagroup == " << is_megagroup << " from " << source << ": "
        << oneline(to_string(channel));
  }

  if (is_megagroup) {
    LOG_IF(ERROR, sign_messages) << "Need to sign messages in the supergroup " << channel_id << " from " << source;
    sign_messages = true;
  } else {
    LOG_IF(ERROR, is_slow_mode_enabled) << "Slow mode enabled in the " << channel_id << " from " << source;
    LOG_IF(ERROR, is_gigagroup) << "Receive broadcast group as channel " << channel_id << " from " << source;
    is_slow_mode_enabled = false;
    is_gigagroup = false;
  }
  if (is_gigagroup) {
    remove_dialog_suggested_action(SuggestedAction{SuggestedAction::Type::ConvertToGigagroup, DialogId(channel_id)});
  }

  DialogParticipantStatus status = [&] {
    bool has_left = (channel.flags_ & CHANNEL_FLAG_USER_HAS_LEFT) != 0;
    bool is_creator = (channel.flags_ & CHANNEL_FLAG_USER_IS_CREATOR) != 0;

    if (is_creator) {
      bool is_anonymous = channel.admin_rights_ != nullptr &&
                          (channel.admin_rights_->flags_ & telegram_api::chatAdminRights::ANONYMOUS_MASK) != 0;
      return DialogParticipantStatus::Creator(!has_left, is_anonymous, string());
    } else if (channel.admin_rights_ != nullptr) {
      return DialogParticipantStatus(false, std::move(channel.admin_rights_), string(),
                                     is_megagroup ? ChannelType::Megagroup : ChannelType::Broadcast);
    } else if (channel.banned_rights_ != nullptr) {
      return DialogParticipantStatus(!has_left, std::move(channel.banned_rights_));
    } else if (has_left) {
      return DialogParticipantStatus::Left();
    } else {
      return DialogParticipantStatus::Member();
    }
  }();

  if (is_min) {
    Channel *c = get_channel_force(channel_id);
    if (c != nullptr) {
      LOG(DEBUG) << "Receive known min " << channel_id;

      auto old_join_to_send = get_channel_join_to_send(c);
      auto old_join_request = get_channel_join_request(c);
      on_update_channel_title(c, channel_id, std::move(channel.title_));
      on_update_channel_username(c, channel_id, std::move(channel.username_));
      on_update_channel_photo(c, channel_id, std::move(channel.photo_));
      on_update_channel_default_permissions(c, channel_id, RestrictedRights(channel.default_banned_rights_));
      on_update_channel_has_location(c, channel_id, channel.has_geo_);
      on_update_channel_noforwards(c, channel_id, channel.noforwards_);

      if (c->has_linked_channel != has_linked_channel || c->is_slow_mode_enabled != is_slow_mode_enabled ||
          c->is_megagroup != is_megagroup || c->restriction_reasons != restriction_reasons || c->is_scam != is_scam ||
          c->is_fake != is_fake || c->is_gigagroup != is_gigagroup) {
        c->has_linked_channel = has_linked_channel;
        c->is_slow_mode_enabled = is_slow_mode_enabled;
        c->is_megagroup = is_megagroup;
        c->restriction_reasons = std::move(restriction_reasons);
        c->is_scam = is_scam;
        c->is_fake = is_fake;
        c->is_gigagroup = is_gigagroup;

        c->is_changed = true;
        invalidate_channel_full(channel_id, !c->is_slow_mode_enabled);
      }
      if (c->join_to_send != join_to_send || c->join_request != join_request) {
        c->join_to_send = join_to_send;
        c->join_request = join_request;

        c->need_save_to_database = true;
      }
      // sign_messages isn't known for min-channels
      if (c->is_verified != is_verified) {
        c->is_verified = is_verified;

        c->is_changed = true;
      }
      if (old_join_to_send != get_channel_join_to_send(c) || old_join_request != get_channel_join_request(c)) {
        c->is_changed = true;
      }

      update_channel(c, channel_id);
    } else {
      auto min_channel = td::make_unique<MinChannel>();
      min_channel->photo_ =
          get_dialog_photo(td_->file_manager_.get(), DialogId(channel_id), access_hash, std::move(channel.photo_));
      if (td_->auth_manager_->is_bot()) {
        min_channel->photo_.minithumbnail.clear();
      }
      min_channel->title_ = std::move(channel.title_);
      min_channel->is_megagroup_ = is_megagroup;

      min_channels_.set(channel_id, std::move(min_channel));
    }
    return;
  }
  if (!has_access_hash) {
    LOG(ERROR) << "Receive non-min " << channel_id << " without access_hash from " << source;
    return;
  }

  if (status.is_creator()) {
    // to correctly calculate is_ownership_transferred in on_update_channel_status
    get_channel_force(channel_id);
  }

  Channel *c = add_channel(channel_id, "on_channel");
  auto old_join_to_send = get_channel_join_to_send(c);
  auto old_join_request = get_channel_join_request(c);
  if (c->access_hash != access_hash) {
    c->access_hash = access_hash;
    c->need_save_to_database = true;
  }
  on_update_channel_title(c, channel_id, std::move(channel.title_));
  if (c->date != channel.date_) {
    c->date = channel.date_;
    c->is_changed = true;
  }
  on_update_channel_photo(c, channel_id, std::move(channel.photo_));
  on_update_channel_status(c, channel_id, std::move(status));
  on_update_channel_username(c, channel_id, std::move(channel.username_));  // uses status, must be called after
  on_update_channel_default_permissions(c, channel_id, RestrictedRights(channel.default_banned_rights_));
  on_update_channel_has_location(c, channel_id, channel.has_geo_);
  on_update_channel_noforwards(c, channel_id, channel.noforwards_);

  bool need_update_participant_count = have_participant_count && participant_count != c->participant_count;
  if (need_update_participant_count) {
    c->participant_count = participant_count;
    c->is_changed = true;
  }

  bool need_invalidate_channel_full = false;
  if (c->has_linked_channel != has_linked_channel || c->is_slow_mode_enabled != is_slow_mode_enabled ||
      c->is_megagroup != is_megagroup || c->restriction_reasons != restriction_reasons || c->is_scam != is_scam ||
      c->is_fake != is_fake || c->is_gigagroup != is_gigagroup) {
    c->has_linked_channel = has_linked_channel;
    c->is_slow_mode_enabled = is_slow_mode_enabled;
    c->is_megagroup = is_megagroup;
    c->restriction_reasons = std::move(restriction_reasons);
    c->is_scam = is_scam;
    c->is_fake = is_fake;
    c->is_gigagroup = is_gigagroup;
    c->join_to_send = join_to_send;
    c->join_request = join_request;

    c->is_changed = true;
    need_invalidate_channel_full = true;
  }
  if (c->join_to_send != join_to_send || c->join_request != join_request) {
    c->join_to_send = join_to_send;
    c->join_request = join_request;

    c->need_save_to_database = true;
  }
  if (c->is_verified != is_verified || c->sign_messages != sign_messages) {
    c->is_verified = is_verified;
    c->sign_messages = sign_messages;

    c->is_changed = true;
  }
  if (old_join_to_send != get_channel_join_to_send(c) || old_join_request != get_channel_join_request(c)) {
    c->is_changed = true;
  }

  if (c->cache_version != Channel::CACHE_VERSION) {
    c->cache_version = Channel::CACHE_VERSION;
    c->need_save_to_database = true;
  }
  c->is_received_from_server = true;
  update_channel(c, channel_id);

  if (need_update_participant_count) {
    auto channel_full = get_channel_full(channel_id, true, "on_chat_update");
    if (channel_full != nullptr && channel_full->participant_count != participant_count) {
      channel_full->participant_count = participant_count;
      channel_full->is_changed = true;
      update_channel_full(channel_full, channel_id, "on_chat_update");
    }
  }

  if (need_invalidate_channel_full) {
    invalidate_channel_full(channel_id, !c->is_slow_mode_enabled);
  }

  bool has_active_group_call = (channel.flags_ & CHANNEL_FLAG_HAS_ACTIVE_GROUP_CALL) != 0;
  bool is_group_call_empty = (channel.flags_ & CHANNEL_FLAG_IS_GROUP_CALL_NON_EMPTY) == 0;
  td_->messages_manager_->on_update_dialog_group_call(DialogId(channel_id), has_active_group_call, is_group_call_empty,
                                                      "receive channel");
}

void ContactsManager::on_chat_update(telegram_api::channelForbidden &channel, const char *source) {
  ChannelId channel_id(channel.id_);
  if (!channel_id.is_valid()) {
    LOG(ERROR) << "Receive invalid " << channel_id << " from " << source << ": " << to_string(channel);
    return;
  }

  if (channel.flags_ == 0 && channel.access_hash_ == 0 && channel.title_.empty()) {
    Channel *c = get_channel_force(channel_id);
    LOG(ERROR) << "Receive empty " << to_string(channel) << " from " << source << ", have "
               << to_string(get_supergroup_object(channel_id, c));
    if (c == nullptr && !have_min_channel(channel_id)) {
      min_channels_.set(channel_id, td::make_unique<MinChannel>());
    }
    return;
  }

  Channel *c = add_channel(channel_id, "on_channel_forbidden");
  auto old_join_to_send = get_channel_join_to_send(c);
  auto old_join_request = get_channel_join_request(c);
  if (c->access_hash != channel.access_hash_) {
    c->access_hash = channel.access_hash_;
    c->need_save_to_database = true;
  }
  on_update_channel_title(c, channel_id, std::move(channel.title_));
  on_update_channel_photo(c, channel_id, nullptr);
  if (c->date != 0) {
    c->date = 0;
    c->is_changed = true;
  }
  int32 unban_date = (channel.flags_ & CHANNEL_FLAG_HAS_UNBAN_DATE) != 0 ? channel.until_date_ : 0;
  on_update_channel_status(c, channel_id, DialogParticipantStatus::Banned(unban_date));
  // on_update_channel_username(c, channel_id, "");  // don't know if channel username is empty, so don't update it
  tl_object_ptr<telegram_api::chatBannedRights> banned_rights;  // == nullptr
  on_update_channel_default_permissions(c, channel_id, RestrictedRights(banned_rights));
  // on_update_channel_has_location(c, channel_id, false);
  on_update_channel_noforwards(c, channel_id, false);
  td_->messages_manager_->on_update_dialog_group_call(DialogId(channel_id), false, false, "receive channelForbidden");

  bool sign_messages = false;
  bool join_to_send = false;
  bool join_request = false;
  bool is_slow_mode_enabled = false;
  bool is_megagroup = (channel.flags_ & CHANNEL_FLAG_IS_MEGAGROUP) != 0;
  bool is_verified = false;
  bool is_scam = false;
  bool is_fake = false;

  {
    bool is_broadcast = (channel.flags_ & CHANNEL_FLAG_IS_BROADCAST) != 0;
    LOG_IF(ERROR, is_broadcast == is_megagroup)
        << "Receive wrong channel flag is_broadcast == is_megagroup == " << is_megagroup << " from " << source << ": "
        << oneline(to_string(channel));
  }

  if (is_megagroup) {
    sign_messages = true;
  }

  bool need_invalidate_channel_full = false;
  if (c->is_slow_mode_enabled != is_slow_mode_enabled || c->is_megagroup != is_megagroup ||
      !c->restriction_reasons.empty() || c->is_scam != is_scam || c->is_fake != is_fake ||
      c->join_to_send != join_to_send || c->join_request != join_request) {
    // c->has_linked_channel = has_linked_channel;
    c->is_slow_mode_enabled = is_slow_mode_enabled;
    c->is_megagroup = is_megagroup;
    c->restriction_reasons.clear();
    c->is_scam = is_scam;
    c->is_fake = is_fake;
    c->join_to_send = join_to_send;
    c->join_request = join_request;

    c->is_changed = true;
    need_invalidate_channel_full = true;
  }
  if (c->join_to_send != join_to_send || c->join_request != join_request) {
    c->join_to_send = join_to_send;
    c->join_request = join_request;

    c->need_save_to_database = true;
  }
  if (c->sign_messages != sign_messages || c->is_verified != is_verified) {
    c->sign_messages = sign_messages;
    c->is_verified = is_verified;

    c->is_changed = true;
  }
  if (old_join_to_send != get_channel_join_to_send(c) || old_join_request != get_channel_join_request(c)) {
    c->is_changed = true;
  }

  bool need_drop_participant_count = c->participant_count != 0;
  if (need_drop_participant_count) {
    c->participant_count = 0;
    c->is_changed = true;
  }

  if (c->cache_version != Channel::CACHE_VERSION) {
    c->cache_version = Channel::CACHE_VERSION;
    c->need_save_to_database = true;
  }
  c->is_received_from_server = true;
  update_channel(c, channel_id);

  if (need_drop_participant_count) {
    auto channel_full = get_channel_full(channel_id, true, "on_chat_update");
    if (channel_full != nullptr && channel_full->participant_count != 0) {
      channel_full->participant_count = 0;
      channel_full->administrator_count = 0;
      channel_full->is_changed = true;
      update_channel_full(channel_full, channel_id, "on_chat_update 2");
    }
  }
  if (need_invalidate_channel_full) {
    invalidate_channel_full(channel_id, !c->is_slow_mode_enabled);
  }
}

void ContactsManager::on_upload_profile_photo(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file) {
  auto it = uploaded_profile_photos_.find(file_id);
  CHECK(it != uploaded_profile_photos_.end());

  double main_frame_timestamp = it->second.main_frame_timestamp;
  bool is_animation = it->second.is_animation;
  int32 reupload_count = it->second.reupload_count;
  auto promise = std::move(it->second.promise);

  uploaded_profile_photos_.erase(it);

  LOG(INFO) << "Uploaded " << (is_animation ? "animated" : "static") << " profile photo " << file_id
            << " with reupload_count = " << reupload_count;
  FileView file_view = td_->file_manager_->get_file_view(file_id);
  if (file_view.has_remote_location() && input_file == nullptr) {
    if (file_view.main_remote_location().is_web()) {
      return promise.set_error(Status::Error(400, "Can't use web photo as profile photo"));
    }
    if (reupload_count == 3) {  // upload, ForceReupload repair file reference, reupload
      return promise.set_error(Status::Error(400, "Failed to reupload the file"));
    }

    // delete file reference and forcely reupload the file
    if (is_animation) {
      CHECK(file_view.get_type() == FileType::Animation);
      LOG_CHECK(file_view.main_remote_location().is_common()) << file_view.main_remote_location();
    } else {
      CHECK(file_view.get_type() == FileType::Photo);
      LOG_CHECK(file_view.main_remote_location().is_photo()) << file_view.main_remote_location();
    }
    auto file_reference =
        is_animation ? FileManager::extract_file_reference(file_view.main_remote_location().as_input_document())
                     : FileManager::extract_file_reference(file_view.main_remote_location().as_input_photo());
    td_->file_manager_->delete_file_reference(file_id, file_reference);
    upload_profile_photo(file_id, is_animation, main_frame_timestamp, std::move(promise), reupload_count + 1, {-1});
    return;
  }
  CHECK(input_file != nullptr);

  td_->create_handler<UploadProfilePhotoQuery>(std::move(promise))
      ->send(file_id, std::move(input_file), is_animation, main_frame_timestamp);
}

void ContactsManager::on_upload_profile_photo_error(FileId file_id, Status status) {
  LOG(INFO) << "File " << file_id << " has upload error " << status;
  CHECK(status.is_error());

  auto it = uploaded_profile_photos_.find(file_id);
  CHECK(it != uploaded_profile_photos_.end());

  auto promise = std::move(it->second.promise);

  uploaded_profile_photos_.erase(it);

  promise.set_error(std::move(status));  // TODO check that status has valid error code
}

td_api::object_ptr<td_api::UserStatus> ContactsManager::get_user_status_object(UserId user_id, const User *u) const {
  if (u->is_bot) {
    return make_tl_object<td_api::userStatusOnline>(std::numeric_limits<int32>::max());
  }

  int32 was_online = get_user_was_online(u, user_id);
  switch (was_online) {
    case -3:
      return make_tl_object<td_api::userStatusLastMonth>();
    case -2:
      return make_tl_object<td_api::userStatusLastWeek>();
    case -1:
      return make_tl_object<td_api::userStatusRecently>();
    case 0:
      return make_tl_object<td_api::userStatusEmpty>();
    default: {
      int32 time = G()->unix_time();
      if (was_online > time) {
        return make_tl_object<td_api::userStatusOnline>(was_online);
      } else {
        return make_tl_object<td_api::userStatusOffline>(was_online);
      }
    }
  }
}

td_api::object_ptr<td_api::updateUser> ContactsManager::get_update_unknown_user_object(UserId user_id) {
  return td_api::make_object<td_api::updateUser>(td_api::make_object<td_api::user>(
      user_id.get(), "", "", "", "", td_api::make_object<td_api::userStatusEmpty>(), nullptr, nullptr, false, false,
      false, false, false, "", false, false, false, td_api::make_object<td_api::userTypeUnknown>(), "", false));
}

int64 ContactsManager::get_user_id_object(UserId user_id, const char *source) const {
  if (user_id.is_valid() && get_user(user_id) == nullptr && unknown_users_.count(user_id) == 0) {
    LOG(ERROR) << "Have no info about " << user_id << " from " << source;
    unknown_users_.insert(user_id);
    send_closure(G()->td(), &Td::send_update, get_update_unknown_user_object(user_id));
  }
  return user_id.get();
}

tl_object_ptr<td_api::user> ContactsManager::get_user_object(UserId user_id) const {
  return get_user_object(user_id, get_user(user_id));
}

tl_object_ptr<td_api::user> ContactsManager::get_user_object(UserId user_id, const User *u) const {
  if (u == nullptr) {
    return nullptr;
  }
  tl_object_ptr<td_api::UserType> type;
  if (u->is_deleted) {
    type = make_tl_object<td_api::userTypeDeleted>();
  } else if (u->is_bot) {
    type = make_tl_object<td_api::userTypeBot>(u->can_join_groups, u->can_read_all_group_messages, u->is_inline_bot,
                                               u->inline_query_placeholder, u->need_location_bot,
                                               u->can_be_added_to_attach_menu);
  } else {
    type = make_tl_object<td_api::userTypeRegular>();
  }

  auto emoji_status = u->last_sent_emoji_status != 0 ? u->emoji_status.get_emoji_status_object() : nullptr;
  return make_tl_object<td_api::user>(
      user_id.get(), u->first_name, u->last_name, u->username, u->phone_number, get_user_status_object(user_id, u),
      get_profile_photo_object(td_->file_manager_.get(), u->photo), std::move(emoji_status), u->is_contact,
      u->is_mutual_contact, u->is_verified, u->is_premium, u->is_support,
      get_restriction_reason_description(u->restriction_reasons), u->is_scam, u->is_fake, u->is_received,
      std::move(type), u->language_code, u->attach_menu_enabled);
}

vector<int64> ContactsManager::get_user_ids_object(const vector<UserId> &user_ids, const char *source) const {
  return transform(user_ids, [this, source](UserId user_id) { return get_user_id_object(user_id, source); });
}

tl_object_ptr<td_api::users> ContactsManager::get_users_object(int32 total_count,
                                                               const vector<UserId> &user_ids) const {
  if (total_count == -1) {
    total_count = narrow_cast<int32>(user_ids.size());
  }
  return td_api::make_object<td_api::users>(total_count, get_user_ids_object(user_ids, "get_users_object"));
}

tl_object_ptr<td_api::userFullInfo> ContactsManager::get_user_full_info_object(UserId user_id) const {
  return get_user_full_info_object(user_id, get_user_full(user_id));
}

tl_object_ptr<td_api::userFullInfo> ContactsManager::get_user_full_info_object(UserId user_id,
                                                                               const UserFull *user_full) const {
  CHECK(user_full != nullptr);
  td_api::object_ptr<td_api::botInfo> bot_info;
  bool is_bot = is_user_bot(user_id);
  bool is_premium = is_user_premium(user_id);
  td_api::object_ptr<td_api::formattedText> bio_object;
  if (is_bot) {
    auto menu_button = get_bot_menu_button_object(td_, user_full->menu_button.get());
    auto commands =
        transform(user_full->commands, [](const auto &command) { return command.get_bot_command_object(); });
    bot_info = td_api::make_object<td_api::botInfo>(
        user_full->about, user_full->description,
        get_photo_object(td_->file_manager_.get(), user_full->description_photo),
        td_->animations_manager_->get_animation_object(user_full->description_animation_file_id),
        std::move(menu_button), std::move(commands),
        user_full->group_administrator_rights == AdministratorRights()
            ? nullptr
            : user_full->group_administrator_rights.get_chat_administrator_rights_object(),
        user_full->broadcast_administrator_rights == AdministratorRights()
            ? nullptr
            : user_full->broadcast_administrator_rights.get_chat_administrator_rights_object());
  } else {
    FormattedText bio;
    bio.text = user_full->about;
    bio.entities = find_entities(bio.text, true, true);
    if (!is_user_premium(user_id)) {
      td::remove_if(bio.entities, [&](const MessageEntity &entity) {
        if (entity.type == MessageEntity::Type::EmailAddress) {
          return true;
        }
        if (entity.type == MessageEntity::Type::Url &&
            !LinkManager::is_internal_link(utf8_utf16_substr(bio.text, entity.offset, entity.length))) {
          return true;
        }
        return false;
      });
    }
    bio_object = get_formatted_text_object(bio, true, 0);
  }
  auto voice_messages_forbidden = is_premium ? user_full->voice_messages_forbidden : false;
  return make_tl_object<td_api::userFullInfo>(get_chat_photo_object(td_->file_manager_.get(), user_full->photo),
                                              user_full->is_blocked, user_full->can_be_called,
                                              user_full->supports_video_calls, user_full->has_private_calls,
                                              !user_full->private_forward_name.empty(), voice_messages_forbidden,
                                              user_full->need_phone_number_privacy_exception, std::move(bio_object),
                                              get_premium_payment_options_object(user_full->premium_gift_options),
                                              user_full->common_chat_count, std::move(bot_info));
}

td_api::object_ptr<td_api::updateBasicGroup> ContactsManager::get_update_unknown_basic_group_object(ChatId chat_id) {
  return td_api::make_object<td_api::updateBasicGroup>(td_api::make_object<td_api::basicGroup>(
      chat_id.get(), 0, DialogParticipantStatus::Banned(0).get_chat_member_status_object(), true, 0));
}

int64 ContactsManager::get_basic_group_id_object(ChatId chat_id, const char *source) const {
  if (chat_id.is_valid() && get_chat(chat_id) == nullptr && unknown_chats_.count(chat_id) == 0) {
    LOG(ERROR) << "Have no info about " << chat_id << " from " << source;
    unknown_chats_.insert(chat_id);
    send_closure(G()->td(), &Td::send_update, get_update_unknown_basic_group_object(chat_id));
  }
  return chat_id.get();
}

tl_object_ptr<td_api::basicGroup> ContactsManager::get_basic_group_object(ChatId chat_id) {
  return get_basic_group_object(chat_id, get_chat(chat_id));
}

tl_object_ptr<td_api::basicGroup> ContactsManager::get_basic_group_object(ChatId chat_id, const Chat *c) {
  if (c == nullptr) {
    return nullptr;
  }
  if (c->migrated_to_channel_id.is_valid()) {
    get_channel_force(c->migrated_to_channel_id);
  }
  return get_basic_group_object_const(chat_id, c);
}

tl_object_ptr<td_api::basicGroup> ContactsManager::get_basic_group_object_const(ChatId chat_id, const Chat *c) const {
  return make_tl_object<td_api::basicGroup>(
      chat_id.get(), c->participant_count, get_chat_status(c).get_chat_member_status_object(), c->is_active,
      get_supergroup_id_object(c->migrated_to_channel_id, "get_basic_group_object"));
}

tl_object_ptr<td_api::basicGroupFullInfo> ContactsManager::get_basic_group_full_info_object(ChatId chat_id) const {
  return get_basic_group_full_info_object(get_chat_full(chat_id));
}

tl_object_ptr<td_api::basicGroupFullInfo> ContactsManager::get_basic_group_full_info_object(
    const ChatFull *chat_full) const {
  CHECK(chat_full != nullptr);
  auto bot_commands = transform(chat_full->bot_commands, [td = td_](const BotCommands &commands) {
    return commands.get_bot_commands_object(td);
  });
  return make_tl_object<td_api::basicGroupFullInfo>(
      get_chat_photo_object(td_->file_manager_.get(), chat_full->photo), chat_full->description,
      get_user_id_object(chat_full->creator_user_id, "basicGroupFullInfo"),
      transform(chat_full->participants,
                [this](const DialogParticipant &chat_participant) { return get_chat_member_object(chat_participant); }),
      chat_full->invite_link.get_chat_invite_link_object(this), std::move(bot_commands));
}

td_api::object_ptr<td_api::updateSupergroup> ContactsManager::get_update_unknown_supergroup_object(
    ChannelId channel_id) const {
  auto min_channel = get_min_channel(channel_id);
  bool is_megagroup = min_channel == nullptr ? false : min_channel->is_megagroup_;
  return td_api::make_object<td_api::updateSupergroup>(td_api::make_object<td_api::supergroup>(
      channel_id.get(), string(), 0, DialogParticipantStatus::Banned(0).get_chat_member_status_object(), 0, false,
      false, false, !is_megagroup, false, false, !is_megagroup, false, false, string(), false, false));
}

int64 ContactsManager::get_supergroup_id_object(ChannelId channel_id, const char *source) const {
  if (channel_id.is_valid() && get_channel(channel_id) == nullptr && unknown_channels_.count(channel_id) == 0) {
    if (have_min_channel(channel_id)) {
      LOG(INFO) << "Have only min " << channel_id << " received from " << source;
    } else {
      LOG(ERROR) << "Have no info about " << channel_id << " received from " << source;
    }
    unknown_channels_.insert(channel_id);
    send_closure(G()->td(), &Td::send_update, get_update_unknown_supergroup_object(channel_id));
  }
  return channel_id.get();
}

tl_object_ptr<td_api::supergroup> ContactsManager::get_supergroup_object(ChannelId channel_id) const {
  return get_supergroup_object(channel_id, get_channel(channel_id));
}

tl_object_ptr<td_api::supergroup> ContactsManager::get_supergroup_object(ChannelId channel_id, const Channel *c) {
  if (c == nullptr) {
    return nullptr;
  }
  return td_api::make_object<td_api::supergroup>(
      channel_id.get(), c->username, c->date, get_channel_status(c).get_chat_member_status_object(),
      c->participant_count, c->has_linked_channel, c->has_location, c->sign_messages, get_channel_join_to_send(c),
      get_channel_join_request(c), c->is_slow_mode_enabled, !c->is_megagroup, c->is_gigagroup, c->is_verified,
      get_restriction_reason_description(c->restriction_reasons), c->is_scam, c->is_fake);
}

tl_object_ptr<td_api::supergroupFullInfo> ContactsManager::get_supergroup_full_info_object(ChannelId channel_id) const {
  return get_supergroup_full_info_object(get_channel_full(channel_id), channel_id);
}

tl_object_ptr<td_api::supergroupFullInfo> ContactsManager::get_supergroup_full_info_object(
    const ChannelFull *channel_full, ChannelId channel_id) const {
  CHECK(channel_full != nullptr);
  double slow_mode_delay_expires_in = 0;
  if (channel_full->slow_mode_next_send_date != 0) {
    slow_mode_delay_expires_in = max(channel_full->slow_mode_next_send_date - G()->server_time(), 1e-3);
  }
  auto bot_commands = transform(channel_full->bot_commands, [td = td_](const BotCommands &commands) {
    return commands.get_bot_commands_object(td);
  });
  return td_api::make_object<td_api::supergroupFullInfo>(
      get_chat_photo_object(td_->file_manager_.get(), channel_full->photo), channel_full->description,
      channel_full->participant_count, channel_full->administrator_count, channel_full->restricted_count,
      channel_full->banned_count, DialogId(channel_full->linked_channel_id).get(), channel_full->slow_mode_delay,
      slow_mode_delay_expires_in, channel_full->can_get_participants, channel_full->can_set_username,
      channel_full->can_set_sticker_set, channel_full->can_set_location, channel_full->can_view_statistics,
      channel_full->is_all_history_available, channel_full->sticker_set_id.get(),
      channel_full->location.get_chat_location_object(), channel_full->invite_link.get_chat_invite_link_object(this),
      std::move(bot_commands),
      get_basic_group_id_object(channel_full->migrated_from_chat_id, "get_supergroup_full_info_object"),
      channel_full->migrated_from_max_message_id.get());
}

tl_object_ptr<td_api::SecretChatState> ContactsManager::get_secret_chat_state_object(SecretChatState state) {
  switch (state) {
    case SecretChatState::Waiting:
      return make_tl_object<td_api::secretChatStatePending>();
    case SecretChatState::Active:
      return make_tl_object<td_api::secretChatStateReady>();
    case SecretChatState::Closed:
    case SecretChatState::Unknown:
      return make_tl_object<td_api::secretChatStateClosed>();
    default:
      UNREACHABLE();
      return nullptr;
  }
}

td_api::object_ptr<td_api::updateSecretChat> ContactsManager::get_update_unknown_secret_chat_object(
    SecretChatId secret_chat_id) {
  return td_api::make_object<td_api::updateSecretChat>(td_api::make_object<td_api::secretChat>(
      secret_chat_id.get(), 0, get_secret_chat_state_object(SecretChatState::Unknown), false, string(), 0));
}

int32 ContactsManager::get_secret_chat_id_object(SecretChatId secret_chat_id, const char *source) const {
  if (secret_chat_id.is_valid() && get_secret_chat(secret_chat_id) == nullptr &&
      unknown_secret_chats_.count(secret_chat_id) == 0) {
    LOG(ERROR) << "Have no info about " << secret_chat_id << " from " << source;
    unknown_secret_chats_.insert(secret_chat_id);
    send_closure(G()->td(), &Td::send_update, get_update_unknown_secret_chat_object(secret_chat_id));
  }
  return secret_chat_id.get();
}

tl_object_ptr<td_api::secretChat> ContactsManager::get_secret_chat_object(SecretChatId secret_chat_id) {
  return get_secret_chat_object(secret_chat_id, get_secret_chat(secret_chat_id));
}

tl_object_ptr<td_api::secretChat> ContactsManager::get_secret_chat_object(SecretChatId secret_chat_id,
                                                                          const SecretChat *secret_chat) {
  if (secret_chat == nullptr) {
    return nullptr;
  }
  get_user_force(secret_chat->user_id);
  return get_secret_chat_object_const(secret_chat_id, secret_chat);
}

tl_object_ptr<td_api::secretChat> ContactsManager::get_secret_chat_object_const(SecretChatId secret_chat_id,
                                                                                const SecretChat *secret_chat) const {
  return td_api::make_object<td_api::secretChat>(secret_chat_id.get(),
                                                 get_user_id_object(secret_chat->user_id, "secretChat"),
                                                 get_secret_chat_state_object(secret_chat->state),
                                                 secret_chat->is_outbound, secret_chat->key_hash, secret_chat->layer);
}

tl_object_ptr<td_api::chatInviteLinkInfo> ContactsManager::get_chat_invite_link_info_object(const string &invite_link) {
  auto it = invite_link_infos_.find(invite_link);
  if (it == invite_link_infos_.end()) {
    return nullptr;
  }

  auto invite_link_info = it->second.get();
  CHECK(invite_link_info != nullptr);

  DialogId dialog_id = invite_link_info->dialog_id;
  string title;
  const DialogPhoto *photo = nullptr;
  DialogPhoto invite_link_photo;
  string description;
  int32 participant_count = 0;
  vector<int64> member_user_ids;
  bool creates_join_request = false;
  bool is_public = false;
  bool is_member = false;
  td_api::object_ptr<td_api::ChatType> chat_type;

  if (dialog_id.is_valid()) {
    switch (dialog_id.get_type()) {
      case DialogType::Chat: {
        auto chat_id = dialog_id.get_chat_id();
        const Chat *c = get_chat(chat_id);

        if (c != nullptr) {
          title = c->title;
          photo = &c->photo;
          participant_count = c->participant_count;
          is_member = c->status.is_member();
        } else {
          LOG(ERROR) << "Have no information about " << chat_id;
        }
        chat_type = td_api::make_object<td_api::chatTypeBasicGroup>(
            get_basic_group_id_object(chat_id, "get_chat_invite_link_info_object"));
        break;
      }
      case DialogType::Channel: {
        auto channel_id = dialog_id.get_channel_id();
        const Channel *c = get_channel(channel_id);

        bool is_megagroup = false;
        if (c != nullptr) {
          title = c->title;
          photo = &c->photo;
          is_public = is_channel_public(c);
          is_megagroup = c->is_megagroup;
          participant_count = c->participant_count;
          is_member = c->status.is_member();
        } else {
          LOG(ERROR) << "Have no information about " << channel_id;
        }
        chat_type = td_api::make_object<td_api::chatTypeSupergroup>(
            get_supergroup_id_object(channel_id, "get_chat_invite_link_info_object"), !is_megagroup);
        break;
      }
      default:
        UNREACHABLE();
    }
    description = get_dialog_about(dialog_id);
  } else {
    title = invite_link_info->title;
    invite_link_photo = as_fake_dialog_photo(invite_link_info->photo, dialog_id);
    photo = &invite_link_photo;
    description = invite_link_info->description;
    participant_count = invite_link_info->participant_count;
    member_user_ids = get_user_ids_object(invite_link_info->participant_user_ids, "get_chat_invite_link_info_object");
    creates_join_request = invite_link_info->creates_join_request;
    is_public = invite_link_info->is_public;

    if (invite_link_info->is_chat) {
      chat_type = td_api::make_object<td_api::chatTypeBasicGroup>(0);
    } else {
      chat_type = td_api::make_object<td_api::chatTypeSupergroup>(0, !invite_link_info->is_megagroup);
    }
  }

  if (dialog_id.is_valid()) {
    td_->messages_manager_->force_create_dialog(dialog_id, "get_chat_invite_link_info_object");
  }
  int32 accessible_for = 0;
  if (dialog_id.is_valid() && !is_member) {
    auto access_it = dialog_access_by_invite_link_.find(dialog_id);
    if (access_it != dialog_access_by_invite_link_.end()) {
      accessible_for = td::max(1, access_it->second.accessible_before - G()->unix_time() - 1);
    }
  }

  return make_tl_object<td_api::chatInviteLinkInfo>(dialog_id.get(), accessible_for, std::move(chat_type), title,
                                                    get_chat_photo_info_object(td_->file_manager_.get(), photo),
                                                    description, participant_count, std::move(member_user_ids),
                                                    creates_join_request, is_public);
}

void ContactsManager::get_support_user(Promise<td_api::object_ptr<td_api::user>> &&promise) {
  if (support_user_id_.is_valid()) {
    return promise.set_value(get_user_object(support_user_id_));
  }

  auto query_promise = PromiseCreator::lambda(
      [actor_id = actor_id(this), promise = std::move(promise)](Result<UserId> &&result) mutable {
        if (result.is_error()) {
          promise.set_error(result.move_as_error());
        } else {
          send_closure(actor_id, &ContactsManager::on_get_support_user, result.move_as_ok(), std::move(promise));
        }
      });
  td_->create_handler<GetSupportUserQuery>(std::move(query_promise))->send();
}

void ContactsManager::on_get_support_user(UserId user_id, Promise<td_api::object_ptr<td_api::user>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  const User *u = get_user(user_id);
  if (u == nullptr) {
    return promise.set_error(Status::Error(500, "Can't find support user"));
  }
  if (!u->is_support) {
    LOG(ERROR) << "Receive non-support " << user_id << ", but expected a support user";
  }

  support_user_id_ = user_id;
  promise.set_value(get_user_object(user_id, u));
}

void ContactsManager::get_current_state(vector<td_api::object_ptr<td_api::Update>> &updates) const {
  for (auto user_id : unknown_users_) {
    if (!have_min_user(user_id)) {
      updates.push_back(get_update_unknown_user_object(user_id));
    }
  }
  for (auto chat_id : unknown_chats_) {
    if (!have_chat(chat_id)) {
      updates.push_back(get_update_unknown_basic_group_object(chat_id));
    }
  }
  for (auto channel_id : unknown_channels_) {
    if (!have_channel(channel_id)) {
      updates.push_back(get_update_unknown_supergroup_object(channel_id));
    }
  }
  for (auto secret_chat_id : unknown_secret_chats_) {
    if (!have_secret_chat(secret_chat_id)) {
      updates.push_back(get_update_unknown_secret_chat_object(secret_chat_id));
    }
  }

  users_.foreach([&](const UserId &user_id, const unique_ptr<User> &user) {
    updates.push_back(td_api::make_object<td_api::updateUser>(get_user_object(user_id, user.get())));
  });
  channels_.foreach([&](const ChannelId &channel_id, const unique_ptr<Channel> &channel) {
    updates.push_back(td_api::make_object<td_api::updateSupergroup>(get_supergroup_object(channel_id, channel.get())));
  });
  // chat objects can contain channel_id, so they must be sent after channels
  chats_.foreach([&](const ChatId &chat_id, const unique_ptr<Chat> &chat) {
    updates.push_back(td_api::make_object<td_api::updateBasicGroup>(get_basic_group_object_const(chat_id, chat.get())));
  });
  // secret chat objects contain user_id, so they must be sent after users
  secret_chats_.foreach([&](const SecretChatId &secret_chat_id, const unique_ptr<SecretChat> &secret_chat) {
    updates.push_back(
        td_api::make_object<td_api::updateSecretChat>(get_secret_chat_object_const(secret_chat_id, secret_chat.get())));
  });

  users_full_.foreach([&](const UserId &user_id, const unique_ptr<UserFull> &user_full) {
    updates.push_back(td_api::make_object<td_api::updateUserFullInfo>(
        user_id.get(), get_user_full_info_object(user_id, user_full.get())));
  });
  channels_full_.foreach([&](const ChannelId &channel_id, const unique_ptr<ChannelFull> &channel_full) {
    updates.push_back(td_api::make_object<td_api::updateSupergroupFullInfo>(
        channel_id.get(), get_supergroup_full_info_object(channel_full.get(), channel_id)));
  });
  chats_full_.foreach([&](const ChatId &chat_id, const unique_ptr<ChatFull> &chat_full) {
    updates.push_back(td_api::make_object<td_api::updateBasicGroupFullInfo>(
        chat_id.get(), get_basic_group_full_info_object(chat_full.get())));
  });
}

}  // namespace td
