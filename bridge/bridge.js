/**
 * stock-bridge — Node.js WebSocket bridge
 *
 * Browser clients connect via WebSocket on :3001
 * Bridge maintains one TCP connection to C backend on :8765
 *
 * Flow:
 *   Browser --WS--> bridge --TCP--> C backend
 *   C backend --TCP--> bridge --WS broadcast--> all browsers
 */

const net = require('net');
const { WebSocketServer } = require('ws');

const BACKEND_HOST = '127.0.0.1';
const BACKEND_PORT = 8765;
const WS_PORT      = 3001;

let tcpSocket    = null;
let tcpConnected = false;
let tcpBuffer    = '';
let reconnectTimer = null;

const wsClients = new Set();

/* ── Helpers ─────────────────────────────────────────────────── */

function broadcast(obj) {
  const msg = typeof obj === 'string' ? obj : JSON.stringify(obj);
  for (const ws of wsClients) {
    if (ws.readyState === 1) ws.send(msg);
  }
}

function sendToBackend(msg) {
  if (!tcpConnected || !tcpSocket) return false;
  tcpSocket.write(msg.endsWith('\n') ? msg : msg + '\n');
  return true;
}

/* ── TCP connection ──────────────────────────────────────────── */

function connectTcp() {
  if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
  console.log(`[bridge] Connecting to backend ${BACKEND_HOST}:${BACKEND_PORT}...`);

  tcpSocket = new net.Socket();

  tcpSocket.connect(BACKEND_PORT, BACKEND_HOST, () => {
    tcpConnected = true;
    console.log('[bridge] Backend connected');
    broadcast({ type: 'bridge_status', connected: true });
  });

  tcpSocket.on('data', (chunk) => {
    tcpBuffer += chunk.toString();
    const lines = tcpBuffer.split('\n');
    tcpBuffer = lines.pop(); // keep incomplete tail
    for (const line of lines) {
      if (line.trim()) broadcast(line.trim());
    }
  });

  tcpSocket.on('close', () => {
    tcpConnected = false;
    console.log('[bridge] Backend disconnected — retrying in 3s');
    broadcast({ type: 'bridge_status', connected: false });
    reconnectTimer = setTimeout(connectTcp, 3000);
  });

  tcpSocket.on('error', (err) => {
    console.error('[bridge] TCP error:', err.message);
    tcpSocket.destroy();
  });
}

/* ── WebSocket server ────────────────────────────────────────── */

const wss = new WebSocketServer({ port: WS_PORT });
console.log(`[bridge] Listening on ws://localhost:${WS_PORT}`);

wss.on('connection', (ws) => {
  wsClients.add(ws);
  console.log(`[bridge] Browser connected  (total: ${wsClients.size})`);

  // immediate status so UI knows if backend is up
  ws.send(JSON.stringify({ type: 'bridge_status', connected: tcpConnected }));

  ws.on('message', (data) => {
    const msg = data.toString();
    if (!sendToBackend(msg)) {
      ws.send(JSON.stringify({ type: 'error', message: 'Backend not connected' }));
    }
  });

  ws.on('close', () => {
    wsClients.delete(ws);
    console.log(`[bridge] Browser disconnected (total: ${wsClients.size})`);
  });

  ws.on('error', (err) => {
    console.error('[bridge] Client error:', err.message);
    wsClients.delete(ws);
  });
});

connectTcp();
