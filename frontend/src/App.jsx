import { useEffect, useEffectEvent, useRef, useState } from 'react'

const HISTORY = 60      // samples kept for sparklines (1 per second)
// No telemetry for this long = link is dead (a pulled plug never sends a TCP close).
// Must outlast a Wi-Fi scan: the radio hops channels for ~2-4 s and telemetry pauses.
const STALE_MS = 6000
const RETRY_MS = 2000

function initialIp() {
  const q = new URLSearchParams(location.search).get('ip')
  if (q) return q
  // Served from the ESP itself (Phase 5): talk back to the same host.
  if (!['localhost', '127.0.0.1'].includes(location.hostname)) return location.host
  try { return localStorage.getItem('esp-ip') || '' } catch { return '' }
}

// Telemetry frames have no "type"; replies to commands (pong, scan, error) do and go to onEvent,
// plus a synthetic {type: 'disconnected'} so pending commands can give up.
function useTelemetry(ip, onEvent) {
  const [status, setStatus] = useState('idle')
  const wsRef = useRef(null)
  const handleEvent = useEffectEvent(onEvent)
  const [data, setData] = useState({ ip, list: [] }) // tagged with ip so switching devices starts fresh

  useEffect(() => {
    if (!ip) return
    let ws, retry, watchdog, stopped = false

    const drop = () => {
      clearTimeout(watchdog)
      ws.onclose = ws.onmessage = null
      ws.close()
      if (stopped) return
      setStatus('offline')
      handleEvent({ type: 'disconnected' })
      retry = setTimeout(connect, RETRY_MS)
    }
    const kick = () => { clearTimeout(watchdog); watchdog = setTimeout(drop, STALE_MS) }

    function connect() {
      setStatus('connecting')
      ws = wsRef.current = new WebSocket(`ws://${ip}/ws`)
      kick()
      ws.onclose = drop
      ws.onmessage = (e) => {
        let d
        try { d = JSON.parse(e.data) } catch { return }
        kick()
        setStatus('live')
        if (d.type) return handleEvent(d)
        setData((prev) => ({ ip, list: prev.ip === ip ? [...prev.list.slice(1 - HISTORY), d] : [d] }))
      }
    }

    connect()
    return () => { stopped = true; clearTimeout(retry); drop() }
  }, [ip])

  const send = (obj) => wsRef.current?.readyState === WebSocket.OPEN && wsRef.current.send(JSON.stringify(obj))
  return { status, history: data.ip === ip ? data.list : [], send }
}

function tempInfo(t) {
  if (t < 30) return ['var(--cool)', 'Cool']
  if (t < 45) return ['var(--ok)', 'Normal']
  if (t < 60) return ['var(--warm)', 'Warm']
  return ['var(--hot)', 'Hot']
}

function rssiLabel(r) {
  if (r == null) return 'No signal'
  if (r >= -55) return 'Excellent'
  if (r >= -67) return 'Good'
  if (r >= -75) return 'Fair'
  return 'Weak'
}

function fmtUptime(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60
  return `${d ? d + 'd ' : ''}${h}h ${String(m).padStart(2, '0')}m ${String(s % 60).padStart(2, '0')}s`
}

const kb = (b) => (b / 1024).toFixed(1) + ' KB'

function Spark({ values, color }) {
  const v = values.filter((x) => x != null)
  if (v.length < 2) return <svg className="spark" aria-hidden="true" />
  const min = Math.min(...v), max = Math.max(...v), span = max - min || 1
  const off = HISTORY - v.length // right-align so new samples enter from the right
  const pts = v.map((x, i) => `${((off + i) / (HISTORY - 1)) * 100},${29 - ((x - min) / span) * 28}`).join(' ')
  return (
    <svg className="spark" viewBox="0 0 100 30" preserveAspectRatio="none" aria-hidden="true">
      <polyline points={pts} fill="none" stroke={color} strokeWidth="2" vectorEffect="non-scaling-stroke" />
    </svg>
  )
}

function Card({ title, value, sub, children }) {
  return (
    <section className="card">
      <h2>{title}</h2>
      <div className="value">{value}</div>
      <div className="sub">{sub}</div>
      {children}
    </section>
  )
}

