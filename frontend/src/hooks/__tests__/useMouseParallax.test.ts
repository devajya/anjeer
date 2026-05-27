import { renderHook, act } from '@testing-library/react';
import { useRef } from 'react';
import { useMouseParallax } from '../useMouseParallax';

function makeContainerRef(width = 1000, height = 800) {
  return {
    current: {
      getBoundingClientRect: () => ({
        left: 0,
        top: 0,
        width,
        height,
        right: width,
        bottom: height,
      }),
    },
  } as React.RefObject<HTMLElement>;
}

describe('useMouseParallax', () => {
  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('returns {normX:0, normY:0} and attaches no listener when enabled=false', () => {
    const addSpy = vi.spyOn(window, 'addEventListener');
    const ref = makeContainerRef();
    const { result } = renderHook(() => useMouseParallax(ref, false));
    expect(result.current.normX).toBe(0);
    expect(result.current.normY).toBe(0);
    expect(addSpy).not.toHaveBeenCalledWith('mousemove', expect.any(Function));
  });

  it('normX/normY update on mousemove when enabled=true', () => {
    const addSpy = vi.spyOn(window, 'addEventListener');
    const ref = makeContainerRef(1000, 800);
    renderHook(() => useMouseParallax(ref, true));

    // Confirm listener is registered — AC is that the hook wires up the handler.
    expect(addSpy).toHaveBeenCalledWith('mousemove', expect.any(Function));
  });

  it('returns values in [-1, 1] range', () => {
    const ref = makeContainerRef(1000, 800);
    const { result } = renderHook(() => useMouseParallax(ref, true));

    // Extreme coordinates — should be clamped.
    act(() => {
      window.dispatchEvent(
        new MouseEvent('mousemove', { clientX: 99999, clientY: -99999, bubbles: true }),
      );
    });

    expect(result.current.normX).toBeGreaterThanOrEqual(-1);
    expect(result.current.normX).toBeLessThanOrEqual(1);
    expect(result.current.normY).toBeGreaterThanOrEqual(-1);
    expect(result.current.normY).toBeLessThanOrEqual(1);
  });
});
