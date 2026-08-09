// Copyright 2024 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "rmw_client_data.hpp"

#include <fastcdr/FastBuffer.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <zenoh.hxx>

#include "attachment_helpers.hpp"
#include "cdr.hpp"
#include "liveliness_utils.hpp"
#include "logging_macros.hpp"
#include "message_type_support.hpp"
#include "qos.hpp"
#include "rmw_context_impl_s.hpp"

#include "rcpputils/scope_exit.hpp"

#include "rcutils/env.h"

#include "rmw/error_handling.h"
#include "rmw/get_topic_endpoint_info.h"
#include "rmw/impl/cpp/macros.hpp"

#include "tracetools/tracetools.h"

namespace rmw_zenoh_cpp
{
namespace
{
///=============================================================================
// Parse a non-negative integer environment variable, returning
// default_value if the variable is unset, empty, or invalid.
int64_t parse_non_negative_int_envar(const char * envar_name, int64_t default_value)
{
  const char * envar_value = nullptr;
  if (nullptr != rcutils_get_env(envar_name, &envar_value)) {
    RMW_ZENOH_LOG_WARN_NAMED(
      "rmw_zenoh_cpp",
      "Unable to read environment variable %s, using default value %" PRId64 ".",
      envar_name, default_value);
    return default_value;
  }
  if (envar_value == nullptr || strcmp(envar_value, "") == 0) {
    return default_value;
  }
  char * end = nullptr;
  errno = 0;
  const int64_t value = std::strtoll(envar_value, &end, 10);
  if (errno != 0 || end == envar_value || *end != '\0' || value < 0) {
    RMW_ZENOH_LOG_WARN_NAMED(
      "rmw_zenoh_cpp",
      "Invalid value '%s' for environment variable %s, using default value %" PRId64 ".",
      envar_value, envar_name, default_value);
    return default_value;
  }
  return value;
}

///=============================================================================
// Tunables for the bounded service-request retry, each read once on first
// use:
//
// RMW_ZENOH_SERVICE_REQUEST_RETRIES (default 3): maximum number of times a
// service request is re-issued after its zenoh query was finalized without
// any reply. Under connectivity churn the transport can fail to schedule
// the query message, in which case the query is finalized immediately
// (ResponseFinal with zero replies); without a retry the rcl request would
// pend forever.
//
// RMW_ZENOH_SERVICE_REQUEST_RETRY_WINDOW_MS (default 5000): a request is
// only re-issued if the failed query lived for less than this window since
// the request was first sent. A query that lived longer may well have
// reached the server (e.g. it timed out while the server was processing
// it), and re-sending a possibly-delivered non-idempotent request is worse
// than failing.
//
// Each re-issue is delayed by an exponential backoff: 100 ms before
// attempt 2, 500 ms before attempt 3 and 2000 ms before attempt 4 (and any
// later attempt), clamped so the re-issue still happens inside the retry
// window. Production evidence showed that immediate re-issues are useless:
// the first send and every retry all fast-finalized within ~20 ms, inside
// the same connectivity-failure window, after which the request was
// reported lost. The default schedule totals 2600 ms, which fits the
// default 5000 ms retry window.
std::size_t service_request_max_retries()
{
  static const std::size_t retries = static_cast<std::size_t>(
    parse_non_negative_int_envar("RMW_ZENOH_SERVICE_REQUEST_RETRIES", 3));
  return retries;
}

std::chrono::milliseconds service_request_retry_window()
{
  static const std::chrono::milliseconds window{
    parse_non_negative_int_envar("RMW_ZENOH_SERVICE_REQUEST_RETRY_WINDOW_MS", 5000)};
  return window;
}

// Backoff to apply before re-issuing attempt `next_attempt` (>= 2).
std::chrono::milliseconds service_request_retry_backoff(std::size_t next_attempt)
{
  static constexpr std::chrono::milliseconds schedule[] = {
    std::chrono::milliseconds(100),
    std::chrono::milliseconds(500),
    std::chrono::milliseconds(2000)};
  static constexpr std::size_t schedule_size = sizeof(schedule) / sizeof(schedule[0]);
  const std::size_t index = next_attempt < 2 ?
    0 : std::min(next_attempt - 2, schedule_size - 1);
  return schedule[index];
}

// Whether the current thread is inside ClientData::issue_query(). zenoh-c
// drops the reply closure inline when z_querier_get fails synchronously,
// which fires the on_drop handler re-entrantly on this thread. This flag
// lets on_query_finalized() distinguish that local failure (keep the
// original "return an error to the caller" semantics, no retry) from an
// asynchronous query finalization (retry candidate).
thread_local bool issuing_query_on_this_thread = false;
}  // namespace

///=============================================================================
std::shared_ptr<ClientData> ClientData::make(
  std::shared_ptr<zenoh::Session> session,
  const rmw_node_t * const node,
  const rmw_client_t * client,
  liveliness::NodeInfo node_info,
  std::size_t node_id,
  std::size_t service_id,
  const std::string & service_name,
  const rosidl_service_type_support_t * type_support,
  const rmw_qos_profile_t * qos_profile)
{
  // Adapt any 'best available' QoS options
  rmw_qos_profile_t adapted_qos_profile = *qos_profile;
  rmw_ret_t ret = QoS::get().best_available_qos(
    nullptr, nullptr, &adapted_qos_profile, nullptr);
  if (RMW_RET_OK != ret) {
    RMW_SET_ERROR_MSG("Failed to obtain adapted_qos_profile.");
    return nullptr;
  }

  rcutils_allocator_t * allocator = &node->context->options.allocator;

  const rosidl_type_hash_t * type_hash = type_support->get_type_hash_func(type_support);
  auto service_members = static_cast<const service_type_support_callbacks_t *>(type_support->data);
  auto request_members = static_cast<const message_type_support_callbacks_t *>(
    service_members->request_members_->data);
  auto response_members = static_cast<const message_type_support_callbacks_t *>(
    service_members->response_members_->data);
  auto request_type_support = std::make_shared<RequestTypeSupport>(service_members);
  auto response_type_support = std::make_shared<ResponseTypeSupport>(service_members);

  // Note: Service request/response types will contain a suffix Request_ or Response_.
  // We remove the suffix when appending the type to the liveliness tokens for
  // better reusability within GraphCache.
  std::string service_type = request_type_support->get_name();
  size_t suffix_substring_position = service_type.rfind("Request_");
  if (std::string::npos != suffix_substring_position) {
    service_type = service_type.substr(0, suffix_substring_position);
  } else {
    RMW_ZENOH_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "Unexpected type %s for client %s. Report this bug",
      service_type.c_str(), service_name.c_str());
    return nullptr;
  }

