package com.stockapp.ui;

import com.stockapp.backend.BackendClient;
import com.stockapp.model.AlertModel;
import javafx.collections.*;
import javafx.geometry.*;
import javafx.scene.control.*;
import javafx.scene.control.cell.PropertyValueFactory;
import javafx.scene.layout.*;
import javafx.scene.paint.Color;
import javafx.scene.text.*;

import java.util.List;

/**
 * AlertView  –  "Alerts" tab.
 *
 * Lets the user set price alerts and displays fired alerts.
 */
public class AlertView extends VBox {

    private final ObservableList<AlertModel> alerts =
            FXCollections.observableArrayList();
    private final ObservableList<String> log =
            FXCollections.observableArrayList();

    private final BackendClient backend = BackendClient.getInstance();

    @SuppressWarnings("unchecked")
    public AlertView() {
        setSpacing(0);

        /* ── Active alerts table ────────────────────────────────── */
        TableView<AlertModel> table = new TableView<>(alerts);
        table.setColumnResizePolicy(TableView.CONSTRAINED_RESIZE_POLICY);
        table.setPlaceholder(new Label("No active alerts"));

        TableColumn<AlertModel, Integer> idCol   = new TableColumn<>("ID");
        idCol.setCellValueFactory(new PropertyValueFactory<>("id"));
        idCol.setPrefWidth(50);

        TableColumn<AlertModel, String>  symCol  = new TableColumn<>("Symbol");
        symCol.setCellValueFactory(new PropertyValueFactory<>("symbol"));
        symCol.setPrefWidth(80);

        TableColumn<AlertModel, String>  condCol = new TableColumn<>("Condition");
        condCol.setCellValueFactory(new PropertyValueFactory<>("condition"));
        condCol.setPrefWidth(80);

        TableColumn<AlertModel, Double>  prCol   = new TableColumn<>("Trigger $");
        prCol.setCellValueFactory(new PropertyValueFactory<>("price"));
        prCol.setPrefWidth(100);
        prCol.setCellFactory(col -> new TableCell<>() {
            @Override protected void updateItem(Double item, boolean empty) {
                super.updateItem(item, empty);
                setText(item == null || empty ? null : String.format("%.4f", item));
            }
        });

        TableColumn<AlertModel, Boolean> actCol  = new TableColumn<>("Status");
        actCol.setCellValueFactory(new PropertyValueFactory<>("active"));
        actCol.setCellFactory(col -> new TableCell<>() {
            @Override protected void updateItem(Boolean item, boolean empty) {
                super.updateItem(item, empty);
                if (empty || item == null) { setText(null); return; }
                setText(item ? "Active" : "Fired");
                setStyle(item ? "-fx-text-fill:#26a69a;" : "-fx-text-fill:#ef5350;");
            }
        });

        table.getColumns().addAll(idCol, symCol, condCol, prCol, actCol);
        VBox.setVgrow(table, Priority.ALWAYS);

        /* ── Input bar ──────────────────────────────────────────── */
        TextField symF   = new TextField(); symF.setPromptText("Symbol"); symF.setPrefWidth(100);
        TextField priceF = new TextField(); priceF.setPromptText("Price"); priceF.setPrefWidth(100);

        ToggleGroup condGroup = new ToggleGroup();
        RadioButton aboveRb   = new RadioButton("Above"); aboveRb.setToggleGroup(condGroup); aboveRb.setSelected(true);
        RadioButton belowRb   = new RadioButton("Below"); belowRb.setToggleGroup(condGroup);
        aboveRb.setTextFill(Color.LIGHTGRAY);
        belowRb.setTextFill(Color.LIGHTGRAY);

        Button addBtn = new Button("Add Alert");
        addBtn.setOnAction(e -> {
            try {
                String sym   = symF.getText().trim().toUpperCase();
                double price = Double.parseDouble(priceF.getText().trim());
                String cond  = aboveRb.isSelected() ? "above" : "below";
                if (sym.isEmpty()) return;
                backend.alertAdd(sym, cond, price);
                backend.alertList();
                symF.clear(); priceF.clear();
            } catch (NumberFormatException ex) {
                showError("Price must be a number.");
            }
        });

        Button removeBtn = new Button("Remove");
        removeBtn.setOnAction(e -> {
            AlertModel sel = table.getSelectionModel().getSelectedItem();
            if (sel != null) {
                backend.alertRemove(sel.getId());
                backend.alertList();
            }
        });

        Button refreshBtn = new Button("Refresh");
        refreshBtn.setOnAction(e -> backend.alertList());

        HBox toolbar = new HBox(8, symF, priceF, aboveRb, belowRb, addBtn, removeBtn, refreshBtn);
        toolbar.setPadding(new Insets(8));
        toolbar.setAlignment(Pos.CENTER_LEFT);

        /* ── Alert log (fired alerts) ───────────────────────────── */
        Label logLabel = new Label("Alert Log");
        logLabel.setFont(Font.font("Monospaced", FontWeight.BOLD, 12));
        logLabel.setTextFill(Color.LIGHTGRAY);
        logLabel.setPadding(new Insets(6, 10, 2, 10));

        ListView<String> logView = new ListView<>(log);
        logView.setPrefHeight(120);
        logView.setStyle("-fx-background-color:#111120; -fx-control-inner-background:#111120;");

        getChildren().addAll(table, toolbar, logLabel, logView);

        /* ── Listeners ──────────────────────────────────────────── */
        backend.addAlertListListener(this::onAlertList);
        backend.addAlertListener((alert, currentPrice) -> {
            alert.setActive(false);
            // update in table if present
            for (AlertModel a : alerts) {
                if (a.getId() == alert.getId()) { a.setActive(false); break; }
            }
            String entry = String.format("ALERT FIRED: %s %s $%.4f  (current: $%.4f)",
                    alert.getSymbol(), alert.getCondition(),
                    alert.getPrice(), currentPrice);
            log.add(0, entry);  // newest first
            if (log.size() > 200) log.remove(log.size() - 1);

            showNotification(entry);
        });

        backend.alertList();
    }

    private void onAlertList(List<AlertModel> fresh) {
        alerts.setAll(fresh);
    }

    private void showNotification(String msg) {
        Alert a = new Alert(Alert.AlertType.INFORMATION, msg, ButtonType.OK);
        a.setTitle("Price Alert Fired");
        a.setHeaderText(null);
        a.show();  // non-blocking
    }

    private static void showError(String msg) {
        Alert a = new Alert(Alert.AlertType.ERROR, msg, ButtonType.OK);
        a.setHeaderText(null);
        a.showAndWait();
    }
}
