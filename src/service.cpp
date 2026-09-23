// Flow Offload Scheduler - service transport and client.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Locking contract
// ----------------
//  * connection_mutex guards the live-connection registry only. It is never held
//    while a job runs and never held while the pool is joined.
//  * request_stop() closes sockets (which unblocks blocked reads) rather than
//    waiting on them, so no thread ever waits on another thread's progress.
//  * shutdown() joins the pool only after the registry lock has been released.

#include "flow_offload/service.hpp"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

#include "flow_offload/export.hpp"
#include "flow_offload/serialize.hpp"
#include "socket.hpp"

namespace flow_offload {
namespace {

std::uint32_t read_u32(const std::byte* p) noexcept {
  return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) << 24U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 8U) |
         static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3]));
}

std::uint64_t read_u64(const std::byte* p) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8U) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i]));
  }
  return value;
}

/// Operation byte echoed from a frame header the server has not fully read yet.
[[nodiscard]] Operation read_operation(const std::byte* header) noexcept {
  const std::uint8_t raw = std::to_integer<std::uint8_t>(header[6]);
  if (raw < 1U || raw > 11U) {
    return Operation::Hello;
  }
  return static_cast<Operation>(raw);
}

}  // namespace

struct Service::Impl {
  Service* owner{nullptr};
  Engine* engine{nullptr};
  ServiceConfig config{};
  detail::Socket listener{};
  std::uint16_t bound_port{0};
  ThreadPool pool{};
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> started{false};
  std::atomic<std::uint64_t> frames_handled{0};
  std::atomic<std::uint64_t> frames_rejected{0};
  std::atomic<std::uint64_t> connections_accepted{0};
  std::atomic<std::uint64_t> connections_refused{0};

  mutable std::mutex connection_mutex;
  std::map<std::uint64_t, std::shared_ptr<detail::Socket>> connections;
  std::uint64_t next_connection{1};

  void handle_connection(const std::shared_ptr<detail::Socket>& socket, std::uint64_t id);
  void drop_connection(std::uint64_t id);
};

Service::Service(Engine& engine, const ServiceConfig& config) : impl_(std::make_unique<Impl>()) {
  impl_->owner = this;
  impl_->engine = &engine;
  impl_->config = config;
}

Service::~Service() { shutdown(); }

Status Service::start() {
  if (impl_->started.load()) {
    return Status::accepted(ReasonCode::AcceptedNoChangeRequired);
  }
  if (!detail::ensure_socket_runtime()) {
    return Status::refused(ReasonCode::ProtocolIoError);
  }
  std::uint16_t bound_port = 0;
  ReasonCode failure = ReasonCode::Ok;
  if (!detail::listen_on(impl_->config.bind_address, impl_->config.port, bound_port,
                         impl_->listener, failure)) {
    return Status::refused(failure);
  }
  impl_->bound_port = bound_port;
  impl_->stop_requested.store(false);
  impl_->pool.start(impl_->config.workers, impl_->config.max_queue_depth);
  impl_->started.store(true);
  return Status::accepted(ReasonCode::ProtocolOk);
}

std::uint16_t Service::port() const { return impl_->bound_port; }

void Service::request_stop() {
  impl_->stop_requested.store(true);
  impl_->listener.close();
  std::lock_guard<std::mutex> lock(impl_->connection_mutex);
  for (auto& entry : impl_->connections) {
    if (entry.second) {
      entry.second->close();
    }
  }
}

void Service::run() {
  while (!impl_->stop_requested.load()) {
    ReasonCode failure = ReasonCode::Ok;
    detail::Socket accepted;
    if (!detail::accept_connection(impl_->listener, accepted, failure)) {
      if (impl_->stop_requested.load() || !impl_->listener.valid()) {
        break;
      }
      continue;
    }
    if (!accepted.valid()) {
      break;
    }
    std::uint64_t id = 0;
    std::shared_ptr<detail::Socket> shared;
    bool submitted = false;
    {
      std::lock_guard<std::mutex> lock(impl_->connection_mutex);
      if (impl_->connections.size() >= impl_->config.max_connections) {
        impl_->connections_refused.fetch_add(1);
        accepted.close();
        continue;
      }
      id = impl_->next_connection++;
      shared = std::make_shared<detail::Socket>(std::move(accepted));
      impl_->connections.emplace(id, shared);
      impl_->connections_accepted.fetch_add(1);
      submitted = impl_->pool.try_submit([this, shared, id] { impl_->handle_connection(shared, id); });
      if (!submitted) {
        impl_->connections_refused.fetch_add(1);
        shared->close();
        impl_->connections.erase(id);
      }
    }
  }
}

