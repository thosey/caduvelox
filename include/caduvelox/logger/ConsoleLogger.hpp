#pragma once

#include "caduvelox/logger/Logger.hpp"

namespace caduvelox {
class ConsoleLogger : public Logger {
  public:
    void logMessage(std::string_view msg) override;
    void logError(std::string_view msg) override;
};
} // namespace caduvelox
