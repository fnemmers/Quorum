package com.stockapp.backend;

import com.stockapp.model.*;
import javafx.application.Platform;

import java.io.*;
import java.net.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.*;
import java.util.logging.*;

/**
 * BackendClient
 * ─────────────
 * Manages the TCP connection to the C backend on localhost:8765.
 *
 * - Sends JSON commands (subscribe, history, portfolio_*, alert_*)
 * - Dispatches incoming JSON events to registered listeners on the FX thread
 * - Auto-reconnects if the C process crashes or hasn't started yet
 */
public class BackendClient {

    private static final Logger LOG  = Logger.getLogger(BackendClient.class.getName());
    public  static final int    PORT = 8765;

    /* ── Listener interfaces ─────────────────────────────────────── */
    public interface QuoteListener    { void onQuote(StockQuote q); }
    public interface HistoryListener  { void onHistory(String symbol, List<OHLCBar> bars); }
    public interface AlertListener    { void onAlert(AlertModel a, double currentPrice); }
    public interface PortfolioListener{ void onPortfolio(List<Holding> holdings); }
    public interface AlertListListener{ void onAlertList(List<AlertModel> alerts); }

    private final List<QuoteListener>     quoteListeners     = new CopyOnWriteArrayList<>();
    private final List<HistoryListener>   historyListeners   = new CopyOnWriteArrayList<>();
    private final List<AlertListener>     alertListeners     = new CopyOnWriteArrayList<>();
    private final List<PortfolioListener> portfolioListeners = new CopyOnWriteArrayList<>();
    private final List<AlertListListener> alertListListeners = new CopyOnWriteArrayList<>();

    public void addQuoteListener    (QuoteListener l)     { quoteListeners.add(l); }
    public void addHistoryListener  (HistoryListener l)   { historyListeners.add(l); }
    public void addAlertListener    (AlertListener l)     { alertListeners.add(l); }
    public void addPortfolioListener(PortfolioListener l) { portfolioListeners.add(l); }
    public void addAlertListListener(AlertListListener l) { alertListListeners.add(l); }

    /* ── Connection state ────────────────────────────────────────── */
    private volatile Socket         socket;
    private volatile PrintWriter    writer;
    private volatile boolean        running = true;
    private final    ExecutorService executor = Executors.newCachedThreadPool(r -> {
        Thread t = new Thread(r, "backend-client");
        t.setDaemon(true);
        return t;
    });

    /* ── Singleton ─────────────────────────────────────────────── */
    private static BackendClient instance;
    public  static BackendClient getInstance() {
        if (instance == null) instance = new BackendClient();
        return instance;
    }

    private BackendClient() {}

    /* ── Start ───────────────────────────────────────────────────── */

    public void start() {
        executor.submit(this::connectLoop);
    }

    private void connectLoop() {
        while (running) {
            try {
                LOG.info("Connecting to backend...");
                socket = new Socket();
                socket.connect(new InetSocketAddress("127.0.0.1", PORT), 3000);
                socket.setSoTimeout(0);  // blocking reads

                writer = new PrintWriter(
                        new BufferedWriter(new OutputStreamWriter(socket.getOutputStream())),
                        true);

                LOG.info("Connected to backend");
                readLoop(socket);

            } catch (IOException e) {
                LOG.warning("Backend connection failed: " + e.getMessage());
            }

            if (running) {
                try { Thread.sleep(2000); } catch (InterruptedException ignored) {}
            }
        }
    }

    /* ── Read loop – parses incoming JSON lines ──────────────────── */