void Service::Impl::drop_connection(std::uint64_t id) {
  std::lock_guard<std::mutex> lock(connection_mutex);
  const auto it = connections.find(id);
  if (it != connections.end()) {
    if (it->second) {
      it->second->close();
    }
    connections.erase(it);
  }
}

void Service::Impl::handle_connection(const std::shared_ptr<detail::Socket>& socket,
                                      std::uint64_t id) {
  for (;;) {
    if (stop_requested.load()) {
      break;
    }
    std::vector<std::byte> header(kFrameHeaderBytes, std::byte{0});
    ReasonCode failure = ReasonCode::Ok;
    if (!detail::recv_exact(*socket, std::span<std::byte>(header.data(), header.size()), failure)) {
      break;
    }
    const std::uint32_t payload_length = read_u32(header.data());
    if (payload_length > kMaxFramePayloadBytes ||
        static_cast<std::size_t>(payload_length) + kFrameOverheadBytes > config.max_frame_bytes) {
      frames_rejected.fetch_add(1);
      Frame refusal{};
      refusal.operation = read_operation(header.data());
      refusal.correlation = read_u64(header.data() + 8);
      refusal.status = ReasonCode::ProtocolOversizedFrame;
      std::vector<std::byte> encoded;
      if (encode_frame(refusal, encoded, config.max_frame_bytes)) {
        (void)detail::send_all(*socket, std::span<const std::byte>(encoded.data(), encoded.size()),
                               failure);
      }
      break;
    }
    std::vector<std::byte> frame_bytes;
    try {
      frame_bytes.assign(kFrameHeaderBytes + payload_length + kFrameDigestBytes, std::byte{0});
    } catch (...) {
      frames_rejected.fetch_add(1);
      break;
    }
    std::memcpy(frame_bytes.data(), header.data(), header.size());
    {
      std::span<std::byte> tail(frame_bytes.data() + kFrameHeaderBytes,
                                static_cast<std::size_t>(payload_length) + kFrameDigestBytes);
      if (!detail::recv_exact(*socket, tail, failure)) {
        break;
      }
    }
    Frame request{};
    if (!decode_frame(std::span<const std::byte>(frame_bytes.data(), frame_bytes.size()), request,
                      config.max_frame_bytes)) {
      frames_rejected.fetch_add(1);
      Frame refusal{};
      refusal.correlation = 0;
      refusal.status = request.status == ReasonCode::ProtocolOk ? ReasonCode::ProtocolMalformedFrame
                                                                : request.status;
      std::vector<std::byte> encoded;
      if (encode_frame(refusal, encoded, config.max_frame_bytes)) {
        (void)detail::send_all(*socket, std::span<const std::byte>(encoded.data(), encoded.size()),
                               failure);
      }
      continue;
    }
    Frame response{};
    (void)owner->dispatch(request, response);
    frames_handled.fetch_add(1);
    std::vector<std::byte> encoded;
    if (!encode_frame(response, encoded, config.max_frame_bytes)) {
      Frame refusal{};
      refusal.correlation = request.correlation;
      refusal.operation = request.operation;
      refusal.status = ReasonCode::ProtocolBackpressure;
      if (!encode_frame(refusal, encoded, config.max_frame_bytes)) {
        break;
      }
    }
    if (!detail::send_all(*socket, std::span<const std::byte>(encoded.data(), encoded.size()),
                          failure)) {
      break;
    }
    if (request.operation == Operation::Shutdown) {
      owner->request_stop();
      break;
    }
  }
  drop_connection(id);
}

