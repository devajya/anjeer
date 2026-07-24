import { useState, memo } from 'react'
import type { EvalPosteriorUpdateMessage, EvalAccumulationSignalMessage, EvalExecutionGuidanceMessage } from '../types/messages'
import { PosteriorDisplay } from './PosteriorDisplay'
import { SettlementEV } from './SettlementEV'
import { AccumulationDisplay } from './AccumulationDisplay'
import { ExecutionGuidanceDisplay } from './ExecutionGuidanceDisplay'
import './EvalPanel.css'

interface EvalPanelProps {
  children?: React.ReactNode
  onExpandedChange?: (expanded: boolean) => void
  posteriorUpdate?: EvalPosteriorUpdateMessage | null
  accumulationSignal?: EvalAccumulationSignalMessage | null
  executionGuidance?: EvalExecutionGuidanceMessage | null
}

export const EvalPanel = memo(function EvalPanel({ children, onExpandedChange, posteriorUpdate = null, accumulationSignal = null, executionGuidance = null }: EvalPanelProps) {
  const [expanded, setExpanded] = useState(true)

  function toggle() {
    const next = !expanded
    setExpanded(next)
    onExpandedChange?.(next)
  }

  return (
    <div
      className={`eval-panel${expanded ? ' eval-panel--expanded' : ''}`}
      data-testid="eval-panel"
      data-expanded={expanded}
    >
      <button
        className="eval-panel__toggle"
        onClick={toggle}
        aria-label={expanded ? 'Collapse eval panel' : 'Expand eval panel'}
        data-testid="eval-panel-toggle"
      >
        <span className="eval-panel__toggle-label">Eval</span>
        <span className="eval-panel__toggle-chevron" aria-hidden="true">
          {expanded ? '›' : '‹'}
        </span>
      </button>
      {expanded && (
        <div className="eval-panel__body" data-testid="eval-panel-body">
          <SettlementEV posteriorUpdate={posteriorUpdate} />
          <div className="eval-panel__divider" />
          <PosteriorDisplay posteriorUpdate={posteriorUpdate} />
          <div className="eval-panel__divider" />
          <AccumulationDisplay accumulationSignal={accumulationSignal} />
          <div className="eval-panel__divider" />
          <ExecutionGuidanceDisplay executionGuidance={executionGuidance} />
          {children}
        </div>
      )}
    </div>
  )
})
