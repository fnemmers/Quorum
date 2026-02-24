package com.stockapp.ui;

import com.stockapp.backend.BackendClient;
import com.stockapp.model.*;
import javafx.collections.*;
import javafx.geometry.*;
import javafx.scene.control.*;
import javafx.scene.control.cell.PropertyValueFactory;
import javafx.scene.layout.*;
import javafx.scene.paint.Color;
import javafx.scene.text.Font;

import java.util.*;

/**
 * QuoteView  –  "Watchlist" tab.
 *
 * Left: watchlist table (symbol, price, bid, ask, volume, change)
 * Right: candlestick chart for the selected symbol
 */
public class QuoteView extends SplitPane {

    private final ObservableMap<String, StockQuote> quoteMap =
            FXCollections.observableHashMap();
    private final ObservableList<StockQuote> quoteList =
            FXCollections.observableArrayList();

    private final CandlestickChart chart   = new CandlestickChart();
    private final BackendClient    backend = BackendClient.getInstance();

    /* track last close for change % */
    private final Map<String, Double> prevClose = new HashMap<>();

    @SuppressWarnings("unchecked")
    public QuoteView() {
        /* ── Watchlist table ────────────────────────────────────── */
        TableView<StockQuote> table = new TableView<>(quoteList);
        table.setColumnResizePolicy(TableView.CONSTRAINED_RESIZE_POLICY);
        table.getStyleClass().add("quote-table");
        table.setPlaceholder(new Label("Add a symbol below"));

        TableColumn<StockQuote, String> symCol   = col("Symbol", "symbol", 80);
        TableColumn<StockQuote, Double> priceCol = col("Price",  "price",  90);
        TableColumn<StockQuote, Double> bidCol   = col("Bid",    "bid",    90);
        TableColumn<StockQuote, Double> askCol   = col("Ask",    "ask",    90);
        TableColumn<StockQuote, Long>   volCol   = col("Volume", "volume", 100);

        /* Change % column – computed on the fly */
        TableColumn<StockQuote, String> chgCol = new TableColumn<>("Chg %");
        chgCol.setCellValueFactory(cd -> {
            StockQuote q = cd.getValue();
            double prev  = prevClose.getOrDefault(q.getSymbol(), q.getPrice());
            double pct   = prev == 0 ? 0 : (q.getPrice() - prev) / prev * 100.0;
            return new javafx.beans.property.SimpleStringProperty(
                    String.format("%+.2f%%", pct));
        });
        chgCol.setCellFactory(col -> new TableCell<>() {
            @Override protected void updateItem(String s, boolean empty) {
                super.updateItem(s, empty);
                setText(empty || s == null ? null : s);
                if (s != null && s.startsWith("+"))
                    setStyle("-fx-text-fill: #26a69a;");
                else
                    setStyle("-fx-text-fill: #ef5350;");
            }
        });

        table.getColumns().addAll(symCol, priceCol, bidCol, askCol, volCol, chgCol);

        /* load chart on row select */
        table.getSelectionModel().selectedItemProperty().addListener((obs, old, sel) -> {
            if (sel == null) return;
            loadChart(sel.getSymbol());
        });

        /* ── Add-symbol bar ─────────────────────────────────────── */
        TextField symField = new TextField();
        symField.setPromptText("Symbol (e.g. AAPL)");
        symField.setPrefWidth(140);

        Button addBtn = new Button("Subscribe");
        addBtn.setOnAction(e -> {
            String s = symField.getText().trim().toUpperCase();
            if (!s.isEmpty()) { subscribeSymbol(s); symField.clear(); }
        });
        symField.setOnAction(e -> addBtn.fire());

        Button removeBtn = new Button("Remove");
        removeBtn.setOnAction(e -> {
            StockQuote sel = table.getSelectionModel().getSelectedItem();
            if (sel != null) {
                backend.unsubscribe(sel.getSymbol());
                quoteMap.remove(sel.getSymbol());
                quoteList.remove(sel);
                chart.clear();
            }
        });

        HBox toolbar = new HBox(8, symField, addBtn, removeBtn);
        toolbar.setPadding(new Insets(8));
        toolbar.setAlignment(Pos.CENTER_LEFT);

        VBox left = new VBox(table, toolbar);
        VBox.setVgrow(table, Priority.ALWAYS);
        left.setMinWidth(420);

        /* ── Right: chart + timespan controls ───────────────────── */
        HBox chartToolbar = new HBox(8);
        chartToolbar.setPadding(new Insets(8));
        chartToolbar.setAlignment(Pos.CENTER_LEFT);

        Label chartLabel = new Label("Timespan:");
        chartLabel.setTextFill(Color.LIGHTGRAY);
        ToggleGroup tg = new ToggleGroup();
        String[] spans = {"1D","5D","1M","3M","1Y"};
        for (String span : spans) {
            ToggleButton tb = new ToggleButton(span);
            tb.setToggleGroup(tg);
            if ("1M".equals(span)) tb.setSelected(true);
            tb.setOnAction(e -> {
                StockQuote sel = table.getSelectionModel().getSelectedItem();
                if (sel != null) loadChartSpan(sel.getSymbol(), span);
            });
            chartToolbar.getChildren().add(tb);
        }

        VBox right = new VBox(chartToolbar, chart);
        VBox.setVgrow(chart, Priority.ALWAYS);

        getItems().addAll(left, right);
        setDividerPositions(0.4);

        /* ── Backend listeners ───────────────────────────────────── */
        backend.addQuoteListener(this::onQuote);
        backend.addHistoryListener((sym, bars) -> {
            StockQuote sel = table.getSelectionModel().getSelectedItem();
            if (sel != null && sel.getSymbol().equals(sym))
                chart.setData(sym, bars);
        });
    }

