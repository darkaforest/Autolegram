//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/ConnectionState.h"
#include "td/telegram/files/FileId.h"
#include "td/telegram/net/MtprotoHeader.h"
#include "td/telegram/net/NetQuery.h"
#include "td/telegram/net/NetQueryStats.h"
#include "td/telegram/td_api.h"
#include "td/telegram/TdCallback.h"
#include "td/telegram/TdDb.h"
#include "td/telegram/TdParameters.h"
#include "td/telegram/telegram_api.h"
#include "td/telegram/TermsOfService.h"

#include "td/db/DbKey.h"

#include "td/actor/actor.h"
#include "td/actor/MultiTimeout.h"

#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/Container.h"
#include "td/utils/FlatHashMap.h"
#include "td/utils/logging.h"
#include "td/utils/Promise.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"

#include <memory>
#include <unordered_map>
#include <utility>

namespace td {

class AnimationsManager;
class AttachMenuManager;
class AudiosManager;
class AuthManager;
class BackgroundManager;
class CallManager;
class CallbackQueriesManager;
class ConfigManager;
class ContactsManager;
class CountryInfoManager;
class DeviceTokenManager;
class DocumentsManager;
class DownloadManager;
class FileManager;
class FileReferenceManager;
class GameManager;
class GroupCallManager;
class InlineQueriesManager;
class HashtagHints;
class LanguagePackManager;
class LinkManager;
class MessagesManager;
class NetStatsManager;
class NotificationManager;
class NotificationSettingsManager;
class OptionManager;
class PasswordManager;
class PhoneNumberManager;
class PollManager;
class PrivacyManager;
class SecureManager;
class SecretChatsManager;
class SponsoredMessageManager;
class StateManager;
class StickersManager;
class StorageManager;
class ThemeManager;
class TopDialogManager;
class UpdatesManager;
class VideoNotesManager;
class VideosManager;
class VoiceNotesManager;
class WebPagesManager;

extern int VERBOSITY_NAME(td_init);
extern int VERBOSITY_NAME(td_requests);

// Td may start closing after explicit "close" or "destroy" query.
// Or it may start closing by itself, because authorization is lost.
// It any case the parent will be notified via updateAuthorizationState.
//
// Td needs a way to know that it will receive no more queries.
// It happens after "hangup".
//
// Parent needs a way to know that it will receive no more updates.
// It happens after destruction of callback or after on_closed.
class Td final : public Actor {
 public:
  Td(const Td &) = delete;
  Td(Td &&) = delete;
  Td &operator=(const Td &) = delete;
  Td &operator=(Td &&) = delete;
  ~Td() final;

  struct Options {
    std::shared_ptr<NetQueryStats> net_query_stats;
  };

  Td(unique_ptr<TdCallback> callback, Options options);

  void request(uint64 id, tl_object_ptr<td_api::Function> function);

  void destroy();

  void schedule_get_terms_of_service(int32 expires_in);

  void schedule_get_promo_data(int32 expires_in);

  void on_update(BufferSlice &&update);

  void on_result(NetQueryPtr query);

  void on_online_updated(bool force, bool send_update);

  void on_update_status_success(bool is_online);

  bool is_online() const;

  void set_is_online(bool is_online);

  void set_is_bot_online(bool is_bot_online);

  unique_ptr<AudiosManager> audios_manager_;
  unique_ptr<CallbackQueriesManager> callback_queries_manager_;
  unique_ptr<DocumentsManager> documents_manager_;
  unique_ptr<OptionManager> option_manager_;
  unique_ptr<VideoNotesManager> video_notes_manager_;
  unique_ptr<VideosManager> videos_manager_;

  unique_ptr<AnimationsManager> animations_manager_;
  ActorOwn<AnimationsManager> animations_manager_actor_;
  unique_ptr<AttachMenuManager> attach_menu_manager_;
  ActorOwn<AttachMenuManager> attach_menu_manager_actor_;
  unique_ptr<AuthManager> auth_manager_;
  ActorOwn<AuthManager> auth_manager_actor_;
  unique_ptr<BackgroundManager> background_manager_;
  ActorOwn<BackgroundManager> background_manager_actor_;
  unique_ptr<ContactsManager> contacts_manager_;
  ActorOwn<ContactsManager> contacts_manager_actor_;
  unique_ptr<CountryInfoManager> country_info_manager_;
  ActorOwn<CountryInfoManager> country_info_manager_actor_;
  unique_ptr<DownloadManager> download_manager_;
  ActorOwn<DownloadManager> download_manager_actor_;
  unique_ptr<FileManager> file_manager_;
  ActorOwn<FileManager> file_manager_actor_;
  unique_ptr<FileReferenceManager> file_reference_manager_;
  ActorOwn<FileReferenceManager> file_reference_manager_actor_;
  unique_ptr<GameManager> game_manager_;
  ActorOwn<GameManager> game_manager_actor_;
  unique_ptr<GroupCallManager> group_call_manager_;
  ActorOwn<GroupCallManager> group_call_manager_actor_;
  unique_ptr<InlineQueriesManager> inline_queries_manager_;
  ActorOwn<InlineQueriesManager> inline_queries_manager_actor_;
  unique_ptr<LinkManager> link_manager_;
  ActorOwn<LinkManager> link_manager_actor_;
  unique_ptr<MessagesManager> messages_manager_;
  ActorOwn<MessagesManager> messages_manager_actor_;
  unique_ptr<NotificationManager> notification_manager_;
  ActorOwn<NotificationManager> notification_manager_actor_;
  unique_ptr<NotificationSettingsManager> notification_settings_manager_;
  ActorOwn<NotificationSettingsManager> notification_settings_manager_actor_;
  unique_ptr<PollManager> poll_manager_;
  ActorOwn<PollManager> poll_manager_actor_;
  unique_ptr<SponsoredMessageManager> sponsored_message_manager_;
  ActorOwn<SponsoredMessageManager> sponsored_message_manager_actor_;
  unique_ptr<StickersManager> stickers_manager_;
  ActorOwn<StickersManager> stickers_manager_actor_;
  unique_ptr<ThemeManager> theme_manager_;
  ActorOwn<ThemeManager> theme_manager_actor_;
  unique_ptr<TopDialogManager> top_dialog_manager_;
  ActorOwn<TopDialogManager> top_dialog_manager_actor_;
  unique_ptr<UpdatesManager> updates_manager_;
  ActorOwn<UpdatesManager> updates_manager_actor_;
  unique_ptr<VoiceNotesManager> voice_notes_manager_;
  ActorOwn<VoiceNotesManager> voice_notes_manager_actor_;
  unique_ptr<WebPagesManager> web_pages_manager_;
  ActorOwn<WebPagesManager> web_pages_manager_actor_;

