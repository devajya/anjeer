import { createContext, useContext } from 'react'
import type { ReactNode } from 'react'
import type { GameMode } from '../types/messages'

export interface GameConfig {
  mode:          GameMode
  allowMultiQty: boolean
  wipeOnTrade:   boolean
}

const DEFAULT_CONFIG: GameConfig = {
  mode:          'simple',
  allowMultiQty: false,
  wipeOnTrade:   true,
}

const GameConfigContext = createContext<GameConfig>(DEFAULT_CONFIG)

export function deriveGameConfig(mode: GameMode): GameConfig {
  return {
    mode,
    allowMultiQty: mode !== 'simple',
    wipeOnTrade:   mode !== 'advanced',
  }
}

export function GameConfigProvider({ mode, children }: { mode: GameMode; children: ReactNode }) {
  return <GameConfigContext.Provider value={deriveGameConfig(mode)}>{children}</GameConfigContext.Provider>
}

export function useGameConfig(): GameConfig {
  return useContext(GameConfigContext)
}
