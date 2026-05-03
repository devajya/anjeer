import { forwardRef, useImperativeHandle, useRef, useState, useEffect } from 'react'
import type { BookState, MyOrder } from '../hooks/useWebSocket'
import type { ErrorMessage, ClientCommand } from '../types/messages'
import { useOrderForm } from '../hooks/useOrderForm'
import './SuitPanel.css'

// AGENT-CTX: Fixed player colour palette — 8 slots, hues chosen to be visually
// distinct and readable on a dark background. Keyed by player_id (1-indexed).
// When the server sends best_bid_player / best_ask_player in a future slice,
// look up the id here and pass it as `bidPlayerColor` / `askPlayerColor` props.
export const PLAYER_COLORS: Record<number, string> = {
  1: '#1e40af', // blue
  2: '#7c3aed', // violet
  3: '#b45309', // amber
  4: '#0f766e', // teal
  5: '#be185d', // pink
  6: '#1d4ed8', // indigo
  7: '#65a30d', // lime
  8: '#c2410c', // orange
}

export interface SuitPanelHandle {
  focusBid:   () => void
  focusOffer: () => void
}

interface Props {
  suit: string
  book: BookState
  playerId: number | null
  error: ErrorMessage | null
  /** Last executed trade price for this suit, null if no trades yet. */
  lastTradePrice: number | null
  /** Player's available cash — used to disable BUY when best_ask exceeds balance. */
  balance?: number | null
  /**
   * How many cards of this suit the player currently holds.
   * null/undefined = hand not yet dealt (sell controls remain enabled to avoid
   * blocking the UI before round_start arrives).
   */
  suitCardCount?: number | null
  /**
   * True when the player's own order is sitting at the current best bid.
   * Disables all sell-side controls — executing a sell would be a self-trade.
   */
  isOwnBestBid?: boolean
  /**
   * True when the player's own order is sitting at the current best ask.
   * Disables all buy-side controls — executing a buy would be a self-trade.
   */
  isOwnBestAsk?: boolean
  /**
   * The player's own resting orders for this suit. Passed into useOrderForm for
   * submit-time self-trade detection on the price-input path.
   * AGENT-CTX: Makeshift client-side guard until server sends per-order player IDs.
   * When best_bid_player / best_ask_player arrive from the server, replace this
   * with server-side rejection and remove the myOrdersForSuit param from useOrderForm.
   */
  myOrdersForSuit?: MyOrder[]
  /**
   * Player ID who placed the current best bid. Null until the server sends
   * best_bid_player in a future slice — column stays neutral while null.
   */
  bidPlayerColor?: string | null
  /**
   * Player ID who placed the current best ask. Same caveat as bidPlayerColor.
   */
  askPlayerColor?: string | null
  onSendMessage: (cmd: ClientCommand) => void
  /** True when this suit is keyboard-focused. Applies a focus ring to the panel. */
  selected?: boolean
  /** Called when the player clicks the panel to keyboard-focus it. */
  onSelect?: (suit: string) => void
}

const SUIT_SYMBOLS: Record<string, string> = {
  clubs:    '♣',
  diamonds: '♦',
  hearts:   '♥',
  spades:   '♠',
}

