// Flow Offload Scheduler - minimal cross-platform TCP socket layer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "socket.hpp"

#include <cstring>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace flow_offload {
namespace detail {
namespace {

#if defined(_WIN32)
using RawSocket = SOCKET;
constexpr RawSocket kInvalidRaw = INVALID_SOCKET;

std::once_flag g_wsa_once;
bool g_wsa_ready = false;

void initialise_winsock() noexcept {
  WSADATA data{};
  g_wsa_ready = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

[[nodiscard]] RawSocket to_raw(std::uintptr_t value) noexcept {
  return static_cast<RawSocket>(value);
}
#else
using RawSocket = int;
constexpr RawSocket kInvalidRaw = -1;

[[nodiscard]] RawSocket to_raw(std::uintptr_t value) noexcept {
  return static_cast<RawSocket>(value);
}
#endif

[[nodiscard]] bool is_transient(int error) noexcept {
#if defined(_WIN32)
  return error == WSAEINTR;
#else
  return error == EINTR;
#endif
}

[[nodiscard]] int last_error() noexcept {
#if defined(_WIN32)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

}  // namespace

bool ensure_socket_runtime() noexcept {
#if defined(_WIN32)
  std::call_once(g_wsa_once, initialise_winsock);
  return g_wsa_ready;
#else
  return true;
#endif
}

Socket::Socket(std::uintptr_t raw) noexcept : raw_(raw) {}

Socket::Socket(Socket&& other) noexcept : raw_(other.raw_) { other.raw_ = kInvalid; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    raw_ = other.raw_;
    other.raw_ = kInvalid;
  }
  return *this;
}

Socket::~Socket() { close(); }

bool Socket::valid() const noexcept { return raw_ != kInvalid; }

void Socket::close() noexcept {
  if (raw_ == kInvalid) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(to_raw(raw_));
#else
  ::close(to_raw(raw_));
#endif
  raw_ = kInvalid;
}

bool listen_on(const std::string& address, std::uint16_t port, std::uint16_t& bound_port,
               Socket& out, ReasonCode& failure) noexcept {
  if (!ensure_socket_runtime()) {
    failure = ReasonCode::ProtocolIoError;
    return false;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(address.empty() ? nullptr : address.c_str(), service.c_str(), &hints, &results) != 0 ||
      results == nullptr) {
    failure = ReasonCode::ProtocolIoError;
    return false;
  }
  RawSocket raw = kInvalidRaw;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    raw = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (raw == kInvalidRaw) {
      continue;
    }
    int reuse = 1;
    ::setsockopt(raw, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 static_cast<int>(sizeof(reuse)));
    if (::bind(raw, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
        ::listen(raw, static_cast<int>(kMaxConnections)) == 0) {
      break;
    }
#if defined(_WIN32)
    ::closesocket(raw);
#else
    ::close(raw);
#endif
    raw = kInvalidRaw;
  }
  ::freeaddrinfo(results);
  if (raw == kInvalidRaw) {
    failure = ReasonCode::ProtocolIoError;
    return false;
  }
  sockaddr_in bound{};
#if defined(_WIN32)
  int bound_length = static_cast<int>(sizeof(bound));
#else
  socklen_t bound_length = static_cast<socklen_t>(sizeof(bound));
#endif
  if (::getsockname(raw, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
#if defined(_WIN32)
    ::closesocket(raw);
#else
    ::close(raw);
#endif
    failure = ReasonCode::ProtocolIoError;
    return false;
  }
  bound_port = ntohs(bound.sin_port);
  out = Socket(static_cast<std::uintptr_t>(raw));
  return true;
}

bool accept_connection(const Socket& listener, Socket& out, ReasonCode& failure) noexcept {
  if (!listener.valid()) {
    failure = ReasonCode::ProtocolShuttingDown;
    return false;
  }
  for (;;) {
    const RawSocket raw = ::accept(to_raw(listener.raw()), nullptr, nullptr);
    if (raw != kInvalidRaw) {
      int nodelay = 1;
      ::setsockopt(raw, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
                   static_cast<int>(sizeof(nodelay)));
      out = Socket(static_cast<std::uintptr_t>(raw));
      return true;
    }
    if (is_transient(last_error())) {
      continue;
    }
    failure = listener.valid() ? ReasonCode::ProtocolIoError : ReasonCode::ProtocolShuttingDown;
    return false;
  }
}

bool connect_to(const std::string& address, std::uint16_t port, Socket& out,
                ReasonCode& failure) noexcept {
  if (!ensure_socket_runtime()) {
    failure = ReasonCode::ProtocolIoError;
    return false;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(address.c_str(), service.c_str(), &hints, &results) != 0 || results == nullptr) {
    failure = ReasonCode::ProtocolIoError;
    return false;
  }
  RawSocket raw = kInvalidRaw;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    raw = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (raw == kInvalidRaw) {
      continue;
    }
    if (::connect(raw, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      break;
    }
#if defined(_WIN32)
    ::closesocket(raw);
#else
    ::close(raw);
#endif
    raw = kInvalidRaw;
  }
  ::freeaddrinfo(results);
  if (raw == kInvalidRaw) {
    failure = ReasonCode::ProtocolIoError;
    return false;
  }
  int nodelay = 1;
  ::setsockopt(raw, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
               static_cast<int>(sizeof(nodelay)));
  out = Socket(static_cast<std::uintptr_t>(raw));
  return true;
}

bool send_all(const Socket& socket, std::span<const std::byte> data, ReasonCode& failure) noexcept {
  if (!socket.valid()) {
    failure = ReasonCode::ProtocolNotConnected;
    return false;
  }
  std::size_t sent = 0;
  while (sent < data.size()) {
    const std::size_t remaining = data.size() - sent;
    const int chunk = remaining > 1U << 20U ? static_cast<int>(1U << 20U) : static_cast<int>(remaining);
    const int written = ::send(to_raw(socket.raw()),
                               reinterpret_cast<const char*>(data.data() + sent), chunk, 0);
    if (written <= 0) {
      if (written < 0 && is_transient(last_error())) {
        continue;
      }
      failure = ReasonCode::ProtocolIoError;
      return false;
    }
    sent += static_cast<std::size_t>(written);
  }
  return true;
}

bool recv_exact(const Socket& socket, std::span<std::byte> data, ReasonCode& failure) noexcept {
  if (!socket.valid()) {
    failure = ReasonCode::ProtocolNotConnected;
    return false;
  }
  std::size_t received = 0;
  while (received < data.size()) {
    const std::size_t remaining = data.size() - received;
    const int chunk = remaining > 1U << 20U ? static_cast<int>(1U << 20U) : static_cast<int>(remaining);
    const int read = ::recv(to_raw(socket.raw()),
                            reinterpret_cast<char*>(data.data() + received), chunk, 0);
    if (read == 0) {
      // Orderly close before the requested bytes arrived: a truncated frame is
      // never accepted as a complete one.
      failure = ReasonCode::ProtocolTruncatedFrame;
      return false;
    }
    if (read < 0) {
      if (is_transient(last_error())) {
        continue;
      }
      failure = ReasonCode::ProtocolIoError;
      return false;
    }
    received += static_cast<std::size_t>(read);
  }
  return true;
}

}  // namespace detail
}  // namespace flow_offload