Status Service::dispatch(const Frame& request, Frame& response) {
  response = Frame{};
  response.version = kProtocolVersion;
  response.operation = request.operation;
  response.correlation = request.correlation;
  response.status = ReasonCode::ProtocolOk;
  std::vector<std::byte> payload;

  switch (request.operation) {
    case Operation::Hello: {
      std::string client_name;
      std::uint32_t client_version = 0;
      if (!decode_hello_request(request.payload, client_name, client_version)) {
        response.status = ReasonCode::ProtocolMalformedFrame;
        return Status::refused(response.status);
      }
      const EngineSnapshot snapshot = impl_->engine->snapshot();
      HelloResponse hello{};
      hello.library_version = std::string{version_string()};
      hello.epoch = snapshot.epoch;
      hello.boot = snapshot.boot;
      if (!encode_hello_response(hello, payload)) {
        response.status = ReasonCode::ProtocolBackpressure;
        return Status::refused(response.status);
      }
      break;
    }
    case Operation::DeclareTarget: {
      TargetDescriptor descriptor{};
      if (!decode_target_declaration(request.payload, descriptor)) {
        response.status = ReasonCode::ProtocolMalformedFrame;
        return Status::refused(response.status);
      }
      response.status = impl_->engine->declare_target(descriptor).code();
      break;
    }
    case Operation::RegisterFlow: {
      FlowDescriptor descriptor{};
      if (!decode_flow_registration(request.payload, descriptor)) {
        response.status = ReasonCode::ProtocolMalformedFrame;
        return Status::refused(response.status);
      }
      response.status = impl_->engine->register_flow(descriptor).code();
      break;
    }
    case Operation::IngestLoad: {
      LoadEvidence evidence{};
      Timestamp instant{};
      if (!decode_load_evidence(request.payload, evidence, instant)) {
        response.status = ReasonCode::ProtocolMalformedFrame;
        return Status::refused(response.status);
      }
      response.status = impl_->engine->ingest_load(evidence, instant).code();
      break;
    }
    case Operation::SetPolicy: {
      Policy policy{};
      if (!decode_policy(request.payload, policy)) {
        response.status = ReasonCode::ProtocolMalformedFrame;
        return Status::refused(response.status);
      }
      response.status = impl_->engine->set_policy(policy).code();
      break;
    }
    case Operation::RegisterContract: {
      MigrationContract contract{};
      if (!decode_contract(request.payload, contract)) {
        response.status = ReasonCode::ProtocolMalformedFrame;
        return Status::refused(response.status);
      }
      response.status = impl_->engine->register_migration_contract(contract).code();
      break;
    }
    case Operation::Place: {
      PlacementRequest placement{};
      if (!decode_placement_request(request.payload, placement)) {
        response.status = ReasonCode::ProtocolMalformedFrame;
        return Status::refused(response.status);
      }
      const PlacementResult result = impl_->engine->place(placement);
      response.status = result.status;
      if (!encode_placement_result(result, payload)) {
        response.status = ReasonCode::ProtocolBackpressure;
        return Status::refused(response.status);
      }
      break;
    }
    case Operation::Rebalance: {
      RebalanceRequest rebalance{};
      if (!decode_rebalance_request(request.payload, rebalance)) {
        response.status = ReasonCode::ProtocolMalformedFrame;
        return Status::refused(response.status);
      }
      const RebalanceReport report = impl_->engine->rebalance(rebalance);
      response.status = report.status;
      if (!encode_rebalance_report(report, payload)) {
        response.status = ReasonCode::ProtocolBackpressure;
        return Status::refused(response.status);
      }
      break;
    }
    case Operation::Explain: {
      FlowId flow{};
      if (!decode_explain_request(request.payload, flow)) {
        response.status = ReasonCode::ProtocolMalformedFrame;
        return Status::refused(response.status);
      }
      Explanation explanation{};
      const Status status = impl_->engine->explain_flow(flow, explanation);
      response.status = status.code();
      if (!encode_explanation(explanation, payload)) {
        response.status = ReasonCode::ProtocolBackpressure;
        return Status::refused(response.status);
      }
      break;
    }
    case Operation::Export: {
      ExportFormat format = ExportFormat::Json;
      if (!decode_export_request(request.payload, format)) {
        response.status = ReasonCode::ProtocolMalformedFrame;
        return Status::refused(response.status);
      }
      const EngineExport state = impl_->engine->export_state();
      std::string rendered;
      switch (format) {
        case ExportFormat::Binary:
          rendered = export_binary(state);
          break;
        case ExportFormat::Text:
          rendered = export_text(state);
          break;
        case ExportFormat::Json:
        default:
          rendered = export_json(state);
          break;
      }
      if (rendered.empty()) {
        response.status = ReasonCode::ProtocolBackpressure;
        return Status::refused(response.status);
      }
      if (rendered.size() > kMaxFramePayloadBytes) {
        response.status = ReasonCode::ProtocolOversizedFrame;
        return Status::refused(response.status);
      }
      try {
        payload.assign(reinterpret_cast<const std::byte*>(rendered.data()),
                       reinterpret_cast<const std::byte*>(rendered.data()) + rendered.size());
      } catch (...) {
        response.status = ReasonCode::ProtocolBackpressure;
        return Status::refused(response.status);
      }
      break;
    }
    case Operation::Shutdown: {
      response.status = ReasonCode::AcceptedShutdownRequested;
      break;
    }
    default:
      response.status = ReasonCode::ProtocolUnknownOperation;
      return Status::refused(response.status);
  }
  response.payload = std::move(payload);
  return Status{response.status};
}

