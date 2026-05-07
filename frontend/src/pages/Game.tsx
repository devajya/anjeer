import { useState, useEffect, useRef, useCallback } from 'react'
import { useWebSocket } from '../hooks/useWebSocket'
import { useAuth } from '../hooks/useAuth'
import { useKeyBinds } from '../hooks/useKeyBinds'
import { useKeyboardShortcuts } from '../hooks/useKeyboardShortcuts'
import { ConnectionBanner } from '../components/ConnectionBanner'
import { RoundCountdown } from '../components/RoundCountdown'
import { MarketOverview } from '../components/MarketOverview'
import { SuitPanel, type SuitPanelHandle } from '../components/SuitPanel'
import { TradeFeed } from '../components/TradeFeed'
import { MyOrders } from '../components/MyOrders'
import { RoundEndModal } from '../components/RoundEndModal'
import { InterRoundScreen } from '../components/InterRoundScreen'
import { GameEndScreen } from '../components/GameEndScreen'
import { SessionError } from '../components/SessionError'
import { PlayerBadge } from '../components/PlayerBadge'
import { ShortcutHelp } from '../components/ShortcutHelp'
import { SpectatorBadge } from '../components/SpectatorBadge'
import { slotColorSemi } from '../utils/playerColors'
import '../App.css'

// AGENT-CTX: App.css is imported here (not a Game.css) because all .app__* class
// names originate there and are reused by the existing component tests. Renaming
// would be cosmetic churn without benefit. Leave until a design-system pass.
const SUIT_ORDER = ['clubs', 'diamonds', 'hearts', 'spades'] as const