    private void subscribeSymbol(String symbol) {
        if (!quoteMap.containsKey(symbol)) {
            StockQuote q = new StockQuote(symbol, 0, 0, 0, 0, 0);
            quoteMap.put(symbol, q);
            quoteList.add(q);
            backend.subscribe(symbol);
            backend.requestSnapshot(symbol);
        }
    }

    private void onQuote(StockQuote incoming) {
        StockQuote existing = quoteMap.get(incoming.getSymbol());
        if (existing == null) return;
        // track prev price for change %
        prevClose.putIfAbsent(incoming.getSymbol(), incoming.getPrice());
        existing.setPrice(incoming.getPrice());
        existing.setBid(incoming.getBid());
        existing.setAsk(incoming.getAsk());
        existing.setVolume(incoming.getVolume());
        existing.setTimestamp(incoming.getTimestamp());
        // force table refresh
        int idx = quoteList.indexOf(existing);
        if (idx >= 0) { quoteList.set(idx, existing); }
    }

    private void loadChart(String symbol) {
        loadChartSpan(symbol, "1M");
    }

    private void loadChartSpan(String symbol, String span) {
        String from, to, timespan;
        int    multiplier = 1;
        java.time.LocalDate today = java.time.LocalDate.now();
        to = today.toString();
        switch (span) {
            case "1D" -> { from = today.minusDays(1).toString();    timespan = "minute"; multiplier = 5;  }
            case "5D" -> { from = today.minusDays(5).toString();    timespan = "hour";   }
            case "3M" -> { from = today.minusMonths(3).toString();  timespan = "day";    }
            case "1Y" -> { from = today.minusYears(1).toString();   timespan = "day";    }
            default   -> { from = today.minusMonths(1).toString();  timespan = "day";    } /* 1M */
        }
        chart.clear();
        backend.requestHistory(symbol, multiplier, timespan, from, to);
    }

    @SuppressWarnings("unchecked")
    private static <S, T> TableColumn<S, T> col(String title, String prop, double w) {
        TableColumn<S, T> c = new TableColumn<>(title);
        c.setCellValueFactory(new PropertyValueFactory<>(prop));
        c.setPrefWidth(w);
        if (!prop.equals("symbol") && !prop.equals("volume")) {
            c.setCellFactory(col -> new TableCell<>() {
                @Override protected void updateItem(T item, boolean empty) {
                    super.updateItem(item, empty);
                    setText(item == null || empty ? null : String.format("%.4f", item));
                }
            });
        }
        return c;
    }
}
