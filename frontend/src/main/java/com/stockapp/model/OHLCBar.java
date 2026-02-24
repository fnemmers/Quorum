package com.stockapp.model;

import java.time.Instant;
import java.time.LocalDateTime;
import java.time.ZoneId;

/** One OHLCV candlestick bar. */
public class OHLCBar {
    private final long   timestamp;  // Unix ms
    private final double open;
    private final double high;
    private final double low;
    private final double close;
    private final long   volume;

    public OHLCBar(long timestamp, double open, double high,
                   double low, double close, long volume) {
        this.timestamp = timestamp;
        this.open      = open;
        this.high      = high;
        this.low       = low;
        this.close     = close;
        this.volume    = volume;
    }

    public long   getTimestamp() { return timestamp; }
    public double getOpen()      { return open; }
    public double getHigh()      { return high; }
    public double getLow()       { return low; }
    public double getClose()     { return close; }
    public long   getVolume()    { return volume; }

    public boolean isBullish()   { return close >= open; }

    public LocalDateTime toLocalDateTime() {
        return LocalDateTime.ofInstant(
                Instant.ofEpochMilli(timestamp), ZoneId.systemDefault());
    }

    @Override public String toString() {
        return String.format("[%s] O=%.2f H=%.2f L=%.2f C=%.2f V=%,d",
                toLocalDateTime(), open, high, low, close, volume);
    }
}
