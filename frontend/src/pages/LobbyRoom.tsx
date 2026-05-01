import { useState, useEffect, useRef } from 'react'
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
    sendMessage,
  } = useWebSocket('/ws')

  const [lobbyId, setLobbyId]       = useState<string | null>(null)
  const [fetchError, setFetchError] = useState<string | null>(null)
  const [startError, setStartError] = useState<string | null>(null)

  // AGENT-CTX: Refs let the unmount cleanup read current lobbyId / connected
  // without adding them as deps (which would re-run the cleanup on every
  // connection blip and falsely send leave_lobby). sendMessage is stable
  // (empty-dep useCallback in useWebSocket) so it's the only dep needed.
  const lobbyIdRef   = useRef<string | null>(null)
  const connectedRef = useRef(false)
  // AGENT-CTX: Two flags prevent double-sending leave_lobby:
  // navigatedToGame — skips cleanup when the player transitions to /game
  // (they haven't left; they've joined the session).
  // leaveSent — prevents the unmount cleanup from re-sending when handleBack
  // already sent it synchronously before calling navigate().
  const navigatedToGameRef = useRef(false)
  const leaveSentRef       = useRef(false)

  useEffect(() => { lobbyIdRef.current = lobbyId }, [lobbyId])
  useEffect(() => { connectedRef.current = connected }, [connected])

  // Unmount cleanup: send leave_lobby for any navigation except navigate-to-game.
  // AGENT-CTX: Covers browser back/forward and direct URL changes where
  // handleBack() is never called. React runs LobbyRoom effect cleanups before
  // useWebSocket's internal cleanup closes the socket, so sendMessage is safe here.
  useEffect(() => {
    return () => {
      if (
        !navigatedToGameRef.current &&
        !leaveSentRef.current &&
        lobbyIdRef.current &&
        connectedRef.current
      ) {
        leaveSentRef.current = true
        sendMessage({ type: 'leave_lobby', lobby_id: lobbyIdRef.current })
      }
    }
  }, [sendMessage])

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
      navigatedToGameRef.current = true
      navigate(`/game?lobby_id=${lobbyStarted.lobby_id}`)
    }
  }, [lobbyStarted, lobbyId, navigate])

  function handleBack() {
    // AGENT-CTX: Send leave_lobby before navigate() so the message goes out
    // while the socket is still guaranteed open. The unmount cleanup will skip
    // it (leaveSentRef guard) to avoid a duplicate send.
    if (lobbyId && connected && !leaveSentRef.current) {
      leaveSentRef.current = true
      sendMessage({ type: 'leave_lobby', lobby_id: lobbyId })
    }
    navigate('/lobby')
  }

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
  const isOwner  = user != null && lobbyState != null && user.id === lobbyState.creator_id
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
        <button className="lr__back" onClick={handleBack}>
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
                    {lobbyState && p.player_id === lobbyState.creator_id && (
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
