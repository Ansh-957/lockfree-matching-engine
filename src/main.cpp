// Entry point: thread orchestration for the full pipeline
//
//   ingest (core 0)  CoinbaseFeed or synthetic generator -> SPSC #1
//   engine (core 1)  SPSC #1 -> MatchingEngine -> SPSC #2
//   output (core 2)  SPSC #2 -> trade tape + throughput stats
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
    std::atomic<uint64_t> pool_free{0};   // published by the engine thread
};

// ---- config ----------------------------------------------------------------

struct Config {
    bool        live       = true;
    std::string product    = "BTC-USD";
    unsigned    duration_s = 0;   // 0 = run until Ctrl+C
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
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: matching-engine [--live|--synthetic] "
                         "[--product BTC-USD] [--duration seconds]\n";
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
        const EngineMessage msg = gen.next();
        while (!in.try_push(msg)) {
            if (g_shutdown.load(std::memory_order_acquire)) return;
            cpu_relax();  // backpressure: wait instead of dropping
        }
        c.ingested.fetch_add(1, std::memory_order_relaxed);
    }
}

void run_engine(InputQueue& in, OutputQueue& out, Counters& c,
                const std::atomic<bool>& stop) {
    MatchingEngine eng;
    c.pool_free.store(eng.pool_available(), std::memory_order_relaxed);

    EngineMessage msg;
    uint64_t since_publish = 0;

    while (true) {
        if (in.try_pop(msg)) {
            const auto& fills = eng.process(msg);
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
}

// ingested_fn abstracts where the ingest count lives: the synthetic loop
// bumps our counter, but the live feed counts pushes internally
void run_output(OutputQueue& out, Counters& c, const std::atomic<bool>& stop,
                const std::function<uint64_t()>& ingested_fn) {
    using clock = std::chrono::steady_clock;
    const auto start     = clock::now();
    auto       next_stat = start + std::chrono::seconds(5);

    uint64_t tape_printed = 0;
    uint64_t last_ingested = 0;
    OutputMessage msg;

    const auto print_stats = [&] {
        const auto now = clock::now();
        const auto t   = std::chrono::duration_cast<std::chrono::seconds>(
                             now - start).count();
        const uint64_t ing  = ingested_fn();
        const uint64_t rate = (ing - last_ingested) / 5;
        last_ingested = ing;
        std::cout << "[stats t=" << t << "s]"
                  << " ingested=" << ing << " (" << rate << "/s)"
                  << " processed=" << c.processed.load(std::memory_order_relaxed)
                  << " fills=" << c.fills.load(std::memory_order_relaxed)
                  << " dropped_out=" << c.out_dropped.load(std::memory_order_relaxed)
                  << " pool_free=" << c.pool_free.load(std::memory_order_relaxed)
                  << "\n";
    };

    while (true) {
        if (out.try_pop(msg)) {
            if (const auto* fill = std::get_if<FillMessage>(&msg)) {
                // sample the tape: the first few fills prove the pipeline
                // end to end; after that only 1-in-a-million (synthetic mode
                // produces tens of millions of fills)
                if (tape_printed < 5 || (tape_printed & ((1u << 20) - 1)) == 0) {
                    std::cout << "[fill #" << tape_printed + 1 << "] "
                              << "taker=" << fill->aggressive_id
                              << " maker=" << fill->passive_id
                              << " px=" << ticks_to_dollars(fill->price)
                              << " qty=" << fill->quantity << "\n";
                }
                ++tape_printed;
            }
        } else if (stop.load(std::memory_order_acquire)) {
            break;
        } else {
            if (clock::now() >= next_stat) {
                print_stats();
                next_stat += std::chrono::seconds(5);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    print_stats();
}

} // namespace

int main(int argc, char* argv[]) {
    const Config cfg = parse_args(argc, argv);

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "matching-engine v0.1.0 | mode="
              << (cfg.live ? "live" : "synthetic")
              << (cfg.live ? " product=" + cfg.product : "")
              << (cfg.duration_s ? " duration=" + std::to_string(cfg.duration_s) + "s"
                                 : " (Ctrl+C to stop)")
              << "\n";

    InputQueue  input_queue;
    OutputQueue output_queue;
    Counters    counters;

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
        run_engine(input_queue, output_queue, counters, engine_stop);
    });

    // ---- output thread (core 2): tape + stats ----
    const std::function<uint64_t()> ingested_fn =
        cfg.live
            ? std::function<uint64_t()>([&feed] { return feed.pushed(); })
            : std::function<uint64_t()>([&counters] {
                  return counters.ingested.load(std::memory_order_relaxed);
              });
    std::thread output_thread([&] {
        pin_to_core(2);
        run_output(output_queue, counters, output_stop, ingested_fn);
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

    if (cfg.live) {
        std::cout << "[shutdown] feed: live_levels=" << feed.handler().live_levels()
                  << " malformed=" << feed.handler().malformed_count()
                  << " skipped=" << feed.handler().skipped_count()
                  << " seq_gaps=" << feed.handler().sequence_gaps()
                  << " feed_dropped=" << feed.dropped() << "\n";
    }
    std::cout << "[shutdown] clean exit\n";
    return 0;
}
