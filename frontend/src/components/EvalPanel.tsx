import { useState } from 'react'
import './EvalPanel.css'

interface EvalPanelProps {
  children?: React.ReactNode
  onExpandedChange?: (expanded: boolean) => void
}

export function EvalPanel({ children, onExpandedChange }: EvalPanelProps) {
  const [expanded, setExpanded] = useState(false)

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
          {children}
        </div>
      )}
    </div>
  )
}
