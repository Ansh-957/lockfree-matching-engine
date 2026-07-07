/// @file trade_logger.cpp
/// @brief Implementation of the binary trade logger.

#include "metrics/trade_logger.h"

namespace engine {

TradeLogger::~TradeLogger() {
    close();
}

bool TradeLogger::open(const std::string& filepath) {
    // Open in binary append mode. Creates the file if it doesn't exist.
    file_ = std::fopen(filepath.c_str(), "ab");
    if (!file_) {
        return false;
    }

    // Write a simple header/magic number so we can validate the file format later.
    // Format: "FILL" (4 bytes) + version (uint32_t = 1) + record size (uint32_t).
    constexpr char     magic[]     = "FILL";
    constexpr uint32_t version     = 1;
    constexpr uint32_t record_size = sizeof(FillMessage);

    std::fwrite(magic, 1, 4, file_);
    std::fwrite(&version, sizeof(version), 1, file_);
    std::fwrite(&record_size, sizeof(record_size), 1, file_);

    return true;
}

void TradeLogger::log(const FillMessage& fill) {
    if (!file_) return;

    // Write the fill as raw bytes. FillMessage is trivially copyable.
    static_assert(std::is_trivially_copyable_v<FillMessage>,
        "FillMessage must be trivially copyable for binary serialization");

    std::fwrite(&fill, sizeof(FillMessage), 1, file_);
    ++total_logged_;
}

void TradeLogger::flush() {
    if (file_) {
        std::fflush(file_);
    }
}

void TradeLogger::close() {
    if (file_) {
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
}

} // namespace engine