  ActorOwn<CallManager> call_manager_;
  ActorOwn<PhoneNumberManager> change_phone_number_manager_;
  ActorOwn<ConfigManager> config_manager_;
  ActorOwn<PhoneNumberManager> confirm_phone_number_manager_;
  ActorOwn<DeviceTokenManager> device_token_manager_;
  ActorOwn<HashtagHints> hashtag_hints_;
  ActorOwn<LanguagePackManager> language_pack_manager_;
  ActorOwn<NetStatsManager> net_stats_manager_;
  ActorOwn<PasswordManager> password_manager_;
  ActorOwn<PrivacyManager> privacy_manager_;
  ActorOwn<SecureManager> secure_manager_;
  ActorOwn<SecretChatsManager> secret_chats_manager_;
  ActorOwn<StateManager> state_manager_;
  ActorOwn<StorageManager> storage_manager_;
  ActorOwn<PhoneNumberManager> verify_phone_number_manager_;

  class ResultHandler : public std::enable_shared_from_this<ResultHandler> {
   public:
    ResultHandler() = default;
    ResultHandler(const ResultHandler &) = delete;
    ResultHandler &operator=(const ResultHandler &) = delete;
    virtual ~ResultHandler() = default;

    virtual void on_result(BufferSlice packet) {
      UNREACHABLE();
    }

    virtual void on_error(Status status) {
      UNREACHABLE();
    }

    friend class Td;

   protected:
    void send_query(NetQueryPtr query);

    Td *td_ = nullptr;
    bool is_query_sent_ = false;

   private:
    void set_td(Td *td);
  };

  template <class HandlerT, class... Args>
  std::shared_ptr<HandlerT> create_handler(Args &&...args) {
    LOG_CHECK(close_flag_ < 2) << close_flag_
#if TD_CLANG || TD_GCC
                               << ' ' << __PRETTY_FUNCTION__
#endif
        ;
    auto ptr = std::make_shared<HandlerT>(std::forward<Args>(args)...);
    ptr->set_td(this);
    return ptr;
  }

  void send_update(tl_object_ptr<td_api::Update> &&object);

  static td_api::object_ptr<td_api::Object> static_request(td_api::object_ptr<td_api::Function> function);

 private:
  static constexpr int64 ONLINE_ALARM_ID = 0;
  static constexpr int64 PING_SERVER_ALARM_ID = -1;
  static constexpr int32 PING_SERVER_TIMEOUT = 300;
  static constexpr int64 TERMS_OF_SERVICE_ALARM_ID = -2;
  static constexpr int64 PROMO_DATA_ALARM_ID = -3;

  void on_connection_state_changed(ConnectionState new_state);

  void run_request(uint64 id, tl_object_ptr<td_api::Function> function);

  void send_result(uint64 id, tl_object_ptr<td_api::Object> object);
  void send_error(uint64 id, Status error);
  void send_error_impl(uint64 id, tl_object_ptr<td_api::error> error);
  void send_error_raw(uint64 id, int32 code, CSlice error);
  void answer_ok_query(uint64 id, Status status);

  ActorShared<Td> create_reference();

  void inc_actor_refcnt();
  void dec_actor_refcnt();

  void inc_request_actor_refcnt();
  void dec_request_actor_refcnt();

  void close();
  void on_closed();

  void dec_stop_cnt();

  unique_ptr<TdCallback> callback_;
  Options td_options_;

  MtprotoHeader::Options options_;

  TdParameters parameters_;

  ConnectionState connection_state_ = ConnectionState::Empty;

  std::unordered_multimap<uint64, int32> request_set_;
  int actor_refcnt_ = 0;
  int request_actor_refcnt_ = 0;
  int stop_cnt_ = 2;
  bool destroy_flag_ = false;
  int close_flag_ = 0;

  enum class State : int32 { WaitParameters, Run, Close } state_ = State::WaitParameters;
  uint64 set_parameters_request_id_ = 0;

  FlatHashMap<uint64, std::shared_ptr<ResultHandler>> result_handlers_;
  enum : int8 { RequestActorIdType = 1, ActorIdType = 2 };
  Container<ActorOwn<Actor>> request_actors_;

  bool is_online_ = false;
  bool is_bot_online_ = false;
  NetQueryRef update_status_query_;

  int64 alarm_id_ = 1;
  FlatHashMap<int64, uint64> pending_alarms_;
  MultiTimeout alarm_timeout_{"AlarmTimeout"};

  TermsOfService pending_terms_of_service_;

  struct DownloadInfo {
    int64 offset = -1;
    int64 limit = -1;
    vector<uint64> request_ids;
  };
  FlatHashMap<FileId, DownloadInfo, FileIdHash> pending_file_downloads_;

  vector<std::pair<uint64, td_api::object_ptr<td_api::Function>>> pending_preauthentication_requests_;

  vector<std::pair<uint64, td_api::object_ptr<td_api::Function>>> pending_set_parameters_requests_;
  vector<std::pair<uint64, td_api::object_ptr<td_api::Function>>> pending_init_requests_;

  template <class T>
  void complete_pending_preauthentication_requests(const T &func);

  td_api::object_ptr<td_api::AuthorizationState> get_fake_authorization_state_object() const;

  vector<td_api::object_ptr<td_api::Update>> get_fake_current_state() const;

  static void on_alarm_timeout_callback(void *td_ptr, int64 alarm_id);
  void on_alarm_timeout(int64 alarm_id);

  td_api::object_ptr<td_api::updateTermsOfService> get_update_terms_of_service_object() const;

  void on_get_terms_of_service(Result<std::pair<int32, TermsOfService>> result, bool dummy);

  void on_get_promo_data(Result<telegram_api::object_ptr<telegram_api::help_PromoData>> r_promo_data, bool dummy);

  template <class T>
  friend class RequestActor;        // uses send_result/send_error
  friend class AuthManager;         // uses send_result/send_error, TODO pass Promise<>
  friend class PhoneNumberManager;  // uses send_result/send_error, TODO pass Promise<>

  void add_handler(uint64 id, std::shared_ptr<ResultHandler> handler);
  std::shared_ptr<ResultHandler> extract_handler(uint64 id);

  void clear_requests();

  void on_file_download_finished(FileId file_id);

  class OnRequest;

  class DownloadFileCallback;

  std::shared_ptr<DownloadFileCallback> download_file_callback_;

  class UploadFileCallback;

  std::shared_ptr<UploadFileCallback> upload_file_callback_;

  std::shared_ptr<ActorContext> old_context_;

  static int *get_log_verbosity_level(Slice name);

  template <class T>
  Promise<T> create_request_promise(uint64 id) {
    return PromiseCreator::lambda([id = id, actor_id = actor_id(this)](Result<T> r_state) {
      if (r_state.is_error()) {
        send_closure(actor_id, &Td::send_error, id, r_state.move_as_error());
      } else {
        send_closure(actor_id, &Td::send_result, id, r_state.move_as_ok());
      }
    });
  }

  Promise<Unit> create_ok_request_promise(uint64 id);

  static bool is_authentication_request(int32 id);

  static bool is_synchronous_request(const td_api::Function *function);

  static bool is_preinitialization_request(int32 id);