  // Convert the type hash to a string so that it can be included in the keyexpr.
  char * type_hash_c_str = nullptr;
  rcutils_ret_t stringify_ret = rosidl_stringify_type_hash(
    type_hash,
    *allocator,
    &type_hash_c_str);
  if (RCUTILS_RET_BAD_ALLOC == stringify_ret) {
    // rosidl_stringify_type_hash already set the error
    return nullptr;
  }
  auto free_type_hash_c_str = rcpputils::make_scope_exit(
    [&allocator, &type_hash_c_str]() {
      allocator->deallocate(type_hash_c_str, allocator->state);
    });

  std::size_t domain_id = node_info.domain_id_;
  auto entity = liveliness::Entity::make(
    session->get_zid(),
    std::to_string(node_id),
    std::to_string(service_id),
    liveliness::EntityType::Client,
    std::move(node_info),
    liveliness::TopicInfo{
      std::move(domain_id),
      service_name,
      std::move(service_type),
      type_hash_c_str,
      std::move(adapted_qos_profile)}
  );
  if (entity == nullptr) {
    RMW_ZENOH_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "Unable to generate keyexpr for liveliness token for the client %s.",
      service_name.c_str());
    return nullptr;
  }

  zenoh::KeyExpr querier_ke(entity->topic_info()->topic_keyexpr_);
  auto options = zenoh::Session::QuerierOptions::create_default();
  options.target = Z_QUERY_TARGET_ALL_COMPLETE;
  // Among action-related services, only get_result usually requires a long response time.
  // In most scenarios, a shorter timeout is sufficient and helps prevent excessive waiting
  // in case a service reply is missed.
  if (querier_ke.intersects(zenoh::KeyExpr("**/_action/get_result/**"))) {
    // The default timeout for a z_get query is 10 seconds and if a response is not received within
    // this window, the queryable will return an invalid reply. However, it is common for actions,
    // which are implemented using services, to take an extended duration to complete. Hence, we set
    // the timeout_ms to the largest supported value to account for most realistic scenarios.
    options.timeout_ms = std::numeric_limits<uint64_t>::max();
  }
  // Latest consolidation guarantees unicity of replies for the same key expression,
  // which optimizes bandwidth. The default is "None", which imples replies may come in any order
  // and any number.
  options.consolidation = zenoh::ConsolidationMode::Z_CONSOLIDATION_MODE_NONE;
  auto querier = session->declare_querier(querier_ke, std::move(options));

  std::shared_ptr<ClientData> client_data = std::shared_ptr<ClientData>(
    new ClientData{
      node,
      client,
      entity,
      session,
      std::move(querier),
      request_members,
      response_members,
      request_type_support,
      response_type_support
    });

  return client_data;
}

