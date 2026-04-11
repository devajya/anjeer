// AGENT-CTX: Single source of truth for all WebSocket message shapes.
// Every message the server sends must be represented here.
// Discriminated union on `type` — add new variants as slices introduce new events.
// Do not add UI-only data here; this file mirrors the wire protocol exactly.

export interface HeartbeatMessage {
  type: 'heartbeat'
  /** Unix timestamp in milliseconds from the server clock. */
  server_ts: number
}

// AGENT-CTX: ServerMessage is the union of all server-to-client message types.
// Pattern: switch (msg.type) { case 'heartbeat': ... } exhausts the union.
export type ServerMessage = HeartbeatMessage
