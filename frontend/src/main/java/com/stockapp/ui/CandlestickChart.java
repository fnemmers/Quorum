package com.stockapp.ui;

import com.stockapp.model.OHLCBar;
import javafx.scene.canvas.*;
import javafx.scene.layout.Pane;
import javafx.scene.paint.Color;
import javafx.scene.text.*;

import java.time.format.DateTimeFormatter;
import java.util.List;

/**
 * CandlestickChart
 * ────────────────
 * A JavaFX Canvas-based candlestick / OHLCV chart.
 * No external charting library needed.
 *
 * Bullish bars (close ≥ open) are drawn in green; bearish in red.
 * Resizes automatically when the parent Pane resizes.
 */
public class CandlestickChart extends Pane {

    private static final Color BG        = Color.rgb(18,  18,  30);
    private static final Color BULL      = Color.rgb(38, 166, 154);   /* teal  */
    private static final Color BEAR      = Color.rgb(239,  83,  80);  /* red   */
    private static final Color GRID      = Color.rgb(40,  40,  60);
    private static final Color TEXT_COL  = Color.rgb(200, 200, 220);
    private static final int   PAD_L     = 70;  /* left  padding (price axis) */
    private static final int   PAD_B     = 40;  /* bottom padding (time axis) */
    private static final int   PAD_T     = 20;
    private static final int   PAD_R     = 20;

    private static final DateTimeFormatter TIME_FMT =
            DateTimeFormatter.ofPattern("MMM dd");

    private final Canvas canvas = new Canvas();

    private List<OHLCBar> bars;
    private String        symbol = "";

    public CandlestickChart() {
        getChildren().add(canvas);
        canvas.widthProperty().bind(widthProperty());
        canvas.heightProperty().bind(heightProperty());
        widthProperty().addListener(e  -> redraw());
        heightProperty().addListener(e -> redraw());
    }

    public void setData(String symbol, List<OHLCBar> bars) {
        this.symbol = symbol;
        this.bars   = bars;
        redraw();
    }

    public void clear() {
        bars = null;
        symbol = "";
        redraw();
    }

    private void redraw() {
        GraphicsContext g = canvas.getGraphicsContext2D();
        double W = canvas.getWidth();
        double H = canvas.getHeight();

        g.setFill(BG);
        g.fillRect(0, 0, W, H);

        if (bars == null || bars.isEmpty()) {
            g.setFill(TEXT_COL);
            g.setFont(Font.font("Monospaced", 14));
            g.fillText("No data – enter a symbol and click Load", PAD_L, H / 2);
            return;
        }

        double chartW = W - PAD_L - PAD_R;
        double chartH = H - PAD_T - PAD_B;

        /* visible range – last 200 bars max */
        int start = Math.max(0, bars.size() - 200);
        List<OHLCBar> visible = bars.subList(start, bars.size());
        int n = visible.size();

        /* price range */
        double minPrice = visible.stream().mapToDouble(OHLCBar::getLow).min().orElse(0);
        double maxPrice = visible.stream().mapToDouble(OHLCBar::getHigh).max().orElse(1);
        double range    = maxPrice - minPrice;
        if (range == 0) range = 1;
        double pad      = range * 0.05;
        minPrice -= pad; maxPrice += pad; range = maxPrice - minPrice;

        /* candlestick width */
        double slotW  = chartW / Math.max(n, 1);
        double candleW = Math.max(1, slotW * 0.6);

        /* helpers */
        final double fMinPrice = minPrice, fRange = range;
        java.util.function.DoubleUnaryOperator toY = price ->
                PAD_T + chartH - ((price - fMinPrice) / fRange) * chartH;

        /* grid lines */
        g.setStroke(GRID);
        g.setLineWidth(0.5);
        int gridLines = 6;
        for (int gi = 0; gi <= gridLines; gi++) {
            double price = minPrice + (range * gi / gridLines);
            double y     = toY.applyAsDouble(price);
            g.strokeLine(PAD_L, y, W - PAD_R, y);

            g.setFill(TEXT_COL);
            g.setFont(Font.font("Monospaced", 10));
            g.fillText(String.format("%.2f", price), 2, y + 4);
        }

        /* bars */
        for (int i = 0; i < n; i++) {
            OHLCBar bar = visible.get(i);
            double  cx  = PAD_L + i * slotW + slotW / 2.0;

            double yOpen  = toY.applyAsDouble(bar.getOpen());
            double yClose = toY.applyAsDouble(bar.getClose());
            double yHigh  = toY.applyAsDouble(bar.getHigh());
            double yLow   = toY.applyAsDouble(bar.getLow());

            Color col = bar.isBullish() ? BULL : BEAR;
            g.setStroke(col);
            g.setFill(col);
            g.setLineWidth(1);

            /* wick */
            g.strokeLine(cx, yHigh, cx, yLow);

            /* body */
            double bodyTop = Math.min(yOpen, yClose);
            double bodyH   = Math.max(1, Math.abs(yClose - yOpen));
            g.fillRect(cx - candleW / 2.0, bodyTop, candleW, bodyH);
        }

        /* time axis labels (every ~30 bars) */
        g.setFill(TEXT_COL);
        g.setFont(Font.font("Monospaced", 10));
        int labelStep = Math.max(1, n / 8);
        for (int i = 0; i < n; i += labelStep) {
            OHLCBar bar = visible.get(i);
            double  cx  = PAD_L + i * slotW + slotW / 2.0;
            String  lbl = bar.toLocalDateTime().format(TIME_FMT);
            g.fillText(lbl, cx - 18, H - 8);
        }

        /* title */
        g.setFill(TEXT_COL);
        g.setFont(Font.font("Monospaced", FontWeight.BOLD, 14));
        g.fillText(symbol + "  –  Candlestick Chart (" + n + " bars)", PAD_L, 14);
    }
}
