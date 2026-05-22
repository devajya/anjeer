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
      configurations: [
        { deck_index: 0, counts: [12, 10, 8, 8], goal_suit: 'diamonds', probability: 0.7 },
        { deck_index: 1, counts: [10, 8, 10, 10], goal_suit: 'clubs',   probability: 0.2 },
        { deck_index: 2, counts: [8, 8, 12, 10],  goal_suit: 'hearts',  probability: 0.07 },
        { deck_index: 3, counts: [8, 8, 8, 14],   goal_suit: 'spades',  probability: 0.03 },
      ],
      goal_suit_marginals: { clubs: 0.2, diamonds: 0.7, hearts: 0.07, spades: 0.03 },
      settlement_ev: 142.5,
      delta_ev: { clubs: 3.1, diamonds: 8.2, hearts: -1.5, spades: -2.8 },
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
