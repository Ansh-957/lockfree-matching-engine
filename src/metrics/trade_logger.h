#pragma once

/// @file trade_logger.h
/// @brief Append-only binary trade logger for post-trade analysis.
///
/// Writes FillMessage records to a binary file for later replay/analysis.
/// Each record is written as raw bytes (FillMessage is trivially copyable).
/// The logger is designed for the output thread, not the matching hot path.

#include <cstddef>
#include <cstdio>
#include <string>

#include "transport/message.h"

namespace engine {

/// @brief Logs fills to a binary file in append-only mode.
class TradeLogger {
public:
    TradeLogger() = default;
    ~TradeLogger();

    // Non-copyable, non-movable — owns a file handle.
    TradeLogger(const TradeLogger&)            = delete;
    TradeLogger& operator=(const TradeLogger&) = delete;
    TradeLogger(TradeLogger&&)                 = delete;
    TradeLogger& operator=(TradeLogger&&)      = delete;

    /// @brief Open the log file for writing. Must be called before log().
    /// @param filepath  Path to the binary log file.
    /// @return true if the file was opened successfully.
    bool open(const std::string& filepath);

    /// @brief Log a fill to the binary file.
    /// @param fill  The fill message to write.
    void log(const FillMessage& fill);

    /// @brief Flush any buffered writes to disk.
    void flush();

    /// @brief Close the log file.
    void close();

    /// @brief Total number of fills logged since open().
    [[nodiscard]] size_t total_logged() const noexcept { return total_logged_; }

private:
    std::FILE* file_         = nullptr;  ///< Binary log file handle
    size_t     total_logged_ = 0;        ///< Number of fills written
};

} // namespace engine
