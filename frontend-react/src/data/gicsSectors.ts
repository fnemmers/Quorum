/*
 * GICS sector map for the S&P 500. Compact list of the ~190 most
 * commonly-picked names by LLM ensembles — megacaps + the names that
 * actually show up in consensus runs. Anything not in this map is
 * tagged "Other" by the diversity computation.
 *
 * Eleven canonical GICS sectors:
 *   Communication Services, Consumer Discretionary, Consumer Staples,
 *   Energy, Financials, Health Care, Industrials, Information Technology,
 *   Materials, Real Estate, Utilities.
 */

export type GicsSector =
  | 'Communication Services'
  | 'Consumer Discretionary'
  | 'Consumer Staples'
  | 'Energy'
  | 'Financials'
  | 'Health Care'
  | 'Industrials'
  | 'Information Technology'
  | 'Materials'
  | 'Real Estate'
  | 'Utilities'
  | 'Other';

export const GICS_SECTORS: GicsSector[] = [
  'Communication Services', 'Consumer Discretionary', 'Consumer Staples',
  'Energy', 'Financials', 'Health Care', 'Industrials',
  'Information Technology', 'Materials', 'Real Estate', 'Utilities',
];

export const SECTOR_COLOR: Record<GicsSector, string> = {
  'Communication Services': '#7e57c2',
  'Consumer Discretionary': '#ef6c00',
  'Consumer Staples':       '#558b2f',
  'Energy':                 '#bf360c',
  'Financials':             '#1976d2',
  'Health Care':            '#c2185b',
  'Industrials':            '#5d4037',
  'Information Technology': '#00838f',
  'Materials':              '#827717',
  'Real Estate':            '#6a1b9a',
  'Utilities':              '#283593',
  'Other':                  '#616161',
};

