import { renderHook } from '@testing-library/react'
import { describe, test, expect, vi } from 'vitest'
import { useKeyboardShortcuts, serializeCombo } from '../useKeyboardShortcuts'
import { DEFAULT_BINDS } from '../useKeyBinds'

function makeOpts(overrides: Partial<Parameters<typeof useKeyboardShortcuts>[0]> = {}) {
  return {
    binds: DEFAULT_BINDS,
    enabled: true,
    onSuitFocus:       vi.fn(),
    onSubmitBuy:       vi.fn(),
    onSubmitSell:      vi.fn(),
    onNudgeBuy:        vi.fn(),
    onNudgeSell:       vi.fn(),
    onCancelBestBuy:   vi.fn(),
    onCancelBestSell:  vi.fn(),
    onToggleShortcuts: vi.fn(),
    ...overrides,
  }
}

function fireKey(key: string, modifiers: Partial<KeyboardEventInit> = {}) {
  window.dispatchEvent(new KeyboardEvent('keydown', { key, bubbles: true, ...modifiers }))
}

describe('serializeCombo', () => {
  test('plain key returns key verbatim', () => {
    const e = new KeyboardEvent('keydown', { key: 'c' })
    expect(serializeCombo(e)).toBe('c')
  })

  test('ctrl+z returns Control+z', () => {
    const e = new KeyboardEvent('keydown', { key: 'z', ctrlKey: true })
    expect(serializeCombo(e)).toBe('Control+z')
  })

  test('modifier-only press returns empty string', () => {
    const e = new KeyboardEvent('keydown', { key: 'Control', ctrlKey: true })
    expect(serializeCombo(e)).toBe('')
  })

  test('multiple modifiers are ordered Alt < Control < Shift', () => {
    const e = new KeyboardEvent('keydown', { key: 'k', ctrlKey: true, shiftKey: true })
    expect(serializeCombo(e)).toBe('Control+Shift+k')
  })
})

describe('useKeyboardShortcuts — suit focus', () => {
  test('c fires onSuitFocus("clubs")', () => {
    const opts = makeOpts()
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('c')
    expect(opts.onSuitFocus).toHaveBeenCalledWith('clubs')
  })

  test('d fires onSuitFocus("diamonds")', () => {
    const opts = makeOpts()
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('d')
    expect(opts.onSuitFocus).toHaveBeenCalledWith('diamonds')
  })

  test('h fires onSuitFocus("hearts")', () => {
    const opts = makeOpts()
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('h')
    expect(opts.onSuitFocus).toHaveBeenCalledWith('hearts')
  })

  test('s fires onSuitFocus("spades")', () => {
    const opts = makeOpts()
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('s')
    expect(opts.onSuitFocus).toHaveBeenCalledWith('spades')
  })
})

describe('useKeyboardShortcuts — order actions', () => {
  test('b fires onSubmitBuy', () => {
    const opts = makeOpts()
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('b')
    expect(opts.onSubmitBuy).toHaveBeenCalledOnce()
  })

  test('a fires onSubmitSell', () => {
    const opts = makeOpts()
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('a')
    expect(opts.onSubmitSell).toHaveBeenCalledOnce()
  })

  test('ArrowUp fires onNudgeBuy', () => {
    const opts = makeOpts()
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('ArrowUp')
    expect(opts.onNudgeBuy).toHaveBeenCalledOnce()
  })

  test('ArrowDown fires onNudgeSell', () => {
    const opts = makeOpts()
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('ArrowDown')
    expect(opts.onNudgeSell).toHaveBeenCalledOnce()
  })

  test('Ctrl+z fires onCancelBestBuy', () => {
    const opts = makeOpts()
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('z', { ctrlKey: true })
    expect(opts.onCancelBestBuy).toHaveBeenCalledOnce()
  })

  test('Ctrl+x fires onCancelBestSell', () => {
    const opts = makeOpts()
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('x', { ctrlKey: true })
    expect(opts.onCancelBestSell).toHaveBeenCalledOnce()
  })

  test('? fires onToggleShortcuts', () => {
    const opts = makeOpts()
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('?')
    expect(opts.onToggleShortcuts).toHaveBeenCalledOnce()
  })
})

describe('useKeyboardShortcuts — enabled guard', () => {
  test('no callbacks fire when enabled=false', () => {
    const opts = makeOpts({ enabled: false })
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('c')
    fireKey('b')
    fireKey('ArrowUp')
    expect(opts.onSuitFocus).not.toHaveBeenCalled()
    expect(opts.onSubmitBuy).not.toHaveBeenCalled()
    expect(opts.onNudgeBuy).not.toHaveBeenCalled()
  })
})

describe('useKeyboardShortcuts — custom binds', () => {
  test('remapped action uses new key combo', () => {
    const opts = makeOpts({
      binds: { ...DEFAULT_BINDS, suit_clubs: 'q' },
    })
    renderHook(() => useKeyboardShortcuts(opts))
    fireKey('q')
    expect(opts.onSuitFocus).toHaveBeenCalledWith('clubs')
    // original default 'c' should no longer trigger clubs
    fireKey('c')
    expect(opts.onSuitFocus).toHaveBeenCalledTimes(1)
  })
})

describe('useKeyboardShortcuts — input suppression', () => {
  test('does not fire when key pressed inside an INPUT', () => {
    const opts = makeOpts()
    renderHook(() => useKeyboardShortcuts(opts))
    const input = document.createElement('input')
    document.body.appendChild(input)
    input.dispatchEvent(new KeyboardEvent('keydown', { key: 'b', bubbles: true }))
    expect(opts.onSubmitBuy).not.toHaveBeenCalled()
    document.body.removeChild(input)
  })
})
