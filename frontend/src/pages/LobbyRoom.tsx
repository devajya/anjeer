import { useState, useEffect, useRef } from 'react'
import { useParams, useNavigate, useLocation } from 'react-router-dom'
import { useWebSocket } from '../hooks/useWebSocket'
import { useAuth } from '../hooks/useAuth'
import { findLobbyByCode } from '../api/lobbyApi'
import { StaleLobbyModal } from '../components/StaleLobbyModal'
import type { LobbyPlayer } from '../types/messages'
import './LobbyRoom.css'

// ─── Seat geometry ────────────────────────────────────────────────────────────

interface SeatPos {
  xPct: number
  yPct: number
  isCreatorSlot: boolean
}

// Creator always at 90° (top-center). Others fan out symmetrically within ±SPAN°.
function computeSeatPositions(n: number): SeatPos[] {
  if (n === 0) return []
  if (n === 1) return [{ xPct: 50, yPct: 8, isCreatorSlot: true }]

  const CX = 50, CY = 88, RX = 42, RY = 80, SPAN = 76

  const others = n - 1
  const rightN = Math.floor(others / 2)
  const leftN  = others - rightN
  const pos: SeatPos[] = []

  // Right side (angles < 90° → visual right)
  for (let i = rightN; i >= 1; i--) {
    const a = ((90 - (i / rightN) * SPAN) * Math.PI) / 180
    pos.push({ xPct: CX + RX * Math.cos(a), yPct: CY - RY * Math.sin(a), isCreatorSlot: false })
  }

  // Creator slot
  pos.push({ xPct: CX, yPct: CY - RY, isCreatorSlot: true })

  // Left side (angles > 90° → visual left)
  for (let i = 1; i <= leftN; i++) {
    const a = ((90 + (i / leftN) * SPAN) * Math.PI) / 180
    pos.push({ xPct: CX + RX * Math.cos(a), yPct: CY - RY * Math.sin(a), isCreatorSlot: false })
  }

  return pos
}

// ─── Seat component ───────────────────────────────────────────────────────────

const DIFFS = ['easy', 'medium', 'hard'] as const
const DIFF_LABEL: Record<typeof DIFFS[number], string> = { easy: 'E', medium: 'M', hard: 'H' }

function BotIcon() {
  return (
    <svg viewBox="0 0 20 20" fill="none" xmlns="http://www.w3.org/2000/svg" className="seat__bot-icon">
      <path d="M10 5V3" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round"/>
      <circle cx="10" cy="2.5" r="1" fill="currentColor"/>
      <rect x="3" y="5" width="14" height="11" rx="2.5" stroke="currentColor" strokeWidth="1.5"/>
      <circle cx="7.5" cy="10" r="1.5" fill="currentColor"/>
      <circle cx="12.5" cy="10" r="1.5" fill="currentColor"/>
      <path d="M7.5 13h5" stroke="currentColor" strokeWidth="1.3" strokeLinecap="round"/>
    </svg>
  )
}

interface SeatProps {
  pos:             SeatPos
  player:          LobbyPlayer | null
  isOwner:         boolean
  isMe:            boolean
  isCreator:       boolean
  isPending:       boolean
  canAddBot:       boolean
  onClickEmpty:    () => void
  onDiffSelect:    (d: typeof DIFFS[number]) => void
  onRemoveBot:     (uuid: string) => void
  onChangeDiff:    (uuid: string, to: typeof DIFFS[number]) => void
  onCancelPending: () => void
}

function Seat({
  pos, player, isOwner, isMe, isCreator, isPending, canAddBot,
  onClickEmpty, onDiffSelect, onRemoveBot, onChangeDiff, onCancelPending,
}: SeatProps) {
  const style = { left: `${pos.xPct}%`, top: `${pos.yPct}%` } as const

  if (player === null) {
    return (
      <div
        className={`seat seat--empty${isOwner && canAddBot ? ' seat--clickable' : ''}${isPending ? ' seat--pending' : ''}`}
        style={style}
        role={isOwner && canAddBot && !isPending ? 'button' : undefined}
        tabIndex={isOwner && canAddBot && !isPending ? 0 : undefined}
        onClick={isOwner && canAddBot && !isPending ? onClickEmpty : undefined}
        aria-label={isOwner && canAddBot ? 'Add bot to seat' : 'Empty seat'}
      >
        {isPending ? (
          <>
            <p className="seat__pick-label">Add bot</p>
            <div className="seat__diff-picker">
              {DIFFS.map(d => (
                <button key={d} className="seat__diff-opt" aria-label={d} onClick={() => onDiffSelect(d)}>
                  {DIFF_LABEL[d]}
                </button>
              ))}
            </div>
            {/* Bot fills the next open seat (rightmost), not the clicked seat — keeps mouse travel minimal */}
            <button className="seat__cancel" onClick={(e) => { e.stopPropagation(); onCancelPending() }}>
              ×
            </button>
          </>
        ) : (
          <div className="seat__add">{isOwner && canAddBot ? '+' : '○'}</div>
        )}
      </div>
    )
  }

  const diff = player.bot_difficulty

  return (
    <div
      className={`seat${player.is_bot ? ' seat--bot' : ' seat--player'}${isCreator ? ' seat--creator' : ''}${isMe ? ' seat--me' : ''}`}
      style={style}
    >
      <div className="seat__avatar">
        {player.is_bot ? <BotIcon /> : (player.username?.[0]?.toUpperCase() ?? '?')}
      </div>
      <div className="seat__name" title={player.username}>
        {player.username}
      </div>

      {/* Per-seat difficulty toggle — only on bot seats */}
      {player.is_bot && diff && (
        <div className="seat__diff-toggle">
          {DIFFS.map(d => (
            <button
              key={d}
              className={`seat__diff-seg${diff === d ? ' seat__diff-seg--active' : ''}`}
              onClick={() => {
                if (isOwner && diff !== d && player.bot_uuid)
                  onChangeDiff(player.bot_uuid, d)
              }}
              disabled={!isOwner || diff === d}
              aria-label={d}
            >
              {DIFF_LABEL[d]}
            </button>
          ))}
        </div>
      )}

      {player.is_bot && isOwner && player.bot_uuid && (
        <button
          className="seat__remove"
          onClick={() => onRemoveBot(player.bot_uuid!)}
          aria-label="Remove bot"
        >
          ×
        </button>
      )}
    </div>
  )
}