void Service::shutdown() {
  request_stop();
  // The registry lock must not be held while joining, because connection
  // handlers take it on their way out.
  impl_->pool.shutdown();
  impl_->listener.close();
  impl_->started.store(false);
}

bool Service::started() const { return impl_->started.load(); }

bool Service::stopped() const { return impl_->stop_requested.load(); }

ServiceStats Service::stats() const {
  ServiceStats stats{};
  stats.frames_handled = impl_->frames_handled.load();
  stats.frames_rejected = impl_->frames_rejected.load();
  stats.connections_accepted = impl_->connections_accepted.load();
  stats.connections_refused = impl_->connections_refused.load();
  return stats;
}

// ===========================================================================
// Client
// ===========================================================================

struct Client::Impl {
  detail::Socket socket{};
  std::uint64_t next_correlation{1};
  std::size_t max_frame_bytes{kMaxFrameBytes};
  mutable std::mutex io_mutex;
};

Client::Client() : impl_(std::make_unique<Impl>()) {}

Client::~Client() { close(); }

Status Client::connect(const std::string& address, std::uint16_t port, std::size_t max_frame_bytes) {
  close();
  ReasonCode failure = ReasonCode::Ok;
  detail::Socket socket;
  if (!detail::connect_to(address, port, socket, failure)) {
    return Status::refused(failure);
  }
  impl_->socket = std::move(socket);
  impl_->max_frame_bytes = max_frame_bytes < kFrameOverheadBytes ? kMaxFrameBytes : max_frame_bytes;
  return Status::accepted(ReasonCode::ProtocolOk);
}

void Client::close() {
  std::lock_guard<std::mutex> lock(impl_->io_mutex);
  impl_->socket.close();
}

bool Client::connected() const {
  std::lock_guard<std::mutex> lock(impl_->io_mutex);
  return impl_->socket.valid();
}

Status Client::call(Operation operation, std::span<const std::byte> payload,
                    std::vector<std::byte>& response, ReasonCode& status) {
  std::lock_guard<std::mutex> lock(impl_->io_mutex);
  status = ReasonCode::ProtocolIoError;
  if (!impl_->socket.valid()) {
    status = ReasonCode::ProtocolNotConnected;
    return Status::refused(status);
  }
  Frame request{};
  request.operation = operation;
  request.correlation = impl_->next_correlation++;
  try {
    request.payload.assign(payload.begin(), payload.end());
  } catch (...) {
    status = ReasonCode::ProtocolBackpressure;
    return Status::refused(status);
  }
  std::vector<std::byte> encoded;
  if (!encode_frame(request, encoded, impl_->max_frame_bytes)) {
    status = ReasonCode::ProtocolOversizedFrame;
    return Status::refused(status);
  }
  ReasonCode failure = ReasonCode::Ok;
  if (!detail::send_all(impl_->socket, std::span<const std::byte>(encoded.data(), encoded.size()),
                        failure)) {
    status = failure;
    return Status::refused(status);
  }
  std::vector<std::byte> header(kFrameHeaderBytes, std::byte{0});
  if (!detail::recv_exact(impl_->socket, std::span<std::byte>(header.data(), header.size()),
                          failure)) {
    status = failure;
    return Status::refused(status);
  }
  const std::uint32_t payload_length = read_u32(header.data());
  if (payload_length > kMaxFramePayloadBytes ||
      static_cast<std::size_t>(payload_length) + kFrameOverheadBytes > impl_->max_frame_bytes) {
    status = ReasonCode::ProtocolOversizedFrame;
    return Status::refused(status);
  }
  std::vector<std::byte> frame_bytes;
  try {
    frame_bytes.assign(kFrameHeaderBytes + payload_length + kFrameDigestBytes, std::byte{0});
  } catch (...) {
    status = ReasonCode::ProtocolBackpressure;
    return Status::refused(status);
  }
  std::memcpy(frame_bytes.data(), header.data(), header.size());
  {
    std::span<std::byte> tail(frame_bytes.data() + kFrameHeaderBytes,
                              static_cast<std::size_t>(payload_length) + kFrameDigestBytes);
    if (!detail::recv_exact(impl_->socket, tail, failure)) {
      status = failure;
      return Status::refused(status);
    }
  }
  Frame reply{};
  if (!decode_frame(std::span<const std::byte>(frame_bytes.data(), frame_bytes.size()), reply,
                    impl_->max_frame_bytes)) {
    status = reply.status == ReasonCode::ProtocolOk ? ReasonCode::ProtocolMalformedFrame
                                                    : reply.status;
    return Status::refused(status);
  }
  if (reply.correlation != request.correlation) {
    status = ReasonCode::ProtocolMalformedFrame;
    return Status::refused(status);
  }
  response = std::move(reply.payload);
  status = reply.status;
  return Status{reply.status};
}

