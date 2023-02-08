/*
 * Copyright (c) 2022-2023 Cisco Systems, Inc. and others.  All rights reserved.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <stdarg.h>
#include <string>
#include <sys/time.h>

#include "logger.h"

namespace laps {
Logger::Logger(const char *log_filename, const char *debug_filename) {

  /*
   * Initialize defaults
   */
  debugEnabled = false;
  logFile_REALFILE = false;
  debugFile_REALFILE = false;
  width_filename = 20;
  width_function = 20;

  /*
   * Open log file
   */
  if (log_filename == NULL)
    logFile = stdout;

  else if ((logFile = fopen(log_filename, "a+")) == NULL)
    throw strerror(errno);
  else
    // Indicate this is a real file so that we can close it when needed.
    logFile_REALFILE = true;

  /*
   * Open the debug log file
   */
  if (debug_filename == NULL)
    debugFile = logFile;

  else {
    // Check if debug file is the same as the log_filename
    if (log_filename != NULL and strcmp(log_filename, debug_filename) == 0)
      debugFile = logFile;

    else if ((debugFile = fopen(debug_filename, "w")) == NULL)
      throw strerror(errno);
    else
      // Indicate this is a real file so that we can close it when needed.
      debugFile_REALFILE = true;
  }
}

Logger::~Logger() {

  /*
   * Close open files
   */
  if (logFile_REALFILE && logFile != NULL)
    fclose(logFile);

  if (debugFile_REALFILE && debugFile != NULL)
    fclose(debugFile);
}

void Logger::enableDebug(void) { debugEnabled = true; }

void Logger::disableDebug(void) { debugEnabled = false; }

void Logger::setWidthFunction(u_char width) {
  if (width > 5 && width < 60)
    width_function = width;
}

void Logger::setWidthFilename(u_char width) {
  if (width > 5 && width < 60)
    width_filename = width;
}

void Logger::DebugPrint(const char *filename, int line_num,
                        const char *func_name, const char *msg, ...) {
  // If debug isn't enabled, we do nothing.
  if (!debugEnabled)
    return;

  va_list args;

  // Begin the args
  va_start(args, msg);

  printV("DEBUG", debugFile, filename, line_num, func_name, msg, args);

  // Free/end the args
  va_end(args);
}

/*
 * Implemented quicr transport log handler callback
 */
void Logger::log(qtransport::LogLevel level, const std::string &msg) {
  const char *sev;

  switch (level) {
  case qtransport::LogLevel::debug: {
    if (debugEnabled)
      Print("DEBUG", "logHandler::log()", msg.c_str());
    return;
  }
  case qtransport::LogLevel::fatal:
    sev = "FATAL";
    break;
  case qtransport::LogLevel::error:
    sev = "ERROR";
    break;
  case qtransport::LogLevel::warn:
    sev = "WARN";
    break;
  default:
    sev = "INFO";
    break;
  }

  Print(sev, "logHandler::log()", msg.c_str());
}

void Logger::Print(const char *sev, const char *func_name, const char *msg,
                   ...) {
  va_list args; // varialbe args

  // Begin the args
  va_start(args, msg);

  // Print without the filename and line number included
  printV(sev, logFile, NULL, 0, func_name, msg, args);
  fflush(logFile);

  // Free/end the args
  va_end(args);
}

void Logger::printV(const char *sev, FILE *output, const char *filename,
                    int line_num, const char *func_name, const char *msg,
                    va_list args) {
  char bufmsg[4096]; // Updated message that includes filename/line number
  const char *fname; // Filename pointer

  char fmt[512]; // Updated message that includes filename/line number

  char time_str[128];
  struct timeval tv;
  struct tm t;

  // Get current time
  gettimeofday(&tv, NULL);
  gmtime_r(&tv.tv_sec, &t);
  strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%S", &t);

  // If we have a filename, include it in the print
  if (filename != NULL) {

    // Strip off the path on filename if exists
    (fname = strrchr(filename, '/')) != NULL ? fname++ : fname = filename;

    // Build the format spec
    snprintf(fmt, sizeof(fmt),
             "%%s.%%06u | %%-8s | %%%ds[%%05d] | %%-%ds | %%s\n",
             width_filename, width_function);

    // Update the message to include file/line
    snprintf(bufmsg, sizeof(bufmsg),
             fmt /*"%s.%06u | %-10s | %20s[%05d] | %-26s | %s\n"*/, time_str,
             tv.tv_usec, sev, fname, line_num, func_name, msg);
  } else {
    // Build the format spec
    snprintf(fmt, sizeof(fmt), "%%s.%%06u | %%-8s | %%-%ds | %%s\n",
             width_function);

    // Update the message to include function name only
    snprintf(bufmsg, sizeof(bufmsg), fmt /* "%s.%06u | %-10s | %-30s | %s\n" */,
             time_str, tv.tv_usec, sev, func_name, msg);
  }

  // Print the message
  vfprintf(output, bufmsg, args);
}
} // namespace laps