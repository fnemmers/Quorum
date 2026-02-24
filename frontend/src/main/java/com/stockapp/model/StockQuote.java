package com.stockapp.model;

import javafx.beans.property.*;

/** Live quote received from the C backend. */
public class StockQuote {
    private final StringProperty  symbol    = new SimpleStringProperty();
    private final DoubleProperty  price     = new SimpleDoubleProperty();
    private final DoubleProperty  bid       = new SimpleDoubleProperty();
    private final DoubleProperty  ask       = new SimpleDoubleProperty();
    private final LongProperty    volume    = new SimpleLongProperty();
    private final LongProperty    timestamp = new SimpleLongProperty();

    public StockQuote() {}

    public StockQuote(String symbol, double price, double bid, double ask,
                      long volume, long timestamp) {
        this.symbol.set(symbol);
        this.price.set(price);
        this.bid.set(bid);
        this.ask.set(ask);
        this.volume.set(volume);
        this.timestamp.set(timestamp);
    }

    /* -- Property accessors -- */
    public StringProperty  symbolProperty()    { return symbol; }
    public DoubleProperty  priceProperty()     { return price; }
    public DoubleProperty  bidProperty()       { return bid; }
    public DoubleProperty  askProperty()       { return ask; }
    public LongProperty    volumeProperty()    { return volume; }
    public LongProperty    timestampProperty() { return timestamp; }

    public String  getSymbol()    { return symbol.get(); }
    public double  getPrice()     { return price.get(); }
    public double  getBid()       { return bid.get(); }
    public double  getAsk()       { return ask.get(); }
    public long    getVolume()    { return volume.get(); }
    public long    getTimestamp() { return timestamp.get(); }

    public void setSymbol(String v)    { symbol.set(v); }
    public void setPrice(double v)     { price.set(v); }
    public void setBid(double v)       { bid.set(v); }
    public void setAsk(double v)       { ask.set(v); }
    public void setVolume(long v)      { volume.set(v); }
    public void setTimestamp(long v)   { timestamp.set(v); }

    /** Spread in absolute terms */
    public double spread() { return ask.get() - bid.get(); }

    @Override public String toString() {
        return String.format("%s  $%.4f  bid=%.4f  ask=%.4f  vol=%,d",
                getSymbol(), getPrice(), getBid(), getAsk(), getVolume());
    }
}