export default function App() {
  const [ip, setIp] = useState(initialIp)
  const [draft, setDraft] = useState(ip)
  const [rtt, setRtt] = useState(null)
  const [scan, setScan] = useState({ busy: false, aps: null })
  const [cmdError, setCmdError] = useState('')
  const [ledColor, setLedColor] = useState('#ff00ff')

  const { status, history, send } = useTelemetry(ip, (d) => {
    if (d.type === 'pong') setRtt(Math.round(performance.now() - d.t))
    if (d.type === 'scan') setScan({ busy: false, aps: d.aps.toSorted((a, b) => b.rssi - a.rssi) })
    if (d.type === 'error') { setCmdError(d.msg); setScan((s) => ({ ...s, busy: false })) }
    if (d.type === 'disconnected') setScan((s) => ({ ...s, busy: false })) // its result went to the dead socket
  })
  const live = status === 'live'
  const scanning = scan.busy && live
  const t = history.at(-1)

  const submit = (e) => {
    e.preventDefault()
    const next = draft.trim()
    setIp(next)
    try { localStorage.setItem('esp-ip', next) } catch { /* private mode: fine */ }
  }

  const [tColor, tLabel] = t ? tempInfo(t.temp) : ['var(--muted)', '']
  const usedPct = t?.heap_total ? ((t.heap_total - t.heap) / t.heap_total) * 100 : 0

  return (
    <main>
      <header>
        <h1>ESP32-S3</h1>
        <span className={`pill ${status}`} role="status">{status}</span>
        <form onSubmit={submit}>
          <label htmlFor="ip">Device IP</label>
          <input id="ip" value={draft} onChange={(e) => setDraft(e.target.value)}
                 placeholder="192.168.1.182" spellCheck="false" />
          <button type="submit">Connect</button>
        </form>
      </header>

      {!ip && <p className="hint">Enter the IP your ESP32 printed in the serial monitor.</p>}

      {t && (
        <div className={`grid ${status === 'live' ? '' : 'stale'}`}>
          <Card title="Temperature" value={<span style={{ color: tColor }}>{t.temp.toFixed(1)} °C</span>} sub={tLabel}>
            <Spark values={history.map((h) => h.temp)} color={tColor} />
          </Card>

          <Card title="Memory" value={t.heap_total ? `${usedPct.toFixed(0)}% used` : `${kb(t.heap)} free`}
                sub={`${t.heap_total ? `${kb(t.heap)} free of ${kb(t.heap_total)} · ` : ''}lowest ${kb(t.heap_min)}`}>
            <div className="bar" role="meter" aria-label="Heap used" aria-valuenow={Math.round(usedPct)}
                 aria-valuemin="0" aria-valuemax="100">
              <div style={{ width: `${usedPct}%` }} />
            </div>
            <Spark values={history.map((h) => h.heap)} color="var(--accent)" />
          </Card>

          <Card title="Wi-Fi signal" value={t.rssi == null ? '—' : `${t.rssi} dBm`} sub={rssiLabel(t.rssi)}>
            <Spark values={history.map((h) => h.rssi)} color="var(--accent)" />
          </Card>

          <Card title="Uptime" value={fmtUptime(t.uptime)} sub="since last boot" />

          <section className="card wide">
            <h2>Controls</h2>
            <div className="controls">
              <button disabled={!live} onClick={() => { setCmdError(''); send({ cmd: 'ping', t: performance.now() }) }}>
                Ping
              </button>
              <span className="sub">{rtt == null ? 'round trip: —' : `round trip: ${rtt} ms`}</span>

              <button disabled={!live || scanning}
                      onClick={() => { setCmdError(''); setScan((s) => ({ ...s, busy: true })); send({ cmd: 'scan' }) }}>
                {scanning ? 'Scanning…' : 'Scan Wi-Fi'}
              </button>

              <label className="led">
                LED
                <input type="color" value={ledColor} disabled={!live}
                       onChange={(e) => { setLedColor(e.target.value); send({ cmd: 'led', rgb: e.target.value }) }} />
              </label>
              <button className="secondary" disabled={!live} onClick={() => send({ cmd: 'led', rgb: null })}>
                LED auto
              </button>
            </div>

            {cmdError && <p className="error" role="alert">Device said: {cmdError}</p>}

            {scan.aps && (
              <table>
                <thead><tr><th>Network</th><th>Signal</th><th>Ch</th><th>Security</th></tr></thead>
                <tbody>
                  {scan.aps.map((ap, i) => (
                    <tr key={i}>
                      <td>{ap.ssid || <em>(hidden)</em>}</td>
                      <td>{ap.rssi} dBm</td>
                      <td>{ap.ch}</td>
                      <td>{ap.open ? 'Open' : 'Secured'}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            )}
          </section>
        </div>
      )}
    </main>
  )
}
