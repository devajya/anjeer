import { createContext, useContext } from 'react'
import type { ReactNode } from 'react'
import type { GameMode } from '../types/messages'

export interface GameConfig {
  mode:          GameMode
  allowMultiQty: boolean
  wipeOnTrade:   boolean
  feedTier:      'mbp1' | 'mbpn' | 'mbo'
}

const FEED_TIER: Record<GameMode, GameConfig['feedTier']> = {
  simple:       'mbp1',
  intermediate: 'mbpn',
  advanced:     'mbo',
}

const DEFAULT_CONFIG: GameConfig = {
  mode:          'simple',
  allowMultiQty: false,
  wipeOnTrade:   true,
  feedTier:      'mbp1',
}

const GameConfigContext = createContext<GameConfig>(DEFAULT_CONFIG)

export function GameConfigProvider({ mode, children }: { mode: GameMode; children: ReactNode }) {
  const config: GameConfig = {
    mode,
    allowMultiQty: mode !== 'simple',
    wipeOnTrade:   mode !== 'advanced',
    feedTier:      FEED_TIER[mode],
  }
  return <GameConfigContext.Provider value={config}>{children}</GameConfigContext.Provider>
}

export function useGameConfig(): GameConfig {
  return useContext(GameConfigContext)
}
