import { useState, useEffect } from 'react'
import { useParams, useNavigate } from 'react-router-dom'
import { useWebSocket } from '../hooks/useWebSocket'
import { useAuth } from '../hooks/useAuth'
import { findLobbyByCode } from '../api/lobbyApi'
import './LobbyRoom.css'

export function LobbyRoom() {
  const { code }     = useParams<{ code: string }>()
  const navigate     = useNavigate()
  const { user }     = useAuth()
  const {
    connected,
    lobbyState,
    lobbyStarted,
    subscribeLobby,
    unsubscribeLobby,
  } = useWebSocket('/ws')

  const [lobbyId, setLobbyId]       = useState<string | null>(null)
  const [fetchError, setFetchError] = useState<string | null>(null)
  const [startError, setStartError] = useState<string | null>(null)

  // Step 1: Resolve lobby_id from code via REST — needed for WS subscription.
  useEffect(() => {
    let cancelled = false
    async function resolve() {
      try {
        const found = await findLobbyByCode(code)
        if (!found) { if (!cancelled) setFetchError('Lobby not found'); return }
        if (!cancelled) setLobbyId(found.id)
      } catch {
        if (!cancelled) setFetchError('Network error')
      }
    }
    resolve()
    return () => { cancelled = true }
  }, [code])

  // Step 2: Send subscribe_lobby once lobbyId is known and WS is open.
  useEffect(() => {
    if (!lobbyId || !connected) return
    subscribeLobby(lobbyId)
    return () => { unsubscribeLobby(lobbyId) }
  }, [lobbyId, connected, subscribeLobby, unsubscribeLobby])

  // Step 3: Navigate to game when lobby_started arrives for THIS lobby.
  useEffect(() => {
    if (lobbyStarted && lobbyId && lobbyStarted.lobby_id === lobbyId) {
      navigate(`/game?lobby_id=${lobbyStarted.lobby_id}`)
    }
  }, [lobbyStarted, lobbyId, navigate])

  async function handleStart() {
    if (!lobbyId) return
    setStartError(null)
    try {
      const res = await fetch(`/lobbies/${lobbyId}/start`, {
        method: 'POST',
        credentials: 'include',
      })
      if (!res.ok) {
        const body = await res.json().catch(() => ({})) as { error?: string }
        setStartError(body.error ?? 'Failed to start game')
      }
      // On success the server publishes lobby_started → useWebSocket updates
      // lobbyStarted → the useEffect above fires navigate('/game?lobby_id=...')
    } catch {
      setStartError('Network error')
    }
  }

  const players  = lobbyState?.players ?? []
  const isOwner  = user != null && lobbyState != null && user.id === lobbyState.owner_id
  const canStart = isOwner && players.length >= (lobbyState?.min_players ?? 2)

  if (fetchError) {
    return (
      <div className="lr__error-page" role="alert">{fetchError}</div>
    )
  }

  return (
    <div className="lr">
      {/* ── Header ─────────────────────────────────────────────────────── */}
      <header className="lr__header">
        <button className="lr__back" onClick={() => navigate('/lobby')}>
          ← Lobbies
        </button>
        <span className="lr__brand">Anjeer</span>
        <div className="lr__header-right" />
      </header>

      {/* ── Body ───────────────────────────────────────────────────────── */}
      <div className="lr__body">
        <div className="lr__panel">

          {/* ── Title ──────────────────────────────────────────────────── */}
          <div>
            <div className="lr__title-row">
              <h1 className="lr__title">Game Lobby</h1>
              {code && <span className="lr__code">{code}</span>}
            </div>
            {lobbyState && (
              <p className="lr__status-meta">
                {players.length} / {lobbyState.max_players} players · {lobbyState.status}
              </p>
            )}
          </div>

          {/* ── Player list ────────────────────────────────────────────── */}
          <div>
            <p className="lr__section-label">Players</p>
            {players.length === 0 ? (
              <p className="lr__waiting">Waiting for players to join…</p>
            ) : (
              <ul className="lr__players">
                {players.map(p => (
                  <li key={p.player_id} className="lr__player">
                    <div className="lr__player-avatar">
                      {p.username[0]?.toUpperCase() ?? '?'}
                    </div>
                    <span className="lr__player-name">{p.username}</span>
                    {lobbyState && p.player_id === lobbyState.owner_id && (
                      <span className="lr__owner-badge">host</span>
                    )}
                  </li>
                ))}
              </ul>
            )}
          </div>

          {/* ── Owner actions ──────────────────────────────────────────── */}
          {isOwner && (
            <div className="lr__actions">
              <button
                className="lr__start-btn"
                onClick={handleStart}
                disabled={!canStart}
              >
                Start Game
              </button>
              {!canStart && lobbyState && players.length < lobbyState.min_players && (
                <p className="lr__hint">
                  Need {lobbyState.min_players - players.length} more player(s)
                </p>
              )}
              {startError && (
                <p className="lr__alert" role="alert">{startError}</p>
              )}
            </div>
          )}

        </div>
      </div>
    </div>
  )
}
