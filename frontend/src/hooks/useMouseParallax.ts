import { useEffect, useRef, useState, RefObject } from 'react';

export interface MouseParallaxState {
  normX: number;
  normY: number;
}

export function useMouseParallax(
  containerRef: RefObject<HTMLElement>,
  enabled: boolean,
): MouseParallaxState {
  const [state, setState] = useState<MouseParallaxState>({ normX: 0, normY: 0 });
  const rafId = useRef<number | null>(null);
  const pending = useRef<MouseParallaxState | null>(null);

  useEffect(() => {
    if (!enabled) return;

    function onMove(e: MouseEvent) {
      const el = containerRef.current ?? document.documentElement;
      const rect = el.getBoundingClientRect();
      const cx = rect.left + rect.width / 2;
      const cy = rect.top + rect.height / 2;
      const hw = rect.width / 2 || window.innerWidth / 2;
      const hh = rect.height / 2 || window.innerHeight / 2;
      pending.current = {
        normX: Math.max(-1, Math.min(1, (e.clientX - cx) / hw)),
        normY: Math.max(-1, Math.min(1, (e.clientY - cy) / hh)),
      };
      if (rafId.current === null) {
        rafId.current = requestAnimationFrame(() => {
          rafId.current = null;
          if (pending.current) setState(pending.current);
        });
      }
    }

    window.addEventListener('mousemove', onMove);
    return () => {
      window.removeEventListener('mousemove', onMove);
      if (rafId.current !== null) cancelAnimationFrame(rafId.current);
      rafId.current = null;
      setState({ normX: 0, normY: 0 });
    };
  }, [enabled, containerRef]);

  return state;
}
