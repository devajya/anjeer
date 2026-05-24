import { render, screen, fireEvent } from '@testing-library/react'
import { describe, it, expect } from 'vitest'
import { EvalPanel } from '../components/EvalPanel'

describe('EvalPanel', () => {
  it('T22: is collapsed by default', () => {
    render(<EvalPanel />)
    const panel = screen.getByTestId('eval-panel')
    expect(panel).not.toHaveClass('eval-panel--expanded')
    expect(panel.dataset.expanded).toBe('false')
  })

  it('T23: clicking toggle expands the panel', () => {
    render(<EvalPanel />)
    const toggle = screen.getByTestId('eval-panel-toggle')
    fireEvent.click(toggle)
    const panel = screen.getByTestId('eval-panel')
    expect(panel).toHaveClass('eval-panel--expanded')
    expect(panel.dataset.expanded).toBe('true')
    expect(screen.getByTestId('eval-panel-body')).toBeInTheDocument()
  })

  it('T24: second click collapses the panel', () => {
    render(<EvalPanel />)
    const toggle = screen.getByTestId('eval-panel-toggle')
    fireEvent.click(toggle)
    fireEvent.click(toggle)
    const panel = screen.getByTestId('eval-panel')
    expect(panel).not.toHaveClass('eval-panel--expanded')
    expect(panel.dataset.expanded).toBe('false')
    expect(screen.queryByTestId('eval-panel-body')).not.toBeInTheDocument()
  })
})
