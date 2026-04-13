import { useState } from 'react'
import type { ClientCommand } from '../types/messages'

export interface OrderFormState {
  bidInput: string
  offerInput: string
  setBidInput: (v: string) => void
  setOfferInput: (v: string) => void
  submitBid: (e: React.FormEvent) => void
  submitOffer: (e: React.FormEvent) => void
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
): OrderFormState {
  const [bidInput, setBidInput] = useState('')
  const [offerInput, setOfferInput] = useState('')

  function submitBid(e: React.FormEvent) {
    e.preventDefault()
    const p = parseInt(bidInput, 10)
    if (isNaN(p)) return
    onSendMessage({ type: 'submit_order', suit, side: 'buy', price: p })
    setBidInput('')
  }

  function submitOffer(e: React.FormEvent) {
    e.preventDefault()
    const p = parseInt(offerInput, 10)
    if (isNaN(p)) return
    onSendMessage({ type: 'submit_order', suit, side: 'sell', price: p })
    setOfferInput('')
  }

  return { bidInput, offerInput, setBidInput, setOfferInput, submitBid, submitOffer }
}
