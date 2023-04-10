
#include "config.h"
#include <cstdlib>

namespace laps {

Config::Config() : logger(NULL) {

  init_logger();

  init_defaults();
  cfg_from_env();
}

void Config::init_defaults() {
  client_bind_addr = "127.0.0.1";
  client_port = 33434;
  protocol = quicr::RelayInfo::Protocol::UDP;

  disable_splithz = false;

  tls_key_filename = NULL;
  tls_cert_filename = NULL;

  cache_map_capacity = 20000;
  cache_max_buffers = 10;
}

void Config::cfg_from_env() {
  char *envVar;

  envVar = getenv("LAPS_TLS_CERT_FILENAME");
  if (envVar) {
    tls_cert_filename = envVar;
  }

  envVar = getenv("LAPS_TLS_KEY_FILENAME");
  if (envVar) {
    tls_key_filename = envVar;
  }

  envVar = getenv("LAPS_CLIENT_BIND_ADDR");
  if (envVar) {
    client_bind_addr = envVar;
  }

  envVar = getenv("LAPS_DISABLE_SPLITHZ");
  if (envVar) {
    disable_splithz = atoi(envVar) == 1 ? true : false;
  }

  envVar = getenv("LAPS_CLIENT_PORT");
  if (envVar) {
    client_port = atoi(envVar);
  }

  envVar = getenv("LAPS_CACHE_MAX_BUFFERS");
  if (envVar) {
    cache_max_buffers = atoi(envVar);
  }

  envVar = getenv("LAPS_CACHE_MAP_CAPACITY");
  if (envVar) {
    cache_map_capacity = atol(envVar);
  }
}

bool Config::init_logger() {
  try {
    if (logger != NULL)
      delete logger;

    logger = new Logger(NULL, NULL);

  } catch (char const *str) {
    std::cout << "Failed to open log file for read/write : " << str
              << std::endl;
    return true;
  }

  // Set up defaults for logging
  logger->setWidthFilename(15);
  logger->setWidthFunction(18);

  if (std::getenv("LAPS_DEBUG"))
    logger->enableDebug();

  return false;
}
} // namespace laps