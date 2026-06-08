import { useState, useEffect, useRef, useCallback } from 'react'
import { useSearchParams, useNavigate } from 'react-router-dom'
import { useWebSocket } from '../hooks/useWebSocket'
import { useAuth } from '../hooks/useAuth'
import { useKeyBinds } from '../hooks/useKeyBinds'
import { useKeyboardShortcuts } from '../hooks/useKeyboardShortcuts'
import { useReconnect } from '../hooks/useReconnect'
import { GameConfigProvider, deriveGameConfig } from '../contexts/GameConfigContext'
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
import { ReconnectOverlay } from '../components/ReconnectOverlay'
import { StaleLobbyModal } from '../components/StaleLobbyModal'
import { EvalPanel } from '../components/EvalPanel'
import { MbpNDepthPanel } from '../components/MbpNDepthPanel'
import { MboFeedPanel, useMboDepth } from '../components/MboFeedPanel'
import { slotColorSemi } from '../utils/playerColors'
import { SUIT_ORDER } from '../utils/suits'
import '../App.css'

export function Game() {
  const { user } = useAuth()
  const navigate = useNavigate()
  const { binds } = useKeyBinds()
  const [searchParams] = useSearchParams()
  const lobbyId = searchParams.get('lobby_id') ?? ''

  const [windowSeconds, setWindowSeconds] = useState(20)
  useEffect(() => {
    fetch('/config/client')
      .then(r => r.ok ? r.json() : null)
      .then((data: { reconnect_window_seconds?: number } | null) => {
        if (data?.reconnect_window_seconds) setWindowSeconds(data.reconnect_window_seconds)
      })
      .catch(() => {})
  }, [])

  const {
    connected, playerId, books, trades, myOrders, errors, sendMessage,
    startsAt, roundEndAt, hand, initialHand, playerSlot, roundEnd, balance,
    waitingForStart, ownsBestBidBySuit, ownsBestAskBySuit,
    interRound, gameEnded, sessionError,
    roster, deltas, allBalances, allHandTotals, spectatorCount,
    reconnectTokenMsg, gameStateSnapshot, reconnectWindowExpired, queueState,
    currentOwnerPlayerId, currentOwnerUsername,
    evalPosteriorUpdate, evalAccumulationSignal, evalExecutionGuidance,
    bookDepths, mboLogs,
    gameMode,
  } = useWebSocket('/ws')

  const mboDepth = useMboDepth(mboLogs)

  const {
    status: reconnectStatus,
    onTokenReceived,
    onSnapshotReceived,
    onWindowExpired,
  } = useReconnect(lobbyId, windowSeconds, connected, sendMessage)

  useEffect(() => {
    if (reconnectTokenMsg) onTokenReceived(reconnectTokenMsg.token, reconnectTokenMsg.expires_at)
  }, [reconnectTokenMsg]) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (gameStateSnapshot) onSnapshotReceived()
  }, [gameStateSnapshot]) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (reconnectWindowExpired) onWindowExpired()
  }, [reconnectWindowExpired]) // eslint-disable-line react-hooks/exhaustive-deps

  const [selectedSuit, setSelectedSuit] = useState<string | null>(null)
  const [showShortcuts, setShowShortcuts] = useState(false)
  const [evalExpanded, setEvalExpanded] = useState(true)

  const suitPanelRefs   = useRef<Record<string, SuitPanelHandle | null>>({})
  const tradeFeedRef    = useRef<HTMLDivElement>(null)
  const centerPanelRef  = useRef<HTMLDivElement>(null)

  const [focusedPanel,        setFocusedPanel]        = useState<'trade_feed' | 'center' | null>(null)
  const [tradeFeedCollapsed,  setTradeFeedCollapsed]  = useState(false)
  const [centerCollapsed,     setCenterCollapsed]     = useState(false)

  useEffect(() => {
    setTradeFeedCollapsed(false)
  }, [gameMode])

  const handleFocusTradeFeed   = useCallback(() => {
    setFocusedPanel('trade_feed')
    tradeFeedRef.current?.scrollIntoView({ behavior: 'smooth', block: 'nearest' })
  }, [])
  const handleFocusCenterPanel = useCallback(() => {
    setFocusedPanel('center')
    centerPanelRef.current?.scrollIntoView({ behavior: 'smooth', block: 'nearest' })
  }, [])
  const handleTogglePanel = useCallback(() => {
    if (focusedPanel === 'trade_feed') setTradeFeedCollapsed(v => !v)
    else if (focusedPanel === 'center') setCenterCollapsed(v => !v)
  }, [focusedPanel])

  const [roundEndDismissed, setRoundEndDismissed] = useState(false)
  useEffect(() => {
    if (roundEnd) setRoundEndDismissed(false)
  }, [roundEnd])

  const [interRoundDismissed, setInterRoundDismissed] = useState(false)
  useEffect(() => {
    if (interRound) setInterRoundDismissed(false)
  }, [interRound])

  const isOwner = playerId !== null && currentOwnerPlayerId !== null && playerId === currentOwnerPlayerId

  function handleStartNextRound() { sendMessage({ type: 'start_next_round' }) }
  function handleEndGame()        { sendMessage({ type: 'end_game' }) }

  useEffect(() => {
    if (!connected || playerSlot !== null || reconnectStatus !== 'reconnecting') return
    const id = setTimeout(() => onWindowExpired(), 3000)
    return () => clearTimeout(id)
  }, [connected, playerSlot, reconnectStatus, onWindowExpired])

  const [noTokenOverflow, setNoTokenOverflow] = useState(false)
  const isInGame = playerSlot !== null || waitingForStart !== null || startsAt !== null || hand !== null
  useEffect(() => {
    if (!connected || isInGame || reconnectStatus !== 'connected') return
    const id = setTimeout(() => setNoTokenOverflow(true), 3000)
    return () => clearTimeout(id)
  }, [connected, isInGame, reconnectStatus])

  const showRoundEnd   = roundEnd !== null && !roundEndDismissed
  const showInterRound = interRound !== null && !interRoundDismissed

  const activeMode    = gameMode ?? 'simple'
  const { allowMultiQty } = deriveGameConfig(activeMode)

  const handleSubmitBuy = useCallback(() => {
    if (!selectedSuit) return
    suitPanelRefs.current[selectedSuit]?.focusBid()
  }, [selectedSuit])

  const handleSubmitSell = useCallback(() => {
    if (!selectedSuit) return
    suitPanelRefs.current[selectedSuit]?.focusOffer()
  }, [selectedSuit])

  const handleAcceptBuy = useCallback(() => {
    if (!selectedSuit) return
    const bid = books[selectedSuit]?.best_bid
    if (bid == null) return
    const qty = allowMultiQty ? (bookDepths[selectedSuit]?.bids[0]?.qty ?? 1) : 1
    sendMessage({ type: 'submit_order', suit: selectedSuit, side: 'sell', price: bid, qty })
  }, [selectedSuit, books, sendMessage, allowMultiQty, bookDepths])

  const handleAcceptSell = useCallback(() => {
    if (!selectedSuit) return
    const ask = books[selectedSuit]?.best_ask
    if (ask == null) return
    const qty = allowMultiQty ? (bookDepths[selectedSuit]?.asks[0]?.qty ?? 1) : 1
    sendMessage({ type: 'submit_order', suit: selectedSuit, side: 'buy', price: ask, qty })
  }, [selectedSuit, books, sendMessage, allowMultiQty, bookDepths])

  const handleNudgeBuy = useCallback(() => {
    if (!selectedSuit) return
    sendMessage({ type: 'nudge', suit: selectedSuit, side: 'buy' })
  }, [selectedSuit, sendMessage])

  const handleNudgeSell = useCallback(() => {
    if (!selectedSuit) return
    sendMessage({ type: 'nudge', suit: selectedSuit, side: 'sell' })
  }, [selectedSuit, sendMessage])

  const handleCancelBestBuy = useCallback(() => {
    const order = myOrders.find(o => o.suit === selectedSuit && o.side === 'buy')
    if (order) sendMessage({ type: 'cancel_order', order_id: order.order_id })
  }, [selectedSuit, myOrders, sendMessage])

  const handleCancelBestSell = useCallback(() => {
    const order = myOrders.find(o => o.suit === selectedSuit && o.side === 'sell')
    if (order) sendMessage({ type: 'cancel_order', order_id: order.order_id })
  }, [selectedSuit, myOrders, sendMessage])

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
    onCancelBestBuy:     handleCancelBestBuy,
    onCancelBestSell:    handleCancelBestSell,
    onToggleShortcuts:   () => setShowShortcuts(v => !v),
    onFocusTradeFeed:    handleFocusTradeFeed,
    onFocusCenterPanel:  handleFocusCenterPanel,
    onTogglePanel:       handleTogglePanel,
  })

  if (queueState.status === 'overflow' || noTokenOverflow) return (
    <StaleLobbyModal title="This lobby is full" message="The lobby and wait queue are both full." />
  )
  if (sessionError) return <SessionError sessionError={sessionError} />
  if (gameEnded)    return <GameEndScreen gameEnded={gameEnded} playerSlot={playerSlot} />

  const activeSuits = SUIT_ORDER.filter(s => s in books)

  const lastTradePrices: Record<string, number> = {}
  for (const t of trades) {
    if (!(t.suit in lastTradePrices)) lastTradePrices[t.suit] = t.price
  }

  function handleCancel(orderId: number) {
    sendMessage({ type: 'cancel_order', order_id: orderId })
  }

  const displayBalance = balance ?? null
  const hasCenterCol   = activeMode !== 'simple'

  const layoutClass = [
    'app__layout',
    evalExpanded                      ? 'app__layout--eval-open'      : '',
    activeMode === 'intermediate'     ? 'app__layout--center-col'     : '',
    activeMode === 'advanced'         ? 'app__layout--advanced'       : '',
    tradeFeedCollapsed                ? 'app__layout--feed-collapsed' : '',
  ].filter(Boolean).join(' ')

  return (
    <GameConfigProvider mode={activeMode}>
      <>
        <ReconnectOverlay status={reconnectStatus} lobbyId={lobbyId} />
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
            playerSlot={playerSlot}
            isOwner={isOwner}
            ownerUsername={currentOwnerUsername}
            onStartNextRound={handleStartNextRound}
            onEndGame={handleEndGame}
            onCountdownExpired={() => setInterRoundDismissed(true)}
          />
        )}
        <main className="app">
          <header className="app__header">
            <h1 className="app__title">Anjeer</h1>
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
                onLeave={() => {
                  sendMessage({ type: 'leave_lobby', lobby_id: lobbyId })
                  navigate('/lobby', { state: { leftGame: lobbyId } })
                }}
              />
            )}
          </header>

          <div className={layoutClass}>

            {/* ── LEFT: Trade feed + My Orders ── */}
            <div className="app__left-col">
              <div ref={tradeFeedRef} className={`panel panel--feed${tradeFeedCollapsed ? ' panel--collapsed' : ''}`} data-panel-id="trade-feed">
                <div className="panel__header" onClick={() => setTradeFeedCollapsed(v => !v)} style={{ cursor: 'pointer' }}>
                  <span className="panel__title">Trade History</span>
                  <span className="panel__collapse-icon">{tradeFeedCollapsed ? '▶' : '◀'}</span>
                </div>
                {!tradeFeedCollapsed && (
                  <div className="panel__body">
                    <TradeFeed trades={trades} roster={roster} />
                  </div>
                )}
              </div>
              {activeMode !== 'advanced' && (
                <div className="panel" data-panel-id="my-orders">
                  <div className="panel__header">
                    <span className="panel__title">My Orders</span>
                  </div>
                  <div className="panel__body">
                    <MyOrders orders={myOrders} onCancel={handleCancel} />
                  </div>
                </div>
              )}
            </div>

            {/* ── CENTER: Depth (intermediate) or Order Feed (advanced) ── */}
            {hasCenterCol && (
              <div className="app__center-col">
                <div ref={centerPanelRef} className={`panel panel--center${centerCollapsed ? ' panel--collapsed' : ''}`} data-panel-id="center-panel">
                  <div className="panel__header">
                    <span className="panel__title">
                      {activeMode === 'intermediate' ? 'Order Book Depth' : 'Order Feed'}
                    </span>
                  </div>
                  <div className="panel__body">
                    {activeMode === 'intermediate'
                      ? <MbpNDepthPanel bookDepths={bookDepths} />
                      : <MboFeedPanel
                          mboLogs={mboLogs}
                          myOrders={myOrders}
                          onCancel={handleCancel}
                          roster={roster}
                          playerSlot={playerSlot}
                        />
                    }
                  </div>
                </div>
              </div>
            )}

            {/* ── RIGHT: Market overview + Suit panels ── */}
            <div className="app__right-col">
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
                        bestBidQty={bookDepths[suit]?.bids[0]?.qty ?? mboDepth[suit]?.bidQty ?? null}
                        bestAskQty={bookDepths[suit]?.asks[0]?.qty ?? mboDepth[suit]?.askQty ?? null}
                      />
                    </div>
                  ))
                )}
              </div>
            </div>

            {/* ── EVAL: Collapsible eval panel ── */}
            <EvalPanel
              onExpandedChange={setEvalExpanded}
              posteriorUpdate={evalPosteriorUpdate}
              accumulationSignal={evalAccumulationSignal}
              executionGuidance={evalExecutionGuidance}
            />

          </div>
        </main>
      </>
    </GameConfigProvider>
  )
}
