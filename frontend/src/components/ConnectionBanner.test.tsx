import { render, screen } from '@testing-library/react'
import { describe, test, expect } from 'vitest'
import { ConnectionBanner } from './ConnectionBanner'

// ---------------------------------------------------------------------------
// AC: Client displays connection status (connected / disconnected)
// ---------------------------------------------------------------------------

describe('ConnectionBanner — connection status', () => {
  test('renders "Disconnected" when connected=false', () => {
    render(<ConnectionBanner connected={false} />)
    expect(screen.getByTestId('status')).toHaveTextContent('Disconnected')
  })

  test('renders "Connected" when connected=true', () => {
    render(<ConnectionBanner connected={true} />)
    expect(screen.getByTestId('status')).toHaveTextContent('Connected')
  })
})
