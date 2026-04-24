import { useState, useEffect } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAuth } from '../hooks/useAuth'
import { listLobbies } from '../api/lobbyApi'
import type { LobbyView } from '../types/lobby'
import './LobbyBrowser.css'

type Tab = 'starting' | 'active'

export function LobbyBrowser() {
  const navigate              = useNavigate()
  const { user }              = useAuth()
  const [tab, setTab]         = useState<Tab>('starting')
  const [lobbies, setLobbies]   = useState<LobbyView[]>([])
  const [loading, setLoading]   = useState(true)
  const [error, setError]       = useState<string | null>(null)
  const [joining, setJoining]   = useState<string | null>(null)  // lobby id being joined
  const [creating, setCreating] = useState(false)
  const [codeInput, setCodeInput] = useState('')
  const [joinCodeError, setJoinCodeError] = useState<string | null>(null)

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

  async function joinLobby(lobby: LobbyView) {
    setJoining(lobby.id)
    try {
      const res = await fetch(`/lobbies/${lobby.id}/join`, {
        method: 'POST',
        credentials: 'include',
      })
      if (res.ok) {
        navigate(`/lobby/${lobby.code}`)
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
      const res = await fetch('/lobbies', { method: 'POST', credentials: 'include' })
      if (res.ok) {
        const lobby: LobbyView = await res.json()
        navigate(`/lobby/${lobby.code}`)
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
    const code = codeInput.trim().toUpperCase()
    const match = waitingLobbies.find(l => l.code === code)
    if (!match) { setJoinCodeError('Lobby not found — try refreshing'); return }
    await joinLobby(match)
  }

  const waitingLobbies = lobbies.filter(l => l.status === 'waiting')
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
          <button className="lp__hamburger" aria-label="Menu">☰</button>
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
            <button
              className="lp__btn lp__btn--create lp__tab-create"
              onClick={handleCreate}
              disabled={creating}
              aria-label="Create a new lobby"
            >
              {creating ? '…' : 'Create Lobby'}
            </button>
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
                  const isOwn     = user?.id === lobby.owner_id
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
                {activeLobbies.map(lobby => (
                  <div key={lobby.id} className="lp__card">
                    <div className="lp__card-left">
                      <span className="lp__card-title">Game {lobby.code}</span>
                      <span className="lp__card-meta">
                        <span className="lp__card-meta-icon">♟</span>
                        Standard · In progress
                      </span>
                      <span className="lp__card-players">
                        <span className="lp__card-meta-icon">👥</span>
                        {lobby.player_count} players
                      </span>
                    </div>
                    {/* No join button for in-progress games in Slice 6 */}
                  </div>
                ))}
              </>
            )}
          </div>

        </div>
      </div>
    </div>
  )
}