  static bool is_preauthentication_request(int32 id);

  template <class T>
  void on_request(uint64 id, const T &request) = delete;

  void on_request(uint64 id, const td_api::setTdlibParameters &request);

  void on_request(uint64 id, const td_api::getAuthorizationState &request);

  void on_request(uint64 id, td_api::setAuthenticationPhoneNumber &request);

  void on_request(uint64 id, td_api::setAuthenticationEmailAddress &request);

  void on_request(uint64 id, const td_api::resendAuthenticationCode &request);

  void on_request(uint64 id, td_api::checkAuthenticationEmailCode &request);

  void on_request(uint64 id, td_api::checkAuthenticationCode &request);

  void on_request(uint64 id, td_api::registerUser &request);

  void on_request(uint64 id, td_api::requestQrCodeAuthentication &request);

  void on_request(uint64 id, td_api::checkAuthenticationPassword &request);

  void on_request(uint64 id, const td_api::requestAuthenticationPasswordRecovery &request);

  void on_request(uint64 id, td_api::checkAuthenticationPasswordRecoveryCode &request);

  void on_request(uint64 id, td_api::recoverAuthenticationPassword &request);

  void on_request(uint64 id, const td_api::logOut &request);

  void on_request(uint64 id, const td_api::close &request);

  void on_request(uint64 id, const td_api::destroy &request);

  void on_request(uint64 id, td_api::checkAuthenticationBotToken &request);

  void on_request(uint64 id, td_api::confirmQrCodeAuthentication &request);

  void on_request(uint64 id, td_api::setDatabaseEncryptionKey &request);

  void on_request(uint64 id, const td_api::getCurrentState &request);

  void on_request(uint64 id, td_api::getPasswordState &request);

  void on_request(uint64 id, td_api::setPassword &request);

  void on_request(uint64 id, td_api::setLoginEmailAddress &request);

  void on_request(uint64 id, const td_api::resendLoginEmailAddressCode &request);

  void on_request(uint64 id, td_api::checkLoginEmailAddressCode &request);

  void on_request(uint64 id, td_api::getRecoveryEmailAddress &request);

  void on_request(uint64 id, td_api::setRecoveryEmailAddress &request);

  void on_request(uint64 id, td_api::checkRecoveryEmailAddressCode &request);

  void on_request(uint64 id, const td_api::resendRecoveryEmailAddressCode &request);

  void on_request(uint64 id, td_api::requestPasswordRecovery &request);

  void on_request(uint64 id, td_api::checkPasswordRecoveryCode &request);

  void on_request(uint64 id, td_api::recoverPassword &request);

  void on_request(uint64 id, const td_api::resetPassword &request);

  void on_request(uint64 id, const td_api::cancelPasswordReset &request);

  void on_request(uint64 id, td_api::getTemporaryPasswordState &request);

  void on_request(uint64 id, td_api::createTemporaryPassword &request);

  void on_request(uint64 id, td_api::processPushNotification &request);

  void on_request(uint64 id, td_api::registerDevice &request);

  void on_request(uint64 id, td_api::getUserPrivacySettingRules &request);

  void on_request(uint64 id, td_api::setUserPrivacySettingRules &request);

  void on_request(uint64 id, const td_api::getAccountTtl &request);

  void on_request(uint64 id, const td_api::setAccountTtl &request);

  void on_request(uint64 id, td_api::deleteAccount &request);

  void on_request(uint64 id, td_api::changePhoneNumber &request);

  void on_request(uint64 id, td_api::checkChangePhoneNumberCode &request);

  void on_request(uint64 id, td_api::resendChangePhoneNumberCode &request);

  void on_request(uint64 id, const td_api::getActiveSessions &request);

  void on_request(uint64 id, const td_api::terminateSession &request);

  void on_request(uint64 id, const td_api::terminateAllOtherSessions &request);

  void on_request(uint64 id, const td_api::toggleSessionCanAcceptCalls &request);

  void on_request(uint64 id, const td_api::toggleSessionCanAcceptSecretChats &request);

  void on_request(uint64 id, const td_api::setInactiveSessionTtl &request);

  void on_request(uint64 id, const td_api::getConnectedWebsites &request);

  void on_request(uint64 id, const td_api::disconnectWebsite &request);

  void on_request(uint64 id, const td_api::disconnectAllWebsites &request);

  void on_request(uint64 id, const td_api::getMe &request);

  void on_request(uint64 id, const td_api::getUser &request);

  void on_request(uint64 id, const td_api::getUserFullInfo &request);

  void on_request(uint64 id, const td_api::getBasicGroup &request);

  void on_request(uint64 id, const td_api::getBasicGroupFullInfo &request);

  void on_request(uint64 id, const td_api::getSupergroup &request);

  void on_request(uint64 id, const td_api::getSupergroupFullInfo &request);

  void on_request(uint64 id, const td_api::getSecretChat &request);

  void on_request(uint64 id, const td_api::getChat &request);

  void on_request(uint64 id, const td_api::getMessage &request);

  void on_request(uint64 id, const td_api::getMessageLocally &request);

  void on_request(uint64 id, const td_api::getRepliedMessage &request);

  void on_request(uint64 id, const td_api::getChatPinnedMessage &request);

  void on_request(uint64 id, const td_api::getCallbackQueryMessage &request);

  void on_request(uint64 id, const td_api::getMessageThread &request);

  void on_request(uint64 id, const td_api::getMessageViewers &request);

  void on_request(uint64 id, const td_api::getMessages &request);

  void on_request(uint64 id, const td_api::getChatSponsoredMessage &request);

  void on_request(uint64 id, const td_api::getMessageLink &request);

  void on_request(uint64 id, const td_api::getMessageEmbeddingCode &request);

  void on_request(uint64 id, td_api::getMessageLinkInfo &request);

  void on_request(uint64 id, td_api::translateText &request);

  void on_request(uint64 id, const td_api::recognizeSpeech &request);

  void on_request(uint64 id, const td_api::rateSpeechRecognition &request);

  void on_request(uint64 id, const td_api::getFile &request);

  void on_request(uint64 id, td_api::getRemoteFile &request);

  void on_request(uint64 id, td_api::getStorageStatistics &request);

  void on_request(uint64 id, td_api::getStorageStatisticsFast &request);

  void on_request(uint64 id, td_api::getDatabaseStatistics &request);

  void on_request(uint64 id, td_api::optimizeStorage &request);

  void on_request(uint64 id, td_api::getNetworkStatistics &request);

  void on_request(uint64 id, td_api::resetNetworkStatistics &request);

  void on_request(uint64 id, td_api::addNetworkStatistics &request);

  void on_request(uint64 id, const td_api::setNetworkType &request);

  void on_request(uint64 id, const td_api::getAutoDownloadSettingsPresets &request);

  void on_request(uint64 id, const td_api::setAutoDownloadSettings &request);

  void on_request(uint64 id, const td_api::getTopChats &request);

