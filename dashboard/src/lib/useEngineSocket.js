import { useEffect, useRef, useState } from 'react'

function resolveUrl() {
  const param = new URLSearchParams(window.location.search).get('ws')
  if (param) return param
  if (import.meta.env.VITE_WS_URL) return import.meta.env.VITE_WS_URL
  return `ws://${window.location.hostname || 'localhost'}:9100`
}

// connects to the engine dashboard feed, parses each frame, reconnects
// with capped backoff. status: 'connecting' | 'live' | 'down'
export function useEngineSocket() {
  const [snapshot, setSnapshot] = useState(null)
  const [status, setStatus] = useState('connecting')
  const urlRef = useRef(resolveUrl())
  const attemptRef = useRef(0)

  useEffect(() => {
    let ws = null
    let retryTimer = null
    let closed = false

    const connect = () => {
      if (closed) return
      setStatus((s) => (s === 'live' ? s : 'connecting'))
      ws = new WebSocket(urlRef.current)

      ws.onopen = () => {
        attemptRef.current = 0
        setStatus('live')
      }

      ws.onmessage = (ev) => {
        try {
          setSnapshot(JSON.parse(ev.data))
        } catch {
          // malformed frame: keep the connection
        }
      }

      ws.onclose = () => {
        if (closed) return
        setStatus('down')
        const delay = Math.min(500 * 2 ** attemptRef.current, 5000)
        attemptRef.current += 1
        retryTimer = setTimeout(connect, delay)
      }

      ws.onerror = () => ws.close()
    }

    connect()
    return () => {
      closed = true
      clearTimeout(retryTimer)
      if (ws) ws.close()
    }
  }, [])

  return { snapshot, status, url: urlRef.current }
}