///=============================================================================
ClientData::ClientData(
  const rmw_node_t * rmw_node,
  const rmw_client_t * rmw_client,
  std::shared_ptr<liveliness::Entity> entity,
  std::shared_ptr<zenoh::Session> sess,
  zenoh::Querier querier,
  const void * request_type_support_impl,
  const void * response_type_support_impl,
  std::shared_ptr<RequestTypeSupport> request_type_support,
  std::shared_ptr<ResponseTypeSupport> response_type_support)
: rmw_node_(rmw_node),
  rmw_client_(rmw_client),
  entity_(std::move(entity)),
  sess_(std::move(sess)),
  querier_(std::move(querier)),
  request_type_support_impl_(request_type_support_impl),
  response_type_support_impl_(response_type_support_impl),
  request_type_support_(request_type_support),
  response_type_support_(response_type_support),
  wait_set_data_(nullptr),
  sequence_number_(1),
  is_shutdown_(false),
  initialized_(false)
{
  std::string liveliness_keyexpr = this->entity_->liveliness_keyexpr();
  zenoh::ZResult result;
  this->token_ = sess_->liveliness_declare_token(
    zenoh::KeyExpr(liveliness_keyexpr),
    zenoh::Session::LivelinessDeclarationOptions::create_default(),
    &result);
  if (result != Z_OK) {
    RMW_ZENOH_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "Unable to create liveliness token for the client.");
    throw std::runtime_error("Unable to create liveliness token for the client.");
  }

  initialized_ = true;
}

///=============================================================================
const liveliness::TopicInfo & ClientData::topic_info() const
{
  return entity_->topic_info().value();
}

///=============================================================================
bool ClientData::liveliness_is_valid() const
{
  // The z_check function is now internal in zenoh-1.0.0 so we assume
  // the liveliness token is still initialized as long as this entity has
  // not been shutdown.
  return !is_shutdown_.load(std::memory_order_acquire);
}

///=============================================================================
std::array<uint8_t, RMW_GID_STORAGE_SIZE> ClientData::copy_gid() const
{
  return entity_->copy_gid();
}

///=============================================================================
void ClientData::add_new_reply(std::unique_ptr<ZenohReply> reply)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const rmw_qos_profile_t adapted_qos_profile =
    entity_->topic_info().value().qos_;
  if (adapted_qos_profile.history != RMW_QOS_POLICY_HISTORY_KEEP_ALL &&
    reply_queue_.size() >= adapted_qos_profile.depth)
  {
    // Log warning if message is discarded due to hitting the queue depth
    RMW_ZENOH_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "Query queue depth of %ld reached, discarding oldest Query "
      "for client for %s",
      adapted_qos_profile.depth,
      this->entity_->topic_info().value().topic_keyexpr_.c_str());
    reply_queue_.pop_front();
  }
  reply_queue_.emplace_back(std::move(reply));

  // Since we added new data, trigger user callback and guard condition if they are available
  data_callback_mgr_.trigger_callback();
  if (wait_set_data_ != nullptr) {
    std::lock_guard<std::mutex> wait_set_lock(wait_set_data_->condition_mutex);
    wait_set_data_->triggered = true;
    wait_set_data_->condition_variable.notify_one();
  }
}

