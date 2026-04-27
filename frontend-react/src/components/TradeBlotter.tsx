import { useStore } from '../store/useStore';

export default function TradeBlotter() {
  const { blotter } = useStore();

  return (
    <div className="bg-panel border border-border rounded p-3 space-y-2">
      <div className="text-xs text-subtle uppercase tracking-widest font-bold">Trade Blotter</div>
      <div className="overflow-y-auto max-h-44">
        {blotter.length === 0 && (
          <div className="text-ink text-xs">No fills yet</div>
        )}
        <table className="w-full text-xs font-mono">
          <thead>
            <tr className="text-subtle border-b border-border">
              <th className="text-left pb-1">Time</th>
              <th className="text-left pb-1">Sym</th>
              <th className="text-left pb-1">Side</th>
              <th className="text-right pb-1">Qty</th>
              <th className="text-right pb-1">Price</th>
              <th className="text-left pb-1 pl-2">Strategy</th>
            </tr>
          </thead>
          <tbody>
            {blotter.map((e) => (
              <tr key={e.id} className="border-b border-border/50">
                <td className="py-0.5 text-subtle">
                  {new Date(e.ts).toLocaleTimeString()}
                </td>
                <td className="text-accent font-bold">{e.symbol}</td>
                <td className={e.side === 'buy' ? 'text-bull' : 'text-bear'}>
                  {e.side.toUpperCase()}
                </td>
                <td className="text-right">{e.quantity}</td>
                <td className="text-right">${e.price.toFixed(2)}</td>
                <td className="text-subtle pl-2">{e.strategy}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}
