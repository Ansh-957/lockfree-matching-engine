// Entry point: thread orchestration for the full pipeline
//
//   ingest (core 0)  CoinbaseFeed or synthetic generator -> SPSC #1
//   engine (core 1)  SPSC #1 -> MatchingEngine -> SPSC #2
//   output (core 3)  SPSC #2 -> trade tape + throughput stats
//                    (core 3, not 2: cpu2 is cpu1's hyperthread sibling)
//
// Shutdown is staged in pipeline order so no queue is abandoned with a
// producer still running:
//   signal/duration -> stop ingest, join it
//                   -> engine_stop; engine drains SPSC #1, joins
//                   -> output_stop; output drains SPSC #2, joins
//
// The signal handler only sets an atomic flag: that is the whole list of
// things a signal handler may safely do (iostream, malloc, locks are all
// forbidden in signal context).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include "core/matching_engine.h"
#include "core/types.h"
#include "feed/coinbase_feed.h"
#include "feed/feed_handler.h"
#include "metrics/metrics_collector.h"
#include "metrics/trade_logger.h"
#include "metrics/tsc_clock.h"
#include "tools/synthetic_workload.h"
#include "transport/message.h"
#include "transport/spsc_queue.h"

namespace {

using namespace engine;

// ---- shutdown flags ------------------------------------------------------

std::atomic<bool> g_shutdown{false};   // set by SIGINT/SIGTERM or --duration

void signal_handler(int) {
    g_shutdown.store(true, std::memory_order_release);
}

// ---- CPU affinity --------------------------------------------------------

// Pinning a thread to one core keeps its working set in that core's L1/L2
// and stops the scheduler from migrating it mid-burst. Best effort: on
// failure (fewer cores, no permission) the thread simply runs unpinned.
bool pin_to_core(unsigned core) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)core;
    return false;
#endif
}

// SCHED_FIFO means "run until you block or a higher-priority RT task
// preempts you" - no timeslice round-robin against normal processes.
// Requires root (or CAP_SYS_NICE); silently degrades to normal scheduling.
bool try_realtime_priority() {
#if defined(__linux__)
    sched_param param{};
    param.sched_priority = 80;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
#else
    return false;
#endif
}

// polite busy-wait: PAUSE tells the CPU this is a spin loop (saves power,
// yields pipeline resources to the sibling hyperthread)
inline void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

// ---- cross-thread counters (stats only, all relaxed) ----------------------

struct Counters {
    std::atomic<uint64_t> ingested{0};    // synthetic mode: pushed into SPSC #1
    std::atomic<uint64_t> processed{0};   // messages through the engine
    std::atomic<uint64_t> fills{0};       // fills emitted
    std::atomic<uint64_t> out_dropped{0}; // SPSC #2 full
    std::atomic<uint64_t> lat_dropped{0}; // telemetry ring full
    std::atomic<uint64_t> pool_free{0};   // published by the engine thread
};

// ---- telemetry samples (Commit 11) -----------------------------------------
//
// The engine thread must never do histogram work: binning, percentile
// bookkeeping, even a shared atomic bump would put cache traffic into the
// measured path. Instead it ships raw nanosecond readings over a third
// SPSC ring and the metrics (output) thread does all the aggregation.
// If the ring is ever full the sample is DROPPED, not waited on - losing
// a telemetry point is free, stalling the matching loop is not.

struct LatencySample {
    enum class Kind : uint8_t {
        Match,  // time spent inside MatchingEngine::process()
        Order,  // ingest push -> engine done (queue wait + match)
    };
    Kind     kind = Kind::Match;
    uint64_t ns   = 0;
};
static_assert(std::is_trivially_copyable_v<LatencySample>);

using LatencyQueue = SPSCQueue<LatencySample, 1u << 16>;

// ---- config ----------------------------------------------------------------

struct Config {
    bool        live       = true;
    std::string product    = "BTC-USD";
    unsigned    duration_s = 0;   // 0 = run until Ctrl+C
    std::string log_path   = "trades.bin";  // empty = logging disabled
};