///=============================================================================
rmw_ret_t ClientData::take_response(
  rmw_service_info_t * request_header,
  void * ros_response,
  bool * taken)
{
  std::lock_guard<std::mutex> lock(mutex_);
  *taken = false;

  if (this->is_shutdown() || reply_queue_.empty()) {
    // This tells rcl that the check for a new message was done, but no messages have come in yet.
    return RMW_RET_OK;
  }
  std::unique_ptr<ZenohReply> latest_reply = std::move(reply_queue_.front());
  reply_queue_.pop_front();

  auto & reply = latest_reply->get_sample();

  if (!reply.is_ok()) {
    RMW_SET_ERROR_MSG("invalid reply sample");
    return RMW_RET_ERROR;
  }

  const zenoh::Sample & sample = reply.get_ok();

  // Object that manages the raw buffer
  std::vector<uint8_t> payload = sample.get_payload().as_vector();
  if (payload.size() == 0) {
    RMW_ZENOH_LOG_DEBUG_NAMED(
      "rmw_zenoh_cpp",
      "ClientData not able to get slice data");
    return RMW_RET_ERROR;
  }

  // Fill in the request_header
  if (!sample.get_attachment().has_value()) {
    RMW_ZENOH_LOG_DEBUG_NAMED(
      "rmw_zenoh_cpp",
      "ClientData take_request attachment is empty");
    return RMW_RET_ERROR;
  }

  eprosima::fastcdr::FastBuffer fastbuffer(
    reinterpret_cast<char *>(const_cast<uint8_t *>(payload.data())), payload.size());

  // Object that serializes the data
  rmw_zenoh_cpp::Cdr deser(fastbuffer);
  if (!response_type_support_->deserialize_ros_message(
      deser.get_cdr(),
      ros_response,
      response_type_support_impl_))
  {
    RMW_SET_ERROR_MSG("could not deserialize ROS response");
    return RMW_RET_ERROR;
  }

  rmw_zenoh_cpp::AttachmentData attachment(std::move(sample.get_attachment().value().get()));
  request_header->request_id.sequence_number = attachment.sequence_number();
  if (request_header->request_id.sequence_number < 0) {
    RMW_SET_ERROR_MSG("Failed to get sequence_number from client call attachment");
    return RMW_RET_ERROR;
  }
  request_header->source_timestamp = attachment.source_timestamp();
  if (request_header->source_timestamp < 0) {
    RMW_SET_ERROR_MSG("Failed to get source_timestamp from client call attachment");
    return RMW_RET_ERROR;
  }
  memcpy(
    request_header->request_id.writer_guid,
    attachment.copy_gid().data(),
    RMW_GID_STORAGE_SIZE);
  request_header->received_timestamp = latest_reply->get_received_timestamp();

  *taken = true;

  return RMW_RET_OK;
}

