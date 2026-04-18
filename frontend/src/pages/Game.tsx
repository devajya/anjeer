import { useState, useEffect } from 'react'
import { useWebSocket } from '../hooks/useWebSocket'
import { useAuth } from '../hooks/useAuth'
import { ConnectionBanner } from '../components/ConnectionBanner'
import { RoundCountdown } from '../components/RoundCountdown'
import { HandPanel } from '../components/HandPanel'
import { SuitPanel } from '../components/SuitPanel'
import { TradeFeed } from '../components/TradeFeed'
import { MyOrders } from '../components/MyOrders'
import { RoundEndModal } from '../components/RoundEndModal'
import { PlayerBadge } from '../components/PlayerBadge'
import '../App.css'

// AGENT-CTX: App.css is imported here (not a Game.css) because all .app__* class
// names originate there and are reused by the existing component tests. Renaming
// would be cosmetic churn without benefit. Leave until a design-system pass.
const SUIT_ORDER = ['clubs', 'diamonds', 'hearts', 'spades'] as const

export function Game() {
  const { user, logout } = useAuth()
  const {
    connected, playerId, books, trades, myOrders, errors, sendMessage,
    startsAt, roundEndAt, hand, initialHand, playerSlot, roundEnd, balance,
    waitingForStart, ownsBestBidBySuit, ownsBestAskBySuit,
  } = useWebSocket('/ws')

  // AGENT-CTX: Local dismissed flag so the modal can be closed without mutating
  // hook state. Reset whenever a new round_end payload arrives (round_end changes
  // referential identity each time because useWebSocket creates a new object).
  const [roundEndDismissed, setRoundEndDismissed] = useState(false)
  useEffect(() => {
    if (roundEnd) setRoundEndDismissed(false)
  }, [roundEnd])

  const showRoundEnd = roundEnd !== null && !roundEndDismissed
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