Status Client::call_raw(std::span<const std::byte> request_frame,
                        std::vector<std::byte>& response_frame) {
  std::lock_guard<std::mutex> lock(impl_->io_mutex);
  response_frame.clear();
  if (!impl_->socket.valid()) {
    return Status::refused(ReasonCode::ProtocolNotConnected);
  }
  ReasonCode failure = ReasonCode::Ok;
  if (!detail::send_all(impl_->socket, request_frame, failure)) {
    return Status::refused(failure);
  }
  std::vector<std::byte> header(kFrameHeaderBytes, std::byte{0});
  if (!detail::recv_exact(impl_->socket, std::span<std::byte>(header.data(), header.size()),
                          failure)) {
    return Status::refused(failure);
  }
  const std::uint32_t payload_length = read_u32(header.data());
  if (payload_length > kMaxFramePayloadBytes ||
      static_cast<std::size_t>(payload_length) + kFrameOverheadBytes > impl_->max_frame_bytes) {
    return Status::refused(ReasonCode::ProtocolOversizedFrame);
  }
  try {
    response_frame.assign(kFrameHeaderBytes + payload_length + kFrameDigestBytes, std::byte{0});
  } catch (...) {
    return Status::refused(ReasonCode::ProtocolBackpressure);
  }
  std::memcpy(response_frame.data(), header.data(), header.size());
  std::span<std::byte> tail(response_frame.data() + kFrameHeaderBytes,
                            static_cast<std::size_t>(payload_length) + kFrameDigestBytes);
  if (!detail::recv_exact(impl_->socket, tail, failure)) {
    return Status::refused(failure);
  }
  return Status::accepted(ReasonCode::ProtocolOk);
}

Status Client::send_raw(std::span<const std::byte> bytes) {
  std::lock_guard<std::mutex> lock(impl_->io_mutex);
  if (!impl_->socket.valid()) {
    return Status::refused(ReasonCode::ProtocolNotConnected);
  }
  ReasonCode failure = ReasonCode::Ok;
  if (!detail::send_all(impl_->socket, bytes, failure)) {
    return Status::refused(failure);
  }
  return Status::accepted(ReasonCode::ProtocolOk);
}

Status Client::hello(std::string_view client_name, HelloResponse& out) {
  std::vector<std::byte> payload;
  if (!encode_hello_request(client_name, kVersionMajor * 10000U + kVersionMinor * 100U + kVersionPatch,
                            payload)) {
    return Status::refused(ReasonCode::RejectedOversizedInput);
  }
  std::vector<std::byte> response;
  ReasonCode status = ReasonCode::Ok;
  const Status call_status = call(Operation::Hello, payload, response, status);
  if (call_status.failed()) {
    return call_status;
  }
  if (!decode_hello_response(response, out)) {
    return Status::refused(ReasonCode::ProtocolMalformedFrame);
  }
  return call_status;
}

