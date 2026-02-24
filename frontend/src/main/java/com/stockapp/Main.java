package com.stockapp;

import com.stockapp.backend.BackendClient;
import com.stockapp.ui.*;
import javafx.application.*;
import javafx.geometry.Insets;
import javafx.scene.*;
import javafx.scene.control.*;
import javafx.scene.layout.*;
import javafx.scene.paint.Color;
import javafx.scene.text.*;
import javafx.stage.*;

import java.io.*;
import java.util.logging.*;

/**
 * Main
 * ────
 * JavaFX Application entry point.
 *
 * On startup:
 *  1. Optionally launches the C backend as a subprocess
 *     (looks for stock-backend[.exe] next to the JAR, or uses
 *      the STOCK_BACKEND_BIN env var).
 *  2. Starts the BackendClient (auto-reconnects until backend is ready).
 *  3. Shows the main window with three tabs:
 *       Watchlist | Portfolio | Alerts
 *
 * Pass your Polygon.io API key via:
 *   - Environment variable  POLYGON_API_KEY
 *   - System property       -Dpolygon.api.key=<key>
 *   - First program argument
 */
public class Main extends Application {

    private static final Logger LOG = Logger.getLogger(Main.class.getName());

    private Process backendProcess;

    @Override
    public void start(Stage stage) {
        String apiKey = resolveApiKey();
        if (apiKey == null || apiKey.isBlank()) {
            showApiKeyDialog(stage, key -> doStart(stage, key));
        } else {
            doStart(stage, apiKey);
        }
    }

    private void doStart(Stage stage, String apiKey) {
        launchBackend(apiKey);

        BackendClient.getInstance().start();

        /* ── Build UI ───────────────────────────────────────────── */
        TabPane tabs = new TabPane();
        tabs.setTabClosingPolicy(TabPane.TabClosingPolicy.UNAVAILABLE);
        tabs.setStyle("-fx-background-color:#12121e;");

        Tab watchlistTab  = new Tab("Watchlist",  new QuoteView());
        Tab portfolioTab  = new Tab("Portfolio",  new PortfolioView());
        Tab alertsTab     = new Tab("Alerts",     new AlertView());
        tabs.getTabs().addAll(watchlistTab, portfolioTab, alertsTab);

        /* ── Status bar ─────────────────────────────────────────── */
        Label statusLabel = new Label("Connecting to backend…");
        statusLabel.setTextFill(Color.GRAY);
        statusLabel.setFont(Font.font("Monospaced", 11));
        statusLabel.setPadding(new Insets(3, 8, 3, 8));

        BackendClient.getInstance().addQuoteListener(q ->
                statusLabel.setText("Live  ·  " + q.getSymbol() + " $" + String.format("%.4f", q.getPrice())));

        HBox statusBar = new HBox(statusLabel);
        statusBar.setStyle("-fx-background-color:#0e0e1a;");

        BorderPane root = new BorderPane();
        root.setCenter(tabs);
        root.setBottom(statusBar);
        root.setStyle("-fx-background-color:#12121e;");

        Scene scene = new Scene(root, 1200, 750);
        scene.getStylesheets().add(getClass().getResource("/com/stockapp/dark.css").toExternalForm());

        stage.setScene(scene);
        stage.setTitle("StockApp");
        stage.show();
    }

    /** Resolve API key from env / system property / args */
    private static String resolveApiKey() {
        String k = System.getenv("POLYGON_API_KEY");
        if (k != null && !k.isBlank()) return k;
        k = System.getProperty("polygon.api.key");
        if (k != null && !k.isBlank()) return k;
        // args are not available here; handled in main()
        return null;
    }

    /** Attempt to launch the C backend as a subprocess */
    private void launchBackend(String apiKey) {
        String binEnv = System.getenv("STOCK_BACKEND_BIN");
        String bin    = binEnv != null ? binEnv : findBackendBin();
        if (bin == null) {
            LOG.info("C backend binary not found – assuming it is already running");
            return;
        }
        try {
            backendProcess = new ProcessBuilder(bin, apiKey)
                    .redirectErrorStream(true)
                    .start();
            // forward backend stdout to Java console
            Thread reader = new Thread(() -> {
                try (BufferedReader r = new BufferedReader(
                        new InputStreamReader(backendProcess.getInputStream()))) {
                    String line;
                    while ((line = r.readLine()) != null)
                        System.out.println("[BACKEND] " + line);
                } catch (IOException ignored) {}
            }, "backend-stdout");
            reader.setDaemon(true);
            reader.start();
            LOG.info("Launched backend: " + bin);
        } catch (IOException e) {
            LOG.warning("Could not launch backend: " + e.getMessage());
        }
    }

    private static String findBackendBin() {
        String[] candidates = {
            "stock-backend.exe", "stock-backend",
            "../backend/stock-backend.exe", "../backend/stock-backend"
        };
        for (String c : candidates) {
            if (new java.io.File(c).exists()) return c;
        }
        return null;
    }

    @Override
    public void stop() {
        BackendClient.getInstance().stop();
        if (backendProcess != null && backendProcess.isAlive())
            backendProcess.destroyForcibly();
    }

    /* ── API key input dialog (shown when key not found in env) ─── */

    private void showApiKeyDialog(Stage owner, java.util.function.Consumer<String> callback) {
        Stage dialog = new Stage();
        dialog.initOwner(owner);
        dialog.initModality(Modality.APPLICATION_MODAL);
        dialog.setTitle("Polygon.io API Key");

        Label info = new Label(
            "Enter your Polygon.io API key.\n" +
            "Get a free key at https://polygon.io");
        info.setWrapText(true);
        info.setTextFill(Color.LIGHTGRAY);

        TextField keyField = new TextField();
        keyField.setPromptText("API key");
        keyField.setPrefWidth(360);

        Button okBtn = new Button("Connect");
        okBtn.setDefaultButton(true);
        okBtn.setOnAction(e -> {
            String k = keyField.getText().trim();
            if (!k.isEmpty()) { dialog.close(); callback.accept(k); }
        });

        VBox vb = new VBox(12, info, keyField, okBtn);
        vb.setPadding(new Insets(20));
        vb.setStyle("-fx-background-color:#1e1e2e;");

        dialog.setScene(new Scene(vb));
        dialog.showAndWait();
    }

    /* ── Program entry point ────────────────────────────────────── */

    public static void main(String[] args) {
        // If API key is passed as first argument, expose it as system property
        if (args.length > 0 && !args[0].isBlank())
            System.setProperty("polygon.api.key", args[0]);
        launch(args);
    }
}
