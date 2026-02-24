package com.stockapp.model;

import javafx.beans.property.*;

/** A portfolio position. */
public class Holding {
    private final StringProperty symbol   = new SimpleStringProperty();
    private final DoubleProperty shares   = new SimpleDoubleProperty();
    private final DoubleProperty avgPrice = new SimpleDoubleProperty();
    private final DoubleProperty current  = new SimpleDoubleProperty();

    public Holding(String symbol, double shares, double avgPrice, double current) {
        this.symbol.set(symbol);
        this.shares.set(shares);
        this.avgPrice.set(avgPrice);
        this.current.set(current);
    }

    public StringProperty symbolProperty()   { return symbol; }
    public DoubleProperty sharesProperty()   { return shares; }
    public DoubleProperty avgPriceProperty() { return avgPrice; }
    public DoubleProperty currentProperty()  { return current; }

    public String getSymbol()    { return symbol.get(); }
    public double getShares()    { return shares.get(); }
    public double getAvgPrice()  { return avgPrice.get(); }
    public double getCurrent()   { return current.get(); }
    public void   setCurrent(double v) { current.set(v); }

    /** Unrealised P&L */
    public double getPnL() {
        return (current.get() - avgPrice.get()) * shares.get();
    }

    /** P&L as percentage */
    public double getPnLPct() {
        if (avgPrice.get() == 0) return 0;
        return (current.get() - avgPrice.get()) / avgPrice.get() * 100.0;
    }

    /** Total market value */
    public double getMarketValue() {
        return current.get() * shares.get();
    }
}
