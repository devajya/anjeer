import { useState, useEffect, useRef } from 'react'
import { useSearchParams } from 'react-router-dom'
import { useWebSocket } from '../hooks/useWebSocket'
import { useAuth } from '../hooks/useAuth'
import { ConnectionBanner } from '../components/ConnectionBanner'
import { RoundCountdown } from '../components/RoundCountdown'
import { HandPanel } from '../components/HandPanel'
import { SuitPanel } from '../components/SuitPanel'
import { TradeFeed } from '../components/TradeFeed'
import { MyOrders } from '../components/MyOrders'
import { RoundEndModal } from '../components/RoundEndModal'
import { InterRoundScreen } from '../components/InterRoundScreen'
import { GameEndScreen } from '../components/GameEndScreen'
import { SessionError } from '../components/SessionError'
import { PlayerBadge } from '../components/PlayerBadge'
import '../App.css'

// AGENT-CTX: App.css is imported here (not a Game.css) because all .app__* class
// names originate there and are reused by the existing component tests. Renaming
// would be cosmetic churn without benefit. Leave until a design-system pass.
const SUIT_ORDER = ['clubs', 'diamonds', 'hearts', 'spades'] as const

export function Game() {
  const [searchParams] = useSearchParams()
  const { user, logout } = useAuth()
  const {
    connected, playerId, books, trades, myOrders, errors, sendMessage,
    startsAt, roundEndAt, hand, initialHand, playerSlot, roundEnd, balance,
    waitingForStart, ownsBestBidBySuit, ownsBestAskBySuit,
    interRound, voteTally, gameEnded, sessionError,
  } = useWebSocket('/ws')

  // AGENT-CTX: fromLobby is true when the player arrived via LobbyRoom after
  // lobby_started fired. When true, start_game is sent automatically as soon
  // as all required players are connected — no button press needed in the game
  // room. Without lobby_id (direct /game navigation), the manual Start Game
  // button remains so the flow still works in dev/testing without a lobby.
  const fromLobby     = searchParams.get('lobby_id') !== null
  const autoStartedRef = useRef(false)
  useEffect(() => {
    if (!fromLobby || !waitingForStart) return
    if (waitingForStart.connected !== waitingForStart.required) return
    if (autoStartedRef.current) return
    autoStartedRef.current = true
    sendMessage({ type: 'start_game' })
  }, [fromLobby, waitingForStart, sendMessage])

  // AGENT-CTX: Local dismissed flag so the modal can be closed without mutating
  // hook state. Reset whenever a new round_end payload arrives (round_end changes
  // referential identity each time because useWebSocket creates a new object).
  const [roundEndDismissed, setRoundEndDismissed] = useState(false)
  useEffect(() => {
    if (roundEnd) setRoundEndDismissed(false)
  }, [roundEnd])

  // AGENT-CTX: interRoundDismissed is set true either by onCountdownExpired
  // (timer fires on the client) or implicitly when interRound becomes null
  // (hook clears it on round_start). Reset on every new interRound so the
  // overlay reappears for each subsequent round.
  const [interRoundDismissed, setInterRoundDismissed] = useState(false)
  // AGENT-CTX: hasVotedToEnd prevents duplicate vote_to_end commands within
  // a single inter-round window. Reset with interRoundDismissed on each new round.
  const [hasVotedToEnd, setHasVotedToEnd] = useState(false)
  useEffect(() => {
    if (interRound) {
      setInterRoundDismissed(false)
      setHasVotedToEnd(false)
    }
  }, [interRound])

  // AGENT-CTX: voteTally messages supersede the initial vote_count/votes_required
  // snapshot in interRound. Fall back to interRound values on first render
  // (before any vote_tally arrives) so the tally is never empty.
  const liveVotes         = voteTally?.votes          ?? interRound?.vote_count      ?? 0
  const liveVotesRequired = voteTally?.required       ?? interRound?.votes_required  ?? 0

  function handleVoteToEnd() {
    setHasVotedToEnd(true)
    sendMessage({ type: 'vote_to_end' })
  }

  // AGENT-CTX: sessionError and gameEnded are terminal states — the session is
  // over and there is no game to show beneath. Return early so the full game
  // layout (books, orders, timers) is never rendered in these states.
  // sessionError takes priority over gameEnded in case both arrive in one
  // render cycle (e.g. crash fires after game_ended; shouldn't happen but safe).
  if (sessionError) return <SessionError sessionError={sessionError} />
  if (gameEnded)    return <GameEndScreen gameEnded={gameEnded} playerSlot={playerSlot} />

  const showRoundEnd    = roundEnd !== null && !roundEndDismissed
  // AGENT-CTX: InterRoundScreen is shown while interRound is set AND not locally
  // dismissed. It does not suppress the game layout beneath it — players can see
  // (but not interact with) the board through the semi-transparent backdrop.
  const showInterRound  = interRound !== null && !interRoundDismissed
  const activeSuits  = SUIT_ORDER.filter(s => s in books)

  const lastTradePrices: Record<string, number> = {}
  for (const t of trades) {
    if (!(t.suit in lastTradePrices)) lastTradePrices[t.suit] = t.price
  }

  function handleCancel(orderId: number) {
    sendMessage({ type: 'cancel_order', order_id: orderId })
  }

  const displayBalance = balance ?? null

  return (
    <>
      {showRoundEnd && (
        <RoundEndModal
          roundEnd={roundEnd!}
          playerSlot={playerSlot}
          onDismiss={() => setRoundEndDismissed(true)}
        />
      )}
      {showInterRound && (
        <InterRoundScreen
          interRound={interRound!}
          liveVotes={liveVotes}
          liveVotesRequired={liveVotesRequired}
          playerSlot={playerSlot}
          hasVoted={hasVotedToEnd}
          onVoteToEnd={handleVoteToEnd}
          onCountdownExpired={() => setInterRoundDismissed(true)}
        />
      )}
      <main className="app">
        <header className="app__header">
          <h1 className="app__title">Anjeer</h1>
          <ConnectionBanner connected={connected} />
          <RoundCountdown startsAt={startsAt} roundEndAt={roundEndAt} />
          {waitingForStart && !startsAt && !roundEndAt && (
            <div className="app__lobby">
              <span className="app__lobby-status">
                {waitingForStart.connected}/{waitingForStart.required} players connected
              </span>
              {waitingForStart.connected === waitingForStart.required && (
                <button
                  className="app__start-btn"
                  onClick={() => sendMessage({ type: 'start_game' })}
                >
                  Start Game
                </button>
              )}
            </div>
          )}
          {user && (
            <PlayerBadge
              username={user.username}
              balance={displayBalance}
              onLogout={logout}
            />
          )}
        </header>

        <div className="app__layout">
          <section className="app__suits">
            {activeSuits.length === 0 ? (
              <p className="app__waiting">Waiting for book data…</p>
            ) : (
              activeSuits.map(suit => (
                <SuitPanel
                  key={suit}
                  suit={suit}
                  book={books[suit]}
                  playerId={playerId}
                  error={errors[suit] ?? null}
                  lastTradePrice={lastTradePrices[suit] ?? null}
                  suitCardCount={hand ? hand[suit as keyof typeof hand] : null}
                  isOwnBestBid={ownsBestBidBySuit[suit] ?? false}
                  isOwnBestAsk={ownsBestAskBySuit[suit] ?? false}
                  myOrdersForSuit={myOrders.filter(o => o.suit === suit)}
                  onSendMessage={sendMessage}
                />
              ))
            )}
          </section>

          <section className="app__right">
            <HandPanel hand={hand} initialHand={initialHand} balance={displayBalance} />
            <MyOrders orders={myOrders} onCancel={handleCancel} />
            <TradeFeed trades={trades} />
          </section>

          {/* AGENT-CTX: Reserved for eval engine, position tracker, chat panel.
              Slice 8 populates this with a configurable widget system.
              Hidden below 900px to keep layout clean on smaller screens. */}
          <aside className="app__future" aria-label="Future widgets panel" />
        </div>
      </main>
    </>
  )
}
