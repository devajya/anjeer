import { renderHook, act } from '@testing-library/react'
import { describe, test, expect } from 'vitest'
import { useEvalMetrics } from '../hooks/useEvalMetrics'
import type {
  EvalPosteriorUpdateMessage,
  EvalAccumulationSignalMessage,
  EvalExecutionGuidanceMessage,
} from '../types/messages'

// T25 — hook initialises to null state for all three signal fields.
describe('useEvalMetrics — initial state', () => {
  test('T25: all fields are null on mount', () => {
    const { result } = renderHook(() => useEvalMetrics())
    expect(result.current.posteriorUpdate).toBeNull()
    expect(result.current.accumulationSignal).toBeNull()
    expect(result.current.executionGuidance).toBeNull()
  })
})

// T26 — dispatching eval_posterior_update populates posteriorUpdate.
describe('useEvalMetrics — EvalPosteriorUpdate', () => {
  test('T26: stores latest eval_posterior_update message', () => {
    const { result } = renderHook(() => useEvalMetrics())

    const msg: EvalPosteriorUpdateMessage = {
      type: 'eval_posterior_update',
      player_slot: 2,
      posteriors: { clubs: 0.1, diamonds: 0.7, hearts: 0.1, spades: 0.1 },
      round: 1,
    }

    act(() => { result.current.handleMessage(msg) })

    expect(result.current.posteriorUpdate).toEqual(msg)
    expect(result.current.accumulationSignal).toBeNull()
    expect(result.current.executionGuidance).toBeNull()
  })
})

// T27 — dispatching eval_accumulation_signal populates accumulationSignal.
describe('useEvalMetrics — EvalAccumulationSignal', () => {
  test('T27: stores latest eval_accumulation_signal message', () => {
    const { result } = renderHook(() => useEvalMetrics())

    const msg: EvalAccumulationSignalMessage = {
      type: 'eval_accumulation_signal',
      player_slot: 0,
      suit: 'diamonds',
      net_count: 3,
      confidence: 0.85,
    }

    act(() => { result.current.handleMessage(msg) })

    expect(result.current.accumulationSignal).toEqual(msg)
    expect(result.current.posteriorUpdate).toBeNull()
    expect(result.current.executionGuidance).toBeNull()
  })
})

// T28 — dispatching eval_execution_guidance populates executionGuidance.
describe('useEvalMetrics — EvalExecutionGuidance', () => {
  test('T28: stores latest eval_execution_guidance message', () => {
    const { result } = renderHook(() => useEvalMetrics())

    const msg: EvalExecutionGuidanceMessage = {
      type: 'eval_execution_guidance',
      player_slot: 1,
      action: 'buy',
      suit: 'clubs',
      price: 75,
    }

    act(() => { result.current.handleMessage(msg) })

    expect(result.current.executionGuidance).toEqual(msg)
    expect(result.current.posteriorUpdate).toBeNull()
    expect(result.current.accumulationSignal).toBeNull()
  })
})