  void on_request(uint64 id, const td_api::removeTopChat &request);

  void on_request(uint64 id, const td_api::loadChats &request);

  void on_request(uint64 id, const td_api::getChats &request);

  void on_request(uint64 id, td_api::searchPublicChat &request);

  void on_request(uint64 id, td_api::searchPublicChats &request);

  void on_request(uint64 id, td_api::searchChats &request);

  void on_request(uint64 id, td_api::searchChatsOnServer &request);

  void on_request(uint64 id, const td_api::searchChatsNearby &request);

  void on_request(uint64 id, const td_api::addRecentlyFoundChat &request);

  void on_request(uint64 id, const td_api::removeRecentlyFoundChat &request);

  void on_request(uint64 id, const td_api::clearRecentlyFoundChats &request);

  void on_request(uint64 id, const td_api::getRecentlyOpenedChats &request);

  void on_request(uint64 id, const td_api::getGroupsInCommon &request);

  void on_request(uint64 id, td_api::checkChatUsername &request);

  void on_request(uint64 id, const td_api::getCreatedPublicChats &request);

  void on_request(uint64 id, const td_api::checkCreatedPublicChatsLimit &request);

  void on_request(uint64 id, const td_api::getSuitableDiscussionChats &request);

  void on_request(uint64 id, const td_api::getInactiveSupergroupChats &request);

  void on_request(uint64 id, const td_api::openChat &request);

  void on_request(uint64 id, const td_api::closeChat &request);

  void on_request(uint64 id, const td_api::viewMessages &request);

  void on_request(uint64 id, const td_api::openMessageContent &request);

  void on_request(uint64 id, const td_api::clickAnimatedEmojiMessage &request);

  void on_request(uint64 id, const td_api::getInternalLinkType &request);

  void on_request(uint64 id, td_api::getExternalLinkInfo &request);

  void on_request(uint64 id, td_api::getExternalLink &request);

  void on_request(uint64 id, const td_api::getChatHistory &request);

  void on_request(uint64 id, const td_api::deleteChatHistory &request);

  void on_request(uint64 id, const td_api::deleteChat &request);

  void on_request(uint64 id, const td_api::getMessageThreadHistory &request);

  void on_request(uint64 id, td_api::getChatMessageCalendar &request);

  void on_request(uint64 id, td_api::searchChatMessages &request);

  void on_request(uint64 id, td_api::searchSecretMessages &request);

  void on_request(uint64 id, td_api::searchMessages &request);

  void on_request(uint64 id, const td_api::searchCallMessages &request);

  void on_request(uint64 id, td_api::searchOutgoingDocumentMessages &request);

  void on_request(uint64 id, const td_api::deleteAllCallMessages &request);

  void on_request(uint64 id, const td_api::searchChatRecentLocationMessages &request);

  void on_request(uint64 id, const td_api::getActiveLiveLocationMessages &request);

  void on_request(uint64 id, const td_api::getChatMessageByDate &request);

  void on_request(uint64 id, const td_api::getChatSparseMessagePositions &request);

  void on_request(uint64 id, const td_api::getChatMessageCount &request);

  void on_request(uint64 id, const td_api::getChatScheduledMessages &request);

  void on_request(uint64 id, const td_api::getEmojiReaction &request);

  void on_request(uint64 id, const td_api::getCustomEmojiReactionAnimations &request);

  void on_request(uint64 id, const td_api::getMessageAvailableReactions &request);

  void on_request(uint64 id, const td_api::clearRecentReactions &request);

  void on_request(uint64 id, td_api::addMessageReaction &request);

  void on_request(uint64 id, td_api::removeMessageReaction &request);

  void on_request(uint64 id, td_api::getMessageAddedReactions &request);

  void on_request(uint64 id, td_api::setDefaultReactionType &request);

  void on_request(uint64 id, td_api::getMessagePublicForwards &request);

  void on_request(uint64 id, const td_api::removeNotification &request);

  void on_request(uint64 id, const td_api::removeNotificationGroup &request);

  void on_request(uint64 id, const td_api::deleteMessages &request);

  void on_request(uint64 id, const td_api::deleteChatMessagesBySender &request);

  void on_request(uint64 id, const td_api::deleteChatMessagesByDate &request);

  void on_request(uint64 id, const td_api::readAllChatMentions &request);

  void on_request(uint64 id, const td_api::readAllChatReactions &request);

  void on_request(uint64 id, const td_api::getChatAvailableMessageSenders &request);

  void on_request(uint64 id, const td_api::setChatMessageSender &request);

  void on_request(uint64 id, td_api::sendMessage &request);

  void on_request(uint64 id, td_api::sendMessageAlbum &request);

  void on_request(uint64 id, td_api::sendBotStartMessage &request);

  void on_request(uint64 id, td_api::sendInlineQueryResultMessage &request);

  void on_request(uint64 id, td_api::addLocalMessage &request);

  void on_request(uint64 id, td_api::editMessageText &request);

  void on_request(uint64 id, td_api::editMessageLiveLocation &request);

  void on_request(uint64 id, td_api::editMessageMedia &request);

  void on_request(uint64 id, td_api::editMessageCaption &request);

  void on_request(uint64 id, td_api::editMessageReplyMarkup &request);

  void on_request(uint64 id, td_api::editInlineMessageText &request);

  void on_request(uint64 id, td_api::editInlineMessageLiveLocation &request);

  void on_request(uint64 id, td_api::editInlineMessageMedia &request);

  void on_request(uint64 id, td_api::editInlineMessageCaption &request);

  void on_request(uint64 id, td_api::editInlineMessageReplyMarkup &request);

  void on_request(uint64 id, td_api::editMessageSchedulingState &request);

  void on_request(uint64 id, td_api::setGameScore &request);

  void on_request(uint64 id, td_api::setInlineGameScore &request);

  void on_request(uint64 id, td_api::getGameHighScores &request);

  void on_request(uint64 id, td_api::getInlineGameHighScores &request);

  void on_request(uint64 id, const td_api::deleteChatReplyMarkup &request);

  void on_request(uint64 id, td_api::sendChatAction &request);

  void on_request(uint64 id, td_api::sendChatScreenshotTakenNotification &request);

  void on_request(uint64 id, td_api::forwardMessages &request);

  void on_request(uint64 id, const td_api::resendMessages &request);

  void on_request(uint64 id, td_api::getWebPagePreview &request);

  void on_request(uint64 id, td_api::getWebPageInstantView &request);

  void on_request(uint64 id, const td_api::createPrivateChat &request);

  void on_request(uint64 id, const td_api::createBasicGroupChat &request);

  void on_request(uint64 id, const td_api::createSupergroupChat &request);

  void on_request(uint64 id, td_api::createSecretChat &request);

  void on_request(uint64 id, td_api::createNewBasicGroupChat &request);

  void on_request(uint64 id, td_api::createNewSupergroupChat &request);

  void on_request(uint64 id, td_api::createNewSecretChat &request);

