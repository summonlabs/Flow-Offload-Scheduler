// Flow Offload Scheduler - minimal cross-platform TCP socket layer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal header. The public library never exposes a platform socket type.

#ifndef FLOW_OFFLOAD_SRC_SOCKET_HPP
#define FLOW_OFFLOAD_SRC_SOCKET_HPP

#include <cstdint>
#include <span>
#include <string>

#include "flow_offload/limits.hpp"
#include "flow_offload/reason.hpp"

namespace flow_offload {
namespace detail {

/// One initialisation of the platform socket runtime per process.
[[nodiscard]] bool ensure_socket_runtime() noexcept;

class Socket {
 public:
  Socket() = default;
  explicit Socket(std::uintptr_t raw) noexcept;
  ~Socket();

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uintptr_t raw() const noexcept { return raw_; }
  void close() noexcept;

 private:
  std::uintptr_t raw_{kInvalid};
  static constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
};

/// Binds and listens on the given address. Port zero selects an ephemeral port,
/// which is reported back through \p bound_port.
[[nodiscard]] bool listen_on(const std::string& address, std::uint16_t port,
                             std::uint16_t& bound_port, Socket& out, ReasonCode& failure) noexcept;

[[nodiscard]] bool accept_connection(const Socket& listener, Socket& out,
                                     ReasonCode& failure) noexcept;

[[nodiscard]] bool connect_to(const std::string& address, std::uint16_t port, Socket& out,
                              ReasonCode& failure) noexcept;

/// Sends or receives exactly \p data bytes. A short transfer is an error, so a
/// truncated frame can never be mistaken for a complete one.
[[nodiscard]] bool send_all(const Socket& socket, std::span<const std::byte> data,
                            ReasonCode& failure) noexcept;
[[nodiscard]] bool recv_exact(const Socket& socket, std::span<std::byte> data,
                              ReasonCode& failure) noexcept;

}  // namespace detail
}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_SRC_SOCKET_HPP
