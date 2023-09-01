/*
 * Copyright (c) 2023 Cisco Systems, Inc. and others.  All rights reserved.
 */
#pragma once

#define FLOG_DEBUG(message) \
  logger->debug << __FILE__ << " " << __LINE__ << " " << __FUNCTION__ << " " \
                << message << std::flush

// Logging macros to include associated function
#define FLOG_INFO(message) \
    logger->info << __FUNCTION__ << ": " << message << std::flush

#define FLOG_WARN(message) \
    logger->warning << __FUNCTION__ << ": " << message << std::flush

#define FLOG_ERR(message) \
    logger->error << __FUNCTION__ << ": " << message << std::flush

#define FLOG_CRIT(message) \
    logger->critical << __FUNCTION__ << ": " << message << std::flush
