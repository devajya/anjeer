// Slot-indexed identity colours: sand, slate, terracotta, seafoam, mauve.
// Deliberately excludes red and green, which are reserved for sell/buy indicators.
// Chosen for legibility on dark backgrounds (#0f0f0f) and cohesion with the gold accent theme.
// Semi variants are rgba at 50% opacity — used for gradients and tinted backgrounds.

export const SLOT_COLORS     = ['#e8b86d', '#8ba7d4', '#c4876e', '#7dbcb5', '#b09ec0'] as const
export const SLOT_COLORS_SEMI = [
  'rgba(232, 184, 109, 0.50)',
  'rgba(139, 167, 212, 0.50)',
  'rgba(196, 135, 110, 0.50)',
  'rgba(125, 188, 181, 0.50)',
  'rgba(176, 158, 192, 0.50)',
] as const

// Dim variants for table header backgrounds (~13% opacity)
export const SLOT_COLORS_DIM = [
  'rgba(232, 184, 109, 0.13)',
  'rgba(139, 167, 212, 0.13)',
  'rgba(196, 135, 110, 0.13)',
  'rgba(125, 188, 181, 0.13)',
  'rgba(176, 158, 192, 0.13)',
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
