#pragma once

#include <quicr/quicr_common.h>

#include "logger.h"


namespace laps {
class Config {
public:
  Logger *logger; /// Local source logger reference

  // Client Manager
  std::string
      client_bind_addr; /// Local client bind address, defaults to 127.0.0.1
  uint16_t client_port; /// Local client listening port, defaults to 33434
  quicr::RelayInfo::Protocol protocol;

  bool disable_splithz;   /// Disable split horizon

  const char* tls_cert_filename;
  const char* tls_key_filename;
  uint16_t data_queue_size;

  // Cache
  unsigned int cache_max_buffers;  /// Max number of cache buffers
  unsigned int cache_map_capacity; /// Max capacity for cache map

  // constructor
  Config();

private:
  /**
   * @brief Initialize thread safe logger
   *
   * @return True on error, false if no error
   */
  bool init_logger();

  void init_defaults();
  void cfg_from_env();
};

} // namespace laps
