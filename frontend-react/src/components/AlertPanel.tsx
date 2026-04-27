import { useState } from 'react';
import { useStore } from '../store/useStore';

export default function AlertPanel() {
  const { alerts, addAlert, removeAlert, symbol } = useStore();
  const [sym, setSym]   = useState(symbol);
  const [cond, setCond] = useState<'above' | 'below'>('above');
  const [price, setPrice] = useState('');

  function submit() {
    const p = parseFloat(price);
    if (!sym || isNaN(p)) return;
    addAlert(sym, cond, p);
    setPrice('');
  }

  return (
    <div className="bg-panel border border-border rounded p-3 space-y-3">
      <div className="text-xs text-subtle uppercase tracking-widest font-bold">Price Alerts</div>

      <div className="space-y-1 max-h-28 overflow-y-auto">
        {alerts.length === 0 && <div className="text-ink text-xs">No alerts set</div>}
        {alerts.map((a) => (
          <div key={a.id} className="flex items-center justify-between text-xs font-mono">
            <span className="text-accent w-12 font-bold">{a.symbol}</span>
            <span className={a.condition === 'above' ? 'text-bull' : 'text-bear'}>
              {a.condition} ${a.price.toFixed(2)}
            </span>
            <button onClick={() => removeAlert(a.id)} className="text-ink hover:text-bear">✕</button>
          </div>
        ))}
      </div>

      <div className="border-t border-border pt-2 grid grid-cols-3 gap-1">
        <input
          placeholder="SYM" value={sym} onChange={(e) => setSym(e.target.value.toUpperCase())}
          className="bg-surface border border-border text-ink text-xs px-2 py-1 rounded focus:outline-none focus:border-accent"
        />
        <select
          value={cond} onChange={(e) => setCond(e.target.value as 'above' | 'below')}
          className="bg-surface border border-border text-ink text-xs px-2 py-1 rounded focus:outline-none"
        >
          <option value="above">Above</option>
          <option value="below">Below</option>
        </select>
        <input
          placeholder="Price" type="number" value={price} onChange={(e) => setPrice(e.target.value)}
          className="bg-surface border border-border text-ink text-xs px-2 py-1 rounded focus:outline-none focus:border-accent"
        />
        <button
          onClick={submit}
          className="col-span-3 bg-surface border border-border text-accent text-xs font-bold py-1 rounded hover:border-accent transition"
        >+ Set Alert</button>
      </div>
    </div>
  );
}
