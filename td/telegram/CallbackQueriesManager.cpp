//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/CallbackQueriesManager.h"

#include "td/telegram/AccessRights.h"
#include "td/telegram/AuthManager.h"
#include "td/telegram/ContactsManager.h"
#include "td/telegram/Global.h"
#include "td/telegram/InlineQueriesManager.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/PasswordManager.h"
#include "td/telegram/Td.h"
#include "td/telegram/td_api.h"
#include "td/telegram/telegram_api.h"

#include "td/actor/actor.h"

#include "td/utils/common.h"
#include "td/utils/logging.h"
#include "td/utils/Status.h"

namespace td {

class GetBotCallbackAnswerQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::callbackQueryAnswer>> promise_;
  DialogId dialog_id_;
  MessageId message_id_;

 public:
  explicit GetBotCallbackAnswerQuery(Promise<td_api::object_ptr<td_api::callbackQueryAnswer>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, MessageId message_id, const tl_object_ptr<td_api::CallbackQueryPayload> &payload,
            tl_object_ptr<telegram_api::InputCheckPasswordSRP> &&password) {
    dialog_id_ = dialog_id;
    message_id_ = message_id;

    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
    CHECK(input_peer != nullptr);

    int32 flags = 0;
    BufferSlice data;
    CHECK(payload != nullptr);
    switch (payload->get_id()) {
      case td_api::callbackQueryPayloadData::ID:
        flags = telegram_api::messages_getBotCallbackAnswer::DATA_MASK;
        data = BufferSlice(static_cast<const td_api::callbackQueryPayloadData *>(payload.get())->data_);
        break;
      case td_api::callbackQueryPayloadDataWithPassword::ID:
        CHECK(password != nullptr);
        flags = telegram_api::messages_getBotCallbackAnswer::DATA_MASK |
                telegram_api::messages_getBotCallbackAnswer::PASSWORD_MASK;
        data = BufferSlice(static_cast<const td_api::callbackQueryPayloadDataWithPassword *>(payload.get())->data_);
        break;
      case td_api::callbackQueryPayloadGame::ID:
        flags = telegram_api::messages_getBotCallbackAnswer::GAME_MASK;
        break;
      default:
        UNREACHABLE();
    }

    auto net_query = G()->net_query_creator().create(telegram_api::messages_getBotCallbackAnswer(
        flags, false /*ignored*/, std::move(input_peer), message_id.get_server_message_id().get(), std::move(data),
        std::move(password)));
    net_query->need_resend_on_503_ = false;
    send_query(std::move(net_query));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_getBotCallbackAnswer>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto answer = result_ptr.move_as_ok();
    promise_.set_value(
        td_api::make_object<td_api::callbackQueryAnswer>(answer->message_, answer->alert_, answer->url_));
  }

  void on_error(Status status) final {
    if (status.message() == "DATA_INVALID" || status.message() == "MESSAGE_ID_INVALID") {
      td_->messages_manager_->get_message_from_server({dialog_id_, message_id_}, Auto(), "GetBotCallbackAnswerQuery");
    } else if (status.message() == "BOT_RESPONSE_TIMEOUT") {
      status = Status::Error(502, "The bot is not responding");
    }
    if (status.code() == 502 && td_->messages_manager_->is_message_edited_recently({dialog_id_, message_id_}, 31)) {
      return promise_.set_value(td_api::make_object<td_api::callbackQueryAnswer>());
    }

    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetBotCallbackAnswerQuery");
    promise_.set_error(std::move(status));
  }
};

class SetBotCallbackAnswerQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit SetBotCallbackAnswerQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(int32 flags, int64 callback_query_id, const string &text, const string &url, int32 cache_time) {
    send_query(G()->net_query_creator().create(telegram_api::messages_setBotCallbackAnswer(
        flags, false /*ignored*/, callback_query_id, text, url, cache_time)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_setBotCallbackAnswer>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    if (!result) {
      LOG(INFO) << "Sending answer to a callback query has failed";
    }
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

CallbackQueriesManager::CallbackQueriesManager(Td *td) : td_(td) {
}

void CallbackQueriesManager::answer_callback_query(int64 callback_query_id, const string &text, bool show_alert,
                                                   const string &url, int32 cache_time, Promise<Unit> &&promise) const {
  int32 flags = 0;
  if (!text.empty()) {
    flags |= BOT_CALLBACK_ANSWER_FLAG_HAS_MESSAGE;
  }
  if (show_alert) {
    flags |= BOT_CALLBACK_ANSWER_FLAG_NEED_SHOW_ALERT;
  }
  if (!url.empty()) {
    flags |= BOT_CALLBACK_ANSWER_FLAG_HAS_URL;
  }
  td_->create_handler<SetBotCallbackAnswerQuery>(std::move(promise))
      ->send(flags, callback_query_id, text, url, cache_time);
}

tl_object_ptr<td_api::CallbackQueryPayload> CallbackQueriesManager::get_query_payload(int32 flags, BufferSlice &&data,
                                                                                      string &&game_short_name) {
  bool has_data = (flags & telegram_api::updateBotCallbackQuery::DATA_MASK) != 0;
  bool has_game = (flags & telegram_api::updateBotCallbackQuery::GAME_SHORT_NAME_MASK) != 0;
  if (has_data == has_game) {
    LOG(ERROR) << "Receive wrong flags " << flags << " in a callback query";
    return nullptr;
  }

  if (has_data) {
    return make_tl_object<td_api::callbackQueryPayloadData>(data.as_slice().str());
  }
  if (has_game) {
    return make_tl_object<td_api::callbackQueryPayloadGame>(game_short_name);
  }
  UNREACHABLE();
  return nullptr;
}

void CallbackQueriesManager::on_new_query(int32 flags, int64 callback_query_id, UserId sender_user_id,
                                          DialogId dialog_id, MessageId message_id, BufferSlice &&data,
                                          int64 chat_instance, string &&game_short_name) {
  if (!dialog_id.is_valid()) {
    LOG(ERROR) << "Receive new callback query in invalid " << dialog_id;
    return;
  }
  if (!sender_user_id.is_valid()) {
    LOG(ERROR) << "Receive new callback query from invalid " << sender_user_id << " in " << dialog_id;
    return;
  }
  LOG_IF(ERROR, !td_->contacts_manager_->have_user(sender_user_id)) << "Have no info about " << sender_user_id;
  if (!td_->auth_manager_->is_bot()) {
    LOG(ERROR) << "Receive new callback query";
    return;
  }
  if (!message_id.is_valid()) {
    LOG(ERROR) << "Receive new callback query from " << message_id << " in " << dialog_id << " sent by "
               << sender_user_id;
    return;
  }

  auto payload = get_query_payload(flags, std::move(data), std::move(game_short_name));
  if (payload == nullptr) {
    return;
  }

  td_->messages_manager_->force_create_dialog(dialog_id, "on_new_callback_query", true);
  send_closure(
      G()->td(), &Td::send_update,
      make_tl_object<td_api::updateNewCallbackQuery>(
          callback_query_id, td_->contacts_manager_->get_user_id_object(sender_user_id, "updateNewCallbackQuery"),
          dialog_id.get(), message_id.get(), chat_instance, std::move(payload)));
}

void CallbackQueriesManager::on_new_inline_query(
    int32 flags, int64 callback_query_id, UserId sender_user_id,
    tl_object_ptr<telegram_api::InputBotInlineMessageID> &&inline_message_id, BufferSlice &&data, int64 chat_instance,
    string &&game_short_name) {
  if (!sender_user_id.is_valid()) {
    LOG(ERROR) << "Receive new callback query from invalid " << sender_user_id;
    return;
  }
  LOG_IF(ERROR, !td_->contacts_manager_->have_user(sender_user_id)) << "Have no info about " << sender_user_id;
  if (!td_->auth_manager_->is_bot()) {
    LOG(ERROR) << "Receive new callback query";
    return;
  }
  CHECK(inline_message_id != nullptr);

  auto payload = get_query_payload(flags, std::move(data), std::move(game_short_name));
  if (payload == nullptr) {
    return;
  }
  send_closure(
      G()->td(), &Td::send_update,
      make_tl_object<td_api::updateNewInlineCallbackQuery>(
          callback_query_id, td_->contacts_manager_->get_user_id_object(sender_user_id, "updateNewInlineCallbackQuery"),
          InlineQueriesManager::get_inline_message_id(std::move(inline_message_id)), chat_instance,
          std::move(payload)));
}

void CallbackQueriesManager::send_callback_query(FullMessageId full_message_id,
                                                 tl_object_ptr<td_api::CallbackQueryPayload> &&payload,
                                                 Promise<td_api::object_ptr<td_api::callbackQueryAnswer>> &&promise) {
  if (td_->auth_manager_->is_bot()) {
    return promise.set_error(Status::Error(400, "Bot can't send callback queries to other bot"));
  }

  if (payload == nullptr) {
    return promise.set_error(Status::Error(400, "Payload must be non-empty"));
  }

  auto dialog_id = full_message_id.get_dialog_id();
  td_->messages_manager_->have_dialog_force(dialog_id, "send_callback_query");
  if (!td_->messages_manager_->have_input_peer(dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(400, "Can't access the chat"));
  }

  if (!td_->messages_manager_->have_message_force(full_message_id, "send_callback_query")) {
    return promise.set_error(Status::Error(400, "Message not found"));
  }
  if (full_message_id.get_message_id().is_valid_scheduled()) {
    return promise.set_error(Status::Error(400, "Can't send callback queries from scheduled messages"));
  }
  if (!full_message_id.get_message_id().is_server()) {
    return promise.set_error(Status::Error(400, "Bad message identifier"));
  }

  if (payload->get_id() == td_api::callbackQueryPayloadDataWithPassword::ID) {
    auto password = static_cast<const td_api::callbackQueryPayloadDataWithPassword *>(payload.get())->password_;
    send_closure(
        td_->password_manager_, &PasswordManager::get_input_check_password_srp, std::move(password),
        PromiseCreator::lambda([this, full_message_id, payload = std::move(payload), promise = std::move(promise)](
                                   Result<tl_object_ptr<telegram_api::InputCheckPasswordSRP>> result) mutable {
          if (result.is_error()) {
            return promise.set_error(result.move_as_error());
          }
          send_get_callback_answer_query(full_message_id, std::move(payload), result.move_as_ok(), std::move(promise));
        }));
  } else {
    send_get_callback_answer_query(full_message_id, std::move(payload), nullptr, std::move(promise));
  }
}

void CallbackQueriesManager::send_get_callback_answer_query(
    FullMessageId full_message_id, tl_object_ptr<td_api::CallbackQueryPayload> &&payload,
    tl_object_ptr<telegram_api::InputCheckPasswordSRP> &&password,
    Promise<td_api::object_ptr<td_api::callbackQueryAnswer>> &&promise) {
  TRY_STATUS_PROMISE(promise, G()->close_status());

  auto dialog_id = full_message_id.get_dialog_id();
  if (!td_->messages_manager_->have_input_peer(dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(400, "Can't access the chat"));
  }
  if (!td_->messages_manager_->have_message_force(full_message_id, "send_callback_query")) {
    return promise.set_error(Status::Error(400, "Message not found"));
  }

  td_->create_handler<GetBotCallbackAnswerQuery>(std::move(promise))
      ->send(dialog_id, full_message_id.get_message_id(), payload, std::move(password));
}

}  // namespace td