export function Game() {
const { user, logout } = useAuth()
  const { binds } = useKeyBinds()
  const {
    connected, playerId, books, trades, myOrders, errors, sendMessage,
    startsAt, roundEndAt, hand, initialHand, playerSlot, roundEnd, balance,
    waitingForStart, ownsBestBidBySuit, ownsBestAskBySuit,
    interRound, voteTally, gameEnded, sessionError,
    roster, deltas, allBalances, allHandTotals, spectatorCount,
  } = useWebSocket('/ws')

  // AGENT-CTX: selectedSuit drives keyboard order submission. null = no suit
  // focused; keyboard buy/sell/nudge are no-ops until a suit is focused.
  const [selectedSuit, setSelectedSuit] = useState<string | null>(null)
  const [showShortcuts, setShowShortcuts] = useState(false)

  // Imperative refs to each active SuitPanel — used by keyboard shortcuts to
  // focus the bid/offer price input without submitting at market.
  const suitPanelRefs = useRef<Record<string, SuitPanelHandle | null>>({})

  // AGENT-CTX: fromLobby is true when the player arrived via LobbyRoom after

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

  // AGENT-CTX: submit_buy/sell keyboard shortcuts focus the price input rather
  // than hitting market immediately — the flow is: suit key → b/a → type price → Enter.
  // Market buy/sell remains a button click only.
  const handleSubmitBuy = useCallback(() => {
    if (!selectedSuit) return
    suitPanelRefs.current[selectedSuit]?.focusBid()
  }, [selectedSuit])

  const handleSubmitSell = useCallback(() => {
    if (!selectedSuit) return
    suitPanelRefs.current[selectedSuit]?.focusOffer()
  }, [selectedSuit])

  // Accept the standing bid = sell to the buyer at their price (hit the bid).
  const handleAcceptBuy = useCallback(() => {
    if (!selectedSuit) return
    const bid = books[selectedSuit]?.best_bid
    if (bid == null) return
    sendMessage({ type: 'submit_order', suit: selectedSuit, side: 'sell', price: bid })
  }, [selectedSuit, books, sendMessage])

  // Accept the standing ask = buy from the seller at their price (lift the offer).
  const handleAcceptSell = useCallback(() => {
    if (!selectedSuit) return
    const ask = books[selectedSuit]?.best_ask
    if (ask == null) return
    sendMessage({ type: 'submit_order', suit: selectedSuit, side: 'buy', price: ask })
  }, [selectedSuit, books, sendMessage])

  const handleNudgeBuy = useCallback(() => {
    if (!selectedSuit) return
    sendMessage({ type: 'nudge', suit: selectedSuit, side: 'buy' })
  }, [selectedSuit, sendMessage])

  const handleNudgeSell = useCallback(() => {
    if (!selectedSuit) return
    sendMessage({ type: 'nudge', suit: selectedSuit, side: 'sell' })
  }, [selectedSuit, sendMessage])

  // AGENT-CTX: Cancel targets the player's own best bid/ask for the selected
  // suit. myOrders is sorted newest-first; we cancel the first matching order.
  const handleCancelBestBuy = useCallback(() => {
    const order = myOrders.find(o => o.suit === selectedSuit && o.side === 'buy')
    if (order) sendMessage({ type: 'cancel_order', order_id: order.order_id })
  }, [selectedSuit, myOrders, sendMessage])

  const handleCancelBestSell = useCallback(() => {
    const order = myOrders.find(o => o.suit === selectedSuit && o.side === 'sell')
    if (order) sendMessage({ type: 'cancel_order', order_id: order.order_id })
  }, [selectedSuit, myOrders, sendMessage])

  // AGENT-CTX: Shortcuts are disabled while any modal overlay is visible to
  // prevent accidental order submission during inter-round or round-end screens.
  useKeyboardShortcuts({
    binds,
    enabled: !showRoundEnd && !showInterRound,
    onSuitFocus:       setSelectedSuit,
    onSubmitBuy:       handleSubmitBuy,
    onSubmitSell:      handleSubmitSell,
    onAcceptBuy:       handleAcceptBuy,
    onAcceptSell:      handleAcceptSell,
    onNudgeBuy:        handleNudgeBuy,
    onNudgeSell:       handleNudgeSell,
    onCancelBestBuy:   handleCancelBestBuy,
    onCancelBestSell:  handleCancelBestSell,
    onToggleShortcuts: () => setShowShortcuts(v => !v),
  })

  const displayBalance = balance ?? null

  return (
    <>
      {showShortcuts && (
        <ShortcutHelp binds={binds} onClose={() => setShowShortcuts(false)} />
      )}
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
            </div>
          )}
          {spectatorCount > 0 && <SpectatorBadge count={spectatorCount} />}
          {user && (
            <PlayerBadge
              username={user.username}
              balance={displayBalance}
              onLogout={logout}
            />
          )}
        </header>

        <div className="app__layout">

          {/* ── LEFT: Trade feed + My Orders ── */}
          <div className="app__left-col">
            <div className="panel panel--feed" data-panel-id="trade-feed">
              <div className="panel__header">
                <span className="panel__title">Trade History</span>
              </div>
              <div className="panel__body">
                <TradeFeed trades={trades} roster={roster} />
              </div>
            </div>

            <div className="panel" data-panel-id="my-orders">
              <div className="panel__header">
                <span className="panel__title">My Orders</span>
              </div>
              <div className="panel__body">
                <MyOrders orders={myOrders} onCancel={handleCancel} />
              </div>
            </div>
          </div>

          {/* ── RIGHT: Action + info column ── */}
          <div className="app__right-col">

            {/* 1. Market Overview — merged hand + all-player deltas */}
            <div className="panel" data-panel-id="market-overview">
              <div className="panel__header">
                <span className="panel__title">Market Overview</span>
              </div>
              <div className="panel__body">
                <MarketOverview
                  hand={hand}
                  initialHand={initialHand}
                  deltas={deltas}
                  roster={roster}
                  mySlot={playerSlot}
                  balance={displayBalance}
                  allBalances={allBalances}
                  allHandTotals={allHandTotals}
                />
              </div>
            </div>

            {/* 2. Suit panels — share remaining height equally */}
            <div className="app__suits">
              {activeSuits.length === 0 ? (
                <p className="app__waiting">Waiting for book data…</p>
              ) : (
                activeSuits.map(suit => (
                  <div key={suit} className="app__suit-wrapper" data-panel-id={`suit-${suit}`}>
                    <SuitPanel
                      ref={el => { suitPanelRefs.current[suit] = el }}
                      suit={suit}
                      book={books[suit]}
                      playerId={playerId}
                      error={errors[suit] ?? null}
                      lastTradePrice={lastTradePrices[suit] ?? null}
                      suitCardCount={hand ? hand[suit as keyof typeof hand] : null}
                      isOwnBestBid={ownsBestBidBySuit[suit] ?? false}
                      isOwnBestAsk={ownsBestAskBySuit[suit] ?? false}
                      myOrdersForSuit={myOrders.filter(o => o.suit === suit)}
                      balance={displayBalance}
                      bidPlayerColor={
                        books[suit]?.best_bid_slot != null
                          ? slotColorSemi(books[suit].best_bid_slot!) : null
                      }
                      askPlayerColor={
                        books[suit]?.best_ask_slot != null
                          ? slotColorSemi(books[suit].best_ask_slot!) : null
                      }
                      onSendMessage={sendMessage}
                      selected={suit === selectedSuit}
                      onSelect={setSelectedSuit}
                    />
                  </div>
                ))
              )}
            </div>

          </div>
        </div>
      </main>
    </>
  )
}
