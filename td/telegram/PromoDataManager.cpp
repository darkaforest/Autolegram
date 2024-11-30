//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2024
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/PromoDataManager.h"

#include "td/telegram/AuthManager.h"
#include "td/telegram/DialogSource.h"
#include "td/telegram/Global.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/Td.h"
#include "td/telegram/telegram_api.h"

#include "td/utils/buffer.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Promise.h"

namespace td {

class GetPromoDataQuery final : public Td::ResultHandler {
  Promise<telegram_api::object_ptr<telegram_api::help_PromoData>> promise_;

 public:
  explicit GetPromoDataQuery(Promise<telegram_api::object_ptr<telegram_api::help_PromoData>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send() {
    // we don't poll promo data before authorization
    send_query(G()->net_query_creator().create(telegram_api::help_getPromoData()));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::help_getPromoData>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(result_ptr.move_as_ok());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

PromoDataManager::PromoDataManager(Td *td, ActorShared<> parent) : td_(td), parent_(std::move(parent)) {
}

void PromoDataManager::tear_down() {
  parent_.reset();
}

void PromoDataManager::start_up() {
  init();
}

void PromoDataManager::init() {
  if (G()->close_flag()) {
    return;
  }
  if (is_inited_ || !td_->auth_manager_->is_authorized() || td_->auth_manager_->is_bot()) {
    return;
  }
  is_inited_ = true;

  reload_promo_data();
}

void PromoDataManager::reload_promo_data() {
  if (reloading_promo_data_) {
    need_reload_promo_data_ = true;
    return;
  }
  schedule_get_promo_data(0);
}

void PromoDataManager::schedule_get_promo_data(int32 expires_in) {
  if (!is_inited_) {
    return;
  }

  expires_in = expires_in <= 0 ? 0 : clamp(expires_in, 60, 86400);
  LOG(INFO) << "Schedule getPromoData in " << expires_in;
  set_timeout_in(expires_in);
}

void PromoDataManager::timeout_expired() {
  if (G()->close_flag() || !is_inited_) {
    return;
  }

  reloading_promo_data_ = true;
  auto promise = PromiseCreator::lambda(
      [actor_id = actor_id(this)](Result<telegram_api::object_ptr<telegram_api::help_PromoData>> result) {
        send_closure(actor_id, &PromoDataManager::on_get_promo_data, std::move(result), false);
      });
  td_->create_handler<GetPromoDataQuery>(std::move(promise))->send();
}

void PromoDataManager::on_get_promo_data(Result<telegram_api::object_ptr<telegram_api::help_PromoData>> r_promo_data,
                                         bool dummy) {
  if (G()->close_flag()) {
    return;
  }
  reloading_promo_data_ = false;

  if (r_promo_data.is_error()) {
    LOG(ERROR) << "Receive error for GetPromoData: " << r_promo_data.error();
    return schedule_get_promo_data(60);
  }

  auto promo_data_ptr = r_promo_data.move_as_ok();
  CHECK(promo_data_ptr != nullptr);
  LOG(DEBUG) << "Receive " << to_string(promo_data_ptr);
  int32 expires_at = 0;
  switch (promo_data_ptr->get_id()) {
    case telegram_api::help_promoDataEmpty::ID: {
      auto promo = telegram_api::move_object_as<telegram_api::help_promoDataEmpty>(promo_data_ptr);
      expires_at = promo->expires_;
      td_->messages_manager_->remove_sponsored_dialog();
      break;
    }
    case telegram_api::help_promoData::ID: {
      auto promo = telegram_api::move_object_as<telegram_api::help_promoData>(promo_data_ptr);
      expires_at = promo->expires_;
      bool is_proxy = promo->proxy_;
      td_->messages_manager_->on_get_sponsored_dialog(
          std::move(promo->peer_),
          is_proxy ? DialogSource::mtproto_proxy()
                   : DialogSource::public_service_announcement(promo->psa_type_, promo->psa_message_),
          std::move(promo->users_), std::move(promo->chats_));
      break;
    }
    default:
      UNREACHABLE();
  }
  if (need_reload_promo_data_) {
    need_reload_promo_data_ = false;
    expires_at = 0;
  }
  schedule_get_promo_data(expires_at == 0 ? 0 : expires_at - G()->unix_time());
}

}  // namespace td