  void on_request(uint64 id, const td_api::createCall &request);

  void on_request(uint64 id, const td_api::acceptCall &request);

  void on_request(uint64 id, td_api::sendCallSignalingData &request);

  void on_request(uint64 id, const td_api::discardCall &request);

  void on_request(uint64 id, td_api::sendCallRating &request);

  void on_request(uint64 id, td_api::sendCallDebugInformation &request);

  void on_request(uint64 id, td_api::sendCallLog &request);

  void on_request(uint64 id, const td_api::getVideoChatAvailableParticipants &request);

  void on_request(uint64 id, const td_api::setVideoChatDefaultParticipant &request);

  void on_request(uint64 id, td_api::createVideoChat &request);

  void on_request(uint64 id, const td_api::getVideoChatRtmpUrl &request);

  void on_request(uint64 id, const td_api::replaceVideoChatRtmpUrl &request);

  void on_request(uint64 id, const td_api::getGroupCall &request);

  void on_request(uint64 id, const td_api::startScheduledGroupCall &request);

  void on_request(uint64 id, const td_api::toggleGroupCallEnabledStartNotification &request);

  void on_request(uint64 id, td_api::joinGroupCall &request);

  void on_request(uint64 id, td_api::startGroupCallScreenSharing &request);

  void on_request(uint64 id, const td_api::endGroupCallScreenSharing &request);

  void on_request(uint64 id, td_api::setGroupCallTitle &request);

  void on_request(uint64 id, const td_api::toggleGroupCallMuteNewParticipants &request);

  void on_request(uint64 id, const td_api::revokeGroupCallInviteLink &request);

  void on_request(uint64 id, const td_api::inviteGroupCallParticipants &request);

  void on_request(uint64 id, const td_api::getGroupCallInviteLink &request);

  void on_request(uint64 id, td_api::startGroupCallRecording &request);

  void on_request(uint64 id, const td_api::toggleGroupCallScreenSharingIsPaused &request);

  void on_request(uint64 id, const td_api::endGroupCallRecording &request);

  void on_request(uint64 id, const td_api::toggleGroupCallIsMyVideoPaused &request);

  void on_request(uint64 id, const td_api::toggleGroupCallIsMyVideoEnabled &request);

  void on_request(uint64 id, const td_api::setGroupCallParticipantIsSpeaking &request);

  void on_request(uint64 id, const td_api::toggleGroupCallParticipantIsMuted &request);

  void on_request(uint64 id, const td_api::setGroupCallParticipantVolumeLevel &request);

  void on_request(uint64 id, const td_api::toggleGroupCallParticipantIsHandRaised &request);

  void on_request(uint64 id, const td_api::loadGroupCallParticipants &request);

  void on_request(uint64 id, const td_api::leaveGroupCall &request);

  void on_request(uint64 id, const td_api::endGroupCall &request);

  void on_request(uint64 id, const td_api::getGroupCallStreams &request);

  void on_request(uint64 id, td_api::getGroupCallStreamSegment &request);

  void on_request(uint64 id, const td_api::upgradeBasicGroupChatToSupergroupChat &request);

  void on_request(uint64 id, const td_api::getChatListsToAddChat &request);

  void on_request(uint64 id, const td_api::addChatToList &request);

  void on_request(uint64 id, const td_api::getChatFilter &request);

  void on_request(uint64 id, const td_api::getRecommendedChatFilters &request);

  void on_request(uint64 id, td_api::createChatFilter &request);

  void on_request(uint64 id, td_api::editChatFilter &request);

  void on_request(uint64 id, const td_api::deleteChatFilter &request);

  void on_request(uint64 id, const td_api::reorderChatFilters &request);

  void on_request(uint64 id, td_api::setChatTitle &request);

  void on_request(uint64 id, const td_api::setChatPhoto &request);

  void on_request(uint64 id, const td_api::setChatMessageTtl &request);

  void on_request(uint64 id, const td_api::setChatPermissions &request);

  void on_request(uint64 id, td_api::setChatTheme &request);

  void on_request(uint64 id, td_api::setChatDraftMessage &request);

  void on_request(uint64 id, const td_api::toggleChatHasProtectedContent &request);

  void on_request(uint64 id, const td_api::toggleChatIsPinned &request);

  void on_request(uint64 id, const td_api::toggleChatIsMarkedAsUnread &request);

  void on_request(uint64 id, const td_api::toggleMessageSenderIsBlocked &request);

  void on_request(uint64 id, const td_api::toggleChatDefaultDisableNotification &request);

  void on_request(uint64 id, const td_api::setPinnedChats &request);

  void on_request(uint64 id, const td_api::getAttachmentMenuBot &request);

  void on_request(uint64 id, const td_api::toggleBotIsAddedToAttachmentMenu &request);

  void on_request(uint64 id, td_api::setChatAvailableReactions &request);

  void on_request(uint64 id, td_api::setChatClientData &request);

  void on_request(uint64 id, td_api::setChatDescription &request);

  void on_request(uint64 id, const td_api::setChatDiscussionGroup &request);

  void on_request(uint64 id, td_api::setChatLocation &request);

  void on_request(uint64 id, const td_api::setChatSlowModeDelay &request);

  void on_request(uint64 id, const td_api::pinChatMessage &request);

  void on_request(uint64 id, const td_api::unpinChatMessage &request);

  void on_request(uint64 id, const td_api::unpinAllChatMessages &request);

  void on_request(uint64 id, const td_api::joinChat &request);

  void on_request(uint64 id, const td_api::leaveChat &request);

  void on_request(uint64 id, const td_api::addChatMember &request);

  void on_request(uint64 id, const td_api::addChatMembers &request);

  void on_request(uint64 id, td_api::setChatMemberStatus &request);

  void on_request(uint64 id, const td_api::banChatMember &request);

  void on_request(uint64 id, const td_api::canTransferOwnership &request);

  void on_request(uint64 id, td_api::transferChatOwnership &request);

  void on_request(uint64 id, const td_api::getChatMember &request);

  void on_request(uint64 id, td_api::searchChatMembers &request);

  void on_request(uint64 id, const td_api::getChatAdministrators &request);

  void on_request(uint64 id, const td_api::replacePrimaryChatInviteLink &request);

  void on_request(uint64 id, td_api::createChatInviteLink &request);

  void on_request(uint64 id, td_api::editChatInviteLink &request);

  void on_request(uint64 id, td_api::getChatInviteLink &request);

  void on_request(uint64 id, const td_api::getChatInviteLinkCounts &request);

  void on_request(uint64 id, td_api::getChatInviteLinks &request);

  void on_request(uint64 id, td_api::getChatInviteLinkMembers &request);

  void on_request(uint64 id, td_api::getChatJoinRequests &request);

  void on_request(uint64 id, const td_api::processChatJoinRequest &request);

  void on_request(uint64 id, td_api::processChatJoinRequests &request);

  void on_request(uint64 id, td_api::revokeChatInviteLink &request);

