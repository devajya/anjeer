import { useWebSocket } from './hooks/useWebSocket'
import { ConnectionBanner } from './components/ConnectionBanner'
import { RoundCountdown } from './components/RoundCountdown'
import { HandPanel } from './components/HandPanel'
import { SuitPanel } from './components/SuitPanel'
import { TradeFeed } from './components/TradeFeed'
import { MyOrders } from './components/MyOrders'
import './App.css'

// Canonical suit order matching the engine's Suit enum declaration order.
const SUIT_ORDER = ['clubs', 'diamonds', 'hearts', 'spades'] as const

function App() {
  const {
    connected,
    playerId,
    books,
    trades,
    myOrders,
    errors,
    sendMessage,
    startsAt,
    hand,
    initialHand,
  } = useWebSocket('/ws')

  const activeSuits = SUIT_ORDER.filter(s => s in books)

  // Most recent trade price per suit — used by SuitPanel to show last-traded price.
  // trades is newest-first, so the first match for each suit is the latest.
  const lastTradePrices: Record<string, number> = {}
  for (const t of trades) {
    if (!(t.suit in lastTradePrices)) lastTradePrices[t.suit] = t.price
  }

  function handleCancel(orderId: number) {
    sendMessage({ type: 'cancel_order', order_id: orderId })
  }

  return (
    <main className="app">
      <header className="app__header">
        <h1 className="app__title">Anjeer</h1>
        <ConnectionBanner connected={connected} />
        <RoundCountdown startsAt={startsAt} />
      </header>

      <div className="app__layout">
        {/* ── Column 1: one SuitPanel per active suit ── */}
        <section className="app__suits">
          {activeSuits.length === 0 ? (
            <p className="app__waiting">Waiting for book data…</p>
          ) : (
            activeSuits.map(suit => {
              // AGENT-CTX: Makeshift self-trade guard — derive ownsBestBid/ownsBestAsk
              // from myOrders since the server does not yet send best_bid_player /
              // best_ask_player. When those fields arrive (future slice), replace this
              // derivation with the server-provided values and remove myOrdersForSuit.
              const suitOrders = myOrders.filter(o => o.suit === suit)
              const bestBid = books[suit].best_bid
              const bestAsk = books[suit].best_ask
              const ownsBestBid = bestBid !== null && suitOrders.some(o => o.side === 'buy'  && o.price === bestBid)
              const ownsBestAsk = bestAsk !== null && suitOrders.some(o => o.side === 'sell' && o.price === bestAsk)
              return (
                <SuitPanel
                  key={suit}
                  suit={suit}
                  book={books[suit]}
                  playerId={playerId}
                  error={errors[suit] ?? null}
                  lastTradePrice={lastTradePrices[suit] ?? null}
                  suitCardCount={hand ? hand[suit as keyof typeof hand] : null}
                  isOwnBestBid={ownsBestBid}
                  isOwnBestAsk={ownsBestAsk}
                  myOrdersForSuit={suitOrders}
                  onSendMessage={sendMessage}
                />
              )
            })
          )}
        </section>

        {/* ── Column 2: Hand + My Orders + Trade feed ── */}
        <section className="app__right">
          <HandPanel hand={hand} initialHand={initialHand} />
          <MyOrders orders={myOrders} onCancel={handleCancel} />
          <TradeFeed trades={trades} />
        </section>

        {/* ── Column 3: Placeholder for future widgets ──
            AGENT-CTX: Reserved for eval engine, position tracker, chat panel etc.
            Slice 8 will populate this with a configurable drag-and-resize widget
            system. The column is hidden below 900px to keep the layout clean on
            smaller screens. */}
        <aside className="app__future" aria-label="Future widgets panel">
        </aside>
      </div>
    </main>
  )
}

export default App
