export const SUIT_SYMBOLS: Record<string, string> = {
  clubs: '♣', diamonds: '♦', hearts: '♥', spades: '♠',
}

export const SUIT_ORDER = ['clubs', 'diamonds', 'hearts', 'spades'] as const

/** Returns a CSS class that applies the canonical suit color (via --suit-* token). */
export function suitClass(suit: string): string {
  return `suit--${suit}`
}