  void on_request(uint64 id, td_api::deleteRevokedChatInviteLink &request);

  void on_request(uint64 id, const td_api::deleteAllRevokedChatInviteLinks &request);

  void on_request(uint64 id, td_api::checkChatInviteLink &request);

  void on_request(uint64 id, td_api::joinChatByInviteLink &request);

  void on_request(uint64 id, td_api::getChatEventLog &request);

  void on_request(uint64 id, const td_api::clearAllDraftMessages &request);

  void on_request(uint64 id, const td_api::downloadFile &request);

  void on_request(uint64 id, const td_api::getFileDownloadedPrefixSize &request);

  void on_request(uint64 id, const td_api::cancelDownloadFile &request);

  void on_request(uint64 id, const td_api::getSuggestedFileName &request);

  void on_request(uint64 id, td_api::preliminaryUploadFile &request);

  void on_request(uint64 id, const td_api::cancelPreliminaryUploadFile &request);

  void on_request(uint64 id, td_api::writeGeneratedFilePart &request);

  void on_request(uint64 id, const td_api::setFileGenerationProgress &request);

  void on_request(uint64 id, td_api::finishFileGeneration &request);

  void on_request(uint64 id, const td_api::readFilePart &request);

  void on_request(uint64 id, const td_api::deleteFile &request);

  void on_request(uint64 id, const td_api::addFileToDownloads &request);

  void on_request(uint64 id, const td_api::toggleDownloadIsPaused &request);

  void on_request(uint64 id, const td_api::toggleAllDownloadsArePaused &request);

  void on_request(uint64 id, const td_api::removeFileFromDownloads &request);

  void on_request(uint64 id, const td_api::removeAllFilesFromDownloads &request);

  void on_request(uint64 id, td_api::searchFileDownloads &request);

  void on_request(uint64 id, td_api::getMessageFileType &request);

  void on_request(uint64 id, const td_api::getMessageImportConfirmationText &request);

  void on_request(uint64 id, const td_api::importMessages &request);

  void on_request(uint64 id, const td_api::blockMessageSenderFromReplies &request);

  void on_request(uint64 id, const td_api::getBlockedMessageSenders &request);

  void on_request(uint64 id, td_api::addContact &request);

  void on_request(uint64 id, td_api::importContacts &request);

  void on_request(uint64 id, const td_api::getContacts &request);

  void on_request(uint64 id, td_api::searchContacts &request);

  void on_request(uint64 id, td_api::removeContacts &request);

  void on_request(uint64 id, const td_api::getImportedContactCount &request);

  void on_request(uint64 id, td_api::changeImportedContacts &request);

  void on_request(uint64 id, const td_api::clearImportedContacts &request);

  void on_request(uint64 id, td_api::searchUserByPhoneNumber &request);

  void on_request(uint64 id, const td_api::sharePhoneNumber &request);

  void on_request(uint64 id, const td_api::getRecentInlineBots &request);

  void on_request(uint64 id, td_api::setName &request);

  void on_request(uint64 id, td_api::setBio &request);

  void on_request(uint64 id, td_api::setUsername &request);

  void on_request(uint64 id, const td_api::setEmojiStatus &request);

  void on_request(uint64 id, const td_api::getThemedEmojiStatuses &request);

  void on_request(uint64 id, const td_api::getDefaultEmojiStatuses &request);

  void on_request(uint64 id, const td_api::getRecentEmojiStatuses &request);

  void on_request(uint64 id, const td_api::clearRecentEmojiStatuses &request);

  void on_request(uint64 id, td_api::setCommands &request);

  void on_request(uint64 id, td_api::deleteCommands &request);

  void on_request(uint64 id, td_api::getCommands &request);

  void on_request(uint64 id, td_api::setMenuButton &request);

  void on_request(uint64 id, const td_api::getMenuButton &request);

  void on_request(uint64 id, const td_api::setDefaultGroupAdministratorRights &request);

  void on_request(uint64 id, const td_api::setDefaultChannelAdministratorRights &request);

  void on_request(uint64 id, const td_api::setLocation &request);

  void on_request(uint64 id, td_api::setProfilePhoto &request);

  void on_request(uint64 id, const td_api::deleteProfilePhoto &request);

  void on_request(uint64 id, const td_api::getUserProfilePhotos &request);

  void on_request(uint64 id, td_api::setSupergroupUsername &request);

  void on_request(uint64 id, const td_api::setSupergroupStickerSet &request);

  void on_request(uint64 id, const td_api::toggleSupergroupSignMessages &request);

  void on_request(uint64 id, const td_api::toggleSupergroupJoinToSendMessages &request);

  void on_request(uint64 id, const td_api::toggleSupergroupJoinByRequest &request);

  void on_request(uint64 id, const td_api::toggleSupergroupIsAllHistoryAvailable &request);

  void on_request(uint64 id, const td_api::toggleSupergroupIsBroadcastGroup &request);

  void on_request(uint64 id, const td_api::reportSupergroupSpam &request);

  void on_request(uint64 id, td_api::getSupergroupMembers &request);

  void on_request(uint64 id, td_api::closeSecretChat &request);

  void on_request(uint64 id, td_api::getStickers &request);

  void on_request(uint64 id, td_api::searchStickers &request);

  void on_request(uint64 id, const td_api::getPremiumStickers &request);

  void on_request(uint64 id, const td_api::getInstalledStickerSets &request);

  void on_request(uint64 id, const td_api::getArchivedStickerSets &request);

  void on_request(uint64 id, const td_api::getTrendingStickerSets &request);

  void on_request(uint64 id, const td_api::getAttachedStickerSets &request);

  void on_request(uint64 id, const td_api::getStickerSet &request);

  void on_request(uint64 id, td_api::searchStickerSet &request);

  void on_request(uint64 id, td_api::searchInstalledStickerSets &request);

  void on_request(uint64 id, td_api::searchStickerSets &request);

  void on_request(uint64 id, const td_api::changeStickerSet &request);

  void on_request(uint64 id, const td_api::viewTrendingStickerSets &request);

  void on_request(uint64 id, td_api::reorderInstalledStickerSets &request);

  void on_request(uint64 id, td_api::uploadStickerFile &request);

  void on_request(uint64 id, td_api::getSuggestedStickerSetName &request);

  void on_request(uint64 id, td_api::checkStickerSetName &request);

  void on_request(uint64 id, td_api::createNewStickerSet &request);

  void on_request(uint64 id, td_api::addStickerToSet &request);

  void on_request(uint64 id, td_api::setStickerSetThumbnail &request);

  void on_request(uint64 id, td_api::setStickerPositionInSet &request);

  void on_request(uint64 id, td_api::removeStickerFromSet &request);

  void on_request(uint64 id, const td_api::getRecentStickers &request);

  void on_request(uint64 id, td_api::addRecentSticker &request);

  void on_request(uint64 id, td_api::removeRecentSticker &request);

  void on_request(uint64 id, td_api::clearRecentStickers &request);

