import { render, screen, fireEvent } from '@testing-library/react'
import { describe, test, expect, vi } from 'vitest'
import { SuitPanel } from './SuitPanel'
import type { BookState } from '../hooks/useWebSocket'

const BOOK_EMPTY: BookState = { best_bid: null, best_ask: null }
const BOOK_FULL: BookState  = { best_bid: 45, best_ask: 55 }

function renderPanel(overrides: Partial<Parameters<typeof SuitPanel>[0]> = {}) {
  const onSendMessage = vi.fn()
  render(
    <SuitPanel
      suit="S1"
      book={BOOK_EMPTY}
      playerId={1}
      error={null}
      lastTradePrice={null}
      onSendMessage={onSendMessage}
      {...overrides}
    />
  )
  return { onSendMessage }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

describe('SuitPanel — rendering', () => {
  test('displays the suit name', () => {
    renderPanel()
    expect(screen.getByText('S1')).toBeInTheDocument()
  })

  test('shows bid price when book has a best_bid', () => {
    renderPanel({ book: BOOK_FULL })
    expect(screen.getByText('45')).toBeInTheDocument()
  })

  test('shows "—" when best_bid is null', () => {
    renderPanel({ book: BOOK_EMPTY })
    // Two em-dashes expected (bid and ask sides)
    const dashes = screen.getAllByText('—')
    expect(dashes.length).toBeGreaterThanOrEqual(1)
  })

  test('shows ask price when book has a best_ask', () => {
    renderPanel({ book: BOOK_FULL })
    expect(screen.getByText('55')).toBeInTheDocument()
  })

  test('shows last trade price when provided', () => {
    renderPanel({ lastTradePrice: 50 })
    expect(screen.getByText('50')).toBeInTheDocument()
  })

  test('displays error message when error prop is set', () => {
    renderPanel({ error: { type: 'error', code: 'PRICE_OUT_OF_RANGE', message: 'too low' } })
    expect(screen.getByText(/PRICE_OUT_OF_RANGE/)).toBeInTheDocument()
    expect(screen.getByText(/too low/)).toBeInTheDocument()
  })

  test('does not display error when error is null', () => {
    renderPanel({ error: null })
    expect(screen.queryByText(/PRICE_OUT_OF_RANGE/)).toBeNull()
  })
})

// ---------------------------------------------------------------------------
// Disabled states
// ---------------------------------------------------------------------------

describe('SuitPanel — disabled states', () => {
  test('nudge buttons are disabled when playerId is null', () => {
    renderPanel({ playerId: null })
    const nudgeButtons = screen.getAllByRole('button')
    nudgeButtons.forEach(btn => expect(btn).toBeDisabled())
  })

  test('BUY button is disabled when there is no ask', () => {
    renderPanel({ book: BOOK_EMPTY })
    expect(screen.getByRole('button', { name: /BUY/i })).toBeDisabled()
  })

  test('SELL button is disabled when there is no bid', () => {
    renderPanel({ book: BOOK_EMPTY })
    expect(screen.getByRole('button', { name: /SELL/i })).toBeDisabled()
  })

  test('BUY button is enabled when ask is present', () => {
    renderPanel({ book: BOOK_FULL })
    expect(screen.getByRole('button', { name: /BUY/i })).not.toBeDisabled()
  })

  test('SELL button is enabled when bid is present', () => {
    renderPanel({ book: BOOK_FULL })
    expect(screen.getByRole('button', { name: /SELL/i })).not.toBeDisabled()
  })
})

// ---------------------------------------------------------------------------
// Interactions
// ---------------------------------------------------------------------------

describe('SuitPanel — interactions', () => {
  test('nudge-up button sends nudge buy', () => {
    const { onSendMessage } = renderPanel({ book: BOOK_FULL })
    fireEvent.click(screen.getByTitle(/Nudge bid up/i))
    expect(onSendMessage).toHaveBeenCalledWith({ type: 'nudge', suit: 'S1', side: 'buy' })
  })

  test('nudge-down button sends nudge sell', () => {
    const { onSendMessage } = renderPanel({ book: BOOK_FULL })
    fireEvent.click(screen.getByTitle(/Nudge ask down/i))
    expect(onSendMessage).toHaveBeenCalledWith({ type: 'nudge', suit: 'S1', side: 'sell' })
  })

  test('BUY button submits order at best_ask price', () => {
    const { onSendMessage } = renderPanel({ book: BOOK_FULL })
    fireEvent.click(screen.getByRole('button', { name: /BUY/i }))
    expect(onSendMessage).toHaveBeenCalledWith({
      type: 'submit_order', suit: 'S1', side: 'buy', price: 55,
    })
  })

  test('SELL button submits order at best_bid price', () => {
    const { onSendMessage } = renderPanel({ book: BOOK_FULL })
    fireEvent.click(screen.getByRole('button', { name: /SELL/i }))
    expect(onSendMessage).toHaveBeenCalledWith({
      type: 'submit_order', suit: 'S1', side: 'sell', price: 45,
    })
  })

  test('bid price form submits buy order on Enter', () => {
    const { onSendMessage } = renderPanel()
    const input = screen.getByRole('spinbutton', { name: /Bid price for S1/i })
    fireEvent.change(input, { target: { value: '42' } })
    fireEvent.submit(input.closest('form')!)
    expect(onSendMessage).toHaveBeenCalledWith({
      type: 'submit_order', suit: 'S1', side: 'buy', price: 42,
    })
  })

  test('offer price form submits sell order on Enter', () => {
    const { onSendMessage } = renderPanel()
    const input = screen.getByRole('spinbutton', { name: /Offer price for S1/i })
    fireEvent.change(input, { target: { value: '58' } })
    fireEvent.submit(input.closest('form')!)
    expect(onSendMessage).toHaveBeenCalledWith({
      type: 'submit_order', suit: 'S1', side: 'sell', price: 58,
    })
  })

  test('bid form does not send when input is not a number', () => {
    const { onSendMessage } = renderPanel()
    const input = screen.getByRole('spinbutton', { name: /Bid price for S1/i })
    fireEvent.change(input, { target: { value: '' } })
    fireEvent.submit(input.closest('form')!)
    expect(onSendMessage).not.toHaveBeenCalled()
  })
})
