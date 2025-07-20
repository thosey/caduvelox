#pragma once

// Debug configuration
// Set to 1 to enable verbose debug output, 0 for production builds
#ifndef CADUVELOX_DEBUG
#define CADUVELOX_DEBUG 0
#endif

// Debug logging macro - compiles to nothing when CADUVELOX_DEBUG is 0
#if CADUVELOX_DEBUG
#include "caduvelox/logger/Logger.hpp"
#include <sstream>
#include <iomanip>
#define CADUVELOX_DEBUG_LOG(msg) \
    do { \
        std::ostringstream oss; \
        oss << msg; \
        caduvelox::Logger::getInstance().logMessage(oss.str()); \
    } while(0)
#define CADUVELOX_DEBUG_LOG_HEX(msg, val) \
    do { \
        std::ostringstream oss; \
        oss << msg << "0x" << std::hex << (val) << std::dec; \
        caduvelox::Logger::getInstance().logMessage(oss.str()); \
    } while(0)
#else
#define CADUVELOX_DEBUG_LOG(msg) ((void)0)
#define CADUVELOX_DEBUG_LOG_HEX(msg, val) ((void)0)
#endif
