// feed_parser_test.cpp — Unit tests for feed parsing (Coinbase L2, etc.)
// Built from top-level CMakeLists.txt as part of the unit_tests target.
//
// ============================================================================
// NOTE: These tests will be implemented in Phase 3 when the feed parser
//       and WebSocket client are fully integrated.
// ============================================================================

#include <gtest/gtest.h>
#include "feed/coinbase_feed.h"
#include "feed/feed_handler.h"
#include "core/types.h"

// ---------------------------------------------------------------------------
// TODO stubs — Phase 3
// ---------------------------------------------------------------------------

TEST(FeedParserTest, ParseCoinbaseL2Update) {
    // TODO (Phase 3): Construct a valid Coinbase L2 update JSON message.
    //       Feed it into the CoinbaseFeed parser.
    //       Verify:
    //         - Parsed side (bid/ask) is correct.
    //         - Parsed price and quantity match the JSON payload.
    //         - The resulting internal message type is correct.
    GTEST_SKIP() << "Not implemented — Phase 3";
}

TEST(FeedParserTest, HandleMalformedJson) {
    // TODO (Phase 3): Feed malformed / truncated JSON into the parser.
    //       Verify:
    //         - Parser returns an error code (not a crash).
    //         - No partial data leaks into the order book.
    GTEST_SKIP() << "Not implemented — Phase 3";
}

TEST(FeedParserTest, SequenceGapDetection) {
    // TODO (Phase 3): Send two L2 updates with a sequence gap.
    //       Verify:
    //         - The feed handler detects the gap.
    //         - A snapshot re-request is triggered (or an error is logged).
    GTEST_SKIP() << "Not implemented — Phase 3";
}
