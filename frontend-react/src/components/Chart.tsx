import { useEffect, useRef } from 'react';
import {
  createChart,
  IChartApi,
  ISeriesApi,
  CandlestickData,
  ColorType,
} from 'lightweight-charts';
import { useStore, OHLCBar } from '../store/useStore';

function toChartBar(b: OHLCBar): CandlestickData {
  return {
    time: (b.t / 1000) as CandlestickData['time'],
    open:  b.o,
    high:  b.h,
    low:   b.l,
    close: b.c,
  };
}

interface ChartProps {
  /* Optional override — if omitted, falls back to the selected symbol
   * (or the legacy global `symbol` for standalone use). */
  symbol?: string;
  /* Compact header — for the Tick Evaluation slot alongside MC/Vol. */
  compact?: boolean;
}

export default function Chart({ symbol: propSymbol, compact }: ChartProps = {}) {
  const containerRef = useRef<HTMLDivElement>(null);
  const chartRef     = useRef<IChartApi | null>(null);
  const seriesRef    = useRef<ISeriesApi<'Candlestick'> | null>(null);

  const { barsBySymbol, selectedSymbol, quotes } = useStore();

  const symbol = propSymbol ?? selectedSymbol ?? undefined;
  const bars   = symbol ? (barsBySymbol[symbol] ?? []) : [];
  const quote  = symbol ? quotes[symbol] : undefined;

  /* Create chart once */
  useEffect(() => {
    if (!containerRef.current) return;

    const chart = createChart(containerRef.current, {
      layout: {
        background: { type: ColorType.Solid, color: '#0f1117' },
        textColor: '#8b949e',
      },
      grid: {
        vertLines: { color: '#21262d' },
        horzLines: { color: '#21262d' },
      },
      crosshair: { mode: 1 },
      rightPriceScale: { borderColor: '#30363d' },
      timeScale: { borderColor: '#30363d', timeVisible: true },
      width:  containerRef.current.clientWidth,
      height: containerRef.current.clientHeight,
    });

    const series = chart.addCandlestickSeries({
      upColor:   '#3fb950',
      downColor: '#f85149',
      borderUpColor:   '#3fb950',
      borderDownColor: '#f85149',
      wickUpColor:   '#3fb950',
      wickDownColor: '#f85149',
    });

    chartRef.current  = chart;
    seriesRef.current = series;

    const ro = new ResizeObserver(() => {
      chart.applyOptions({
        width:  containerRef.current!.clientWidth,
        height: containerRef.current!.clientHeight,
      });
    });
    ro.observe(containerRef.current);

    return () => {
      ro.disconnect();
      chart.remove();
    };
  }, []);

  /* Update data whenever the resolved symbol's bars change */
  useEffect(() => {
    if (!seriesRef.current) return;
    if (bars.length === 0) {
      seriesRef.current.setData([]);
      return;
    }
    const sorted = [...bars].sort((a, b) => a.t - b.t);
    seriesRef.current.setData(sorted.map(toChartBar));
    chartRef.current?.timeScale().fitContent();
  }, [bars]);

  /* Real-time tick update — folds the latest quote into the last bar */
  useEffect(() => {
    if (!seriesRef.current || !quote) return;
    const last = bars[bars.length - 1];
    if (!last) return;
    seriesRef.current.update({
      time:  (last.t / 1000) as CandlestickData['time'],
      open:  last.o,
      high:  Math.max(last.h, quote.price),
      low:   Math.min(last.l,  quote.price),
      close: quote.price,
    });
  }, [quote, bars]);

  const pct = quote && bars.length
    ? ((quote.price - bars[0].o) / bars[0].o * 100).toFixed(2)
    : null;

  return (
    <div className="flex flex-col h-full bg-panel border border-border rounded overflow-hidden">
      <div className={`flex items-baseline gap-3 border-b border-border ${
        compact ? 'px-3 py-1.5' : 'px-3 py-2'
      }`}>
        {compact ? (
          <>
            <span className="text-xs text-subtle uppercase tracking-widest font-bold">
              Candlestick
            </span>
            {symbol && (
              <span className="text-accent text-xs font-mono">· {symbol}</span>
            )}
            {quote && (
              <span className="text-ink text-xs font-mono ml-auto">
                ${quote.price.toFixed(2)}
                {pct !== null && (
                  <span className={parseFloat(pct) >= 0 ? 'text-bull ml-2' : 'text-bear ml-2'}>
                    {parseFloat(pct) >= 0 ? '+' : ''}{pct}%
                  </span>
                )}
              </span>
            )}
          </>
        ) : (
          <>
            <span className="text-ink font-bold text-lg">{symbol ?? '—'}</span>
            {quote && (
              <>
                <span className="text-ink text-2xl font-mono">${quote.price.toFixed(2)}</span>
                {pct !== null && (
                  <span className={parseFloat(pct) >= 0 ? 'text-bull' : 'text-bear'}>
                    {parseFloat(pct) >= 0 ? '+' : ''}{pct}% YTD
                  </span>
                )}
                <span className="text-subtle text-xs ml-auto">
                  Bid {quote.bid.toFixed(2)} · Ask {quote.ask.toFixed(2)}
                </span>
              </>
            )}
            {!quote && <span className="text-subtle text-sm">Awaiting quote…</span>}
          </>
        )}
      </div>
      <div className="relative">
        <div
          ref={containerRef}
          className="block w-full"
          style={{ height: 260, background: '#0a0e12' }}
        />
        {!symbol && (
          <div className="absolute inset-0 flex items-center justify-center text-xs italic text-subtle">
            Click a symbol to load its candlestick.
          </div>
        )}
        {symbol && bars.length === 0 && (
          <div className="absolute inset-0 flex items-center justify-center text-xs italic text-subtle">
            Loading history…
          </div>
        )}
      </div>
    </div>
  );
}
