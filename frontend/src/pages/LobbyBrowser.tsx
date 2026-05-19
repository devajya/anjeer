import { useState, useEffect, useRef } from 'react'
import { useNavigate, useSearchParams } from 'react-router-dom'
import { useAuth } from '../hooks/useAuth'
import { listLobbies } from '../api/lobbyApi'
import { useQueueSocket } from '../hooks/useQueueSocket'
import { QueuePopup } from '../components/QueuePopup'
import type { LobbyView } from '../types/lobby'
import './LobbyBrowser.css'

type Tab = 'starting' | 'active'

export function LobbyBrowser() {
  const navigate              = useNavigate()
  const [searchParams]        = useSearchParams()
  const { user, logout }      = useAuth()
  const [tab, setTab]         = useState<Tab>('starting')
  const [lobbies, setLobbies]   = useState<LobbyView[]>([])
  const [loading, setLoading]   = useState(true)
  const [error, setError]       = useState<string | null>(null)
  const [joining, setJoining]   = useState<string | null>(null)  // lobby id being joined
  const [creating, setCreating] = useState(false)
  const [codeInput, setCodeInput] = useState('')
  const [joinCodeError, setJoinCodeError] = useState<string | null>(null)
  const [menuOpen, setMenuOpen] = useState(false)
  // AGENT-CTX: menuRef used for click-outside detection — closes the dropdown when
  // the user clicks anywhere outside the hamburger + menu container.
  const menuRef = useRef<HTMLDivElement>(null)
  // AGENT-CTX: Queue state managed by a lightweight WS hook; overflowLobbyIds persists
  // overflow signals per lobby so the Join button stays disabled after a full-queue attempt.
  const queueSocket       = useQueueSocket()
  const [overflowLobbyIds, setOverflowLobbyIds] = useState<Set<string>>(new Set())

  // Auto-queue when redirected from an expired reconnect window.
  // AGENT-CTX: ReconnectOverlay and Game.tsx grace-period both navigate to
  // /lobby?queue_for=<lobbyId>. Switch to Active tab and enqueue immediately.
  useEffect(() => {
    const queueFor = searchParams.get('queue_for')
    if (!queueFor) return
    setTab('active')
    queueSocket.joinQueue(queueFor)
  }, []) // intentional: mount-only, treat URL param as a one-shot instruction

  useEffect(() => {
    if (!menuOpen) return
    function handleOutsideClick(e: MouseEvent) {
      if (menuRef.current && !menuRef.current.contains(e.target as Node)) {
        setMenuOpen(false)
      }
    }
    document.addEventListener('mousedown', handleOutsideClick)
    return () => document.removeEventListener('mousedown', handleOutsideClick)
  }, [menuOpen])

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
    const { state } = queueSocket
    if (state.status === 'overflow') {
      setOverflowLobbyIds(prev => new Set([...prev, state.lobbyId]))
    }
    if (state.status === 'admitted') {
      navigate(`/game/${state.lobbyCode}`, { state: { lobbyId: state.lobbyId } })
      queueSocket.reset()
    }
  }, [queueSocket.state.status])

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
        body:        JSON.stringify({ mode: 'ui', spawn_bots_on_leave: false, bot_spawn_difficulty: 'medium' }),
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

  const userInitial = user?.username?.[0]?.toUpperCase() ?? '?'

  return (
    <div className="lp">
      {/* ── Header ─────────────────────────────────────────────────────── */}
      <header className="lp__header">
        <div className="lp__header-left">
          <div className="lp__avatar" title={user?.username}>{userInitial}</div>
          <span className="lp__trophy" aria-label="Leaderboard">🏆</span>
        </div>
        <div className="lp__header-right">
          <span className="lp__brand">Anjeer</span>
          {/* AGENT-CTX: menuRef wraps both trigger and dropdown so click-outside
              detection (mousedown on document) ignores clicks within this subtree. */}
          <div className="lp__menu-wrap" ref={menuRef}>
            <button
              className="lp__hamburger"
              aria-label="Menu"
              aria-expanded={menuOpen}
              onClick={() => setMenuOpen(o => !o)}
            >
              ☰
            </button>
            {menuOpen && (
              <div className="lp__menu" role="menu">
                <button
                  className="lp__menu-item"
                  role="menuitem"
                  onClick={() => { setMenuOpen(false); navigate('/settings/keybinds') }}
                >
                  Key Bindings
                </button>
                <button
                  className="lp__menu-item"
                  role="menuitem"
                  onClick={() => { setMenuOpen(false); navigate('/api-keys') }}
                >
                  API Keys
                </button>
                <button
                  className="lp__menu-item"
                  role="menuitem"
                  onClick={() => { setMenuOpen(false); navigate('/docs') }}
                >
                  API Docs
                </button>
                <button
                  className="lp__menu-item lp__menu-item--danger"
                  role="menuitem"
                  onClick={() => { setMenuOpen(false); logout() }}
                >
                  Log Out
                </button>
              </div>
            )}
          </div>
        </div>
      </header>

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
                  <p className="lp__empty">No open lobbies — create one in + New</p>
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
                  <p className="lp__empty">No active games right now</p>
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
                            onClick={() => {
                              queueSocket.joinQueue(lobby.id)
                            }}
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

      {/* ── Queue popup overlay ─────────────────────────────────────────── */}
      {queueSocket.state.status === 'queued' && (
        <QueuePopup
          lobbyId={queueSocket.state.lobbyId}
          position={queueSocket.state.position}
          queueSize={queueSocket.state.queueSize}
          playersAround={queueSocket.state.playersAround}
          onLeave={() => queueSocket.leaveQueue()}
          onSpectate={() => {
            const lobbyId = queueSocket.state.status === 'queued' ? queueSocket.state.lobbyId : null
            const lobby   = activeLobbies.find(l => l.id === lobbyId)
            queueSocket.reset()
            if (lobby) navigate(`/spectate/${lobby.code}`)
          }}
        />
      )}
    </div>
  )
}