// AGENT-CTX: SuitPanel shows a 3-column layout: BID side | suit centre | ASK side.
// Left col:   best-bid price + nudge-up ▲,  then  [SELL] [price-input⏎]
// Centre col: suit badge + last trade price.
// Right col:  ▼ nudge + best-ask price,     then  [price-input⏎] [BUY]
// Entering a price and pressing Enter (or ↑/↓ button) submits the order.
// Keyboard shortcuts b/a focus the bid/offer input; market buy/sell is button-only.
export const SuitPanel = forwardRef<SuitPanelHandle, Props>(function SuitPanel({
  suit,
  book,
  playerId,
  error,
  lastTradePrice,
  suitCardCount = null,
  isOwnBestBid = false,
  isOwnBestAsk = false,
  myOrdersForSuit = [],
  balance = null,
  bidPlayerColor = null,
  askPlayerColor = null,
  onSendMessage,
  selected = false,
  onSelect,
}, ref) {
  const { bidInput, offerInput, setBidInput, setOfferInput, submitBid, submitOffer, selfTradeError } =
    useOrderForm(suit, onSendMessage, myOrdersForSuit)

  const bidInputRef   = useRef<HTMLInputElement>(null)
  const offerInputRef = useRef<HTMLInputElement>(null)

  useImperativeHandle(ref, () => ({
    focusBid:   () => { bidInputRef.current?.focus(); bidInputRef.current?.select() },
    focusOffer: () => { offerInputRef.current?.focus(); offerInputRef.current?.select() },
  }))

  // Ephemeral server error — auto-clears after 2 seconds so it never blocks interaction.
  const [visibleError, setVisibleError] = useState<ErrorMessage | null>(null)
  useEffect(() => {
    if (!error) { setVisibleError(null); return }
    setVisibleError(error)
    const t = setTimeout(() => setVisibleError(null), 2000)
    return () => clearTimeout(t)
  }, [error])

  const [visibleSelfTradeError, setVisibleSelfTradeError] = useState<string | null>(null)
  useEffect(() => {
    if (!selfTradeError) { setVisibleSelfTradeError(null); return }
    setVisibleSelfTradeError(selfTradeError)
    const t = setTimeout(() => setVisibleSelfTradeError(null), 2000)
    return () => clearTimeout(t)
  }, [selfTradeError])

  const disabled = playerId === null
  const hasBid = book.best_bid !== null
  const hasAsk = book.best_ask !== null
  // Disable all sell-side controls when the player holds no cards of this suit.
  // suitCardCount null means hand not yet dealt — allow sells until we know better.
  const noCards = suitCardCount !== null && suitCardCount === 0
  // Disable BUY when the player can't afford the best ask.
  const cantAffordAsk = balance !== null && book.best_ask !== null && book.best_ask > balance

  const BID_NEUTRAL = '#0e0e0e'
  const ASK_NEUTRAL = '#0e0e0e'
  const bidBackground = bidPlayerColor
    ? `linear-gradient(to right, ${bidPlayerColor}, transparent)`
    : BID_NEUTRAL
  const askBackground = askPlayerColor
    ? `linear-gradient(to left, ${askPlayerColor}, transparent)`
    : ASK_NEUTRAL

  return (
    // AGENT-CTX: onClick selects this suit for keyboard shortcuts. The panel
    // itself is not focusable via Tab — keyboard suit selection uses bound keys.
    <div
      className={['sp', selected ? 'sp--selected' : ''].join(' ').trim()}
      onClick={() => onSelect?.(suit)}
    >
      <div className="sp__columns">

        {/* ── LEFT: BID side ── */}
        <div
          className="sp__col sp__col--bid"
          style={{ background: bidBackground }}
        >
          {/* Price row: bid price + nudge-up */}
          <div className="sp__price-row">
            <span className="sp__price sp__price--bid">
              {hasBid ? book.best_bid : '—'}
            </span>
            <button
              className="sp__nudge sp__nudge--up"
              type="button"
              title="Nudge bid up by 1"
              disabled={disabled}
              onClick={() => onSendMessage({ type: 'nudge', suit, side: 'buy' })}
            >
              ▲
            </button>
          </div>

          {/* Action row: [SELL] [price-input → Enter to submit bid] */}
          <div className="sp__action-row">
            <button
              className="sp__action-btn sp__action-btn--sell"
              type="button"
              disabled={disabled || !hasBid || noCards || isOwnBestBid}
              title={
                noCards      ? 'No cards to sell' :
                isOwnBestBid ? 'Cannot sell to your own buy order' :
                hasBid       ? `Sell at ${book.best_bid}` :
                               'No bid to sell into'
              }
              onClick={() =>
                onSendMessage({
                  type: 'submit_order',
                  suit,
                  side: 'sell',
                  price: book.best_bid!,
                })
              }
            >
              SELL
            </button>
            <form className="sp__price-form" noValidate onSubmit={e => { submitBid(e); bidInputRef.current?.blur() }}>
              <input
                ref={bidInputRef}
                className="sp__price-input"
                type="number"
                min={1}
                max={99}
                value={bidInput}
                onChange={e => setBidInput(e.target.value)}
                placeholder="price"
                disabled={disabled}
                aria-label={`Bid price for ${suit}`}
              />
            </form>
          </div>
        </div>

        {/* ── MIDDLE: suit badge + last trade ── */}
        <div className="sp__col sp__col--centre">
          <div className={`sp__badge sp__badge--${suit}`}>{SUIT_SYMBOLS[suit] ?? suit}</div>
          {lastTradePrice !== null ? (
            <span className="sp__last-trade">{lastTradePrice}</span>
          ) : (
            <span className="sp__last-trade sp__last-trade--empty" />
          )}
        </div>

        {/* ── RIGHT: ASK side ── */}
        <div
          className="sp__col sp__col--ask"
          style={{ background: askBackground }}
        >
          {/* Price row: nudge-down + ask price */}
          <div className="sp__price-row sp__price-row--ask">
            <button
              className="sp__nudge sp__nudge--down"
              type="button"
              title={noCards ? 'No cards to sell' : 'Nudge ask down by 1'}
              disabled={disabled || noCards}
              onClick={() => onSendMessage({ type: 'nudge', suit, side: 'sell' })}
            >
              ▼
            </button>
            <span className="sp__price sp__price--ask">
              {hasAsk ? book.best_ask : '—'}
            </span>
          </div>

          {/* Action row: [price-input → Enter to submit offer] [BUY] */}
          <div className="sp__action-row sp__action-row--ask">
            <form className="sp__price-form" noValidate onSubmit={e => { submitOffer(e); offerInputRef.current?.blur() }}>
              <input
                ref={offerInputRef}
                className="sp__price-input"
                type="number"
                min={1}
                max={99}
                value={offerInput}
                onChange={e => setOfferInput(e.target.value)}
                placeholder="price"
                disabled={disabled || noCards}
                aria-label={`Offer price for ${suit}`}
              />
            </form>
            <button
              className="sp__action-btn sp__action-btn--buy"
              type="button"
              disabled={disabled || !hasAsk || isOwnBestAsk || cantAffordAsk}
              title={
                isOwnBestAsk  ? 'Cannot buy your own sell order' :
                cantAffordAsk ? `Insufficient balance (need ${book.best_ask})` :
                hasAsk        ? `Buy at ${book.best_ask}` :
                                'No ask to buy from'
              }
              onClick={() =>
                onSendMessage({
                  type: 'submit_order',
                  suit,
                  side: 'buy',
                  price: book.best_ask!,
                })
              }
            >
              BUY
            </button>
          </div>
        </div>

      </div>

      {visibleError && (
        <p className="sp__error">✗ {visibleError.code}: {visibleError.message}</p>
      )}
      {!visibleError && visibleSelfTradeError && (
        <p className="sp__error">✗ {visibleSelfTradeError}</p>
      )}
    </div>
  )
})
