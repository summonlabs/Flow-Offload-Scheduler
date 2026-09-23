// Flow Offload Scheduler - identity rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "flow_offload/identity.hpp"

#include <cstdio>

namespace flow_offload {

std::string to_string(const BootId& boot) {
  char buffer[40] = {};
  const int written = std::snprintf(buffer, sizeof(buffer), "%016llx%016llx",
                                    static_cast<unsigned long long>(boot.high),
                                    static_cast<unsigned long long>(boot.low));
  if (written <= 0) {
    return std::string{"invalid"};
  }
  return std::string{buffer, static_cast<std::size_t>(written)};
}

}  // namespace flow_offload
