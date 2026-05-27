import { useState, useEffect } from 'react'
import { useParams } from 'react-router-dom'
import { useSpectator } from '../hooks/useSpectator'
import { RoundCountdown } from '../components/RoundCountdown'
import { MarketOverview } from '../components/MarketOverview'
import { SuitPanel } from '../components/SuitPanel'
import { TradeFeed } from '../components/TradeFeed'
import { InterRoundScreen } from '../components/InterRoundScreen'
import { GameEndScreen } from '../components/GameEndScreen'
import { SessionError } from '../components/SessionError'
import { SpectatorBadge } from '../components/SpectatorBadge'
import { ScriptLogPanel } from '../components/ScriptLogPanel'
import { slotColorSemi } from '../utils/playerColors'
import '../App.css'

const SUIT_ORDER = ['clubs', 'diamonds', 'hearts', 'spades'] as const

// AGENT-CTX: SpectatorView is a read-only game view. It uses useSpectator
// (not useWebSocket) so no sendMessage is available and no game commands can
// be sent. SuitPanel and InterRoundScreen receive isSpectator={true} to hide
// order forms and owner controls respectively.
export function SpectatorView() {
  const { lobbyCode } = useParams<{ lobbyCode: string }>()
  const {
    books,
    trades,
    startsAt,
    roundEndAt,
    playerSlot,
    interRound,
    gameEnded,
    sessionError,
    roster,
    deltas,
    allBalances,
    allHandTotals,
    spectatorCount,
    scriptLogs,
  } = useSpectator(lobbyCode ?? '')

  const [interRoundDismissed, setInterRoundDismissed] = useState(false)
  useEffect(() => {
    if (interRound) setInterRoundDismissed(false)
  }, [interRound])

  if (sessionError) return <SessionError sessionError={sessionError} />
  if (gameEnded)    return <GameEndScreen gameEnded={gameEnded} playerSlot={null} />

  const showInterRound = interRound !== null && !interRoundDismissed
  const activeSuits    = SUIT_ORDER.filter(s => s in books)

  const lastTradePrices: Record<string, number> = {}
  for (const t of trades) {
    if (!(t.suit in lastTradePrices)) lastTradePrices[t.suit] = t.price
  }

  const noop = () => {}

  return (
    <>
      {showInterRound && (
        <InterRoundScreen
          interRound={interRound!}
          playerSlot={playerSlot}
          isOwner={false}
          ownerUsername=""
          onStartNextRound={noop}
          onEndGame={noop}
          isSpectator
          onCountdownExpired={() => setInterRoundDismissed(true)}
        />
      )}
      <main className="app">
        <header className="app__header">
          <h1 className="app__title">Anjeer <span className="app__spectator-label">Spectating</span></h1>
          <SpectatorBadge count={spectatorCount} />
          <RoundCountdown startsAt={startsAt} roundEndAt={roundEndAt} />
        </header>

        <div className="app__layout">

          {/* ── LEFT: Trade feed + Script log (API lobbies only) ── */}
          <div className="app__left-col">
            <div className="panel panel--feed" data-panel-id="trade-feed">
              <div className="panel__header">
                <span className="panel__title">Trade History</span>
              </div>
              <div className="panel__body">
                <TradeFeed trades={trades} roster={roster} />
              </div>
            </div>
            {scriptLogs.length > 0 && (
              <div className="panel" data-panel-id="script-log">
                <div className="panel__body" style={{ padding: 0 }}>
                  <ScriptLogPanel logs={scriptLogs} />
                </div>
              </div>
            )}
          </div>

          {/* ── RIGHT: Market overview + suit panels ── */}
          <div className="app__right-col">
            <div className="panel" data-panel-id="market-overview">
              <div className="panel__header">
                <span className="panel__title">Market Overview</span>
              </div>
              <div className="panel__body">
                <MarketOverview
                  hand={null}
                  initialHand={null}
                  deltas={deltas}
                  roster={roster}
                  mySlot={null}
                  balance={null}
                  allBalances={allBalances}
                  allHandTotals={allHandTotals}
                />
              </div>
            </div>

            <div className="app__suits">
              {activeSuits.length === 0 ? (
                <p className="app__waiting">Waiting for book data…</p>
              ) : (
                activeSuits.map(suit => (
                  <div key={suit} className="app__suit-wrapper" data-panel-id={`suit-${suit}`}>
                    <SuitPanel
                      suit={suit}
                      book={books[suit]}
                      playerId={null}
                      error={null}
                      lastTradePrice={lastTradePrices[suit] ?? null}
                      bidPlayerColor={
                        books[suit]?.best_bid_slot != null
                          ? slotColorSemi(books[suit].best_bid_slot!) : null
                      }
                      askPlayerColor={
                        books[suit]?.best_ask_slot != null
                          ? slotColorSemi(books[suit].best_ask_slot!) : null
                      }
                      onSendMessage={noop}
                      isSpectator
                    />
                  </div>
                ))
              )}
            </div>
          </div>
        </div>
      </main>
    </>
  )
}
