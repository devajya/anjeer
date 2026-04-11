import { render, screen } from '@testing-library/react'
import { describe, test, expect } from 'vitest'
import { ConnectionBanner } from './ConnectionBanner'

// ---------------------------------------------------------------------------
// AC: Client displays connection status (connected / disconnected)
// AC: Client displays last received timestamp
// ---------------------------------------------------------------------------

describe('ConnectionBanner — connection status', () => {
  test('renders "Disconnected" when connected=false', () => {
    render(<ConnectionBanner connected={false} lastServerTs={null} />)
    expect(screen.getByTestId('status')).toHaveTextContent('Disconnected')
  })

  test('renders "Connected" when connected=true', () => {
    render(<ConnectionBanner connected={true} lastServerTs={null} />)
    expect(screen.getByTestId('status')).toHaveTextContent('Connected')
  })
})

describe('ConnectionBanner — timestamp display', () => {
  test('renders "—" when lastServerTs is null', () => {
    render(<ConnectionBanner connected={false} lastServerTs={null} />)
    expect(screen.getByTestId('timestamp')).toHaveTextContent('—')
  })

  test('renders a formatted date string when lastServerTs is set', () => {
    // 2024-01-15T10:30:00.000Z in milliseconds
    const ts = new Date('2024-01-15T10:30:00.000Z').getTime()
    render(<ConnectionBanner connected={true} lastServerTs={ts} />)
    // The component formats using toLocaleString() — we just assert it's not "—"
    // and is a non-empty string (locale formatting varies by environment).
    const el = screen.getByTestId('timestamp')
    expect(el).not.toHaveTextContent('—')
    expect(el.textContent).not.toBe('')
  })
})