///=============================================================================
rmw_ret_t ClientData::send_request(
  const void * ros_request,
  int64_t * sequence_id)
{
  std::unique_lock<std::mutex> lock(mutex_);
  if (this->is_shutdown()) {
    return RMW_RET_OK;
  }

  rcutils_allocator_t * allocator = &rmw_node_->context->options.allocator;
  rmw_context_impl_s * context_impl = static_cast<rmw_context_impl_s *>(rmw_node_->context->impl);
  if (context_impl == nullptr) {
    return RMW_RET_INVALID_ARGUMENT;
  }

  size_t max_data_length = (
    request_type_support_->get_estimated_serialized_size(
      ros_request, request_type_support_impl_));

  // Init serialized message byte array
  char * request_bytes = static_cast<char *>(allocator->allocate(
      max_data_length,
      allocator->state));
  if (!request_bytes) {
    RMW_SET_ERROR_MSG("failed allocate request message bytes");
    return RMW_RET_ERROR;
  }
  auto always_free_request_bytes = rcpputils::make_scope_exit(
    [request_bytes, allocator]() {
      allocator->deallocate(request_bytes, allocator->state);
    });

  // Object that manages the raw buffer
  eprosima::fastcdr::FastBuffer fastbuffer(request_bytes, max_data_length);
  // Object that serializes the data
  Cdr ser(fastbuffer);
  if (!request_type_support_->serialize_ros_message(
      ros_request,
      ser.get_cdr(),
      request_type_support_impl_))
  {
    return RMW_RET_ERROR;
  }
  size_t data_length = ser.get_serialized_data_length();
  *sequence_id = sequence_number_++;

  TRACETOOLS_TRACEPOINT(
    rmw_send_request,
    static_cast<const void *>(rmw_client_),
    static_cast<const void *>(ros_request),
    *sequence_id);

  // Track the in-flight request so that the on_drop closure of the query
  // can detect a query that was finalized without any reply (e.g. when the
  // transport failed to schedule the request under connectivity churn) and
  // re-issue it. The entry must be inserted before the query is issued:
  // the reply and drop closures may fire before querier_.get() returns.
  std::vector<uint8_t> request_payload(
    reinterpret_cast<const uint8_t *>(request_bytes),
    reinterpret_cast<const uint8_t *>(request_bytes) + data_length);
  in_flight_requests_.emplace(
    *sequence_id,
    InFlightRequest{request_payload, 1, std::chrono::steady_clock::now(), false});

  // We explicitly release the mutex here to avoid an ABBA deadlock as
  // documented in https://github.com/ros2/rmw_zenoh/issues/484.
  lock.unlock();
  return this->issue_query(*sequence_id, 1, request_payload);
}

