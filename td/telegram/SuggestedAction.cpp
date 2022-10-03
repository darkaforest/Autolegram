//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/SuggestedAction.h"

#include "td/telegram/ChannelId.h"
#include "td/telegram/ConfigManager.h"
#include "td/telegram/ContactsManager.h"
#include "td/telegram/Global.h"
#include "td/telegram/Td.h"

#include "td/actor/actor.h"

#include "td/utils/algorithm.h"
#include "td/utils/misc.h"
#include "td/utils/Status.h"

#include <algorithm>

namespace td {

void SuggestedAction::init(Type type) {
  type_ = type;
}

SuggestedAction::SuggestedAction(Slice action_str) {
  if (action_str == Slice("AUTOARCHIVE_POPULAR")) {
    init(Type::EnableArchiveAndMuteNewChats);
  } else if (action_str == Slice("VALIDATE_PASSWORD")) {
    init(Type::CheckPassword);
  } else if (action_str == Slice("VALIDATE_PHONE_NUMBER")) {
    init(Type::CheckPhoneNumber);
  } else if (action_str == Slice("NEWCOMER_TICKS")) {
    init(Type::ViewChecksHint);
  }
}

SuggestedAction::SuggestedAction(Slice action_str, DialogId dialog_id) {
  CHECK(dialog_id.is_valid());
  if (action_str == Slice("CONVERT_GIGAGROUP")) {
    type_ = Type::ConvertToGigagroup;
    dialog_id_ = dialog_id;
  }
}

SuggestedAction::SuggestedAction(const td_api::object_ptr<td_api::SuggestedAction> &suggested_action) {
  if (suggested_action == nullptr) {
    return;
  }
  switch (suggested_action->get_id()) {
    case td_api::suggestedActionEnableArchiveAndMuteNewChats::ID:
      init(Type::EnableArchiveAndMuteNewChats);
      break;
    case td_api::suggestedActionCheckPassword::ID:
      init(Type::CheckPassword);
      break;
    case td_api::suggestedActionCheckPhoneNumber::ID:
      init(Type::CheckPhoneNumber);
      break;
    case td_api::suggestedActionViewChecksHint::ID:
      init(Type::ViewChecksHint);
      break;
    case td_api::suggestedActionConvertToBroadcastGroup::ID: {
      auto action = static_cast<const td_api::suggestedActionConvertToBroadcastGroup *>(suggested_action.get());
      ChannelId channel_id(action->supergroup_id_);
      if (channel_id.is_valid()) {
        type_ = Type::ConvertToGigagroup;
        dialog_id_ = DialogId(channel_id);
      }
      break;
    }
    case td_api::suggestedActionSetPassword::ID: {
      auto action = static_cast<const td_api::suggestedActionSetPassword *>(suggested_action.get());
      type_ = Type::SetPassword;
      otherwise_relogin_days_ = action->authorization_delay_;
      break;
    }
    default:
      UNREACHABLE();
  }
}

string SuggestedAction::get_suggested_action_str() const {
  switch (type_) {
    case Type::EnableArchiveAndMuteNewChats:
      return "AUTOARCHIVE_POPULAR";
    case Type::CheckPassword:
      return "VALIDATE_PASSWORD";
    case Type::CheckPhoneNumber:
      return "VALIDATE_PHONE_NUMBER";
    case Type::ViewChecksHint:
      return "NEWCOMER_TICKS";
    case Type::ConvertToGigagroup:
      return "CONVERT_GIGAGROUP";
    default:
      return string();
  }
}

td_api::object_ptr<td_api::SuggestedAction> SuggestedAction::get_suggested_action_object() const {
  switch (type_) {
    case Type::Empty:
      return nullptr;
    case Type::EnableArchiveAndMuteNewChats:
      return td_api::make_object<td_api::suggestedActionEnableArchiveAndMuteNewChats>();
    case Type::CheckPassword:
      return td_api::make_object<td_api::suggestedActionCheckPassword>();
    case Type::CheckPhoneNumber:
      return td_api::make_object<td_api::suggestedActionCheckPhoneNumber>();
    case Type::ViewChecksHint:
      return td_api::make_object<td_api::suggestedActionViewChecksHint>();
    case Type::ConvertToGigagroup:
      return td_api::make_object<td_api::suggestedActionConvertToBroadcastGroup>(dialog_id_.get_channel_id().get());
    case Type::SetPassword:
      return td_api::make_object<td_api::suggestedActionSetPassword>(otherwise_relogin_days_);
    default:
      UNREACHABLE();
      return nullptr;
  }
}

td_api::object_ptr<td_api::updateSuggestedActions> get_update_suggested_actions_object(
    const vector<SuggestedAction> &added_actions, const vector<SuggestedAction> &removed_actions) {
  auto get_object = [](const SuggestedAction &action) {
    return action.get_suggested_action_object();
  };
  return td_api::make_object<td_api::updateSuggestedActions>(transform(added_actions, get_object),
                                                             transform(removed_actions, get_object));
}

void update_suggested_actions(vector<SuggestedAction> &suggested_actions,
                              vector<SuggestedAction> &&new_suggested_actions) {
  td::unique(new_suggested_actions);
  if (new_suggested_actions == suggested_actions) {
    return;
  }

  vector<SuggestedAction> added_actions;
  vector<SuggestedAction> removed_actions;
  auto old_it = suggested_actions.begin();
  auto new_it = new_suggested_actions.begin();
  while (old_it != suggested_actions.end() || new_it != new_suggested_actions.end()) {
    if (old_it != suggested_actions.end() && (new_it == new_suggested_actions.end() || *old_it < *new_it)) {
      removed_actions.push_back(*old_it++);
    } else if (old_it == suggested_actions.end() || *new_it < *old_it) {
      added_actions.push_back(*new_it++);
    } else {
      ++old_it;
      ++new_it;
    }
  }
  CHECK(!added_actions.empty() || !removed_actions.empty());
  suggested_actions = std::move(new_suggested_actions);
  send_closure(G()->td(), &Td::send_update, get_update_suggested_actions_object(added_actions, removed_actions));
}

void remove_suggested_action(vector<SuggestedAction> &suggested_actions, SuggestedAction suggested_action) {
  if (td::remove(suggested_actions, suggested_action)) {
    send_closure(G()->td(), &Td::send_update, get_update_suggested_actions_object({}, {suggested_action}));
  }
}

void dismiss_suggested_action(SuggestedAction action, Promise<Unit> &&promise) {
  switch (action.type_) {
    case SuggestedAction::Type::Empty:
      return promise.set_error(Status::Error(400, "Action must be non-empty"));
    case SuggestedAction::Type::EnableArchiveAndMuteNewChats:
    case SuggestedAction::Type::CheckPassword:
    case SuggestedAction::Type::CheckPhoneNumber:
    case SuggestedAction::Type::ViewChecksHint:
      return send_closure_later(G()->config_manager(), &ConfigManager::dismiss_suggested_action, std::move(action),
                                std::move(promise));
    case SuggestedAction::Type::ConvertToGigagroup:
      return send_closure_later(G()->contacts_manager(), &ContactsManager::dismiss_dialog_suggested_action,
                                std::move(action), std::move(promise));
    case SuggestedAction::Type::SetPassword: {
      if (action.otherwise_relogin_days_ <= 0) {
        return promise.set_error(Status::Error(400, "Invalid authorization_delay specified"));
      }
      auto days = narrow_cast<int32>(G()->get_option_integer("otherwise_relogin_days"));
      if (days == action.otherwise_relogin_days_) {
        vector<SuggestedAction> removed_actions{SuggestedAction{SuggestedAction::Type::SetPassword, DialogId(), days}};
        send_closure(G()->td(), &Td::send_update, get_update_suggested_actions_object({}, removed_actions));
        G()->set_option_empty("otherwise_relogin_days");
      }
      return promise.set_value(Unit());
    }
    default:
      UNREACHABLE();
      return;
  }
}

}  // namespace td
