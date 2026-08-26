import { fmtCompact, fmtInt } from '../lib/format'

export default function StatusBar({ stats, rates, url }) {
  const used = stats.pool_capacity
    ? (stats.pool_capacity - stats.pool_free) / stats.pool_capacity
    : 0
  const poolClass = used > 0.9 ? 'bad' : used > 0.7 ? 'warn' : ''

  return (
    <div className="status">
      <div className="scell">
        <span className="k">Msg/s</span>
        <span className="v num">{fmtCompact(rates.msgRate)}</span>
      </div>
      <div className="scell">
        <span className="k">Fill/s</span>
        <span className="v num">{fmtCompact(rates.fillRate)}</span>
      </div>
      <div className="scell">
        <span className="k">Processed</span>
        <span className="v num">{fmtCompact(stats.processed)}</span>
      </div>
      <div className="scell">
        <span className="k">Fills</span>
        <span className="v num">{fmtCompact(stats.fills)}</span>
      </div>
      <div className="scell">
        <span className="k">Resting</span>
        <span className="v num">
          {fmtInt((stats.book_bids ?? 0) + (stats.book_asks ?? 0))}
          <span style={{ color: 'var(--dim)' }}>
            {' '}
            {fmtCompact(stats.book_bids)}b {fmtCompact(stats.book_asks)}a
          </span>
        </span>
      </div>
      <div className="scell">
        <span className="k">Pool</span>
        <span className="v num">{(used * 100).toFixed(2)}%</span>
        <span className={`pool-track ${poolClass}`}>
          <i style={{ width: `${Math.min(100, used * 100)}%` }} />
        </span>
      </div>
      {stats.dropped > 0 && (
        <div className="scell">
          <span className="k">Drop</span>
          <span className="v num" style={{ color: 'var(--ask)' }}>
            {fmtCompact(stats.dropped)}
          </span>
        </div>
      )}
      <div className="scell ml">
        <span className="k">{url.replace(/^ws:\/\//, '')}</span>
        <span className="v num">{stats.clients} cl · 10 Hz</span>
      </div>
    </div>
  )
}
