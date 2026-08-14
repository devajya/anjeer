import { describe, it, vi } from 'vitest'
import { render, act } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import { Profiler, type ProfilerOnRenderCallback } from 'react'
import { writeFileSync } from 'node:fs'
import { Game } from '../pages/Game'

// ── Environment mocks (mirror the existing GamePage tests) ────────────────────

vi.mock('../hooks/useAuth', () => ({
  useAuth: () => ({ user: { id: 4821, username: 'you' }, loading: false, logout: vi.fn() }),
}))
vi.mock('../hooks/useKeyBinds', () => ({
  useKeyBinds: () => ({ binds: {}, loading: false }),
}))
vi.mock('../hooks/useKeyboardShortcuts', () => ({
  useKeyboardShortcuts: () => {},
}))

vi.stubGlobal('fetch', vi.fn(() => Promise.resolve({ ok: false, json: () => Promise.resolve(null) })))

const lsStore: Record<string, string> = {}
vi.stubGlobal('localStorage', {
  getItem: (k: string) => lsStore[k] ?? null,
  setItem: (k: string, v: string) => { lsStore[k] = v },
  removeItem: (k: string) => { delete lsStore[k] },
  clear: () => { Object.keys(lsStore).forEach(k => delete lsStore[k]) },
})

// ── Controllable fake WebSocket ───────────────────────────────────────────────
// The real useWebSocket hook opens `new WebSocket(url, 'anjeer-msgpack')`; we
// capture the instance so the harness can push server messages through the
// hook's real onmessage path — same protocol on both branches.

const sockets: FakeWS[] = []
class FakeWS {
  onopen: ((ev: Event) => void) | null = null
  onmessage: ((ev: MessageEvent) => void) | null = null
  onclose: ((ev: CloseEvent) => void) | null = null
  onerror: ((ev: Event) => void) | null = null
  binaryType = 'blob'
  readyState = 0
  constructor(public url: string, public protocol?: string) {
    sockets.push(this)
    setTimeout(() => { this.readyState = 1; this.onopen?.(new Event('open')) }, 0)
  }
  send() {}
  close() { this.readyState = 3; this.onclose?.(new CloseEvent('close', { code: 1000, wasClean: true })) }
  emit(msg: unknown) { this.onmessage?.(new MessageEvent('message', { data: JSON.stringify(msg) })) }
}
vi.stubGlobal('WebSocket', FakeWS as unknown as typeof WebSocket)

// ── Fixtures (inlined so the harness is identical across branches) ─────────────

const SUITS = ['clubs', 'diamonds', 'hearts', 'spades'] as const
// suit, best_bid, best_ask, bid_slot, ask_slot
const BOOKS: Array<[string, number, number, number, number]> = [
  ['clubs', 7, 9, 2, 1],
  ['diamonds', 11, 13, 0, 3],
  ['hearts', 6, 8, 3, 2],
  ['spades', 9, 12, 1, 0],
]
const ROSTER = [
  { player_slot: 0, username: 'you' },
  { player_slot: 1, username: 'orwell' },
  { player_slot: 2, username: 'kepler' },
  { player_slot: 3, username: 'ada' },
]

// ── Profiler accumulator ──────────────────────────────────────────────────────

interface Acc { commits: number; actual: number; base: number; maxActual: number }
let acc: Acc = { commits: 0, actual: 0, base: 0, maxActual: 0 }
const reset = () => { acc = { commits: 0, actual: 0, base: 0, maxActual: 0 } }
const onRender: ProfilerOnRenderCallback = (_id, _phase, actualDuration, baseDuration) => {
  acc.commits++
  acc.actual += actualDuration
  acc.base += baseDuration
  acc.maxActual = Math.max(acc.maxActual, actualDuration)
}

// ── The measurement ───────────────────────────────────────────────────────────

const N = Number(process.env.PERF_N ?? 1500)   // MBO messages in the measured burst
const OUT = process.env.PERF_OUT                 // optional JSON output path

