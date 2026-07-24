import { forwardRef, useImperativeHandle, useRef, useState, useEffect, memo } from 'react'
import type { BookState, MyOrder } from '../hooks/useWebSocket'
import type { ErrorMessage, ClientCommand } from '../types/messages'
import { useOrderForm } from '../hooks/useOrderForm'
import { useGameConfig } from '../contexts/GameConfigContext'
import './SuitPanel.css'

// AGENT-CTX: Fixed player colour palette — 8 slots, keyed by player_id (1-indexed).
// Hues chosen to be visually distinct on dark backgrounds and cohesive with the
// gold accent theme. Avoids red/green (reserved for sell/buy indicators).
export const PLAYER_COLORS: Record<number, string> = {
  1: '#e8b86d', // sand
  2: '#8ba7d4', // slate blue
  3: '#c4876e', // terracotta
  4: '#7dbcb5', // seafoam
  5: '#b09ec0', // mauve
  6: '#d4956a', // warm amber
  7: '#7aafc2', // steel blue
  8: '#c49ab0', // dusty rose
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
  lastTradePrice: number | null
  balance?: number | null
  suitCardCount?: number | null
  isOwnBestBid?: boolean
  isOwnBestAsk?: boolean
  myOrdersForSuit?: MyOrder[]
  bidPlayerColor?: string | null
  askPlayerColor?: string | null
  onSendMessage: (cmd: ClientCommand) => void
  selected?: boolean
  onSelect?: (suit: string) => void
  isSpectator?: boolean
  /** Qty available at best bid level, shown as "X @ price" in multi-qty mode. */
  bestBidQty?: number | null
  /** Qty available at best ask level, shown as "X @ price" in multi-qty mode. */
  bestAskQty?: number | null
}

const SUIT_SYMBOLS: Record<string, string> = {
  clubs:    '♣',
  diamonds: '♦',
  hearts:   '♥',
  spades:   '♠',
}

function useTimedValue<T>(value: T | null, delayMs: number): T | null {
  const [visible, setVisible] = useState<T | null>(null)
  useEffect(() => {
    if (!value) { setVisible(null); return }
    setVisible(value)
    const t = setTimeout(() => setVisible(null), delayMs)
    return () => clearTimeout(t)
  }, [value, delayMs])
  return visible
}

