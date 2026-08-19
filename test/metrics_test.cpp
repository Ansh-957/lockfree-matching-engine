// Tests for the Commit 11 metrics layer: HDR log-linear histogram,
// binary trade logger, and the collector's summary plumbing.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "metrics/latency_tracker.h"
#include "metrics/metrics_collector.h"
#include "metrics/trade_logger.h"
#include "metrics/tsc_clock.h"
#include "transport/message.h"

using namespace engine;

// ---------------------------------------------------------------------------
// LatencyTracker: bucket geometry
// ---------------------------------------------------------------------------

TEST(LatencyTrackerTest, SmallValuesAreExact) {
    // values below SUB_COUNT get their own 1ns bucket
    for (uint64_t v = 0; v < LatencyTracker::SUB_COUNT; ++v) {
        EXPECT_EQ(LatencyTracker::bucket_index(v), v);
        EXPECT_EQ(LatencyTracker::bucket_upper(v), v);
    }
}

TEST(LatencyTrackerTest, BucketIndexIsMonotonicAndContiguous) {
    // walking values upward one at a time must never skip or reverse a
    // bucket: each step lands in the same bucket or the very next one
    size_t prev = 0;
    for (uint64_t v = 1; v < 1'000'000; ++v) {
        const size_t idx = LatencyTracker::bucket_index(v);
        ASSERT_GE(idx, prev);
        ASSERT_LE(idx - prev, 1u);  // contiguous: no holes
        prev = idx;
    }
}

