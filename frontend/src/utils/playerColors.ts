// Slot-indexed identity colours: yellow, grey, purple, cyan, midnight.
// Deliberately excludes red and green, which are reserved for sell/buy indicators.
// Semi variants are rgba at 50% opacity — used for gradients and tinted backgrounds.

export const SLOT_COLORS     = ['#fbbf24', '#9ca3af', '#a78bfa', '#22d3ee', '#3b82f6'] as const
export const SLOT_COLORS_SEMI = [
  'rgba(251, 191,  36, 0.50)',
  'rgba(156, 163, 175, 0.50)',
  'rgba(167, 139, 250, 0.50)',
  'rgba( 34, 211, 238, 0.50)',
  'rgba( 59, 130, 246, 0.50)',
] as const

// Dim variants for table header backgrounds (~13% opacity)
export const SLOT_COLORS_DIM = [
  'rgba(251, 191,  36, 0.13)',
  'rgba(156, 163, 175, 0.13)',
  'rgba(167, 139, 250, 0.13)',
  'rgba( 34, 211, 238, 0.13)',
  'rgba( 59, 130, 246, 0.13)',
] as const

export function slotColor(slot: number): string {
  return SLOT_COLORS[slot % SLOT_COLORS.length]
}

export function slotColorSemi(slot: number): string {
  return SLOT_COLORS_SEMI[slot % SLOT_COLORS_SEMI.length]
}

export function slotColorDim(slot: number): string {
  return SLOT_COLORS_DIM[slot % SLOT_COLORS_DIM.length]
}