Config parse_args(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--live" || arg == "-l") {
            cfg.live = true;
        } else if (arg == "--synthetic" || arg == "-s") {
            cfg.live = false;
        } else if (arg == "--product" && i + 1 < argc) {
            cfg.product = argv[++i];
        } else if (arg == "--duration" && i + 1 < argc) {
            cfg.duration_s = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (arg == "--log" && i + 1 < argc) {
            cfg.log_path = argv[++i];
        } else if (arg == "--no-log") {
            cfg.log_path.clear();
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: matching-engine [--live|--synthetic] "
                         "[--product BTC-USD] [--duration seconds] "
                         "[--log trades.bin | --no-log]\n";
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << " (see --help)\n";
            std::exit(1);
        }
    }
    return cfg;
}

using InputQueue  = SPSCQueue<EngineMessage, FEED_QUEUE_SIZE>;
using OutputQueue = SPSCQueue<OutputMessage, FEED_QUEUE_SIZE>;

// ---- threads ---------------------------------------------------------------

// synthetic ingest: same generator the benchmarks use, throttled only by
// queue capacity - a local full-pipeline load test with zero network
void run_synthetic_ingest(InputQueue& in, Counters& c) {
    synth::WorkloadConfig wcfg;
    wcfg.cancel_rate = 0.35;
    synth::WorkloadGenerator gen(wcfg);

    while (!g_shutdown.load(std::memory_order_acquire)) {
        EngineMessage msg = gen.next();
        // overwrite the generator's logical timestamp with a real clock
        // reading so the engine can compute end-to-end (queue wait + match)
        // latency for this message
        std::visit([](auto& m) { m.timestamp = tsc::now_ns(); }, msg);
        while (!in.try_push(msg)) {
            if (g_shutdown.load(std::memory_order_acquire)) return;
            cpu_relax();  // backpressure: wait instead of dropping
        }
        c.ingested.fetch_add(1, std::memory_order_relaxed);
    }
}