TEST(LatencyTrackerTest, RelativeErrorBounded) {
    // HDR guarantee: bucket_upper(v's bucket) overestimates v by <= 1/64
    for (uint64_t v : {100ull, 1'000ull, 12'345ull, 999'999ull,
                       50'000'000ull, 3'000'000'000ull}) {
        const uint64_t upper = LatencyTracker::bucket_upper(
            LatencyTracker::bucket_index(v));
        EXPECT_GE(upper, v);
        EXPECT_LE(static_cast<double>(upper - v),
                  static_cast<double>(v) / 64.0 + 1.0)
            << "v=" << v << " upper=" << upper;
    }
}

// ---------------------------------------------------------------------------
// LatencyTracker: statistics
// ---------------------------------------------------------------------------

TEST(LatencyTrackerTest, EmptyTrackerReturnsZero) {
    LatencyTracker t;
    EXPECT_EQ(t.count(), 0u);
    EXPECT_EQ(t.p50(), 0u);
    EXPECT_EQ(t.p99(), 0u);
    EXPECT_EQ(t.max(), 0u);
    EXPECT_EQ(t.min(), 0u);
    EXPECT_EQ(t.mean(), 0u);
}

TEST(LatencyTrackerTest, PercentilesOfUniformDistribution) {
    LatencyTracker t;
    for (uint64_t v = 1; v <= 100'000; ++v) {
        t.record(v);
    }
    EXPECT_EQ(t.count(), 100'000u);
    EXPECT_EQ(t.max(), 100'000u);
    EXPECT_EQ(t.min(), 1u);

    // percentile of uniform 1..N should be ~pct% of N, within the 1/64
    // bucket width plus rounding
    const auto near = [](uint64_t got, uint64_t want) {
        const auto tol = static_cast<uint64_t>(
            static_cast<double>(want) / 64.0 + 2.0);
        return got + tol >= want && got <= want + tol;
    };
    EXPECT_TRUE(near(t.p50(), 50'000))  << "p50=" << t.p50();
    EXPECT_TRUE(near(t.p95(), 95'000))  << "p95=" << t.p95();
    EXPECT_TRUE(near(t.p99(), 99'000))  << "p99=" << t.p99();
    EXPECT_TRUE(near(t.p999(), 99'900)) << "p999=" << t.p999();
}

TEST(LatencyTrackerTest, TailSamplesMoveP999NotP50) {
    // 99% fast, 1% slow: nearest-rank p99.9 (the 9990th of 10000 sorted
    // samples) falls squarely inside the slow tail, while p50/p95 must not
    // see it at all
    LatencyTracker t;
    for (int i = 0; i < 9'900; ++i) t.record(100);
    for (int i = 0; i < 100; ++i)   t.record(50'000);

    EXPECT_LE(t.p50(), 101u);
    EXPECT_LE(t.p95(), 101u);
    EXPECT_GE(t.p999(), 49'000u);
    EXPECT_EQ(t.max(), 50'000u);
}

TEST(LatencyTrackerTest, OverflowGoesToLastBucket) {
    LatencyTracker t;
    const uint64_t huge = uint64_t{1} << 50;  // beyond MAX_BITS
    t.record(huge);
    EXPECT_EQ(t.count(), 1u);
    EXPECT_EQ(t.max(), huge);      // max is tracked exactly even on overflow
    EXPECT_GT(t.p50(), 0u);        // percentile returns the overflow bucket
}

TEST(LatencyTrackerTest, ResetClearsEverything) {
    LatencyTracker t;
    t.record(500);
    t.record(1'000'000);
    t.reset();
    EXPECT_EQ(t.count(), 0u);
    EXPECT_EQ(t.max(), 0u);
    EXPECT_EQ(t.p99(), 0u);
}

TEST(LatencyTrackerTest, MeanIsExact) {
    LatencyTracker t;
    t.record(100);
    t.record(200);
    t.record(300);
    EXPECT_EQ(t.mean(), 200u);  // mean uses the exact sum, not buckets
}

// ---------------------------------------------------------------------------
// TradeLogger: binary round-trip
// ---------------------------------------------------------------------------

TEST(TradeLoggerTest, WritesHeaderAndRecordsReadBackExactly) {
    const std::string path =
        (std::filesystem::temp_directory_path() / "trade_logger_test.bin").string();

    std::vector<FillMessage> fills;
    for (uint32_t i = 1; i <= 100; ++i) {
        fills.push_back(FillMessage{
            .aggressive_id = i,
            .passive_id    = i + 1'000,
            .price         = static_cast<Price>(i * 25),
            .quantity      = i * 3,
            .timestamp     = i * 7,
        });
    }

    {
        TradeLogger logger;
        ASSERT_TRUE(logger.open(path));
        for (const auto& f : fills) logger.log(f);
        EXPECT_EQ(logger.total_logged(), fills.size());
        logger.close();
    }

    std::FILE* f = std::fopen(path.c_str(), "rb");
    ASSERT_NE(f, nullptr);

    char     magic[4];
    uint32_t version = 0, record_size = 0;
    ASSERT_EQ(std::fread(magic, 1, 4, f), 4u);
    ASSERT_EQ(std::fread(&version, sizeof(version), 1, f), 1u);
    ASSERT_EQ(std::fread(&record_size, sizeof(record_size), 1, f), 1u);
    EXPECT_EQ(std::string(magic, 4), "FILL");
    EXPECT_EQ(version, 1u);
    EXPECT_EQ(record_size, sizeof(FillMessage));

    for (const auto& expected : fills) {
        FillMessage got{};
        ASSERT_EQ(std::fread(&got, sizeof(got), 1, f), 1u);
        EXPECT_EQ(got.aggressive_id, expected.aggressive_id);
        EXPECT_EQ(got.passive_id,    expected.passive_id);
        EXPECT_EQ(got.price,         expected.price);
        EXPECT_EQ(got.quantity,      expected.quantity);
        EXPECT_EQ(got.timestamp,     expected.timestamp);
    }
    // and nothing after the last record
    FillMessage extra{};
    EXPECT_EQ(std::fread(&extra, sizeof(extra), 1, f), 0u);

    std::fclose(f);
    std::filesystem::remove(path);
}

TEST(TradeLoggerTest, ReopenTruncatesInsteadOfAppending) {
    const std::string path =
        (std::filesystem::temp_directory_path() / "trade_logger_trunc.bin").string();

    for (int run = 0; run < 2; ++run) {
        TradeLogger logger;
        ASSERT_TRUE(logger.open(path));
        logger.log(FillMessage{});
        logger.close();
    }

    // one header + one record, not two of each
    EXPECT_EQ(std::filesystem::file_size(path),
              12u + sizeof(FillMessage));
    std::filesystem::remove(path);
}

TEST(TradeLoggerTest, LogWithoutOpenIsSafe) {
    TradeLogger logger;
    logger.log(FillMessage{});  // must not crash
    logger.flush();
    EXPECT_EQ(logger.total_logged(), 0u);
}

// ---------------------------------------------------------------------------
// MetricsCollector
// ---------------------------------------------------------------------------

TEST(MetricsCollectorTest, SummaryReflectsRecordedData) {
    MetricsCollector m;
    for (int i = 0; i < 1000; ++i) {
        m.record_match_latency(150);
        m.increment_orders();
    }
    m.record_order_latency(2'000);
    m.increment_matches();

    const std::string s = m.summary();
    EXPECT_NE(s.find("orders processed: 1000"), std::string::npos) << s;
    EXPECT_NE(s.find("fills generated:  1"), std::string::npos) << s;
    EXPECT_EQ(m.match_latency().count(), 1000u);
    EXPECT_EQ(m.order_latency().count(), 1u);

    m.reset();
    EXPECT_EQ(m.total_orders(), 0u);
    EXPECT_EQ(m.match_latency().count(), 0u);
}

// ---------------------------------------------------------------------------
// TSC clock sanity
// ---------------------------------------------------------------------------

TEST(TscClockTest, MonotonicAndRoughlyCalibrated) {
    tsc::calibrate();
    const uint64_t a = tsc::now_ns();
    // burn ~10ms of wall time and check the TSC clock saw the same interval
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(10)) {}
    const uint64_t b = tsc::now_ns();

    ASSERT_GT(b, a);
    const uint64_t elapsed = b - a;
    // within 20% of 10ms - generous, but catches a botched calibration
    // (wrong by 2x) without being flaky under CI scheduling noise
    EXPECT_GT(elapsed, 8'000'000u);
    EXPECT_LT(elapsed, 12'000'000u);
}
