//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/Dependencies.h"

#include "td/telegram/ContactsManager.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/Td.h"
#include "td/telegram/WebPagesManager.h"

#include "td/utils/common.h"
#include "td/utils/logging.h"

namespace td {

void Dependencies::add(UserId user_id) {
  if (user_id.is_valid()) {
    user_ids.insert(user_id);
  }
}

void Dependencies::add(ChatId chat_id) {
  if (chat_id.is_valid()) {
    chat_ids.insert(chat_id);
  }
}

void Dependencies::add(ChannelId channel_id) {
  if (channel_id.is_valid()) {
    channel_ids.insert(channel_id);
  }
}

void Dependencies::add(SecretChatId secret_chat_id) {
  if (secret_chat_id.is_valid()) {
    secret_chat_ids.insert(secret_chat_id);
  }
}

void Dependencies::add(WebPageId web_page_id) {
  if (web_page_id.is_valid()) {
    web_page_ids.insert(web_page_id);
  }
}

void Dependencies::add_dialog_and_dependencies(DialogId dialog_id) {
  if (dialog_id.is_valid() && dialog_ids.insert(dialog_id).second) {
    add_dialog_dependencies(dialog_id);
  }
}

void Dependencies::add_dialog_dependencies(DialogId dialog_id) {
  switch (dialog_id.get_type()) {
    case DialogType::User:
      add(dialog_id.get_user_id());
      break;
    case DialogType::Chat:
      add(dialog_id.get_chat_id());
      break;
    case DialogType::Channel:
      add(dialog_id.get_channel_id());
      break;
    case DialogType::SecretChat:
      add(dialog_id.get_secret_chat_id());
      break;
    case DialogType::None:
      break;
    default:
      UNREACHABLE();
  }
}

void Dependencies::add_message_sender_dependencies(DialogId dialog_id) {
  if (dialog_id.get_type() == DialogType::User) {
    add(dialog_id.get_user_id());
  } else {
    add_dialog_and_dependencies(dialog_id);
  }
}

bool Dependencies::resolve_force(Td *td, const char *source) const {
  bool success = true;
  for (auto user_id : user_ids) {
    if (!td->contacts_manager_->have_user_force(user_id)) {
      LOG(ERROR) << "Can't find " << user_id << " from " << source;
      success = false;
    }
  }
  for (auto chat_id : chat_ids) {
    if (!td->contacts_manager_->have_chat_force(chat_id)) {
      LOG(ERROR) << "Can't find " << chat_id << " from " << source;
      success = false;
    }
  }
  for (auto channel_id : channel_ids) {
    if (!td->contacts_manager_->have_channel_force(channel_id)) {
      if (td->contacts_manager_->have_min_channel(channel_id)) {
        LOG(INFO) << "Can't find " << channel_id << " from " << source << ", but have it as a min-channel";
        continue;
      }
      LOG(ERROR) << "Can't find " << channel_id << " from " << source;
      success = false;
    }
  }
  for (auto secret_chat_id : secret_chat_ids) {
    if (!td->contacts_manager_->have_secret_chat_force(secret_chat_id)) {
      LOG(ERROR) << "Can't find " << secret_chat_id << " from " << source;
      success = false;
    }
  }
  for (auto dialog_id : dialog_ids) {
    if (!td->messages_manager_->have_dialog_force(dialog_id, source)) {
      LOG(ERROR) << "Can't find " << dialog_id << " from " << source;
      td->messages_manager_->force_create_dialog(dialog_id, "resolve_dependencies_force", true);
      success = false;
    }
  }
  for (auto web_page_id : web_page_ids) {
    if (!td->web_pages_manager_->have_web_page_force(web_page_id)) {
      LOG(INFO) << "Can't find " << web_page_id << " from " << source;
      success = false;
    }
  }
  return success;
}

}  // namespace td
