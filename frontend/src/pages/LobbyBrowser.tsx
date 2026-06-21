import { useState, useEffect, memo } from 'react'
import { useNavigate, useSearchParams, useLocation } from 'react-router-dom'
import { useAuth } from '../hooks/useAuth'
import { listLobbies } from '../api/lobbyApi'
import { useWebSocket } from '../hooks/useWebSocket'
import { QueuePopup } from '../components/QueuePopup'
import { Footer } from '../components/Footer'
import type { LobbyView } from '../types/lobby'
import './LobbyBrowser.css'

// Bid sizes (% of half-width) per level — best bid/ask at top, tapering down.
const OB_LEVELS = [72, 57, 41, 26, 13]

const ObWatermark = memo(function ObWatermark() {
  return (
    <div className="lp__ob-watermark" aria-hidden="true">
      {OB_LEVELS.map((w, i) => (
        <div key={i} className="lp__ob-row">
          <div className="lp__ob-side lp__ob-side--bid">
            <div className="lp__ob-bar lp__ob-bar--bid" style={{ width: `${w}%` }} />
          </div>
          <div className="lp__ob-mid" />
          <div className="lp__ob-side lp__ob-side--ask">
            <div className="lp__ob-bar lp__ob-bar--ask" style={{ width: `${w}%` }} />
          </div>
        </div>
      ))}
    </div>
  )
})

type Tab = 'starting' | 'active'