///=============================================================================
rmw_ret_t ClientData::issue_query(
  int64_t sequence_id,
  std::size_t attempt,
  const std::vector<uint8_t> & request_payload)
{
  // NOTE: This method must be called WITHOUT mutex_ held: calling
  // querier_.get() while holding mutex_ can trigger the ABBA deadlock
  // documented in https://github.com/ros2/rmw_zenoh/issues/484.
  auto opts = zenoh::Querier::GetOptions::create_default();
  int64_t source_timestamp = rmw_zenoh_cpp::get_system_time_in_ns();
  opts.attachment = rmw_zenoh_cpp::AttachmentData(
    sequence_id, source_timestamp, entity_->copy_gid()).serialize_to_zbytes();

  std::vector<uint8_t> raw_bytes(request_payload);
  opts.payload = zenoh::Bytes(std::move(raw_bytes));

  std::weak_ptr<rmw_zenoh_cpp::ClientData> client_data = shared_from_this();
  zenoh::ZResult result;
  std::string parameters;
  issuing_query_on_this_thread = true;
  auto restore_issuing_flag = rcpputils::make_scope_exit(
    []() {issuing_query_on_this_thread = false;});
  querier_.get(
    parameters,
    [client_data, sequence_id](const zenoh::Reply & reply) {
      auto sub_data = client_data.lock();
      if (sub_data != nullptr) {
        // Mark the request as replied even for an error reply: an error
        // reply proves the round-trip worked, so the on_drop closure must
        // not re-issue the request.
        sub_data->mark_query_replied(sequence_id);
      }
      if (!reply.is_ok()) {
        auto reply_err_str = reply.get_err().get_payload().as_string();
        if (sub_data != nullptr && sub_data->entity_ != nullptr) {
          const auto & topic_info = sub_data->entity_->topic_info();
          if (topic_info.has_value()) {
            RMW_ZENOH_LOG_ERROR_NAMED(
              "rmw_zenoh_cpp",
              "z_reply_is_ok returned False Reason: %s for service '%s'",
              reply_err_str.c_str(), topic_info->name_.c_str());
          } else {
            RMW_ZENOH_LOG_ERROR_NAMED(
              "rmw_zenoh_cpp",
              "z_reply_is_ok returned False Reason: %s",
              reply_err_str.c_str());
          }
        } else {
          RMW_ZENOH_LOG_ERROR_NAMED(
            "rmw_zenoh_cpp",
            "z_reply_is_ok returned False Reason: %s",
            reply_err_str.c_str());
        }
        return;
      }
      const zenoh::Sample & sample = reply.get_ok();

      if (sub_data == nullptr) {
        RMW_ZENOH_LOG_ERROR_NAMED(
          "rmw_zenoh_cpp",
          "Unable to obtain ClientData from data for %s.",
          std::string(sample.get_keyexpr().as_string_view()).c_str());
        return;
      }

      if (sub_data->is_shutdown()) {
        return;
      }

      sub_data->add_new_reply(
        std::make_unique<rmw_zenoh_cpp::ZenohReply>(reply, get_system_time_in_ns()));
    },
    [client_data, sequence_id, attempt]() {
      // This closure is invoked on a zenoh thread once the query is
      // finalized (ResponseFinal received, query timed out, or the query
      // dropped without ever being scheduled). Keep it non-blocking.
      auto sub_data = client_data.lock();
      if (sub_data == nullptr || sub_data->is_shutdown()) {
        return;
      }
      sub_data->on_query_finalized(sequence_id, attempt);
    },
    std::move(opts),
    &result);
  if (result != Z_OK) {
    // The reply closure was dropped inline by the failed get, so the
    // in-flight bookkeeping for this attempt has already been cleaned up
    // by on_query_finalized().
    RMW_ZENOH_LOG_DEBUG_NAMED(
      "rmw_zenoh_cpp",
      "ClientData unable to call get");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}

///=============================================================================
void ClientData::on_query_finalized(int64_t sequence_id, std::size_t attempt)
{
  std::vector<uint8_t> request_payload;
  std::size_t next_attempt = 0;
  std::string service_name;
  std::chrono::milliseconds backoff{0};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = in_flight_requests_.find(sequence_id);
    if (it == in_flight_requests_.end()) {
      return;
    }
    InFlightRequest & request = it->second;
    if (request.attempts != attempt) {
      // A newer attempt for this sequence_id has been issued; this drop
      // notification is stale. Defensive: each attempt fires exactly one
      // drop, so this should not happen.
      return;
    }
    if (request.replied || this->is_shutdown()) {
      // The request received a reply (possibly an error reply) before the
      // query was finalized: the round-trip worked, nothing to retry.
      in_flight_requests_.erase(it);
      return;
    }
    if (issuing_query_on_this_thread) {
      // querier_.get() failed synchronously and dropped the closure
      // inline. Keep the original semantics: issue_query() reports the
      // error to its caller; do not retry from here.
      if (attempt > 1) {
        RMW_ZENOH_LOG_ERROR_NAMED(
          "rmw_zenoh_cpp",
          "Service request to '%s' with sequence_id %" PRId64
          " lost after %zu attempt(s): unable to re-issue the query.",
          entity_->topic_info().value().name_.c_str(), sequence_id, attempt);
      }
      in_flight_requests_.erase(it);
      return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - request.first_sent_time;
    if (request.attempts >= 1 + service_request_max_retries() ||
      elapsed >= service_request_retry_window())
    {
      RMW_ZENOH_LOG_ERROR_NAMED(
        "rmw_zenoh_cpp",
        "Service request to '%s' with sequence_id %" PRId64
        " lost after %zu attempt(s).",
        entity_->topic_info().value().name_.c_str(), sequence_id, request.attempts);
      in_flight_requests_.erase(it);
      return;
    }
    request.attempts++;
    next_attempt = request.attempts;
    request_payload = request.request_bytes;
    service_name = entity_->topic_info().value().name_;
    // Back off before re-issuing, clamped so the re-issue still happens
    // inside the retry window. `elapsed < window` was checked above, so
    // the remaining budget is positive.
    backoff = std::min(
      service_request_retry_backoff(next_attempt),
      std::chrono::duration_cast<std::chrono::milliseconds>(
        service_request_retry_window() - elapsed));
  }
  RMW_ZENOH_LOG_WARN_NAMED(
    "rmw_zenoh_cpp",
    "Query for service request to '%s' with sequence_id %" PRId64
    " was finalized without a reply; re-issuing in %" PRId64
    " ms (attempt %zu of %zu).",
    service_name.c_str(), sequence_id, static_cast<int64_t>(backoff.count()),
    next_attempt, 1 + service_request_max_retries());
  // This method runs on a zenoh callback thread that must never block, so
  // the backoff sleep and the re-issue run on a short-lived detached
  // thread. Retries are rare (the transport failed to schedule a query),
  // so spawning one thread per retry is acceptable. The thread only holds
  // a weak_ptr: it re-checks liveness, shutdown and the in-flight entry
  // after the sleep, and issues the query outside mutex_ (see
  // ros2/rmw_zenoh#484).
  std::weak_ptr<ClientData> client_data = shared_from_this();
  std::thread(
    [client_data, sequence_id, next_attempt, backoff,
    request_payload = std::move(request_payload)]() {
      std::this_thread::sleep_for(backoff);
      auto sub_data = client_data.lock();
      if (sub_data == nullptr || sub_data->is_shutdown()) {
        return;
      }
      {
        std::lock_guard<std::mutex> lock(sub_data->mutex_);
        auto it = sub_data->in_flight_requests_.find(sequence_id);
        if (it == sub_data->in_flight_requests_.end() ||
        it->second.replied || it->second.attempts != next_attempt)
        {
          // The request was answered, superseded, or dropped (e.g. the
          // client shut down and cleared the bookkeeping) while backing
          // off; nothing to re-issue.
          return;
        }
      }
      sub_data->issue_query(sequence_id, next_attempt, request_payload);
    }).detach();
}

///=============================================================================
void ClientData::mark_query_replied(int64_t sequence_id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = in_flight_requests_.find(sequence_id);
  if (it != in_flight_requests_.end()) {
    it->second.replied = true;
  }
}

///=============================================================================
ClientData::~ClientData()
{
  const rmw_ret_t ret = this->shutdown();
  if (ret != RMW_RET_OK) {
    RMW_ZENOH_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "Error destructing client /%s.",
      entity_->topic_info().value().name_.c_str()
    );
  }
}

