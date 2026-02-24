package com.stockapp.ui;

import com.stockapp.backend.BackendClient;
import com.stockapp.model.Holding;
import javafx.collections.*;
import javafx.geometry.*;
import javafx.scene.control.*;
import javafx.scene.control.cell.PropertyValueFactory;
import javafx.scene.layout.*;
import javafx.scene.paint.Color;
import javafx.scene.text.*;

import java.util.List;

/**
 * PortfolioView  –  "Portfolio" tab.
 *
 * Lets the user add/remove positions and see unrealised P&L.
 */
public class PortfolioView extends VBox {

    private final ObservableList<Holding> holdings =
            FXCollections.observableArrayList();
    private final BackendClient backend = BackendClient.getInstance();
    private final Label totalLabel = new Label("Total value: $0.00   P&L: $0.00");

    @SuppressWarnings("unchecked")
    public PortfolioView() {
        setSpacing(0);

        /* ── Table ──────────────────────────────────────────────── */
        TableView<Holding> table = new TableView<>(holdings);
        table.setColumnResizePolicy(TableView.CONSTRAINED_RESIZE_POLICY);
        table.setPlaceholder(new Label("No positions – add one below"));

        TableColumn<Holding, String> symCol  = plain("Symbol",    "symbol",    80);
        TableColumn<Holding, Double> shrCol  = dbl ("Shares",     "shares",    80);
        TableColumn<Holding, Double> avgCol  = dbl ("Avg Cost",   "avgPrice",  100);
        TableColumn<Holding, Double> curCol  = dbl ("Current",    "current",   100);

        TableColumn<Holding, String> mvCol = new TableColumn<>("Mkt Value");
        mvCol.setCellValueFactory(cd ->
                new javafx.beans.property.SimpleStringProperty(
                        String.format("$%,.2f", cd.getValue().getMarketValue())));

        TableColumn<Holding, String> pnlCol = new TableColumn<>("P&L");
        pnlCol.setCellValueFactory(cd ->
                new javafx.beans.property.SimpleStringProperty(
                        String.format("%+,.2f (%.2f%%)",
                                cd.getValue().getPnL(), cd.getValue().getPnLPct())));
        pnlCol.setCellFactory(col -> new TableCell<>() {
            @Override protected void updateItem(String s, boolean empty) {
                super.updateItem(s, empty);
                setText(empty ? null : s);
                if (s != null)
                    setStyle(s.startsWith("+") ? "-fx-text-fill:#26a69a;" : "-fx-text-fill:#ef5350;");
            }
        });

        table.getColumns().addAll(symCol, shrCol, avgCol, curCol, mvCol, pnlCol);
        VBox.setVgrow(table, Priority.ALWAYS);

        /* ── Input bar ──────────────────────────────────────────── */
        TextField symF    = new TextField(); symF.setPromptText("Symbol");  symF.setPrefWidth(100);
        TextField sharesF = new TextField(); sharesF.setPromptText("Shares"); sharesF.setPrefWidth(80);
        TextField priceF  = new TextField(); priceF.setPromptText("Avg price"); priceF.setPrefWidth(100);

        Button addBtn = new Button("Add Position");
        addBtn.setOnAction(e -> {
            try {
                String sym    = symF.getText().trim().toUpperCase();
                double shares = Double.parseDouble(sharesF.getText().trim());
                double price  = Double.parseDouble(priceF.getText().trim());
                if (sym.isEmpty()) return;
                backend.portfolioAdd(sym, shares, price);
                backend.portfolioGet();
                symF.clear(); sharesF.clear(); priceF.clear();
            } catch (NumberFormatException ex) {
                showError("Shares and price must be numbers.");
            }
        });

        Button removeBtn = new Button("Remove");
        removeBtn.setOnAction(e -> {
            Holding sel = table.getSelectionModel().getSelectedItem();
            if (sel != null) {
                backend.portfolioRemove(sel.getSymbol());
                backend.portfolioGet();
            }
        });

        Button refreshBtn = new Button("Refresh");
        refreshBtn.setOnAction(e -> backend.portfolioGet());

        HBox toolbar = new HBox(8, symF, sharesF, priceF, addBtn, removeBtn, refreshBtn);
        toolbar.setPadding(new Insets(8));
        toolbar.setAlignment(Pos.CENTER_LEFT);

        /* ── Summary bar ────────────────────────────────────────── */
        totalLabel.setFont(Font.font("Monospaced", FontWeight.BOLD, 13));
        totalLabel.setTextFill(Color.LIGHTGRAY);
        HBox summary = new HBox(totalLabel);
        summary.setPadding(new Insets(6, 10, 6, 10));
        summary.setStyle("-fx-background-color: #1e1e2e;");

        getChildren().addAll(table, toolbar, summary);

        /* ── Listeners ──────────────────────────────────────────── */
        backend.addPortfolioListener(this::onPortfolio);
        backend.addQuoteListener(quote -> {
            holdings.stream()
                    .filter(h -> h.getSymbol().equals(quote.getSymbol()))
                    .forEach(h -> h.setCurrent(quote.getPrice()));
            updateSummary();
        });

        // initial load
        backend.portfolioGet();
    }

    private void onPortfolio(List<Holding> fresh) {
        holdings.setAll(fresh);
        updateSummary();
    }

    private void updateSummary() {
        double totalMv  = holdings.stream().mapToDouble(Holding::getMarketValue).sum();
        double totalPnL = holdings.stream().mapToDouble(Holding::getPnL).sum();
        totalLabel.setText(String.format(
                "Total value: $%,.2f   P&L: %s$%,.2f",
                totalMv, totalPnL >= 0 ? "+" : "", totalPnL));
        totalLabel.setTextFill(totalPnL >= 0 ? Color.rgb(38,166,154) : Color.rgb(239,83,80));
    }

    private static void showError(String msg) {
        Alert a = new Alert(Alert.AlertType.ERROR, msg, ButtonType.OK);
        a.setHeaderText(null);
        a.showAndWait();
    }

    private static <T> TableColumn<Holding, T> plain(String t, String p, double w) {
        TableColumn<Holding, T> c = new TableColumn<>(t);
        c.setCellValueFactory(new PropertyValueFactory<>(p));
        c.setPrefWidth(w);
        return c;
    }

    private static TableColumn<Holding, Double> dbl(String t, String p, double w) {
        TableColumn<Holding, Double> c = new TableColumn<>(t);
        c.setCellValueFactory(new PropertyValueFactory<>(p));
        c.setPrefWidth(w);
        c.setCellFactory(col -> new TableCell<>() {
            @Override protected void updateItem(Double item, boolean empty) {
                super.updateItem(item, empty);
                setText(item == null || empty ? null : String.format("%.4f", item));
            }
        });
        return c;
    }
}
