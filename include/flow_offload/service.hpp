// Flow Offload Scheduler - service transport and client.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FLOW_OFFLOAD_SERVICE_HPP
#define FLOW_OFFLOAD_SERVICE_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "flow_offload/engine.hpp"
#include "flow_offload/protocol.hpp"

namespace flow_offload {

struct ServiceConfig {
  std::string bind_address{"127.0.0.1"};
  /// Zero selects an ephemeral port; the chosen port is reported by port().
  std::uint16_t port{0};
  std::size_t max_connections{kMaxConnections};
  std::uint32_t workers{2};
  std::size_t max_queue_depth{kMaxQueueDepth};
  std::size_t max_frame_bytes{kMaxFrameBytes};
};

struct ServiceStats {
  std::uint64_t frames_handled{0};
  std::uint64_t frames_rejected{0};
  std::uint64_t connections_accepted{0};
  std::uint64_t connections_refused{0};
};

/// Serves the placement authority over a framed, bounded TCP protocol. The
/// service owns no scheduling state: every decision is made by the Engine, so
/// the transport can never diverge from the library.
class Service {
 public:
  explicit Service(Engine& engine, const ServiceConfig& config = ServiceConfig{});
  ~Service();

  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  /// Binds, listens and starts the connection workers.
  Status start();

  /// Accepts connections until request_stop() is called. Runs on the calling
  /// thread; the connection handlers run on the pool.
  void run();

  /// Stops accepting and closes every open connection so that a handler blocked
  /// on a read completes instead of waiting. Idempotent and safe to call from a
  /// connection handler.
  void request_stop();

  /// request_stop() followed by joining every worker. Idempotent.
  void shutdown();

  [[nodiscard]] bool started() const;
  [[nodiscard]] bool stopped() const;
  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] ServiceStats stats() const;

  /// Handles one already-decoded frame. Shared by the socket path and the
  /// in-process tests so both exercise exactly the same logic.
  Status dispatch(const Frame& request, Frame& response);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Blocking client for the service protocol. One call is in flight at a time.
class Client {
 public:
  Client();
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  Status connect(const std::string& address, std::uint16_t port,
                 std::size_t max_frame_bytes = kMaxFrameBytes);
  void close();
  [[nodiscard]] bool connected() const;

  /// Sends one frame and waits for the response with the matching correlation.
  Status call(Operation operation, std::span<const std::byte> payload,
              std::vector<std::byte>& response, ReasonCode& status);

  /// Sends already-encoded bytes and reads one raw response frame. It exists so
  /// that adversarial frame handling can be exercised against the real socket
  /// path rather than a mock.
  Status call_raw(std::span<const std::byte> request_frame, std::vector<std::byte>& response_frame);

  /// Sends bytes without waiting for a reply. A caller that sends a partial
  /// frame must use this and then close, because a server waiting for the
  /// missing bytes and a client waiting for a reply would otherwise wait for
  /// each other.
  Status send_raw(std::span<const std::byte> bytes);

  Status hello(std::string_view client_name, HelloResponse& out);
  Status declare_target(const TargetDescriptor& descriptor);
  Status register_flow(const FlowDescriptor& descriptor);
  Status ingest_load(const LoadEvidence& evidence, Timestamp instant);
  Status set_policy(const Policy& policy);
  Status register_contract(const MigrationContract& contract);
  Status place(const PlacementRequest& request, PlacementResult& out);
  Status rebalance(const RebalanceRequest& request, RebalanceReport& out);
  Status explain(FlowId flow, Explanation& out);
  Status export_state(ExportFormat format, std::string& out);
  Status shutdown_server();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_SERVICE_HPP