//==============================================================================
void ClientData::set_on_new_response_callback(
  rmw_event_callback_t callback,
  const void * user_data)
{
  std::lock_guard<std::mutex> lock(mutex_);
  data_callback_mgr_.set_callback(user_data, std::move(callback));
}

///=============================================================================
bool ClientData::queue_has_data_and_attach_condition_if_not(
  rmw_wait_set_data_t * wait_set_data)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!reply_queue_.empty()) {
    return true;
  }
  wait_set_data_ = wait_set_data;

  return false;
}

///=============================================================================
bool ClientData::detach_condition_and_queue_is_empty()
{
  std::lock_guard<std::mutex> lock(mutex_);
  wait_set_data_ = nullptr;

  return reply_queue_.empty();
}

///=============================================================================
rmw_ret_t ClientData::shutdown()
{
  bool expected = false;
  if (!is_shutdown_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
      std::memory_order_relaxed))
  {
    return RMW_RET_OK;
  }

  // Drop the in-flight request bookkeeping: no query will be retried once
  // the client is shutdown, and the entries must not leak.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    in_flight_requests_.clear();
  }

  // Unregister this node from the ROS graph.
  if (initialized_) {
    zenoh::ZResult result;
    std::move(token_).value().undeclare(&result);
    if (result != Z_OK) {
      RMW_ZENOH_LOG_ERROR_NAMED(
        "rmw_zenoh_cpp",
        "Unable to undeclare the liveliness token");
      return RMW_RET_ERROR;
    }
  }
  zenoh::ZResult result;
  std::move(querier_).undeclare(&result);
  if (result != Z_OK) {
    RMW_ZENOH_LOG_ERROR_NAMED(
      "rmw_zenoh_cpp",
      "Unable to undeclare the querier");
    return RMW_RET_ERROR;
  }

  return RMW_RET_OK;
}

///=============================================================================
bool ClientData::is_shutdown() const
{
  return is_shutdown_.load(std::memory_order_acquire);
}
}  // namespace rmw_zenoh_cpp
