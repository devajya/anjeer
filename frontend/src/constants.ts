export const MAX_TRADE_HISTORY = 20

/** Per-suit MBO event log cap. Display-only — the live order book is maintained
 *  incrementally in useWsReducer and is not bounded by this window. */
export const MBO_LOG_CAP = 100