  void on_request(uint64 id, const td_api::getSavedAnimations &request);

  void on_request(uint64 id, td_api::addSavedAnimation &request);

  void on_request(uint64 id, td_api::removeSavedAnimation &request);

  void on_request(uint64 id, td_api::getStickerEmojis &request);

  void on_request(uint64 id, td_api::searchEmojis &request);

  void on_request(uint64 id, td_api::getAnimatedEmoji &request);

  void on_request(uint64 id, td_api::getEmojiSuggestionsUrl &request);

  void on_request(uint64 id, td_api::getCustomEmojiStickers &request);

  void on_request(uint64 id, const td_api::getFavoriteStickers &request);

  void on_request(uint64 id, td_api::addFavoriteSticker &request);

  void on_request(uint64 id, td_api::removeFavoriteSticker &request);

  void on_request(uint64 id, const td_api::getSavedNotificationSound &request);

  void on_request(uint64 id, const td_api::getSavedNotificationSounds &request);

  void on_request(uint64 id, td_api::addSavedNotificationSound &request);

  void on_request(uint64 id, const td_api::removeSavedNotificationSound &request);

  void on_request(uint64 id, const td_api::getChatNotificationSettingsExceptions &request);

  void on_request(uint64 id, const td_api::getScopeNotificationSettings &request);

  void on_request(uint64 id, td_api::setChatNotificationSettings &request);

  void on_request(uint64 id, td_api::setScopeNotificationSettings &request);

  void on_request(uint64 id, const td_api::resetAllNotificationSettings &request);

  void on_request(uint64 id, const td_api::removeChatActionBar &request);

  void on_request(uint64 id, td_api::reportChat &request);

  void on_request(uint64 id, td_api::reportChatPhoto &request);

  void on_request(uint64 id, const td_api::reportMessageReactions &request);

  void on_request(uint64 id, const td_api::getChatStatistics &request);

  void on_request(uint64 id, const td_api::getMessageStatistics &request);

  void on_request(uint64 id, td_api::getStatisticalGraph &request);

  void on_request(uint64 id, const td_api::getMapThumbnailFile &request);

  void on_request(uint64 id, const td_api::getLocalizationTargetInfo &request);

  void on_request(uint64 id, td_api::getLanguagePackInfo &request);

  void on_request(uint64 id, td_api::getLanguagePackStrings &request);

  void on_request(uint64 id, td_api::synchronizeLanguagePack &request);

  void on_request(uint64 id, td_api::addCustomServerLanguagePack &request);

  void on_request(uint64 id, td_api::setCustomLanguagePack &request);

  void on_request(uint64 id, td_api::editCustomLanguagePackInfo &request);

  void on_request(uint64 id, td_api::setCustomLanguagePackString &request);

  void on_request(uint64 id, td_api::deleteLanguagePack &request);

  void on_request(uint64 id, td_api::getOption &request);

  void on_request(uint64 id, td_api::setOption &request);

  void on_request(uint64 id, td_api::setPollAnswer &request);

  void on_request(uint64 id, td_api::getPollVoters &request);

  void on_request(uint64 id, td_api::stopPoll &request);

  void on_request(uint64 id, const td_api::hideSuggestedAction &request);

  void on_request(uint64 id, const td_api::getLoginUrlInfo &request);

  void on_request(uint64 id, const td_api::getLoginUrl &request);

  void on_request(uint64 id, td_api::getInlineQueryResults &request);

  void on_request(uint64 id, td_api::answerInlineQuery &request);

  void on_request(uint64 id, td_api::getWebAppUrl &request);

  void on_request(uint64 id, td_api::sendWebAppData &request);

  void on_request(uint64 id, td_api::openWebApp &request);

  void on_request(uint64 id, const td_api::closeWebApp &request);

  void on_request(uint64 id, td_api::answerWebAppQuery &request);

  void on_request(uint64 id, td_api::getCallbackQueryAnswer &request);

  void on_request(uint64 id, td_api::answerCallbackQuery &request);

  void on_request(uint64 id, td_api::answerShippingQuery &request);

  void on_request(uint64 id, td_api::answerPreCheckoutQuery &request);

  void on_request(uint64 id, td_api::getBankCardInfo &request);

  void on_request(uint64 id, td_api::getPaymentForm &request);

  void on_request(uint64 id, td_api::validateOrderInfo &request);

  void on_request(uint64 id, td_api::sendPaymentForm &request);

  void on_request(uint64 id, const td_api::getPaymentReceipt &request);

  void on_request(uint64 id, const td_api::getSavedOrderInfo &request);

  void on_request(uint64 id, const td_api::deleteSavedOrderInfo &request);

  void on_request(uint64 id, const td_api::deleteSavedCredentials &request);

  void on_request(uint64 id, td_api::createInvoiceLink &request);

  void on_request(uint64 id, td_api::getPassportElement &request);

  void on_request(uint64 id, td_api::getAllPassportElements &request);

  void on_request(uint64 id, td_api::setPassportElement &request);

  void on_request(uint64 id, const td_api::deletePassportElement &request);

  void on_request(uint64 id, td_api::setPassportElementErrors &request);

  void on_request(uint64 id, td_api::getPreferredCountryLanguage &request);

  void on_request(uint64 id, td_api::sendPhoneNumberVerificationCode &request);

  void on_request(uint64 id, const td_api::resendPhoneNumberVerificationCode &request);

  void on_request(uint64 id, td_api::checkPhoneNumberVerificationCode &request);

  void on_request(uint64 id, td_api::sendEmailAddressVerificationCode &request);

  void on_request(uint64 id, const td_api::resendEmailAddressVerificationCode &request);

  void on_request(uint64 id, td_api::checkEmailAddressVerificationCode &request);

  void on_request(uint64 id, td_api::getPassportAuthorizationForm &request);

  void on_request(uint64 id, td_api::getPassportAuthorizationFormAvailableElements &request);

  void on_request(uint64 id, td_api::sendPassportAuthorizationForm &request);

  void on_request(uint64 id, td_api::sendPhoneNumberConfirmationCode &request);

  void on_request(uint64 id, const td_api::resendPhoneNumberConfirmationCode &request);

  void on_request(uint64 id, td_api::checkPhoneNumberConfirmationCode &request);

  void on_request(uint64 id, const td_api::getSupportUser &request);

  void on_request(uint64 id, const td_api::getBackgrounds &request);

  void on_request(uint64 id, td_api::getBackgroundUrl &request);

  void on_request(uint64 id, td_api::searchBackground &request);

  void on_request(uint64 id, td_api::setBackground &request);

  void on_request(uint64 id, const td_api::removeBackground &request);

  void on_request(uint64 id, const td_api::resetBackgrounds &request);

  void on_request(uint64 id, td_api::getRecentlyVisitedTMeUrls &request);

  void on_request(uint64 id, td_api::setBotUpdatesStatus &request);

  void on_request(uint64 id, td_api::sendCustomRequest &request);

