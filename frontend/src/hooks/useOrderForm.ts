import { useState } from 'react'
import type { ClientCommand } from '../types/messages'
import type { MyOrder } from './useWebSocket'

export interface OrderFormState {
  bidInput: string
  offerInput: string
  qtyInput: string
  setBidInput: (v: string) => void
  setOfferInput: (v: string) => void
  setQtyInput: (v: string) => void
  submitBid: (e: React.FormEvent) => void
  submitOffer: (e: React.FormEvent) => void
  // null when no self-trade conflict; set to a human-readable message when blocked.
  // AGENT-CTX: Makeshift client-side guard — checked at submit time from myOrdersForSuit.
  // When the server sends per-order player IDs in a future slice, remove this check
  // and let the server reject self-trades with an error code instead.
  selfTradeError: string | null
  /** Parsed qty ≥ 1, or 1 if input is empty/invalid. */
  parsedQty: number
}

// AGENT-CTX: useOrderForm owns the price-input state and integer-parse
// validation for one suit panel. Extracted from SuitPanel so that the
// component stays purely presentational and the form logic can be tested
// independently. Slice 8 keyboard shortcuts should extend this hook rather
// than SuitPanel. onSendMessage is passed in rather than imported so the
// hook has no dependency on useWebSocket or the broader app state.
export function useOrderForm(
  suit: string,
  onSendMessage: (cmd: ClientCommand) => void,
  // Own resting orders for this suit — used to detect self-trade before sending.
  // Defaults to [] (no orders) so callers that don't need this check can omit it.
  myOrdersForSuit: MyOrder[] = [],
): OrderFormState {
  const [bidInput, setBidInputRaw] = useState('')
  const [offerInput, setOfferInputRaw] = useState('')
  const [qtyInput, setQtyInputRaw] = useState('')
  const [selfTradeError, setSelfTradeError] = useState<string | null>(null)

  const parsedQty = Math.max(1, parseInt(qtyInput, 10) || 1)

  // Clear the self-trade error whenever the user edits an input so the message
  // doesn't linger after they correct the price.
  function setBidInput(v: string) {
    setBidInputRaw(v)
    if (selfTradeError) setSelfTradeError(null)
  }

  function setOfferInput(v: string) {
    setOfferInputRaw(v)
    if (selfTradeError) setSelfTradeError(null)
  }

  function setQtyInput(v: string) {
    setQtyInputRaw(v)
  }

  function submitBid(e: React.FormEvent) {
    e.preventDefault()
    const p = parseInt(bidInput, 10)
    if (isNaN(p) || p < 1 || p > 99) {
      setSelfTradeError('Enter a price between 1 and 99')
      return
    }
    // A buy at price p matches any resting sell at price ≤ p.
    // Self-trade: player has a sell order for this suit at price ≤ p.
    const conflict = myOrdersForSuit.some(o => o.side === 'sell' && o.price <= p)
    if (conflict) {
      setSelfTradeError('Cannot buy your own sell order')
      return
    }
    onSendMessage({ type: 'submit_order', suit, side: 'buy', price: p, qty: parsedQty })
    setBidInputRaw('')
    setSelfTradeError(null)
  }

  function submitOffer(e: React.FormEvent) {
    e.preventDefault()
    const p = parseInt(offerInput, 10)
    if (isNaN(p) || p < 1 || p > 99) {
      setSelfTradeError('Enter a price between 1 and 99')
      return
    }
    // A sell at price p matches any resting buy at price ≥ p.
    // Self-trade: player has a buy order for this suit at price ≥ p.
    const conflict = myOrdersForSuit.some(o => o.side === 'buy' && o.price >= p)
    if (conflict) {
      setSelfTradeError('Cannot sell to your own buy order')
      return
    }
    onSendMessage({ type: 'submit_order', suit, side: 'sell', price: p, qty: parsedQty })
    setOfferInputRaw('')
    setSelfTradeError(null)
  }

  return {
    bidInput, offerInput, qtyInput,
    setBidInput, setOfferInput, setQtyInput,
    submitBid, submitOffer,
    selfTradeError,
    parsedQty,
  }
}
