import { useState, useCallback } from 'react'
import type {
  ServerMessage,
  EvalPosteriorUpdateMessage,
  EvalAccumulationSignalMessage,
  EvalExecutionGuidanceMessage,
} from '../types/messages'

export interface EvalMetricsState {
  posteriorUpdate:    EvalPosteriorUpdateMessage    | null
  accumulationSignal: EvalAccumulationSignalMessage | null
  executionGuidance:  EvalExecutionGuidanceMessage  | null
}

// AGENT-CTX: handleMessage is returned so callers (e.g. Game.tsx) can wire it
// into the useWebSocket dispatch path for eval_* message types. The hook owns
// no WS connection — it is a pure subscriber over whatever message source the
// caller provides. State fields hold the most-recent message of each type; prior
// values are overwritten on each new signal (no accumulation by design — the
// eval pipeline sends deltas, not history).
export function useEvalMetrics(): EvalMetricsState & {
  handleMessage: (msg: ServerMessage) => void
} {
  const [state, setState] = useState<EvalMetricsState>({
    posteriorUpdate:    null,
    accumulationSignal: null,
    executionGuidance:  null,
  })

  const handleMessage = useCallback((msg: ServerMessage) => {
    switch (msg.type) {
      case 'eval_posterior_update':
        setState(s => ({ ...s, posteriorUpdate: msg }))
        break
      case 'eval_accumulation_signal':
        setState(s => ({ ...s, accumulationSignal: msg }))
        break
      case 'eval_execution_guidance':
        setState(s => ({ ...s, executionGuidance: msg }))
        break
      default:
        break
    }
  }, [])

  return { ...state, handleMessage }
}