export function LobbyBrowser() {
  const navigate              = useNavigate()
  const location              = useLocation()
  const [searchParams]        = useSearchParams()
  const { user }              = useAuth()
  const [tab, setTab]         = useState<Tab>('starting')
  const [lobbies, setLobbies]   = useState<LobbyView[]>([])
  const [loading, setLoading]   = useState(true)
  const [error, setError]       = useState<string | null>(null)
  const [joining, setJoining]   = useState<string | null>(null)  // lobby id being joined
  const [creating, setCreating] = useState(false)
  const [codeInput, setCodeInput] = useState('')
  const [joinCodeError, setJoinCodeError] = useState<string | null>(null)
  const { queueState, joinQueue, leaveQueue, resetQueue } = useWebSocket('/ws')
  const [overflowLobbyIds, setOverflowLobbyIds] = useState<Set<string>>(new Set())

  // Auto-queue when redirected from an expired reconnect window.
  // AGENT-CTX: ReconnectOverlay and Game.tsx grace-period both navigate to
  // /lobby?queue_for=<lobbyId>. Switch to Active tab and enqueue immediately.
  useEffect(() => {
    const queueFor = searchParams.get('queue_for')
    if (!queueFor) return
    setTab('active')
    joinQueue(queueFor)
  }, []) // intentional: mount-only, treat URL param as a one-shot instruction

  async function loadLobbies() {
    setError(null)
    try {
      setLobbies(await listLobbies())
    } catch {
      setError('Network error')
    } finally {
      setLoading(false)
    }
  }

  useEffect(() => { loadLobbies() }, [])

  useEffect(() => {
    if (queueState.status === 'overflow') {
      setOverflowLobbyIds(prev => new Set([...prev, queueState.lobbyId]))
    }
    if (queueState.status === 'admitted') {
      const leftGame = (location.state as { leftGame?: string } | null)?.leftGame
      if (leftGame === queueState.lobbyId) { resetQueue(); return }
      navigate(`/game?lobby_id=${queueState.lobbyId}`)
      resetQueue()
    }
  }, [queueState.status])

  async function joinLobby(lobby: LobbyView) {
    setJoining(lobby.id)
    try {
      const res = await fetch(`/lobbies/${lobby.id}/join`, {
        method: 'POST',
        credentials: 'include',
      })
      if (res.ok) {
        navigate(`/lobby/${lobby.code}`, { state: { lobbyId: lobby.id } })
      } else {
        const body = await res.json().catch(() => ({})) as { error?: string }
        setError(body.error ?? 'Failed to join lobby')
      }
    } catch {
      setError('Network error')
    } finally {
      setJoining(null)
    }
  }

  async function handleCreate() {
    setCreating(true)
    setError(null)
    try {
      const res = await fetch('/lobbies', {
        method:      'POST',
        credentials: 'include',
        headers:     { 'Content-Type': 'application/json' },
        // spawn_bots_on_leave defaults to false; owner toggles it in LobbyRoom.
        // bot_spawn_difficulty always medium — difficulty picker removed from creation.
        body:        JSON.stringify({ mode: 'ui', spawn_bots_on_leave: false, bot_spawn_difficulty: 'medium', game_mode: 'simple' }),
      })
      if (res.ok) {
        const lobby: LobbyView = await res.json()
        navigate(`/lobby/${lobby.code}`, { state: { lobbyId: lobby.id } })
      } else {
        setError('Failed to create lobby')
      }
    } finally {
      setCreating(false)
    }
  }

  async function handleJoinByCode(e: React.FormEvent) {
    e.preventDefault()
    setJoinCodeError(null)
    const trimmed = codeInput.trim().toUpperCase()
    const match = waitingLobbies.find(l => l.code === trimmed)
    if (!match) { setJoinCodeError('Lobby not found — try refreshing'); return }
    await joinLobby(match)
  }

  // Starting tab shows only UI-mode lobbies; API lobbies are CLI-only and
  // appear only in the Active tab (for spectating) once the game is live.
  const waitingLobbies = lobbies.filter(l => l.status === 'waiting' && l.mode !== 'api')
  const activeLobbies  = lobbies.filter(l => l.status === 'in_game')

  return (
    <div className="lp">
      {/* ── Body ───────────────────────────────────────────────────────── */}
      <div className="lp__body">
        <div className="lp__panel">

          {/* ── Tabs ─────────────────────────────────────────────────── */}
          <div className="lp__tabs" role="tablist">
            <button
              role="tab"
              aria-selected={tab === 'starting'}
              className={`lp__tab${tab === 'starting' ? ' lp__tab--active' : ''}`}
              onClick={() => setTab('starting')}
            >
              Starting {!loading && `(${waitingLobbies.length})`}
            </button>
            <button
              role="tab"
              aria-selected={tab === 'active'}
              className={`lp__tab${tab === 'active' ? ' lp__tab--active' : ''}`}
              onClick={() => setTab('active')}
            >
              Active {!loading && `(${activeLobbies.length})`}
            </button>
            <div className="lp__create-area">
              <button
                className="lp__btn lp__btn--create"
                onClick={handleCreate}
                disabled={creating}
                aria-label="Create a new lobby"
              >
                {creating ? '…' : 'Create Lobby'}
              </button>
            </div>
          </div>

          {/* ── Content ──────────────────────────────────────────────── */}
          <div className="lp__content">
            {error && <p className="lp__alert" role="alert">{error}</p>}

            {/* ── Tab: Starting ──────────────────────────────────────── */}
            {tab === 'starting' && (
              <>
                {loading && <p className="lp__empty">Loading…</p>}
                {!loading && waitingLobbies.length === 0 && (
                  <div className="lp__empty-rich">
                    <ObWatermark />
                    <span className="lp__empty-rich-headline">No games open</span>
                    <p className="lp__empty-rich-desc">
                      Create one and invite friends, or enter a code below.
                    </p>
                    <button
                      className="lp__btn lp__btn--create lp__empty-rich-cta"
                      onClick={handleCreate}
                      disabled={creating}
                    >
                      {creating ? '…' : 'Create Lobby'}
                    </button>
                  </div>
                )}
                {/* Join-by-code strip — kept at end so the list is the focus */}
                {!loading && (
                  <form className="lp__join-strip" onSubmit={handleJoinByCode}>
                    <input
                      className="lp__join-input"
                      type="text"
                      placeholder="Enter lobby code"
                      aria-label="Lobby code"
                      value={codeInput}
                      onChange={e => { setCodeInput(e.target.value); setJoinCodeError(null) }}
                      maxLength={6}
                    />
                    <button type="submit" className="lp__btn lp__btn--join">
                      Join by Code
                    </button>
                  </form>
                )}
                {joinCodeError && (
                  <p className="lp__alert" role="alert">{joinCodeError}</p>
                )}

                {waitingLobbies.map(lobby => {
                  const isOwn     = user?.id === lobby.creator_id
                  const isJoining = joining === lobby.id
                  return (
                    <div key={lobby.id} className="lp__card">
                      <div className="lp__card-left">
                        {/* AGENT-CTX: code is in its own element so getByText(code)
                            works in tests. LobbyView has no owner_username yet. */}
                        <div className="lp__card-title-row">
                          <span className="lp__card-title">Game</span>
                          {/* code is in its own element so getByText(code) works in tests */}
                          <span className="lp__card-code">{lobby.code}</span>
                          <span className={`lp__mode-badge lp__mode-badge--${lobby.mode ?? 'ui'}`}>
                            {(lobby.mode ?? 'ui').toUpperCase()}
                          </span>
                          <span className={`lp__game-mode-badge lp__game-mode-badge--${lobby.game_mode ?? 'simple'}`}>
                            {lobby.game_mode ? lobby.game_mode.charAt(0).toUpperCase() + lobby.game_mode.slice(1) : 'Simple'}
                          </span>
                        </div>
                        <span className="lp__card-meta">
                          <span className="lp__card-meta-icon">♟</span>
                          Standard
                        </span>
                        <span className="lp__card-players">
                          <span className="lp__card-meta-icon">👥</span>
                          {lobby.player_count} / {lobby.max_players} players
                          {isOwn && (
                            <span className="lp__card-username">&nbsp;· your lobby</span>
                          )}
                        </span>
                      </div>
                      <div className="lp__card-right">
                        <button
                          className="lp__btn lp__btn--join"
                          onClick={() => joinLobby(lobby)}
                          disabled={isOwn || isJoining}
                          aria-label={`Join lobby ${lobby.code}`}
                        >
                          {isJoining ? '…' : isOwn ? 'Yours' : '＋ Join'}
                        </button>
                      </div>
                    </div>
                  )
                })}
              </>
            )}

            {/* ── Tab: Active ────────────────────────────────────────── */}
            {tab === 'active' && (
              <>
                {loading && <p className="lp__empty">Loading…</p>}
                {!loading && activeLobbies.length === 0 && (
                  <div className="lp__empty-rich">
                    <span className="lp__empty-rich-headline">No active games</span>
                    <p className="lp__empty-rich-desc">
                      Games in progress appear here for spectating or joining the queue.
                    </p>
                  </div>
                )}
                {activeLobbies.map(lobby => {
                  const isFull = overflowLobbyIds.has(lobby.id)
                  return (
                    <div key={lobby.id} className="lp__card">
                      <div className="lp__card-left">
                        <div className="lp__card-title-row">
                          <span className="lp__card-title">Game {lobby.code}</span>
                          <span className={`lp__mode-badge lp__mode-badge--${lobby.mode ?? 'ui'}`}>
                            {(lobby.mode ?? 'ui').toUpperCase()}
                          </span>
                          <span className={`lp__game-mode-badge lp__game-mode-badge--${lobby.game_mode ?? 'simple'}`}>
                            {lobby.game_mode ? lobby.game_mode.charAt(0).toUpperCase() + lobby.game_mode.slice(1) : 'Simple'}
                          </span>
                        </div>
                        <span className="lp__card-meta">
                          <span className="lp__card-meta-icon">♟</span>
                          Standard · In progress
                        </span>
                        <span className="lp__card-players">
                          <span className="lp__card-meta-icon">👥</span>
                          {lobby.player_count} players
                        </span>
                      </div>
                      <div className="lp__card-actions">
                        <button
                          className="lp__btn lp__btn--spectate"
                          onClick={() => navigate(`/spectate/${lobby.code}`)}
                          aria-label={`Spectate lobby ${lobby.code}`}
                        >
                          Spectate
                        </button>
                        {/* AGENT-CTX: Only UI-mode active lobbies show Join; API-mode lobbies
                            are accessed via CLI. isFull is set reactively on queue_overflow. */}
                        {lobby.mode !== 'api' && (
                          <button
                            className="lp__btn lp__btn--join"
                            disabled={isFull}
                            onClick={() => joinQueue(lobby.id)}
                            aria-label={isFull ? 'Queue full' : `Join lobby ${lobby.code}`}
                          >
                            {isFull ? 'Queue Full' : 'Join'}
                          </button>
                        )}
                      </div>
                    </div>
                  )
                })}
              </>
            )}
          </div>

        </div>
      </div>

      <Footer />

      {/* ── Queue popup overlay ─────────────────────────────────────────── */}
      {queueState.status === 'queued' && (
        <QueuePopup
          lobbyId={queueState.lobbyId}
          position={queueState.position}
          queueSize={queueState.queueSize}
          playersAround={queueState.playersAround}
          onLeave={() => leaveQueue(queueState.lobbyId)}
          onSpectate={() => {
            const lobbyId = queueState.status === 'queued' ? queueState.lobbyId : null
            const lobby   = activeLobbies.find(l => l.id === lobbyId)
            resetQueue()
            if (lobby) navigate(`/spectate/${lobby.code}`)
          }}
        />
      )}
    </div>
  )
}