Status Client::declare_target(const TargetDescriptor& descriptor) {
  std::vector<std::byte> payload;
  if (!encode_target_declaration(descriptor, payload)) {
    return Status::refused(ReasonCode::RejectedOversizedInput);
  }
  std::vector<std::byte> response;
  ReasonCode status = ReasonCode::Ok;
  return call(Operation::DeclareTarget, payload, response, status);
}

Status Client::register_flow(const FlowDescriptor& descriptor) {
  std::vector<std::byte> payload;
  if (!encode_flow_registration(descriptor, payload)) {
    return Status::refused(ReasonCode::RejectedOversizedInput);
  }
  std::vector<std::byte> response;
  ReasonCode status = ReasonCode::Ok;
  return call(Operation::RegisterFlow, payload, response, status);
}

Status Client::ingest_load(const LoadEvidence& evidence, Timestamp instant) {
  std::vector<std::byte> payload;
  if (!encode_load_evidence(evidence, instant, payload)) {
    return Status::refused(ReasonCode::RejectedOversizedInput);
  }
  std::vector<std::byte> response;
  ReasonCode status = ReasonCode::Ok;
  return call(Operation::IngestLoad, payload, response, status);
}

Status Client::set_policy(const Policy& policy) {
  std::vector<std::byte> payload;
  if (!encode_policy(policy, payload)) {
    return Status::refused(ReasonCode::RejectedOversizedInput);
  }
  std::vector<std::byte> response;
  ReasonCode status = ReasonCode::Ok;
  return call(Operation::SetPolicy, payload, response, status);
}

Status Client::register_contract(const MigrationContract& contract) {
  std::vector<std::byte> payload;
  if (!encode_contract(contract, payload)) {
    return Status::refused(ReasonCode::RejectedOversizedInput);
  }
  std::vector<std::byte> response;
  ReasonCode status = ReasonCode::Ok;
  return call(Operation::RegisterContract, payload, response, status);
}

Status Client::place(const PlacementRequest& request, PlacementResult& out) {
  std::vector<std::byte> payload;
  if (!encode_placement_request(request, payload)) {
    return Status::refused(ReasonCode::RejectedOversizedInput);
  }
  std::vector<std::byte> response;
  ReasonCode status = ReasonCode::Ok;
  const Status call_status = call(Operation::Place, payload, response, status);
  if (call_status.failed()) {
    return call_status;
  }
  if (!decode_placement_result(response, out)) {
    return Status::refused(ReasonCode::ProtocolMalformedFrame);
  }
  return call_status;
}

Status Client::rebalance(const RebalanceRequest& request, RebalanceReport& out) {
  std::vector<std::byte> payload;
  if (!encode_rebalance_request(request, payload)) {
    return Status::refused(ReasonCode::RejectedOversizedInput);
  }
  std::vector<std::byte> response;
  ReasonCode status = ReasonCode::Ok;
  const Status call_status = call(Operation::Rebalance, payload, response, status);
  if (call_status.failed()) {
    return call_status;
  }
  if (!decode_rebalance_report(response, out)) {
    return Status::refused(ReasonCode::ProtocolMalformedFrame);
  }
  return call_status;
}

Status Client::explain(FlowId flow, Explanation& out) {
  std::vector<std::byte> payload;
  if (!encode_explain_request(flow, payload)) {
    return Status::refused(ReasonCode::RejectedOversizedInput);
  }
  std::vector<std::byte> response;
  ReasonCode status = ReasonCode::Ok;
  const Status call_status = call(Operation::Explain, payload, response, status);
  if (!decode_explanation(response, out)) {
    return Status::refused(ReasonCode::ProtocolMalformedFrame);
  }
  return call_status;
}

Status Client::export_state(ExportFormat format, std::string& out) {
  std::vector<std::byte> payload;
  if (!encode_export_request(format, payload)) {
    return Status::refused(ReasonCode::RejectedOversizedInput);
  }
  std::vector<std::byte> response;
  ReasonCode status = ReasonCode::Ok;
  const Status call_status = call(Operation::Export, payload, response, status);
  if (call_status.failed()) {
    return call_status;
  }
  out.assign(reinterpret_cast<const char*>(response.data()), response.size());
  return call_status;
}

Status Client::shutdown_server() {
  std::vector<std::byte> response;
  ReasonCode status = ReasonCode::Ok;
  return call(Operation::Shutdown, std::span<const std::byte>(), response, status);
}

}  // namespace flow_offload
