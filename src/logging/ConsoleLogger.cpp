#include "caduvelox/logger/ConsoleLogger.hpp"

#include <iostream>

namespace caduvelox {
void ConsoleLogger::logMessage(std::string_view msg) {
    std::cout << msg << std::endl;
}

void ConsoleLogger::logError(std::string_view msg) {
    std::cerr << msg << std::endl;
}
}
