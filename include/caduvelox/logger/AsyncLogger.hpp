#pragma once

#include "caduvelox/logger/Logger.hpp"

#include <memory>
#include <string_view>

namespace caduvelox {
class AsyncLogger : public Logger {
  public:
    AsyncLogger(std::unique_ptr<Logger> delegate);
    ~AsyncLogger();
    
    void logMessage(std::string_view msg) override;
    void logError(std::string_view msg) override;
    
  private: 
    class Impl;
    std::unique_ptr<Impl> fImpl;
};
} // namespace caduvelox