  void on_request(uint64 id, td_api::answerCustomQuery &request);

  void on_request(uint64 id, const td_api::setAlarm &request);

  void on_request(uint64 id, td_api::searchHashtags &request);

  void on_request(uint64 id, td_api::removeRecentHashtag &request);

  void on_request(uint64 id, const td_api::getPremiumLimit &request);

  void on_request(uint64 id, const td_api::getPremiumFeatures &request);

  void on_request(uint64 id, const td_api::getPremiumStickerExamples &request);

  void on_request(uint64 id, const td_api::viewPremiumFeature &request);

  void on_request(uint64 id, const td_api::clickPremiumSubscriptionButton &request);

  void on_request(uint64 id, const td_api::getPremiumState &request);

  void on_request(uint64 id, td_api::canPurchasePremium &request);

  void on_request(uint64 id, td_api::assignAppStoreTransaction &request);

  void on_request(uint64 id, td_api::assignGooglePlayTransaction &request);

  void on_request(uint64 id, td_api::acceptTermsOfService &request);

  void on_request(uint64 id, const td_api::getCountries &request);

  void on_request(uint64 id, const td_api::getCountryCode &request);

  void on_request(uint64 id, const td_api::getPhoneNumberInfo &request);

  void on_request(uint64 id, const td_api::getApplicationDownloadLink &request);

  void on_request(uint64 id, td_api::getDeepLinkInfo &request);

  void on_request(uint64 id, const td_api::getApplicationConfig &request);

  void on_request(uint64 id, td_api::saveApplicationLogEvent &request);

  void on_request(uint64 id, td_api::addProxy &request);

  void on_request(uint64 id, td_api::editProxy &request);

  void on_request(uint64 id, const td_api::enableProxy &request);

  void on_request(uint64 id, const td_api::disableProxy &request);

  void on_request(uint64 id, const td_api::removeProxy &request);

  void on_request(uint64 id, const td_api::getProxies &request);

  void on_request(uint64 id, const td_api::getProxyLink &request);

  void on_request(uint64 id, const td_api::pingProxy &request);

  void on_request(uint64 id, const td_api::getUserSupportInfo &request);

  void on_request(uint64 id, td_api::setUserSupportInfo &request);

  void on_request(uint64 id, const td_api::getTextEntities &request);

  void on_request(uint64 id, const td_api::parseTextEntities &request);

  void on_request(uint64 id, const td_api::parseMarkdown &request);

  void on_request(uint64 id, const td_api::getMarkdownText &request);

  void on_request(uint64 id, const td_api::getFileMimeType &request);

  void on_request(uint64 id, const td_api::getFileExtension &request);

  void on_request(uint64 id, const td_api::cleanFileName &request);

  void on_request(uint64 id, const td_api::getLanguagePackString &request);

  void on_request(uint64 id, const td_api::getPhoneNumberInfoSync &request);

  void on_request(uint64 id, const td_api::getPushReceiverId &request);

  void on_request(uint64 id, const td_api::getChatFilterDefaultIconName &request);

  void on_request(uint64 id, const td_api::getJsonValue &request);

  void on_request(uint64 id, const td_api::getJsonString &request);

  void on_request(uint64 id, const td_api::getThemeParametersJsonString &request);

  void on_request(uint64 id, const td_api::setLogStream &request);

  void on_request(uint64 id, const td_api::getLogStream &request);

  void on_request(uint64 id, const td_api::setLogVerbosityLevel &request);

  void on_request(uint64 id, const td_api::getLogVerbosityLevel &request);

  void on_request(uint64 id, const td_api::getLogTags &request);

  void on_request(uint64 id, const td_api::setLogTagVerbosityLevel &request);

  void on_request(uint64 id, const td_api::getLogTagVerbosityLevel &request);

  void on_request(uint64 id, const td_api::addLogMessage &request);

  // test
  void on_request(uint64 id, const td_api::testNetwork &request);
  void on_request(uint64 id, td_api::testProxy &request);
  void on_request(uint64 id, const td_api::testGetDifference &request);
  void on_request(uint64 id, const td_api::testUseUpdate &request);
  void on_request(uint64 id, const td_api::testReturnError &request);
  void on_request(uint64 id, const td_api::testCallEmpty &request);
  void on_request(uint64 id, const td_api::testSquareInt &request);
  void on_request(uint64 id, td_api::testCallString &request);
  void on_request(uint64 id, td_api::testCallBytes &request);
  void on_request(uint64 id, td_api::testCallVectorInt &request);
  void on_request(uint64 id, td_api::testCallVectorIntObject &request);
  void on_request(uint64 id, td_api::testCallVectorString &request);
  void on_request(uint64 id, td_api::testCallVectorStringObject &request);

  template <class T>
  static td_api::object_ptr<td_api::Object> do_static_request(const T &request) {
    return td_api::make_object<td_api::error>(400, "The method can't be executed synchronously");
  }
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getOption &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getTextEntities &request);
  static td_api::object_ptr<td_api::Object> do_static_request(td_api::parseTextEntities &request);
  static td_api::object_ptr<td_api::Object> do_static_request(td_api::parseMarkdown &request);
  static td_api::object_ptr<td_api::Object> do_static_request(td_api::getMarkdownText &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getFileMimeType &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getFileExtension &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::cleanFileName &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getLanguagePackString &request);
  static td_api::object_ptr<td_api::Object> do_static_request(td_api::getPhoneNumberInfoSync &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getPushReceiverId &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getChatFilterDefaultIconName &request);
  static td_api::object_ptr<td_api::Object> do_static_request(td_api::getJsonValue &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getJsonString &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getThemeParametersJsonString &request);
  static td_api::object_ptr<td_api::Object> do_static_request(td_api::setLogStream &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getLogStream &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::setLogVerbosityLevel &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getLogVerbosityLevel &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getLogTags &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::setLogTagVerbosityLevel &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::getLogTagVerbosityLevel &request);
  static td_api::object_ptr<td_api::Object> do_static_request(const td_api::addLogMessage &request);
  static td_api::object_ptr<td_api::Object> do_static_request(td_api::testReturnError &request);

  static DbKey as_db_key(string key);

  static int32 get_database_scheduler_id();

  void finish_set_parameters();

  void init(Result<TdDb::OpenedDatabase> r_opened_database);

  void init_options_and_network();

  void init_connection_creator();

  void init_file_manager();

  void init_managers();

  void clear();

  void close_impl(bool destroy_flag);

  static Status fix_parameters(TdParameters &parameters) TD_WARN_UNUSED_RESULT;

  Status set_parameters(td_api::object_ptr<td_api::setTdlibParameters> parameters) TD_WARN_UNUSED_RESULT;

  static td_api::object_ptr<td_api::error> make_error(int32 code, CSlice error) {
    return td_api::make_object<td_api::error>(code, error.str());
  }

  // Actor
  void start_up() final;
  void tear_down() final;
  void hangup_shared() final;
  void hangup() final;
};

}  // namespace td
