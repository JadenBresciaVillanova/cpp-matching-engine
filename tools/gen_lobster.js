// Generate realistic LOBSTER-format sample data files.
// LOBSTER message format (CSV, no header):
//   timestamp, event_type, order_id, size, price, direction
//
// Event types: 1=Submit, 2=Partial cancel, 3=Full cancel, 4=Execution (visible), 5=Execution (hidden)
// Price: dollar price × 10000 (e.g., $150.25 = 1502500)
// Direction: 1=buy, -1=sell
// Timestamp: seconds from midnight with nanosecond decimals

const fs = require('fs');
const path = require('path');

const SYMBOLS = {
  AAPL: { basePrice: 1850000, volatility: 0.0008, avgSpread: 1000, dailyEvents: 45000 },
  MSFT: { basePrice: 3750000, volatility: 0.0007, avgSpread: 1500, dailyEvents: 38000 },
  AMZN: { basePrice: 1520000, volatility: 0.0009, avgSpread: 2000, dailyEvents: 35000 },
  GOOG: { basePrice: 1400000, volatility: 0.0008, avgSpread: 2500, dailyEvents: 30000 },
  TSLA: { basePrice: 2480000, volatility: 0.0018, avgSpread: 3000, dailyEvents: 55000 },
  SPY:  { basePrice: 4750000, volatility: 0.0004, avgSpread: 100,  dailyEvents: 80000 },
  INTC: { basePrice: 450000,  volatility: 0.0012, avgSpread: 500,  dailyEvents: 25000 },
  META: { basePrice: 3900000, volatility: 0.001,  avgSpread: 2000, dailyEvents: 32000 },
  NVDA: { basePrice: 8800000, volatility: 0.0015, avgSpread: 5000, dailyEvents: 60000 },
};

const DATE = '2024-01-02';
const MARKET_OPEN = 9.5 * 3600;   // 09:30 = 34200s
const MARKET_CLOSE = 16 * 3600;   // 16:00 = 57600s

function gaussRand() {
  let u = 0, v = 0;
  while (u === 0) u = Math.random();
  while (v === 0) v = Math.random();
  return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
}

