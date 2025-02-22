//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2022
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/GameManager.h"

#include "td/telegram/AccessRights.h"
#include "td/telegram/AuthManager.h"
#include "td/telegram/ContactsManager.h"
#include "td/telegram/DialogId.h"
#include "td/telegram/Global.h"
#include "td/telegram/InlineQueriesManager.h"
#include "td/telegram/MessageContentType.h"
#include "td/telegram/MessageId.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/net/DcId.h"
#include "td/telegram/net/NetActor.h"
#include "td/telegram/net/NetQueryCreator.h"
#include "td/telegram/SequenceDispatcher.h"
#include "td/telegram/Td.h"
#include "td/telegram/UpdatesManager.h"

#include "td/utils/buffer.h"
#include "td/utils/logging.h"
#include "td/utils/Status.h"

namespace td {

class SetGameScoreActor final : public NetActorOnce {
  Promise<Unit> promise_;
  DialogId dialog_id_;

 public:
  explicit SetGameScoreActor(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, MessageId message_id, bool edit_message,
            tl_object_ptr<telegram_api::InputUser> input_user, int32 score, bool force, uint64 sequence_dispatcher_id) {
    int32 flags = 0;
    if (edit_message) {
      flags |= telegram_api::messages_setGameScore::EDIT_MESSAGE_MASK;
    }
    if (force) {
      flags |= telegram_api::messages_setGameScore::FORCE_MASK;
    }

    dialog_id_ = dialog_id;

    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Edit);
    if (input_peer == nullptr) {
      on_error(Status::Error(400, "Can't access the chat"));
      stop();
      return;
    }

    CHECK(input_user != nullptr);
    auto query = G()->net_query_creator().create(
        telegram_api::messages_setGameScore(flags, false /*ignored*/, false /*ignored*/, std::move(input_peer),
                                            message_id.get_server_message_id().get(), std::move(input_user), score));

    query->debug("send to MultiSequenceDispatcher");
    send_closure(td_->messages_manager_->sequence_dispatcher_, &MultiSequenceDispatcher::send_with_callback,
                 std::move(query), actor_shared(this), sequence_dispatcher_id);
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_setGameScore>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto ptr = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for SetGameScore: " << to_string(ptr);
    td_->updates_manager_->on_get_updates(std::move(ptr), std::move(promise_));
  }

  void on_error(Status status) final {
    LOG(INFO) << "Receive error for SetGameScore: " << status;
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "SetGameScoreActor");
    promise_.set_error(std::move(status));
  }
};

class SetInlineGameScoreQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit SetInlineGameScoreQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(tl_object_ptr<telegram_api::InputBotInlineMessageID> input_bot_inline_message_id, bool edit_message,
            tl_object_ptr<telegram_api::InputUser> input_user, int32 score, bool force) {
    CHECK(input_bot_inline_message_id != nullptr);
    CHECK(input_user != nullptr);

    int32 flags = 0;
    if (edit_message) {
      flags |= telegram_api::messages_setInlineGameScore::EDIT_MESSAGE_MASK;
    }
    if (force) {
      flags |= telegram_api::messages_setInlineGameScore::FORCE_MASK;
    }

    auto dc_id = DcId::internal(InlineQueriesManager::get_inline_message_dc_id(input_bot_inline_message_id));
    send_query(G()->net_query_creator().create(
        telegram_api::messages_setInlineGameScore(flags, false /*ignored*/, false /*ignored*/,
                                                  std::move(input_bot_inline_message_id), std::move(input_user), score),
        dc_id));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_setInlineGameScore>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    LOG_IF(ERROR, !result_ptr.ok()) << "Receive false in result of setInlineGameScore";

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    LOG(INFO) << "Receive error for SetInlineGameScoreQuery: " << status;
    promise_.set_error(std::move(status));
  }
};

class GetGameHighScoresQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::gameHighScores>> promise_;
  DialogId dialog_id_;

 public:
  explicit GetGameHighScoresQuery(Promise<td_api::object_ptr<td_api::gameHighScores>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, MessageId message_id, tl_object_ptr<telegram_api::InputUser> input_user) {
    dialog_id_ = dialog_id;

    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
    CHECK(input_peer != nullptr);

    CHECK(input_user != nullptr);
    send_query(G()->net_query_creator().create(telegram_api::messages_getGameHighScores(
        std::move(input_peer), message_id.get_server_message_id().get(), std::move(input_user))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_getGameHighScores>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(td_->game_manager_->get_game_high_scores_object(result_ptr.move_as_ok()));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetGameHighScoresQuery");
    promise_.set_error(std::move(status));
  }
};

class GetInlineGameHighScoresQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::gameHighScores>> promise_;

 public:
  explicit GetInlineGameHighScoresQuery(Promise<td_api::object_ptr<td_api::gameHighScores>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(tl_object_ptr<telegram_api::InputBotInlineMessageID> input_bot_inline_message_id,
            tl_object_ptr<telegram_api::InputUser> input_user) {
    CHECK(input_bot_inline_message_id != nullptr);
    CHECK(input_user != nullptr);

    auto dc_id = DcId::internal(InlineQueriesManager::get_inline_message_dc_id(input_bot_inline_message_id));
    send_query(G()->net_query_creator().create(
        telegram_api::messages_getInlineGameHighScores(std::move(input_bot_inline_message_id), std::move(input_user)),
        dc_id));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_getInlineGameHighScores>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(td_->game_manager_->get_game_high_scores_object(result_ptr.move_as_ok()));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

GameManager::GameManager(Td *td, ActorShared<> parent) : td_(td), parent_(std::move(parent)) {
}

GameManager::~GameManager() = default;

void GameManager::tear_down() {
  parent_.reset();
}

void GameManager::set_game_score(FullMessageId full_message_id, bool edit_message, UserId user_id, int32 score,
                                 bool force, Promise<td_api::object_ptr<td_api::message>> &&promise) {
  CHECK(td_->auth_manager_->is_bot());

  if (!td_->messages_manager_->have_message_force(full_message_id, "set_game_score")) {
    return promise.set_error(Status::Error(400, "Message not found"));
  }

  auto dialog_id = full_message_id.get_dialog_id();
  if (!td_->messages_manager_->have_input_peer(dialog_id, AccessRights::Edit)) {
    return promise.set_error(Status::Error(400, "Can't access the chat"));
  }

  auto r_input_user = td_->contacts_manager_->get_input_user(user_id);
  if (r_input_user.is_error()) {
    return promise.set_error(r_input_user.move_as_error());
  }

  if (!td_->messages_manager_->can_set_game_score(full_message_id)) {
    return promise.set_error(Status::Error(400, "Game score can't be set"));
  }

  auto query_promise = PromiseCreator::lambda(
      [actor_id = actor_id(this), full_message_id, promise = std::move(promise)](Result<Unit> &&result) mutable {
        if (result.is_error()) {
          return promise.set_error(result.move_as_error());
        }
        send_closure(actor_id, &GameManager::on_set_game_score, full_message_id, std::move(promise));
      });
  send_closure(td_->create_net_actor<SetGameScoreActor>(std::move(query_promise)), &SetGameScoreActor::send, dialog_id,
               full_message_id.get_message_id(), edit_message, r_input_user.move_as_ok(), score, force,
               MessagesManager::get_sequence_dispatcher_id(dialog_id, MessageContentType::None));
}

void GameManager::on_set_game_score(FullMessageId full_message_id,
                                    Promise<td_api::object_ptr<td_api::message>> &&promise) {
  promise.set_value(td_->messages_manager_->get_message_object(full_message_id, "on_set_game_score"));
}

void GameManager::set_inline_game_score(const string &inline_message_id, bool edit_message, UserId user_id, int32 score,
                                        bool force, Promise<Unit> &&promise) {
  CHECK(td_->auth_manager_->is_bot());

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    return promise.set_error(Status::Error(400, "Invalid inline message identifier specified"));
  }

  auto r_input_user = td_->contacts_manager_->get_input_user(user_id);
  if (r_input_user.is_error()) {
    return promise.set_error(r_input_user.move_as_error());
  }

  td_->create_handler<SetInlineGameScoreQuery>(std::move(promise))
      ->send(std::move(input_bot_inline_message_id), edit_message, r_input_user.move_as_ok(), score, force);
}

void GameManager::get_game_high_scores(FullMessageId full_message_id, UserId user_id,
                                       Promise<td_api::object_ptr<td_api::gameHighScores>> &&promise) {
  CHECK(td_->auth_manager_->is_bot());

  if (!td_->messages_manager_->have_message_force(full_message_id, "get_game_high_scores")) {
    return promise.set_error(Status::Error(400, "Message not found"));
  }

  auto dialog_id = full_message_id.get_dialog_id();
  if (!td_->messages_manager_->have_input_peer(dialog_id, AccessRights::Read)) {
    return promise.set_error(Status::Error(400, "Can't access the chat"));
  }
  auto message_id = full_message_id.get_message_id();
  if (message_id.is_scheduled() || !message_id.is_server()) {
    return promise.set_error(Status::Error(400, "Wrong message identifier specified"));
  }

  auto r_input_user = td_->contacts_manager_->get_input_user(user_id);
  if (r_input_user.is_error()) {
    return promise.set_error(r_input_user.move_as_error());
  }

  td_->create_handler<GetGameHighScoresQuery>(std::move(promise))
      ->send(dialog_id, message_id, r_input_user.move_as_ok());
}

void GameManager::get_inline_game_high_scores(const string &inline_message_id, UserId user_id,
                                              Promise<td_api::object_ptr<td_api::gameHighScores>> &&promise) {
  CHECK(td_->auth_manager_->is_bot());

  auto input_bot_inline_message_id = td_->inline_queries_manager_->get_input_bot_inline_message_id(inline_message_id);
  if (input_bot_inline_message_id == nullptr) {
    return promise.set_error(Status::Error(400, "Invalid inline message identifier specified"));
  }

  auto r_input_user = td_->contacts_manager_->get_input_user(user_id);
  if (r_input_user.is_error()) {
    return promise.set_error(r_input_user.move_as_error());
  }

  td_->create_handler<GetInlineGameHighScoresQuery>(std::move(promise))
      ->send(std::move(input_bot_inline_message_id), r_input_user.move_as_ok());
}

td_api::object_ptr<td_api::gameHighScores> GameManager::get_game_high_scores_object(
    telegram_api::object_ptr<telegram_api::messages_highScores> &&high_scores) {
  td_->contacts_manager_->on_get_users(std::move(high_scores->users_), "get_game_high_scores_object");

  auto result = td_api::make_object<td_api::gameHighScores>();
  for (const auto &high_score : high_scores->scores_) {
    int32 position = high_score->pos_;
    UserId user_id(high_score->user_id_);
    int32 score = high_score->score_;
    if (position <= 0 || !user_id.is_valid() || score < 0) {
      LOG(ERROR) << "Receive wrong " << to_string(high_score);
      continue;
    }
    result->scores_.push_back(make_tl_object<td_api::gameHighScore>(
        position, td_->contacts_manager_->get_user_id_object(user_id, "get_game_high_scores_object"), score));
  }
  return result;
}

}  // namespace td