void run_engine(InputQueue& in, OutputQueue& out, LatencyQueue& lat,
                Counters& c, const std::atomic<bool>& stop) {
    MatchingEngine eng;
    c.pool_free.store(eng.pool_available(), std::memory_order_relaxed);

    EngineMessage msg;
    uint64_t since_publish = 0;
    uint64_t lat_dropped   = 0;  // thread-local; published on exit

    // the hot path's ENTIRE telemetry cost: two RDTSC reads and up to two
    // ring pushes per message. All histogram work happens downstream.
    const auto emit = [&](LatencySample::Kind k, uint64_t ns) {
        if (!lat.try_push(LatencySample{k, ns})) ++lat_dropped;
    };

    while (true) {
        if (in.try_pop(msg)) {
            const uint64_t t0 = tsc::now_ns();
            const auto& fills = eng.process(msg);
            const uint64_t t1 = tsc::now_ns();

            emit(LatencySample::Kind::Match, t1 - t0);
            // ingest stamped msg.timestamp with the same clock at push time
            const auto ingest_ts = std::visit(
                [](const auto& m) { return m.timestamp; }, msg);
            if (ingest_ts != 0 && t1 > ingest_ts) {
                emit(LatencySample::Kind::Order, t1 - ingest_ts);
            }

            for (const Fill& f : fills) {
                if (out.try_push(OutputMessage{f})) {
                    c.fills.fetch_add(1, std::memory_order_relaxed);
                } else {
                    c.out_dropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
            c.processed.fetch_add(1, std::memory_order_relaxed);

            // pool_available() is engine-thread state; publish a snapshot
            // occasionally instead of letting the stats thread race on it
            if (++since_publish >= 1024) {
                since_publish = 0;
                c.pool_free.store(eng.pool_available(), std::memory_order_relaxed);
            }
        } else if (stop.load(std::memory_order_acquire)) {
            break;  // producer joined AND queue drained
        } else {
            cpu_relax();
        }
    }
    c.pool_free.store(eng.pool_available(), std::memory_order_relaxed);
    c.lat_dropped.store(lat_dropped, std::memory_order_relaxed);
}

// The metrics worker thread: drains fills (tape + binary log) and raw
// latency samples (histogram binning). ingested_fn abstracts where the
// ingest count lives: the synthetic loop bumps our counter, but the live
// feed counts pushes internally.
void run_output(OutputQueue& out, LatencyQueue& lat, Counters& c,
                MetricsCollector& metrics, TradeLogger& logger,
                const std::atomic<bool>& stop,
                const std::function<uint64_t()>& ingested_fn) {
    using clock = std::chrono::steady_clock;
    const auto start     = clock::now();
    auto       next_stat = start + std::chrono::seconds(5);

    uint64_t tape_printed = 0;
    uint64_t last_ingested = 0;
    OutputMessage msg;
    LatencySample sample;

    const auto print_stats = [&] {
        const auto now = clock::now();
        const auto t   = std::chrono::duration_cast<std::chrono::seconds>(
                             now - start).count();
        const uint64_t ing  = ingested_fn();
        const uint64_t rate = (ing - last_ingested) / 5;
        last_ingested = ing;
        const auto& ml = metrics.match_latency();
        std::cout << "[stats t=" << t << "s]"
                  << " ingested=" << ing << " (" << rate << "/s)"
                  << " processed=" << c.processed.load(std::memory_order_relaxed)
                  << " fills=" << c.fills.load(std::memory_order_relaxed)
                  << " match_p50=" << ml.p50() << "ns"
                  << " p99=" << ml.p99() << "ns"
                  << " dropped_out=" << c.out_dropped.load(std::memory_order_relaxed)
                  << " pool_free=" << c.pool_free.load(std::memory_order_relaxed)
                  << "\n";
    };

    uint64_t iter = 0;
    while (true) {
        bool did_work = false;

        // drain latency samples first and in batches - in synthetic mode
        // this ring carries ~2 samples per message, far more traffic than
        // the fill queue
        for (int i = 0; i < 4096 && lat.try_pop(sample); ++i) {
            if (sample.kind == LatencySample::Kind::Match) {
                metrics.record_match_latency(sample.ns);
                metrics.increment_orders();
            } else {
                metrics.record_order_latency(sample.ns);
            }
            did_work = true;
        }

        if (out.try_pop(msg)) {
            did_work = true;
            if (const auto* fill = std::get_if<FillMessage>(&msg)) {
                logger.log(*fill);
                metrics.increment_matches();
                // sample the tape: the first few fills prove the pipeline
                // end to end; after that only 1-in-a-million
                if (tape_printed < 5 || (tape_printed & ((1u << 20) - 1)) == 0) {
                    std::cout << "[fill #" << tape_printed + 1 << "] "
                              << "taker=" << fill->aggressive_id
                              << " maker=" << fill->passive_id
                              << " px=" << ticks_to_dollars(fill->price)
                              << " qty=" << fill->quantity << "\n";
                }
                ++tape_printed;
            }
        }

        // check the stats timer even when saturated with work (in synthetic
        // mode the sample ring never runs dry, so "idle" never happens) -
        // but only every 256 iterations, clock reads aren't free
        if (!did_work || (++iter & 0xFF) == 0) {
            if (clock::now() >= next_stat) {
                print_stats();
                next_stat += std::chrono::seconds(5);
            }
        }

        if (!did_work) {
            if (stop.load(std::memory_order_acquire)) {
                break;  // engine joined AND both rings drained
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    print_stats();
    metrics.note_dropped_samples(c.lat_dropped.load(std::memory_order_relaxed));
}

} // namespace

int main(int argc, char* argv[]) {
    const Config cfg = parse_args(argc, argv);

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // learn the TSC frequency now (20ms sleep) rather than lazily inside
    // the first timed message on the hot path
    tsc::calibrate();

    std::cout << "matching-engine v0.1.0 | mode="
              << (cfg.live ? "live" : "synthetic")
              << (cfg.live ? " product=" + cfg.product : "")
              << (cfg.duration_s ? " duration=" + std::to_string(cfg.duration_s) + "s"
                                 : " (Ctrl+C to stop)")
              << "\n";

    InputQueue   input_queue;
    OutputQueue  output_queue;
    LatencyQueue latency_queue;
    Counters     counters;

    MetricsCollector metrics;
    TradeLogger      logger;
    if (!cfg.log_path.empty()) {
        if (logger.open(cfg.log_path)) {
            std::cout << "[init] logging fills to " << cfg.log_path << "\n";
        } else {
            std::cerr << "[init] WARNING: could not open " << cfg.log_path
                      << ", fill logging disabled\n";
        }
    }

    std::atomic<bool> engine_stop{false};
    std::atomic<bool> output_stop{false};

    // live feed object outlives the ingest thread so main can stop() it
    CoinbaseFeed feed{cfg.product};

    // ---- ingest thread (core 0): produces into SPSC #1 ----
    std::thread ingest([&] {
        pin_to_core(0);
        if (cfg.live) {
            feed.start(input_queue);  // blocks in the Asio loop
        } else {
            run_synthetic_ingest(input_queue, counters);
        }
    });

    // ---- engine thread (core 1): the hot path ----
    std::thread engine_thread([&] {
        pin_to_core(1);
        if (try_realtime_priority()) {
            std::cout << "[init] engine thread: SCHED_FIFO acquired\n";
        }
        run_engine(input_queue, output_queue, latency_queue, counters,
                   engine_stop);
    });

    // ---- output thread (core 3): tape + stats ----
    //
    // NOT core 2: on hybrid Intel (this dev box is a Core Ultra 155H),
    // cpu1 and cpu2 are hyperthread SIBLINGS of the same physical P-core.
    // Pinning the output thread there would make its histogram work steal
    // execution resources from the engine's hot loop. cpu3 is the next
    // distinct physical core. (cpu0's sibling is cpu5, so ingest/engine
    // were already separate.)
    const std::function<uint64_t()> ingested_fn =
        cfg.live
            ? std::function<uint64_t()>([&feed] { return feed.pushed(); })
            : std::function<uint64_t()>([&counters] {
                  return counters.ingested.load(std::memory_order_relaxed);
              });
    std::thread output_thread([&] {
        pin_to_core(3);
        run_output(output_queue, latency_queue, counters, metrics, logger,
                   output_stop, ingested_fn);
    });

    // ---- main thread: wait for signal or deadline ----
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(cfg.duration_s);
    while (!g_shutdown.load(std::memory_order_acquire)) {
        if (cfg.duration_s != 0 && std::chrono::steady_clock::now() >= deadline) {
            g_shutdown.store(true, std::memory_order_release);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // ---- staged shutdown, upstream first ----
    std::cout << "[shutdown] stopping ingest...\n";
    if (cfg.live) {
        feed.stop();          // posts a websocket close; run() returns
    }
    ingest.join();            // synthetic loop watches g_shutdown itself

    engine_stop.store(true, std::memory_order_release);
    engine_thread.join();     // drains SPSC #1, then exits

    output_stop.store(true, std::memory_order_release);
    output_thread.join();     // drains SPSC #2, prints final stats

    logger.close();  // flushes the stdio buffer

    if (cfg.live) {
        std::cout << "[shutdown] feed: live_levels=" << feed.handler().live_levels()
                  << " malformed=" << feed.handler().malformed_count()
                  << " skipped=" << feed.handler().skipped_count()
                  << " seq_gaps=" << feed.handler().sequence_gaps()
                  << " feed_dropped=" << feed.dropped() << "\n";
    }
    if (logger.total_logged() > 0) {
        std::cout << "[shutdown] " << logger.total_logged()
                  << " fills logged to " << cfg.log_path << "\n";
    }
    std::cout << "\n" << metrics.summary();
    std::cout << "[shutdown] clean exit\n";
    return 0;
}
