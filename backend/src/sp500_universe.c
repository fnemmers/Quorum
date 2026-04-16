/*
 * sp500_universe.c — Static S&P 500 ticker list.
 *
 * Snapshot date: early 2025. Regenerate from a current constituent source
 * (e.g. Wikipedia's "List of S&P 500 companies", iShares IVV holdings CSV)
 * when you want a fresh universe. Constituents change a few times per
 * year — this is a known source of survivorship bias for backtests that
 * span constituent rebalances. Mention it in NOTES.md.
 *
 * Sorted alphabetically; sp500_contains uses linear scan (fine for ~500
 * entries — about 250 strcmps worst case, sub-microsecond). If you ever
 * need it faster, switch to bsearch since the array is sorted.
 */

#include "sp500_universe.h"
#include <string.h>

static const char *const SP500[] = {
    "A","AAPL","ABBV","ABNB","ABT","ACGL","ACN","ADBE","ADI","ADM",
    "ADP","ADSK","AEE","AEP","AES","AFL","AIG","AIZ","AJG","AKAM",
    "ALB","ALGN","ALL","ALLE","AMAT","AMCR","AMD","AME","AMGN","AMP",
    "AMT","AMZN","ANET","ANSS","AON","AOS","APA","APD","APH","APTV",
    "ARE","ATO","AVB","AVGO","AVY","AWK","AXON","AXP","AZO","BA",
    "BAC","BALL","BAX","BBWI","BBY","BDX","BEN","BF.B","BG","BIIB",
    "BIO","BK","BKNG","BKR","BLDR","BLK","BMY","BR","BRK.B","BRO",
    "BSX","BWA","BX","BXP","C","CAG","CAH","CARR","CAT","CB",
    "CBOE","CBRE","CCI","CCL","CDNS","CDW","CE","CEG","CF","CFG",
    "CHD","CHRW","CHTR","CI","CINF","CL","CLX","CMA","CMCSA","CME",
    "CMG","CMI","CMS","CNC","CNP","COF","COO","COP","COR","COST",
    "CPB","CPRT","CPT","CRL","CRM","CRWD","CSCO","CSGP","CSX","CTAS",
    "CTLT","CTRA","CTSH","CTVA","CVS","CVX","CZR","D","DAL","DAY",
    "DD","DE","DECK","DFS","DG","DGX","DHI","DHR","DIS","DLR",
    "DLTR","DOC","DOV","DOW","DPZ","DRI","DTE","DUK","DVA","DVN",
    "DXCM","EA","EBAY","ECL","ED","EFX","EG","EIX","EL","ELV",
    "EMN","EMR","ENPH","EOG","EPAM","EQIX","EQR","EQT","ES","ESS",
    "ETN","ETR","ETSY","EVRG","EW","EXC","EXPD","EXPE","EXR","F",
    "FANG","FAST","FCX","FDS","FDX","FE","FFIV","FI","FICO","FIS",
    "FITB","FLT","FMC","FOX","FOXA","FRT","FSLR","FTNT","FTV","GD",
    "GDDY","GE","GEHC","GEN","GEV","GILD","GIS","GL","GLW","GM",
    "GNRC","GOOG","GOOGL","GPC","GPN","GRMN","GS","GWW","HAL","HAS",
    "HBAN","HCA","HD","HES","HIG","HII","HLT","HOLX","HON","HPE",
    "HPQ","HRL","HSIC","HST","HSY","HUBB","HUM","HWM","IBM","ICE",
    "IDXX","IEX","IFF","ILMN","INCY","INTC","INTU","INVH","IP","IPG",
    "IQV","IR","IRM","ISRG","IT","ITW","IVZ","J","JBHT","JBL",
    "JCI","JKHY","JNJ","JNPR","JPM","K","KDP","KEY","KEYS","KHC",
    "KIM","KKR","KLAC","KMB","KMI","KMX","KO","KR","KVUE","L",
    "LDOS","LEN","LH","LHX","LIN","LKQ","LLY","LMT","LNT","LOW",
    "LRCX","LULU","LUV","LVS","LW","LYB","LYV","MA","MAA","MAR",
    "MAS","MCD","MCHP","MCK","MCO","MDLZ","MDT","MET","META","MGM",
    "MHK","MKC","MKTX","MLM","MMC","MMM","MNST","MO","MOH","MOS",
    "MPC","MPWR","MRK","MRNA","MRO","MS","MSCI","MSFT","MSI","MTB",
    "MTCH","MTD","MU","NCLH","NDAQ","NDSN","NEE","NEM","NFLX","NI",
    "NKE","NOC","NOW","NRG","NSC","NTAP","NTRS","NUE","NVDA","NVR",
    "NWS","NWSA","NXPI","O","ODFL","OKE","OMC","ON","ORCL","ORLY",
    "OTIS","OXY","PANW","PARA","PAYC","PAYX","PCAR","PCG","PEG","PEP",
    "PFE","PFG","PG","PGR","PH","PHM","PKG","PLD","PLTR","PM",
    "PNC","PNR","PNW","PODD","POOL","PPG","PPL","PRU","PSA","PSX",
    "PTC","PWR","PYPL","QCOM","QRVO","RCL","REG","REGN","RF","RJF",
    "RL","RMD","ROK","ROL","ROP","ROST","RSG","RTX","RVTY","SBAC",
    "SBUX","SCHW","SHW","SJM","SLB","SMCI","SNA","SNPS","SO","SOLV",
    "SPG","SPGI","SRE","STE","STLD","STT","STX","STZ","SWK","SWKS",
    "SYF","SYK","SYY","T","TAP","TDG","TDY","TECH","TEL","TER",
    "TFC","TFX","TGT","TJX","TMO","TMUS","TPR","TRGP","TRMB","TROW",
    "TRV","TSCO","TSLA","TSN","TT","TTWO","TXN","TXT","TYL","UAL",
    "UBER","UDR","UHS","ULTA","UNH","UNP","UPS","URI","USB","V",
    "VICI","VLO","VMC","VRSK","VRSN","VRTX","VTR","VTRS","VZ","WAB",
    "WAT","WBA","WBD","WDC","WEC","WELL","WFC","WM","WMB","WMT",
    "WRB","WST","WTW","WY","WYNN","XEL","XOM","XYL","YUM","ZBH",
    "ZBRA","ZTS"
};

const char *const *sp500_tickers(void) { return SP500; }

size_t sp500_count(void) { return sizeof(SP500) / sizeof(SP500[0]); }

int sp500_contains(const char *symbol) {
    if (!symbol) return 0;
    size_t n = sp500_count();
    for (size_t i = 0; i < n; i++)
        if (strcmp(SP500[i], symbol) == 0) return 1;
    return 0;
}
