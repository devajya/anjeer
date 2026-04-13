import { useWebSocket } from './hooks/useWebSocket'
import { ConnectionBanner } from './components/ConnectionBanner'
import { SuitPanel } from './components/SuitPanel'
import { TradeFeed } from './components/TradeFeed'
import { MyOrders } from './components/MyOrders'
import './App.css'

// AGENT-CTX: active_suits is driven by server config (order_book.active_suits).
// Slice 2: one suit. The frontend learns which suits exist from book_update messages
// — the first book_update on connect populates `books` and therefore `activeSuits`.
// Slice 3: server sends book_update for all 4 suits on connect; this auto-populates.
//
// AGENT-CTX: All WebSocket state is lifted here and passed down as props.
// No context provider or global state store in Slice 2.
// If prop drilling becomes painful in Slice 8, introduce a context then.
function App() {
  const {
    connected,
    lastServerTs,
    playerId,
    books,
    trades,
    myOrders,
    errors,
    sendMessage,
  } = useWebSocket('/ws')

  const activeSuits = Object.keys(books)

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
        <ConnectionBanner connected={connected} lastServerTs={lastServerTs} />
      </header>

      <div className="app__layout">
        {/* ── Column 1: one SuitPanel per active suit ── */}
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
                onSendMessage={sendMessage}
              />
            ))
          )}
        </section>

        {/* ── Column 2: My Orders + Trade feed ── */}
        <section className="app__right">
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