describe('render profile: MBO burst through the real Game tree', () => {
  it(`measures ${N} incremental market-data messages`, async () => {
    render(
      <MemoryRouter initialEntries={['/game?lobby_id=ABCDEF']}>
        <Profiler id="game" onRender={onRender}>
          <Game />
        </Profiler>
      </MemoryRouter>,
    )

    // Let the hook open its socket and fire onopen.
    await act(async () => { await new Promise(r => setTimeout(r, 0)) })
    const ws = sockets[sockets.length - 1]
    if (!ws) throw new Error('no socket opened')
    const emit = (msg: unknown) => act(() => ws.emit(msg))

    // ── Bootstrap: enter a live "advanced" round (renders MboFeedPanel) ──────
    emit({ type: 'player_hello', player_id: 4821 })
    emit({
      type: 'round_start', player_slot: 0,
      hand: { clubs: 2, diamonds: 4, hearts: 3, spades: 1 },
      round_end_at: new Date(Date.now() + 4 * 60 * 1000).toISOString(),
      balance: 312, roster: ROSTER,
      all_hand_totals: [10, 10, 10, 10], all_balances: [312, 288, 341, 259],
      game_mode: 'advanced',
    })
    for (const [suit, bid, ask, bs, as] of BOOKS) {
      emit({ type: 'book_update', v: 1, seq: 1, suit, best_bid: bid, best_ask: ask, best_bid_slot: bs, best_ask_slot: as })
    }

    // Seed resting orders so the synthetic book has depth to maintain/replay.
    let seq = 400
    let orderId = 8100
    const live: Array<{ id: number; suit: string; side: 'buy' | 'sell'; price: number }> = []
    for (const [suit, bid, ask] of BOOKS) {
      for (const [side, price] of [['buy', bid], ['buy', bid - 1], ['sell', ask], ['sell', ask + 1]] as const) {
        const id = orderId++
        emit({ type: 'order_added', v: 1, seq: seq++, order_id: id, suit, side, price, owner_slot: 1, qty: 3 })
        live.push({ id, suit, side, price })
      }
    }

    // ── Measured burst ──────────────────────────────────────────────────────
    // Realistic churn: add a new order, then execute or cancel an existing one,
    // cycling across suits so every message mutates the book.
    reset()
    for (let i = 0; i < N; i++) {
      const [suit, bid, ask] = BOOKS[i % BOOKS.length]
      if (i % 3 === 0) {
        const id = orderId++
        const side = i % 2 === 0 ? 'buy' : 'sell'
        const price = side === 'buy' ? bid - (i % 3) : ask + (i % 3)
        emit({ type: 'order_added', v: 1, seq: seq++, order_id: id, suit, side, price, owner_slot: (i % 3) + 1, qty: 2 + (i % 4) })
        live.push({ id, suit, side, price })
      } else if (i % 3 === 1 && live.length > 8) {
        const victim = live.shift()!
        emit({ type: 'order_executed', v: 1, seq: seq++, order_id: victim.id, suit: victim.suit, price: victim.price, qty_filled: 1, aggressor_order_id: orderId++, buyer_slot: 0, seller_slot: 1 })
      } else if (live.length > 8) {
        const victim = live.shift()!
        emit({ type: 'order_cancelled', v: 1, seq: seq++, order_id: victim.id, suit: victim.suit })
      }
    }

    const result = {
      messages: N,
      commits: acc.commits,
      sumActualMs: +acc.actual.toFixed(2),
      sumBaseMs: +acc.base.toFixed(2),
      avgActualMs: +(acc.actual / N).toFixed(4),
      maxActualMs: +acc.maxActual.toFixed(3),
      memoRatioActualOverBase: +(acc.actual / acc.base).toFixed(3),
    }
    // eslint-disable-next-line no-console
    console.log('PERF_RESULT ' + JSON.stringify(result))
    if (OUT) writeFileSync(OUT, JSON.stringify(result, null, 2))
  }, 120_000)
})
