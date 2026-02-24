package com.stockapp.model;

import javafx.beans.property.*;

/** A price alert record. */
public class AlertModel {
    private final IntegerProperty id        = new SimpleIntegerProperty();
    private final StringProperty  symbol    = new SimpleStringProperty();
    private final StringProperty  condition = new SimpleStringProperty(); // "above"|"below"
    private final DoubleProperty  price     = new SimpleDoubleProperty();
    private final BooleanProperty active    = new SimpleBooleanProperty(true);

    public AlertModel(int id, String symbol, String condition, double price) {
        this.id.set(id);
        this.symbol.set(symbol);
        this.condition.set(condition);
        this.price.set(price);
    }

    public IntegerProperty idProperty()        { return id; }
    public StringProperty  symbolProperty()    { return symbol; }
    public StringProperty  conditionProperty() { return condition; }
    public DoubleProperty  priceProperty()     { return price; }
    public BooleanProperty activeProperty()    { return active; }

    public int     getId()        { return id.get(); }
    public String  getSymbol()    { return symbol.get(); }
    public String  getCondition() { return condition.get(); }
    public double  getPrice()     { return price.get(); }
    public boolean isActive()     { return active.get(); }
    public void    setActive(boolean v) { active.set(v); }

    @Override public String toString() {
        return String.format("Alert#%d %s %s $%.2f [%s]",
                getId(), getSymbol(), getCondition(), getPrice(),
                isActive() ? "active" : "fired");
    }
}
