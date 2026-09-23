// Flow Offload Scheduler - service daemon.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Prints "READY <port>" on standard output once the listener is bound, which is
// how a supervising process learns the chosen ephemeral port without polling.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "flow_offload/engine.hpp"
#include "flow_offload/service.hpp"

namespace {

struct Options {
  std::string store;
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 0;
  std::uint32_t workers = 2;
  bool persist = false;
};

bool parse(int argc, char** argv, Options& out) {
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    if (token == "--store" && i + 1 < argc) {
      out.store = argv[++i];
      out.persist = true;
    } else if (token == "--bind" && i + 1 < argc) {
      out.bind_address = argv[++i];
    } else if (token == "--port" && i + 1 < argc) {
      out.port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (token == "--workers" && i + 1 < argc) {
      out.workers = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (token == "--no-persist") {
      out.persist = false;
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse(argc, argv, options)) {
    std::cerr << "usage: fos_schedd [--store <dir>] [--bind <address>] [--port <n>] [--workers <n>]\n";
    return 2;
  }
  flow_offload::EngineConfig config{};
  config.persistence_enabled = options.persist;
  config.store.directory = options.store;
  config.workers = 0;
  flow_offload::Engine engine(config);
  const flow_offload::Status opened = engine.open();
  if (opened.failed()) {
    std::cerr << "fos_schedd: engine could not open: " << flow_offload::to_string(opened.code()) << "\n";
    return 1;
  }
  flow_offload::ServiceConfig service_config{};
  service_config.bind_address = options.bind_address;
  service_config.port = options.port;
  service_config.workers = options.workers;
  flow_offload::Service service(engine, service_config);
  const flow_offload::Status started = service.start();
  if (started.failed()) {
    std::cerr << "fos_schedd: listener could not start: " << flow_offload::to_string(started.code())
              << "\n";
    return 1;
  }
  std::cout << "READY " << service.port() << "\n";
  std::cout.flush();
  service.run();
  service.shutdown();
  engine.shutdown();
  std::cout << "STOPPED\n";
  std::cout.flush();
  return 0;
}