export const SuitPanel = memo(forwardRef<SuitPanelHandle, Props>(function SuitPanel({
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
  isSpectator = false,
  bestBidQty = null,
  bestAskQty = null,
}, ref) {
  const { allowMultiQty } = useGameConfig()

  // Single-qty form (simple mode)
  const { bidInput, offerInput, setBidInput, setOfferInput,
          submitBid, submitOffer, selfTradeError, parsedQty } = useOrderForm(suit, onSendMessage, myOrdersForSuit)

  // Multi-qty form per-side state
  const [buyQtyInput,  setBuyQtyInput]  = useState('')
  const [sellQtyInput, setSellQtyInput] = useState('')
  const [buyPriceInput,  setBuyPriceInput]  = useState('')
  const [sellPriceInput, setSellPriceInput] = useState('')

  // Refs for imperative handle — point to the limit-order price inputs on each side
  const bidInputRef   = useRef<HTMLInputElement>(null)
  const offerInputRef = useRef<HTMLInputElement>(null)

  useImperativeHandle(ref, () => ({
    focusBid:   () => { bidInputRef.current?.focus();   bidInputRef.current?.select() },
    focusOffer: () => { offerInputRef.current?.focus(); offerInputRef.current?.select() },
  }), [])

  const visibleError          = useTimedValue(error,          2000)
  const visibleSelfTradeError = useTimedValue(selfTradeError, 2000)

  const disabled     = playerId === null
  const hasBid       = book.best_bid !== null
  const hasAsk       = book.best_ask !== null
  const noCards      = suitCardCount !== null && suitCardCount === 0
  const cantAffordAsk = balance !== null && book.best_ask !== null && book.best_ask > balance

  const bidBackground = bidPlayerColor ? `linear-gradient(to right, ${bidPlayerColor}, transparent)` : 'var(--color-bg-base)'
  const askBackground = askPlayerColor ? `linear-gradient(to left, ${askPlayerColor}, transparent)` : 'var(--color-bg-base)'

  const parsedBuyQty  = Math.max(1, parseInt(buyQtyInput,  10) || 1)
  const parsedSellQty = Math.max(1, parseInt(sellQtyInput, 10) || 1)

  function submitMultiLimitBuy(e: React.FormEvent) {
    e.preventDefault()
    const p = parseInt(buyPriceInput, 10)
    if (isNaN(p) || p < 1 || p > 99) return
    onSendMessage({ type: 'submit_order', suit, side: 'buy', price: p, qty: parsedBuyQty })
    setBuyPriceInput('')
    bidInputRef.current?.blur()
  }

  function submitMultiLimitSell(e: React.FormEvent) {
    e.preventDefault()
    const p = parseInt(sellPriceInput, 10)
    if (isNaN(p) || p < 1 || p > 99) return
    onSendMessage({ type: 'submit_order', suit, side: 'sell', price: p, qty: parsedSellQty })
    setSellPriceInput('')
    offerInputRef.current?.blur()
  }

  return (
    <div
      className={['sp', selected ? 'sp--selected' : ''].join(' ').trim()}
      onClick={() => onSelect?.(suit)}
    >
      <div className="sp__columns">

        {allowMultiQty ? (
          <>
            {/* ── LEFT: SELL side (multi-qty) ── */}
            <div className="sp__col sp__col--bid" style={{ background: bidBackground }}>
              <div className="sp__price-row">
                <span className="sp__price sp__price--bid">
                  {hasBid
                    ? (bestBidQty != null ? `${bestBidQty} @ ${book.best_bid}` : String(book.best_bid))
                    : '—'}
                </span>
              </div>
              {!isSpectator && (
                <div className="sp__action-row sp__action-row--sell">
                  <button
                    className="sp__action-btn sp__action-btn--sell"
                    type="button"
                    disabled={disabled || !hasBid || noCards || isOwnBestBid}
                    title={
                      noCards      ? 'No cards to sell' :
                      isOwnBestBid ? 'Cannot sell to your own buy order' :
                      hasBid       ? `Sell up to ${Math.min(bestBidQty ?? 1, suitCardCount ?? 1)} at ${book.best_bid}` :
                                     'No bid to sell into'
                    }
                    onClick={() => {
                      const qty = Math.max(1, Math.min(bestBidQty ?? 1, suitCardCount ?? 1))
                      onSendMessage({ type: 'submit_order', suit, side: 'sell', price: book.best_bid!, qty })
                    }}
                  >
                    SELL
                  </button>
                  <input
                    className="sp__qty-input sp__qty-input--side"
                    type="number" min={1} step={1}
                    value={sellQtyInput}
                    onChange={e => setSellQtyInput(e.target.value)}
                    placeholder="qty"
                    disabled={disabled}
                    aria-label={`Sell quantity for ${suit}`}
                    onClick={e => e.stopPropagation()}
                  />
                </div>
              )}
              {!isSpectator && (
                <form className="sp__price-form sp__price-form--sell" noValidate onSubmit={submitMultiLimitSell}>
                  <input
                    ref={offerInputRef}
                    className="sp__price-input"
                    type="number" min={1} max={99}
                    value={sellPriceInput}
                    onChange={e => setSellPriceInput(e.target.value)}
                    placeholder="limit price"
                    disabled={disabled || noCards}
                    aria-label={`Limit sell price for ${suit}`}
                    onClick={e => e.stopPropagation()}
                  />
                </form>
              )}
            </div>

            {/* ── MIDDLE: suit badge (multi-qty, no shared qty) ── */}
            <div className="sp__col sp__col--centre">
              <div className={`sp__badge sp__badge--${suit}`}>{SUIT_SYMBOLS[suit] ?? suit}</div>
              {lastTradePrice !== null ? (
                <span className="sp__last-trade">{lastTradePrice}</span>
              ) : (
                <span className="sp__last-trade sp__last-trade--empty" />
              )}
            </div>

            {/* ── RIGHT: BUY side (multi-qty) ── */}
            <div className="sp__col sp__col--ask" style={{ background: askBackground }}>
              <div className="sp__price-row sp__price-row--ask">
                <span className="sp__price sp__price--ask">
                  {hasAsk
                    ? (bestAskQty != null ? `${bestAskQty} @ ${book.best_ask}` : String(book.best_ask))
                    : '—'}
                </span>
              </div>
              {!isSpectator && (
                <div className="sp__action-row sp__action-row--ask">
                  <input
                    className="sp__qty-input sp__qty-input--side"
                    type="number" min={1} step={1}
                    value={buyQtyInput}
                    onChange={e => setBuyQtyInput(e.target.value)}
                    placeholder="qty"
                    disabled={disabled}
                    aria-label={`Buy quantity for ${suit}`}
                    onClick={e => e.stopPropagation()}
                  />
                  <button
                    className="sp__action-btn sp__action-btn--buy"
                    type="button"
                    disabled={disabled || !hasAsk || isOwnBestAsk || cantAffordAsk}
                    title={
                      isOwnBestAsk  ? 'Cannot buy your own sell order' :
                      cantAffordAsk ? `Insufficient balance (need ${book.best_ask})` :
                      hasAsk        ? `Buy up to ${Math.min(bestAskQty ?? 1, balance !== null && book.best_ask !== null ? Math.floor(balance / book.best_ask) : 1)} at ${book.best_ask}` :
                                      'No ask to buy from'
                    }
                    onClick={() => {
                      const maxAffordable = balance !== null && book.best_ask !== null ? Math.floor(balance / book.best_ask) : 1
                      const qty = Math.max(1, Math.min(bestAskQty ?? 1, maxAffordable))
                      onSendMessage({ type: 'submit_order', suit, side: 'buy', price: book.best_ask!, qty })
                    }}
                  >
                    BUY
                  </button>
                </div>
              )}
              {!isSpectator && (
                <form className="sp__price-form sp__price-form--buy" noValidate onSubmit={submitMultiLimitBuy}>
                  <input
                    ref={bidInputRef}
                    className="sp__price-input"
                    type="number" min={1} max={99}
                    value={buyPriceInput}
                    onChange={e => setBuyPriceInput(e.target.value)}
                    placeholder="limit price"
                    disabled={disabled}
                    aria-label={`Limit buy price for ${suit}`}
                    onClick={e => e.stopPropagation()}
                  />
                </form>
              )}
            </div>
          </>
        ) : (
          <>
            {/* ── LEFT: BID side (single-qty) ── */}
            <div className="sp__col sp__col--bid" style={{ background: bidBackground }}>
              <div className="sp__price-row">
                <span className="sp__price sp__price--bid">{hasBid ? book.best_bid : '—'}</span>
                {!isSpectator && (
                  <button
                    className="sp__nudge sp__nudge--up"
                    type="button"
                    title="Nudge bid up by 1"
                    disabled={disabled}
                    onClick={() => onSendMessage({ type: 'nudge', suit, side: 'buy' })}
                  >▲</button>
                )}
              </div>
              {!isSpectator && (
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
                    onClick={() => onSendMessage({ type: 'submit_order', suit, side: 'sell', price: book.best_bid!, qty: parsedQty })}
                  >
                    SELL
                  </button>
                  <form className="sp__price-form" noValidate onSubmit={e => { submitBid(e); bidInputRef.current?.blur() }}>
                    <input
                      ref={bidInputRef}
                      className="sp__price-input"
                      type="number" min={1} max={99}
                      value={bidInput}
                      onChange={e => setBidInput(e.target.value)}
                      placeholder="price"
                      disabled={disabled}
                      aria-label={`Bid price for ${suit}`}
                    />
                  </form>
                </div>
              )}
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

            {/* ── RIGHT: ASK side (single-qty) ── */}
            <div className="sp__col sp__col--ask" style={{ background: askBackground }}>
              <div className="sp__price-row sp__price-row--ask">
                {!isSpectator && (
                  <button
                    className="sp__nudge sp__nudge--down"
                    type="button"
                    title={noCards ? 'No cards to sell' : 'Nudge ask down by 1'}
                    disabled={disabled || noCards}
                    onClick={() => onSendMessage({ type: 'nudge', suit, side: 'sell' })}
                  >▼</button>
                )}
                <span className="sp__price sp__price--ask">{hasAsk ? book.best_ask : '—'}</span>
              </div>
              {!isSpectator && (
                <div className="sp__action-row sp__action-row--ask">
                  <form className="sp__price-form" noValidate onSubmit={e => { submitOffer(e); offerInputRef.current?.blur() }}>
                    <input
                      ref={offerInputRef}
                      className="sp__price-input"
                      type="number" min={1} max={99}
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
                    onClick={() => onSendMessage({ type: 'submit_order', suit, side: 'buy', price: book.best_ask!, qty: parsedQty })}
                  >
                    BUY
                  </button>
                </div>
              )}
            </div>
          </>
        )}

      </div>

      {visibleError && (
        <p className="sp__error">✗ {visibleError.code}: {visibleError.message}</p>
      )}
      {!visibleError && visibleSelfTradeError && (
        <p className="sp__error">✗ {visibleSelfTradeError}</p>
      )}
    </div>
  )
}))