export const TICKER_TO_SECTOR: Record<string, GicsSector> = {
  // Information Technology
  AAPL: 'Information Technology', MSFT: 'Information Technology', NVDA: 'Information Technology',
  AVGO: 'Information Technology', ORCL: 'Information Technology', CRM:  'Information Technology',
  ADBE: 'Information Technology', AMD:  'Information Technology', CSCO: 'Information Technology',
  ACN:  'Information Technology', INTC: 'Information Technology', IBM:  'Information Technology',
  QCOM: 'Information Technology', TXN:  'Information Technology', INTU: 'Information Technology',
  NOW:  'Information Technology', AMAT: 'Information Technology', LRCX: 'Information Technology',
  MU:   'Information Technology', PANW: 'Information Technology', KLAC: 'Information Technology',
  ANET: 'Information Technology', ADI:  'Information Technology', CDNS: 'Information Technology',
  SNPS: 'Information Technology', WDAY: 'Information Technology', FTNT: 'Information Technology',
  ROP:  'Information Technology', NXPI: 'Information Technology', PLTR: 'Information Technology',
  MSI:  'Information Technology', CRWD: 'Information Technology', MPWR: 'Information Technology',
  CDW:  'Information Technology', HPE:  'Information Technology', HPQ:  'Information Technology',
  STX:  'Information Technology', WDC:  'Information Technology', SMCI: 'Information Technology',
  TEL:  'Information Technology', FFIV: 'Information Technology', JNPR: 'Information Technology',
  GLW:  'Information Technology', KEYS: 'Information Technology', NTAP: 'Information Technology',
  ENPH: 'Information Technology', GEN:  'Information Technology', EPAM: 'Information Technology',
  SWKS: 'Information Technology', QRVO: 'Information Technology', ZBRA: 'Information Technology',
  FSLR: 'Information Technology', PTC:  'Information Technology', ANSS: 'Information Technology',
  TYL:  'Information Technology', FICO: 'Information Technology',

  // Communication Services
  GOOGL: 'Communication Services', GOOG: 'Communication Services', META: 'Communication Services',
  NFLX:  'Communication Services', DIS:  'Communication Services', T:    'Communication Services',
  VZ:    'Communication Services', TMUS: 'Communication Services', CHTR: 'Communication Services',
  CMCSA: 'Communication Services', WBD:  'Communication Services', PARA: 'Communication Services',
  EA:    'Communication Services', TTWO: 'Communication Services', LYV:  'Communication Services',
  OMC:   'Communication Services', IPG:  'Communication Services', FOX:  'Communication Services',
  FOXA:  'Communication Services', NWS:  'Communication Services', NWSA: 'Communication Services',
  MTCH:  'Communication Services',

  // Consumer Discretionary
  AMZN: 'Consumer Discretionary', TSLA: 'Consumer Discretionary', HD:   'Consumer Discretionary',
  MCD:  'Consumer Discretionary', NKE:  'Consumer Discretionary', LOW:  'Consumer Discretionary',
  SBUX: 'Consumer Discretionary', BKNG: 'Consumer Discretionary', TJX:  'Consumer Discretionary',
  CMG:  'Consumer Discretionary', MAR:  'Consumer Discretionary', HLT:  'Consumer Discretionary',
  ORLY: 'Consumer Discretionary', AZO:  'Consumer Discretionary', GM:   'Consumer Discretionary',
  F:    'Consumer Discretionary', ROST: 'Consumer Discretionary', LULU: 'Consumer Discretionary',
  ABNB: 'Consumer Discretionary', YUM:  'Consumer Discretionary', DRI:  'Consumer Discretionary',
  EBAY: 'Consumer Discretionary', DPZ:  'Consumer Discretionary', GRMN: 'Consumer Discretionary',
  POOL: 'Consumer Discretionary', BBY:  'Consumer Discretionary', DECK: 'Consumer Discretionary',
  TPR:  'Consumer Discretionary', RL:   'Consumer Discretionary', ULTA: 'Consumer Discretionary',
  LVS:  'Consumer Discretionary', MGM:  'Consumer Discretionary', WYNN: 'Consumer Discretionary',
  CZR:  'Consumer Discretionary', NCLH: 'Consumer Discretionary', RCL:  'Consumer Discretionary',
  CCL:  'Consumer Discretionary', DHI:  'Consumer Discretionary', LEN:  'Consumer Discretionary',
  NVR:  'Consumer Discretionary', PHM:  'Consumer Discretionary', BLDR: 'Consumer Discretionary',
  KMX:  'Consumer Discretionary', BWA:  'Consumer Discretionary', APTV: 'Consumer Discretionary',
  EXPE: 'Consumer Discretionary',

  // Consumer Staples
  WMT: 'Consumer Staples', PG:  'Consumer Staples', COST: 'Consumer Staples',
  KO:  'Consumer Staples', PEP: 'Consumer Staples', PM:   'Consumer Staples',
  MO:  'Consumer Staples', MDLZ:'Consumer Staples', CL:   'Consumer Staples',
  KDP: 'Consumer Staples', TGT: 'Consumer Staples', GIS:  'Consumer Staples',
  HSY: 'Consumer Staples', KHC: 'Consumer Staples', KR:   'Consumer Staples',
  STZ: 'Consumer Staples', SYY: 'Consumer Staples', KVUE: 'Consumer Staples',
  EL:  'Consumer Staples', K:   'Consumer Staples', CHD:  'Consumer Staples',
  MNST:'Consumer Staples', MKC: 'Consumer Staples', CLX:  'Consumer Staples',
  KMB: 'Consumer Staples', LW:  'Consumer Staples', HRL:  'Consumer Staples',
  TAP: 'Consumer Staples', TSN: 'Consumer Staples', CAG:  'Consumer Staples',
  SJM: 'Consumer Staples', CPB: 'Consumer Staples', WBA:  'Consumer Staples',
  BG:  'Consumer Staples', ADM: 'Consumer Staples',

  // Energy
  XOM:  'Energy', CVX: 'Energy', COP: 'Energy', SLB: 'Energy',
  EOG:  'Energy', MPC: 'Energy', PSX: 'Energy', VLO: 'Energy',
  OXY:  'Energy', HES: 'Energy', WMB: 'Energy', OKE: 'Energy',
  KMI:  'Energy', BKR: 'Energy', HAL: 'Energy', DVN: 'Energy',
  FANG: 'Energy', APA: 'Energy', CTRA:'Energy', TRGP:'Energy',
  EQT:  'Energy', MRO: 'Energy', CF:  'Energy',

  // Financials
  BRK:  'Financials', JPM: 'Financials', BAC: 'Financials',
  WFC:  'Financials', MS:  'Financials', GS:  'Financials',
  C:    'Financials', SCHW:'Financials', SPGI:'Financials',
  AXP:  'Financials', BLK: 'Financials', CB:  'Financials',
  PGR:  'Financials', MMC: 'Financials', ICE: 'Financials',
  CME:  'Financials', PNC: 'Financials', USB: 'Financials',
  MCO:  'Financials', AON: 'Financials', COF: 'Financials',
  TFC:  'Financials', TRV: 'Financials', ALL: 'Financials',
  AIG:  'Financials', AFL: 'Financials', MET: 'Financials',
  PRU:  'Financials', AMP: 'Financials', BK:  'Financials',
  STT:  'Financials', NDAQ:'Financials', CBOE:'Financials',
  FI:   'Financials', PYPL:'Financials', V:   'Financials',
  MA:   'Financials', FIS: 'Financials', GPN: 'Financials',
  DFS:  'Financials', MTB: 'Financials', HBAN:'Financials',
  RF:   'Financials', FITB:'Financials', CFG: 'Financials',
  KEY:  'Financials', SYF: 'Financials', WRB: 'Financials',
  HIG:  'Financials', ACGL:'Financials', AIZ: 'Financials',
  EG:   'Financials', BEN: 'Financials', IVZ: 'Financials',
  TROW: 'Financials', NTRS:'Financials', MSCI:'Financials',
  FDS:  'Financials', BRO: 'Financials', JKHY:'Financials',
  RJF:  'Financials', BX:  'Financials', KKR: 'Financials',
  L:    'Financials', CINF:'Financials', GL:  'Financials',
  PFG:  'Financials', MKTX:'Financials', CMA: 'Financials',

  // Health Care
  LLY:   'Health Care', UNH: 'Health Care', JNJ: 'Health Care',
  MRK:   'Health Care', ABBV:'Health Care', ABT: 'Health Care',
  TMO:   'Health Care', DHR: 'Health Care', PFE: 'Health Care',
  AMGN:  'Health Care', ISRG:'Health Care', GILD:'Health Care',
  MDT:   'Health Care', BMY: 'Health Care', SYK: 'Health Care',
  CVS:   'Health Care', ELV: 'Health Care', VRTX:'Health Care',
  REGN:  'Health Care', BSX: 'Health Care', BDX: 'Health Care',
  HCA:   'Health Care', CI:  'Health Care', MCK: 'Health Care',
  ZTS:   'Health Care', EW:  'Health Care', HUM: 'Health Care',
  IDXX:  'Health Care', IQV: 'Health Care', DXCM:'Health Care',
  RMD:   'Health Care', WAT: 'Health Care', ALGN:'Health Care',
  COR:   'Health Care', BIIB:'Health Care', BAX: 'Health Care',
  CAH:   'Health Care', LH:  'Health Care', DGX: 'Health Care',
  WST:   'Health Care', VTRS:'Health Care', INCY:'Health Care',
  CRL:   'Health Care', MRNA:'Health Care', PODD:'Health Care',
  CTLT:  'Health Care', BIO: 'Health Care', RVTY:'Health Care',
  TECH:  'Health Care', UHS: 'Health Care', MOH: 'Health Care',
  CNC:   'Health Care', HOLX:'Health Care', STE: 'Health Care',
  HSIC:  'Health Care', GEHC:'Health Care', SOLV:'Health Care',
  DVA:   'Health Care', TFX: 'Health Care', MTD: 'Health Care',
  ILMN:  'Health Care', VRSK:'Health Care',

  // Industrials
  GE:   'Industrials', RTX:  'Industrials', CAT: 'Industrials',
  UNP:  'Industrials', HON:  'Industrials', LMT: 'Industrials',
  BA:   'Industrials', UPS:  'Industrials', DE:  'Industrials',
  ETN:  'Industrials', NOC:  'Industrials', WM:  'Industrials',
  ITW:  'Industrials', GD:   'Industrials', CSX: 'Industrials',
  NSC:  'Industrials', EMR:  'Industrials', PH:  'Industrials',
  TT:   'Industrials', RSG:  'Industrials', FDX: 'Industrials',
  CTAS: 'Industrials', PCAR: 'Industrials', JCI: 'Industrials',
  MMM:  'Industrials', URI:  'Industrials', PWR: 'Industrials',
  GWW:  'Industrials', AXON: 'Industrials', CARR:'Industrials',
  HWM:  'Industrials', IEX:  'Industrials', WAB: 'Industrials',
  TDG:  'Industrials', DOV:  'Industrials', FAST:'Industrials',
  HII:  'Industrials', LHX:  'Industrials', LDOS:'Industrials',
  ODFL: 'Industrials', JBHT: 'Industrials', CHRW:'Industrials',
  EXPD: 'Industrials', LUV:  'Industrials', UAL: 'Industrials',
  DAL:  'Industrials', AAL:  'Industrials', UBER:'Industrials',
  ALLE: 'Industrials', AOS:  'Industrials', BR:  'Industrials',
  CMI:  'Industrials', ROK:  'Industrials', TXT: 'Industrials',
  PNR:  'Industrials', SNA:  'Industrials', MAS: 'Industrials',
  TER:  'Industrials', PAYX: 'Industrials', PAYC:'Industrials',
  ADP:  'Industrials', CPRT: 'Industrials', VRSN:'Industrials',
  GNRC: 'Industrials', FTV:  'Industrials', GEV: 'Industrials',
  J:    'Industrials', DAY:  'Industrials',
  ROL:  'Industrials',

  // Materials
  LIN:  'Materials', SHW: 'Materials', APD: 'Materials',
  ECL:  'Materials', NEM: 'Materials', FCX: 'Materials',
  DOW:  'Materials', PPG: 'Materials', DD:   'Materials',
  CTVA: 'Materials', NUE: 'Materials', VMC:  'Materials',
  MLM:  'Materials', STLD:'Materials', LYB:  'Materials',
  PKG:  'Materials', IP:  'Materials', AVY:  'Materials',
  IFF:  'Materials', BALL:'Materials', CE:   'Materials',
  ALB:  'Materials', EMN: 'Materials', AMCR: 'Materials',
  FMC:  'Materials', MOS: 'Materials', MHK:  'Materials',

  // Real Estate
  PLD:  'Real Estate', AMT: 'Real Estate', WELL: 'Real Estate',
  EQIX: 'Real Estate', CCI: 'Real Estate', SPG:  'Real Estate',
  PSA:  'Real Estate', O:   'Real Estate', DLR:  'Real Estate',
  CSGP: 'Real Estate', CBRE:'Real Estate', EXR:  'Real Estate',
  AVB:  'Real Estate', VICI:'Real Estate', EQR:  'Real Estate',
  IRM:  'Real Estate', VTR: 'Real Estate', MAA:  'Real Estate',
  ESS:  'Real Estate', INVH:'Real Estate', UDR:  'Real Estate',
  ARE:  'Real Estate', BXP: 'Real Estate', KIM:  'Real Estate',
  REG:  'Real Estate', SBAC:'Real Estate', FRT:  'Real Estate',
  HST:  'Real Estate', CPT: 'Real Estate', DOC:  'Real Estate',

  // Utilities
  NEE:  'Utilities', SO:  'Utilities', DUK: 'Utilities',
  SRE:  'Utilities', AEP: 'Utilities', D:   'Utilities',
  CEG:  'Utilities', PCG: 'Utilities', EXC: 'Utilities',
  XEL:  'Utilities', PEG: 'Utilities', WEC: 'Utilities',
  ED:   'Utilities', ETR: 'Utilities', AWK: 'Utilities',
  ES:   'Utilities', DTE: 'Utilities', PPL: 'Utilities',
  EIX:  'Utilities', FE:  'Utilities', AEE: 'Utilities',
  CMS:  'Utilities', CNP: 'Utilities', NI:  'Utilities',
  ATO:  'Utilities', LNT: 'Utilities', EVRG:'Utilities',
  NRG:  'Utilities', PNW: 'Utilities', AES: 'Utilities',
};

export function sectorOf(symbol: string): GicsSector {
  return TICKER_TO_SECTOR[symbol] ?? 'Other';
}
