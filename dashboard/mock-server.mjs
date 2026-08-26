// Mock engine feed: emits the exact JSON schema the C++ DashboardHub
// produces, with a plausible random-walk market. Lets the dashboard be
// developed and demoed without the engine (or on a machine that can't
// build the feed). Run:  npm run mock
//
// Schema contract (keep in sync with src/server/dashboard_hub.h):
//   bids/asks: [price_ticks, qty, orders] best-first
//   fills:     [seq, ts_ms, price_ticks, qty, taker_is_bid] oldest-first

import { WebSocketServer } from 'ws'

const PORT = 9100
const TICK_MS = 100
const DEPTH = 15

// ---- market state ----------------------------------------------------------

let mid = 6_712_400 // ticks = $67,124.00
let momentum = 0
let fillSeq = 0
let processed = 1_250_000
let fills = 58_400
const recentFills = []

const rand = (a, b) => a + Math.random() * (b - a)
const jitter = (v, pct) => v * rand(1 - pct, 1 + pct)

function step() {
  // price: mean-reverting random walk with momentum bursts
  momentum = momentum * 0.92 + rand(-8, 8)
  mid += Math.round(momentum + rand(-4, 4))

  // book: levels cluster at the touch. Size jitters slowly so the ladder
  // doesn't strobe on every 100ms frame (only real size changes flash)
  const side = (dir) => {
    const levels = []
    let p = mid + dir * 2
    for (let i = 0; i < DEPTH; ++i) {
      const base = 12 * (1 + i * 0.55)
      const qty = Math.round(base * rand(0.92, 1.08))
      const orders = Math.max(1, Math.round(qty / 11))
      levels.push([p, qty, orders])
      p += dir * (i < 3 ? 1 : 1 + (i % 3 === 0 ? 1 : 0))
    }
    return levels
  }
  const bids = side(-1)
  const asks = side(+1)

  // fills: bursty, direction follows momentum
  const n = Math.random() < 0.75 ? Math.floor(rand(0, 4)) : Math.floor(rand(4, 12))
  const now = Date.now()
  for (let i = 0; i < n; ++i) {
    const buy = Math.random() < 0.5 + Math.max(-0.35, Math.min(0.35, momentum / 20))
    recentFills.push([
      ++fillSeq,
      now - Math.floor(rand(0, TICK_MS)),
      mid + Math.round(rand(-3, 3)),
      Math.round(rand(1, 60)),
      buy ? 1 : 0,
    ])
  }
  while (recentFills.length > 48) recentFills.shift()
  fills += n

  // throughput ~ live-feed scale with bursts
  processed += Math.round(jitter(1_800, 0.5))

  // latency: matches the shape of the real measurements (see benchmarks.md)
  const spike = Math.random() < 0.04
  const stats = {
    match_p50: Math.round(jitter(28, 0.15)),
    match_p95: Math.round(jitter(165, 0.2)),
    match_p99: Math.round(spike ? rand(1200, 4800) : jitter(567, 0.25)),
    order_p50: Math.round(jitter(700_000, 0.2)),
    order_p99: Math.round(jitter(1_400_000, 0.25)),
    processed,
    fills,
    pool_free: 1_000_000 - Math.round(rand(1_800, 2_600)),
    pool_capacity: 1_000_000,
    dropped: 0,
    clients: wss.clients.size,
    book_bids: Math.round(rand(1900, 2100)),
    book_asks: Math.round(rand(1900, 2100)),
  }

  return JSON.stringify({
    t: now,
    mode: 'live',
    product: 'BTC-USD',
    bids,
    asks,
    fills: recentFills,
    stats,
  })
}

// ---- server ----------------------------------------------------------------

const wss = new WebSocketServer({ port: PORT })

wss.on('connection', (ws, req) => {
  console.log(`[mock] client connected (${req.socket.remoteAddress}), total ${wss.clients.size}`)
  ws.on('close', () => console.log(`[mock] client left, total ${wss.clients.size}`))
})

setInterval(() => {
  if (wss.clients.size === 0) return
  const frame = step()
  for (const c of wss.clients) {
    if (c.readyState === c.OPEN) c.send(frame)
  }
}, TICK_MS)

console.log(`[mock] engine feed simulator on ws://localhost:${PORT} (${1000 / TICK_MS}Hz)`)
