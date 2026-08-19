/// @file trade_logger.cpp
/// @brief Implementation of the binary trade logger.

#include "metrics/trade_logger.h"

#include <memory>
#include <type_traits>

namespace engine {

namespace {
// 1MB of stdio buffering: at sizeof(FillMessage)=40ish bytes that batches
// ~26K fills per write() syscall instead of the default 4-8KB buffer.
// The output thread is not latency-critical, but fewer syscalls means it
// spends more time draining the queue and less time in the kernel.
constexpr size_t FILE_BUFFER_SIZE = 1u << 20;
} // namespace

TradeLogger::~TradeLogger() {
    close();
}

bool TradeLogger::open(const std::string& filepath) {
    // "wb" (truncate), not "ab": each run produces one self-contained file.
    // Appending would write a second header mid-file on the next run and
    // corrupt the format for any reader.
    file_ = std::fopen(filepath.c_str(), "wb");
    if (!file_) {
        return false;
    }

    buffer_ = std::make_unique<char[]>(FILE_BUFFER_SIZE);
    std::setvbuf(file_, buffer_.get(), _IOFBF, FILE_BUFFER_SIZE);

    // Header: magic + version + record size. The record size lets a reader
    // detect a FillMessage layout change instead of misparsing silently.
    constexpr char     magic[4]    = {'F', 'I', 'L', 'L'};
    constexpr uint32_t version     = 1;
    constexpr auto     record_size = static_cast<uint32_t>(sizeof(FillMessage));

    std::fwrite(magic, 1, sizeof(magic), file_);
    std::fwrite(&version, sizeof(version), 1, file_);
    std::fwrite(&record_size, sizeof(record_size), 1, file_);

    total_logged_ = 0;
    return true;
}

void TradeLogger::log(const FillMessage& fill) {
    if (!file_) return;

    static_assert(std::is_trivially_copyable_v<FillMessage>,
        "FillMessage must be trivially copyable for binary serialization");

    if (std::fwrite(&fill, sizeof(FillMessage), 1, file_) == 1) {
        ++total_logged_;
    }
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
    buffer_.reset();
}

} // namespace engine