    private void readLoop(Socket s) throws IOException {
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(s.getInputStream()))) {
            String line;
            while ((line = reader.readLine()) != null) {
                dispatch(line);
            }
        }
    }

    private void dispatch(String json) {
        try {
            Map<String, Object> map = parseSimpleJson(json);
            String type = (String) map.get("type");
            if (type == null) return;

            switch (type) {
                case "quote" -> {
                    StockQuote q = parseQuote(map);
                    Platform.runLater(() -> quoteListeners.forEach(l -> l.onQuote(q)));
                }
                case "history" -> {
                    String sym  = (String) map.get("symbol");
                    List<OHLCBar> bars = parseBars(map);
                    Platform.runLater(() -> historyListeners.forEach(l -> l.onHistory(sym, bars)));
                }
                case "alert" -> {
                    AlertModel a = new AlertModel(
                            toInt(map.get("id")),
                            (String) map.get("symbol"),
                            (String) map.get("condition"),
                            toDouble(map.get("trigger")));
                    double cur = toDouble(map.get("price"));
                    a.setActive(false);
                    Platform.runLater(() -> alertListeners.forEach(l -> l.onAlert(a, cur)));
                }
                case "portfolio" -> {
                    List<Holding> holdings = parseHoldings(map);
                    Platform.runLater(() -> portfolioListeners.forEach(l -> l.onPortfolio(holdings)));
                }
                case "alert_list" -> {
                    List<AlertModel> alerts = parseAlertList(map);
                    Platform.runLater(() -> alertListListeners.forEach(l -> l.onAlertList(alerts)));
                }
                case "error" -> LOG.warning("Backend error: " + map.get("message"));
            }
        } catch (Exception e) {
            LOG.log(Level.WARNING, "Failed to parse: " + json, e);
        }
    }

    /* ── Command senders ─────────────────────────────────────────── */

    public void subscribe(String symbol) {
        send("{\"cmd\":\"subscribe\",\"symbol\":\"" + symbol + "\"}");
    }

    public void unsubscribe(String symbol) {
        send("{\"cmd\":\"unsubscribe\",\"symbol\":\"" + symbol + "\"}");
    }

    public void requestHistory(String symbol, int multiplier, String timespan,
                                String from, String to) {
        send(String.format(
            "{\"cmd\":\"history\",\"symbol\":\"%s\","
          + "\"multiplier\":%d,\"timespan\":\"%s\","
          + "\"from\":\"%s\",\"to\":\"%s\"}",
            symbol, multiplier, timespan, from, to));
    }

    public void requestSnapshot(String symbol) {
        send("{\"cmd\":\"snapshot\",\"symbol\":\"" + symbol + "\"}");
    }

    public void portfolioAdd(String symbol, double shares, double price) {
        send(String.format(
            "{\"cmd\":\"portfolio_add\",\"symbol\":\"%s\","
          + "\"shares\":%.4f,\"price\":%.4f}",
            symbol, shares, price));
    }

    public void portfolioRemove(String symbol) {
        send("{\"cmd\":\"portfolio_remove\",\"symbol\":\"" + symbol + "\"}");
    }

    public void portfolioGet() {
        send("{\"cmd\":\"portfolio_get\"}");
    }

    public void alertAdd(String symbol, String condition, double price) {
        send(String.format(
            "{\"cmd\":\"alert_add\",\"symbol\":\"%s\","
          + "\"condition\":\"%s\",\"price\":%.4f}",
            symbol, condition, price));
    }

    public void alertRemove(int id) {
        send("{\"cmd\":\"alert_remove\",\"id\":" + id + "}");
    }

    public void alertList() {
        send("{\"cmd\":\"alert_list\"}");
    }

    private synchronized void send(String json) {
        if (writer != null) writer.println(json);
        else LOG.warning("Not connected – dropped: " + json);
    }

    public void stop() {
        running = false;
        try { if (socket != null) socket.close(); } catch (IOException ignored) {}
        executor.shutdownNow();
    }

    /* ── Minimal JSON parser (avoids external deps) ──────────────── */
    /*
     * The C backend only sends flat or 1-level-deep JSON, so we use a
     * lightweight hand-rolled parser instead of pulling in a library.
     * For nested arrays (bars, holdings, alerts) we delegate to helper
     * methods that re-parse the relevant segment with the same technique.
     */

    @SuppressWarnings("unchecked")
    private static Map<String, Object> parseSimpleJson(String json) {
        // Use org.json or jackson in production; this handles our specific shapes.
        Map<String, Object> map = new LinkedHashMap<>();
        json = json.trim();
        if (json.startsWith("{")) json = json.substring(1, json.lastIndexOf('}'));

        // tokenise top-level key:"value" pairs (handles nested arrays as raw strings)
        int i = 0;
        while (i < json.length()) {
            // skip whitespace/commas
            while (i < json.length() && (json.charAt(i) == ',' || json.charAt(i) == ' ')) i++;
            if (i >= json.length()) break;

            // parse key
            if (json.charAt(i) != '"') { i++; continue; }
            int ks = i + 1;
            int ke = json.indexOf('"', ks);
            if (ke < 0) break;
            String key = json.substring(ks, ke);
            i = ke + 1;

            // skip ':'
            while (i < json.length() && json.charAt(i) != ':') i++;
            i++;
            while (i < json.length() && json.charAt(i) == ' ') i++;

            if (i >= json.length()) break;
            char c = json.charAt(i);

            if (c == '"') {
                // string value
                int vs = i + 1;
                int ve = json.indexOf('"', vs);
                while (ve > 0 && json.charAt(ve - 1) == '\\') ve = json.indexOf('"', ve + 1);
                if (ve < 0) break;
                map.put(key, json.substring(vs, ve));
                i = ve + 1;
            } else if (c == '[') {
                // array – find matching ]
                int depth = 0, j = i;
                while (j < json.length()) {
                    if (json.charAt(j) == '[') depth++;
                    else if (json.charAt(j) == ']') { if (--depth == 0) break; }
                    j++;
                }
                map.put(key, json.substring(i, j + 1)); // raw array string
                i = j + 1;
            } else if (c == '{') {
                int depth = 0, j = i;
                while (j < json.length()) {
                    if (json.charAt(j) == '{') depth++;
                    else if (json.charAt(j) == '}') { if (--depth == 0) break; }
                    j++;
                }
                map.put(key, json.substring(i, j + 1));
                i = j + 1;
            } else {
                // number or boolean
                int ve = i;
                while (ve < json.length() && json.charAt(ve) != ',' && json.charAt(ve) != '}') ve++;
                String raw = json.substring(i, ve).trim();
                try { map.put(key, Double.parseDouble(raw)); }
                catch (NumberFormatException e) { map.put(key, raw); }
                i = ve;
            }
        }
        return map;
    }

    private static StockQuote parseQuote(Map<String, Object> m) {
        return new StockQuote(
                (String) m.get("symbol"),
                toDouble(m.get("price")),
                toDouble(m.get("bid")),
                toDouble(m.get("ask")),
                (long) toDouble(m.get("volume")),
                (long) toDouble(m.get("ts")));
    }

    private static List<OHLCBar> parseBars(Map<String, Object> m) {
        List<OHLCBar> result = new ArrayList<>();
        Object raw = m.get("bars");
        if (raw == null) return result;
        String arr = raw.toString().trim();
        if (arr.startsWith("[")) arr = arr.substring(1, arr.length() - 1);

        // split on "},{"
        String[] items = arr.split("\\},\\{");
        for (String item : items) {
            item = item.replace("{", "").replace("}", "");
            Map<String, Object> bm = parseSimpleJson("{" + item + "}");
            result.add(new OHLCBar(
                    (long) toDouble(bm.get("t")),
                    toDouble(bm.get("o")),
                    toDouble(bm.get("h")),
                    toDouble(bm.get("l")),
                    toDouble(bm.get("c")),
                    (long) toDouble(bm.get("v"))));
        }
        return result;
    }

    private static List<Holding> parseHoldings(Map<String, Object> m) {
        List<Holding> result = new ArrayList<>();
        Object raw = m.get("holdings");
        if (raw == null) return result;
        String arr = raw.toString().trim();
        if (arr.startsWith("[")) arr = arr.substring(1, arr.length() - 1);
        String[] items = arr.split("\\},\\{");
        for (String item : items) {
            if (item.isBlank()) continue;
            Map<String, Object> hm = parseSimpleJson("{" + item.replace("{","").replace("}","") + "}");
            result.add(new Holding(
                    (String) hm.get("symbol"),
                    toDouble(hm.get("shares")),
                    toDouble(hm.get("avg_price")),
                    toDouble(hm.get("current"))));
        }
        return result;
    }

    private static List<AlertModel> parseAlertList(Map<String, Object> m) {
        List<AlertModel> result = new ArrayList<>();
        Object raw = m.get("alerts");
        if (raw == null) return result;
        String arr = raw.toString().trim();
        if (arr.startsWith("[")) arr = arr.substring(1, arr.length() - 1);
        String[] items = arr.split("\\},\\{");
        for (String item : items) {
            if (item.isBlank()) continue;
            Map<String, Object> am = parseSimpleJson("{" + item.replace("{","").replace("}","") + "}");
            result.add(new AlertModel(
                    toInt(am.get("id")),
                    (String) am.get("symbol"),
                    (String) am.get("condition"),
                    toDouble(am.get("price"))));
        }
        return result;
    }

    private static double toDouble(Object o) {
        if (o instanceof Number) return ((Number) o).doubleValue();
        if (o instanceof String) { try { return Double.parseDouble((String) o); } catch (Exception e) { return 0; } }
        return 0;
    }

    private static int toInt(Object o) { return (int) toDouble(o); }
}
