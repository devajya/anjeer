import { render, fireEvent, act } from '@testing-library/react'
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { AutoAdvanceProgress } from '../AutoAdvanceProgress'

// Helpers to mock the section geometry so getProgress() returns a value
function mockSectionGeometry(container: HTMLElement, progressFraction: number) {
  const section = container.querySelector('.auto-advance-progress') as HTMLElement
  const viewH = window.innerHeight || 768
  const sectionH = 4 * viewH
  const scrollable = sectionH - viewH
  // rect.top = -(progressFraction * scrollable) so getProgress returns progressFraction
  const rectTop = -(progressFraction * scrollable)

  vi.spyOn(section, 'getBoundingClientRect').mockReturnValue({
    top: rectTop, bottom: rectTop + sectionH,
    height: sectionH, width: 1024, left: 0, right: 1024, x: 0, y: rectTop,
    toJSON: () => ({}),
  } as DOMRect)
  Object.defineProperty(section, 'offsetHeight', { value: sectionH, configurable: true })
}

describe('AutoAdvanceProgress', () => {
  beforeEach(() => { vi.useFakeTimers() })
  afterEach(() => { vi.useRealTimers(); vi.restoreAllMocks() })

  it('all 4 blocks are present in DOM', () => {
    const { container } = render(<AutoAdvanceProgress reducedMotion={false} />)
    expect(container.querySelectorAll('.aap-block')).toHaveLength(4)
  })

  it('scroll event at 30% progress → block 1 active (Step 2)', () => {
    const { container } = render(<AutoAdvanceProgress reducedMotion={false} />)
    // 30% of 4 blocks = 1.2 → block index 1, pct 20%
    mockSectionGeometry(container, 0.30)

    act(() => { window.dispatchEvent(new Event('scroll')) })

    expect(container.querySelector('.aap-block--active')?.textContent).toContain('Step 2')
  })

  it('clicking an inactive block immediately switches activeIndex', () => {
    const { container } = render(<AutoAdvanceProgress reducedMotion={false} />)
    const blocks = container.querySelectorAll('.aap-block')

    act(() => { fireEvent.click(blocks[2]) })

    expect(container.querySelector('.aap-block--active')?.textContent).toContain('Step 3')
  })

  it('after click, progressPct resets to 0', () => {
    const { container } = render(<AutoAdvanceProgress reducedMotion={false} />)
    // Scroll to 15% progress so block 0 is at ~60% fill
    mockSectionGeometry(container, 0.15)
    act(() => { window.dispatchEvent(new Event('scroll')) })

    const blocks = container.querySelectorAll('.aap-block')
    act(() => { fireEvent.click(blocks[1]) })

    const fill = container.querySelector('.aap-progress-fill') as HTMLElement
    expect(fill?.style.width).toBe('0%')
  })

  it('when reducedMotion=true, no Canvas in DOM and block 0 is expanded', () => {
    const { container } = render(<AutoAdvanceProgress reducedMotion={true} />)

    expect(container.querySelector('canvas')).toBeNull()

    const activeBlock = container.querySelector('.aap-block--active')
    expect(activeBlock?.textContent).toContain('Step 1')
  })
})