// ─── LobbyRoom page ───────────────────────────────────────────────────────────

export function LobbyRoom() {
  const { code }     = useParams<{ code: string }>()
  const navigate     = useNavigate()
  const location     = useLocation()
  const { user }     = useAuth()
  const {
    connected,
    lobbyState,
    lobbyStarted,
    subscribeLobby,
    unsubscribeLobby,
    sendMessage,
  } = useWebSocket('/ws')

  // AGENT-CTX: Seed lobbyId from navigation state (passed by LobbyBrowser on
  // create/join) so the unmount leave_lobby cleanup works even if the user
  // navigates back before findLobbyByCode's REST call resolves.
  const navLobbyId = (location.state as { lobbyId?: string } | null)?.lobbyId ?? null
  const [lobbyId, setLobbyId]               = useState<string | null>(navLobbyId)
  const [staleLobby, setStaleLobby]         = useState(false)
  const [fetchError, setFetchError]         = useState<string | null>(null)
  const [startError, setStartError]         = useState<string | null>(null)
  const [pendingSeatIdx, setPendingSeatIdx] = useState<number | null>(null)
  const [botAutofill, setBotAutofill]       = useState(false)
  const autofillSyncedRef                   = useRef(false)

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

  useEffect(() => {
    let cancelled = false
    async function resolve() {
      try {
        const found = await findLobbyByCode(code)
        // AGENT-CTX: 'finished' and 'closed' are the terminal statuses in LobbyView.
        // Both map to "stale" from the player's perspective — the game is over.
        if (!found || found.status === 'finished' || found.status === 'closed') {
          if (!cancelled) setStaleLobby(true)
          return
        }
        if (!cancelled) setLobbyId(found.id)
      } catch {
        if (!cancelled) setFetchError('Network error')
      }
    }
    resolve()
    return () => { cancelled = true }
  }, [code])

  useEffect(() => {
    if (!lobbyId || !connected) return
    subscribeLobby(lobbyId)
    return () => { unsubscribeLobby(lobbyId) }
  }, [lobbyId, connected, subscribeLobby, unsubscribeLobby])

  // Sync botAutofill from the first lobby_state snapshot we receive.
  // After that, local state is authoritative (toggle calls PATCH).
  useEffect(() => {
    if (!autofillSyncedRef.current && lobbyState) {
      setBotAutofill(lobbyState.spawn_bots_on_leave)
      autofillSyncedRef.current = true
    }
  }, [lobbyState])

  useEffect(() => {
    if (lobbyStarted && lobbyId && lobbyStarted.lobby_id === lobbyId) {
      navigatedToGameRef.current = true
      if (lobbyState?.mode === 'api') {
        navigate(`/spectate/${lobbyStarted.code}`)
      } else {
        navigate(`/game?lobby_id=${lobbyStarted.lobby_id}`)
      }
    }
  }, [lobbyStarted, lobbyId, lobbyState, navigate])

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
    } catch {
      setStartError('Network error')
    }
  }

  function handleAddBot(difficulty: typeof DIFFS[number]) {
    if (!lobbyId) return
    sendMessage({ type: 'add_bot', lobby_id: lobbyId, difficulty })
    setPendingSeatIdx(null)
  }

  function handleRemoveBot(botUuid: string) {
    if (!lobbyId) return
    sendMessage({ type: 'remove_bot', lobby_id: lobbyId, bot_uuid: botUuid })
  }

  function handleChangeDiff(uuid: string, to: typeof DIFFS[number]) {
    if (!lobbyId) return
    sendMessage({ type: 'remove_bot', lobby_id: lobbyId, bot_uuid: uuid })
    sendMessage({ type: 'add_bot',    lobby_id: lobbyId, difficulty: to })
  }

  async function handleToggleAutofill() {
    if (!isOwner || !lobbyId) return
    const next = !botAutofill
    setBotAutofill(next)
    try {
      const res = await fetch(`/lobbies/${lobbyId}/bot-settings`, {
        method:      'PATCH',
        credentials: 'include',
        headers:     { 'Content-Type': 'application/json' },
        body:        JSON.stringify({ spawn_bots_on_leave: next }),
      })
      if (!res.ok) setBotAutofill(!next)   // revert on server error
    } catch {
      setBotAutofill(!next)
    }
  }

  const players  = lobbyState?.players ?? []
  const maxSlots = lobbyState?.max_players ?? 5
  const isOwner  = user != null && lobbyState != null && user.id === lobbyState.creator_id
  // AGENT-CTX: Start requires 2+ total players (real+bots). Server enforces
  // only that ≥1 real player is present; the combined count is enforced here.
  // Teardown uses real_player_count_==0 (server).
  const canStart  = isOwner && players.length >= (lobbyState?.min_players ?? 2)
  const canAddBot = isOwner && players.length < maxSlots

  // ── Assign players to seat positions ──────────────────────────────────────
  const positions   = computeSeatPositions(maxSlots)
  const creatorIdx  = positions.findIndex(p => p.isCreatorSlot)
  const creatorPlayer = players.find(p => !p.is_bot && p.player_id === lobbyState?.creator_id) ?? null
  const otherPlayers  = players.filter(p => p.is_bot || p.player_id !== lobbyState?.creator_id)

  const seats: Array<LobbyPlayer | null> = Array(positions.length).fill(null)
  if (creatorPlayer && creatorIdx !== -1) seats[creatorIdx] = creatorPlayer
  let oi = 0
  for (let i = 0; i < positions.length; i++) {
    if (i === creatorIdx) continue
    if (oi < otherPlayers.length) seats[i] = otherPlayers[oi++]
  }

  if (staleLobby) return <StaleLobbyModal />

  if (fetchError) {
    return <div className="lr__error-page" role="alert">{fetchError}</div>
  }

  const needMore = isOwner && lobbyState ? Math.max(0, lobbyState.min_players - players.length) : 0

  return (
    <div className="lr">
      {/* ── Header ───────────────────────────────────────────────────────── */}
      <header className="lr__header">
        <button className="lr__back" onClick={handleBack}>← Lobbies</button>
        <span className="lr__brand">Anjeer</span>
        <div className="lr__header-right">
          {code && <span className="lr__code">{code}</span>}
        </div>
      </header>

      {/* ── Arena ────────────────────────────────────────────────────────── */}
      <div className="lr__arena-wrap">
        <div className="lr__arena">
          <div className="lr__table" />
          {positions.map((pos, i) => (
            <Seat
              key={i}
              pos={pos}
              player={seats[i]}
              isOwner={isOwner}
              isMe={!seats[i]?.is_bot && seats[i]?.player_id === user?.id}
              isCreator={!seats[i]?.is_bot && seats[i]?.player_id === lobbyState?.creator_id}
              isPending={pendingSeatIdx === i}
              canAddBot={canAddBot}
              onClickEmpty={() => setPendingSeatIdx(i)}
              onDiffSelect={handleAddBot}
              onRemoveBot={handleRemoveBot}
              onChangeDiff={handleChangeDiff}
              onCancelPending={() => setPendingSeatIdx(null)}
            />
          ))}
        </div>
      </div>

      {/* ── Footer ───────────────────────────────────────────────────────── */}
      <footer className="lr__footer">
        <div className="lr__join-row">
          <span className="lr__join-label">Bot auto-fill</span>
          <div
            className={`lr__toggle${botAutofill ? ' lr__toggle--on' : ''}${isOwner ? ' lr__toggle--interactive' : ''}`}
            aria-label="Auto-fill bots when a player leaves"
            role="switch"
            aria-checked={botAutofill}
            onClick={isOwner ? handleToggleAutofill : undefined}
          >
            <span className="lr__toggle-thumb" />
          </div>
          <span className={`lr__join-state${botAutofill ? ' lr__join-state--on' : ''}`}>
            {botAutofill ? 'On' : 'Off'}
          </span>
        </div>

        <div className="lr__footer-btns">
          {startError && <p className="lr__alert" role="alert">{startError}</p>}
          {isOwner ? (
            <button className="lr__btn lr__btn--primary" onClick={handleStart} disabled={!canStart}>
              {needMore > 0 ? `Need ${needMore} more` : 'Start'}
            </button>
          ) : (
            <button className="lr__btn lr__btn--primary" disabled>Waiting for host</button>
          )}
          <button className="lr__btn lr__btn--ghost" onClick={handleBack}>Leave</button>
        </div>
      </footer>
    </div>
  )
}
