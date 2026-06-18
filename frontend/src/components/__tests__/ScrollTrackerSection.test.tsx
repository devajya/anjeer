import { render, screen } from "@testing-library/react";
import { describe, it, expect, vi, beforeAll } from "vitest";
import { ScrollTrackerSection } from "../ScrollTrackerSection";

// AGENT-CTX: GSAP and ScrollTrigger require a real browser scroll context.
// Mock the dynamic imports so useEffect resolves without error in jsdom.
vi.mock("gsap", () => ({
  gsap: { registerPlugin: vi.fn(), to: vi.fn() },
}));

vi.mock("gsap/ScrollTrigger", () => ({
  ScrollTrigger: {
    create: vi.fn(() => ({ kill: vi.fn() })),
    getAll: vi.fn(() => []),
  },
}));

beforeAll(() => {
  // ResizeObserver not in jsdom
  global.ResizeObserver = class {
    observe() {}
    unobserve() {}
    disconnect() {}
  };
});

describe("ScrollTrackerSection", () => {
  it("renders 4 sub-sections with correct headline text", () => {
    render(<ScrollTrackerSection reducedMotion={false} />);
    expect(screen.getByText("Play in your browser")).toBeInTheDocument();
    expect(screen.getByText("Script from the terminal")).toBeInTheDocument();
    expect(screen.getByText("Spectate any game")).toBeInTheDocument();
    expect(screen.getByText("Play against bots")).toBeInTheDocument();
  });

  it("renders all 4 inline SVGs", () => {
    const { container } = render(
      <ScrollTrackerSection reducedMotion={false} />,
    );
    const svgs = container.querySelectorAll("svg.scroll-tracker-svg");
    expect(svgs).toHaveLength(4);
  });

  it("indicator element exists in DOM", () => {
    const { container } = render(
      <ScrollTrackerSection reducedMotion={false} />,
    );
    const indicator = container.querySelector(".scroll-tracker-indicator");
    expect(indicator).toBeInTheDocument();
  });

  it("when reducedMotion=true renders static grid without the animated indicator", () => {
    const { container } = render(<ScrollTrackerSection reducedMotion={true} />);
    // Static grid is present
    expect(
      container.querySelector(".scroll-tracker-static-grid"),
    ).toBeInTheDocument();
    // Animated indicator is NOT present (reduced-motion uses static path)
    expect(
      container.querySelector(".scroll-tracker-indicator"),
    ).not.toBeInTheDocument();
    // All 4 headlines still in DOM
    expect(screen.getByText("Play in your browser")).toBeInTheDocument();
  });
});