function generateSymbol(sym, cfg) {
  const lines = [];
  let nextOid = 1;
  const activeOrders = new Map(); // oid -> { side, price, qty }
  let mid = cfg.basePrice;
  const totalEvents = cfg.dailyEvents;
  const duration = MARKET_CLOSE - MARKET_OPEN;

  // U-shaped intraday volume curve (high at open/close, low midday)
  function volumeMultiplier(t) {
    const pct = (t - MARKET_OPEN) / duration;
    return 1.5 - 1.0 * Math.sin(pct * Math.PI);
  }

  let t = MARKET_OPEN;
  for (let i = 0; i < totalEvents; i++) {
    // Time advancement with U-shaped clustering
    const baseDt = duration / totalEvents;
    const vm = volumeMultiplier(t);
    const dt = baseDt / vm * (0.5 + Math.random());
    t += dt;
    if (t >= MARKET_CLOSE) break;

    // Add nanosecond jitter
    const ns = Math.floor(Math.random() * 999999999);
    const timestamp = t.toFixed(9).replace(/0+$/, '').replace(/\.$/, '.000000000');

    // Random walk with mean reversion
    const shock = gaussRand() * cfg.volatility * cfg.basePrice;
    const reversion = (cfg.basePrice - mid) * 0.001;
    mid = Math.round(mid + shock + reversion);
    if (mid < cfg.basePrice * 0.9) mid = Math.round(cfg.basePrice * 0.9);
    if (mid > cfg.basePrice * 1.1) mid = Math.round(cfg.basePrice * 1.1);

    const spread = cfg.avgSpread + Math.floor(gaussRand() * cfg.avgSpread * 0.3);
    const halfSpread = Math.max(100, Math.floor(spread / 2));
    const bestBid = mid - halfSpread;
    const bestAsk = mid + halfSpread;

    const r = Math.random();
    const activeIds = [...activeOrders.keys()];

    if (r < 0.42) {
      // Type 1: New limit order submission
      const side = Math.random() < 0.5 ? 1 : -1;
      let price;
      if (Math.random() < 0.15) {
        // Marketable order (at or through opposite side)
        price = side === 1 ? bestAsk + Math.floor(Math.random() * 3) * 100 : bestBid - Math.floor(Math.random() * 3) * 100;
      } else {
        // Passive order (resting)
        const depth = Math.floor(Math.abs(gaussRand()) * 15) * 100;
        price = side === 1 ? bestBid - depth : bestAsk + depth;
      }
      price = Math.max(100, price);
      // Size follows log-normal distribution (lots of small, few large)
      const rawSize = Math.exp(gaussRand() * 0.8 + 3);
      const size = Math.max(1, Math.min(5000, Math.round(rawSize / 100) * 100 || 100));

      const oid = nextOid++;
      activeOrders.set(oid, { side, price, qty: size });
      lines.push(`${timestamp},1,${oid},${size},${price},${side}`);

    } else if (r < 0.68 && activeIds.length > 0) {
      // Type 3: Full cancellation
      const idx = Math.floor(Math.random() * activeIds.length);
      const oid = activeIds[idx];
      const o = activeOrders.get(oid);
      lines.push(`${timestamp},3,${oid},${o.qty},${o.price},${o.side}`);
      activeOrders.delete(oid);

    } else if (r < 0.78 && activeIds.length > 0) {
      // Type 2: Partial cancellation
      const idx = Math.floor(Math.random() * activeIds.length);
      const oid = activeIds[idx];
      const o = activeOrders.get(oid);
      if (o.qty > 100) {
        const cancelQty = Math.round((Math.random() * 0.5 + 0.1) * o.qty / 100) * 100 || 100;
        const actual = Math.min(cancelQty, o.qty - 100);
        if (actual > 0) {
          o.qty -= actual;
          lines.push(`${timestamp},2,${oid},${actual},${o.price},${o.side}`);
        }
      }

    } else if (r < 0.92 && activeIds.length > 0) {
      // Type 4: Execution (visible)
      // Pick orders near the spread
      let candidates = activeIds.filter(id => {
        const o = activeOrders.get(id);
        return (o.side === 1 && o.price >= bestBid - 500) ||
               (o.side === -1 && o.price <= bestAsk + 500);
      });
      if (candidates.length === 0) candidates = activeIds;
      const oid = candidates[Math.floor(Math.random() * candidates.length)];
      const o = activeOrders.get(oid);
      const execQty = Math.min(o.qty, Math.max(100, Math.round(Math.random() * o.qty / 100) * 100 || 100));
      lines.push(`${timestamp},4,${oid},${execQty},${o.price},${o.side}`);
      o.qty -= execQty;
      if (o.qty <= 0) activeOrders.delete(oid);

    } else if (activeIds.length > 0) {
      // Type 5: Hidden execution
      const oid = activeIds[Math.floor(Math.random() * activeIds.length)];
      const o = activeOrders.get(oid);
      const execQty = Math.min(o.qty, Math.max(100, Math.round(Math.random() * 0.3 * o.qty / 100) * 100 || 100));
      lines.push(`${timestamp},5,${oid},${execQty},${o.price},${o.side}`);
      o.qty -= execQty;
      if (o.qty <= 0) activeOrders.delete(oid);

    } else {
      // Fallback: submit a new order when book is empty
      const side = Math.random() < 0.5 ? 1 : -1;
      const price = side === 1 ? bestBid : bestAsk;
      const size = Math.max(100, Math.round(Math.exp(gaussRand() * 0.8 + 3) / 100) * 100 || 100);
      const oid = nextOid++;
      activeOrders.set(oid, { side, price, qty: size });
      lines.push(`${timestamp},1,${oid},${size},${price},${side}`);
    }

    // Prune old orders periodically to keep the book realistic
    if (i % 500 === 0 && activeOrders.size > 2000) {
      const toRemove = [...activeOrders.keys()].slice(0, activeOrders.size - 1500);
      for (const oid of toRemove) activeOrders.delete(oid);
    }
  }

  return lines.join('\n') + '\n';
}

// Generate all symbols
const outDir = path.join(__dirname, '..', 'data', 'lobster');
fs.mkdirSync(outDir, { recursive: true });

for (const [sym, cfg] of Object.entries(SYMBOLS)) {
  const filename = `${sym}_${DATE}_message.csv`;
  const data = generateSymbol(sym, cfg);
  const filepath = path.join(outDir, filename);
  fs.writeFileSync(filepath, data);
  const lines = data.trim().split('\n').length;
  console.log(`  ${filename}: ${lines.toLocaleString()} events`);
}

console.log('\nDone. Files written to data/lobster/');
