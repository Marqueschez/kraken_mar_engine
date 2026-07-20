# Kraken MAR Engine

A high-performance C++ feature extraction engine that processes Level 3 (L3) order book data from Kraken exchange and generates input features for Matrix Autoregressive (MAR) models.

## Overview

This system transforms raw market microstructure data (order book snapshots, trades, order flow) into a comprehensive feature set designed for multivariate time series forecasting. It processes multiple cryptocurrency assets simultaneously, generating 24 economic features per asset at 100ms intervals.

**Key Capabilities:**
- Processes Level 3 order book data with individual order-level granularity
- Extracts 24 features across liquidity, order flow, volatility, and technical dimensions
- Supports multi-asset processing (10 crypto pairs by default)
- Outputs feature matrices suitable for MAR/VAR modeling
- Efficient batch processing with memory management optimizations

## Table of Contents

- [Architecture](#architecture)
- [Data Sources](#data-sources)
- [Features](#features)
- [Matrix Autoregressive Models](#matrix-autoregressive-models)
- [Installation](#installation)
- [Configuration](#configuration)
- [Usage](#usage)
- [Output Format](#output-format)
- [Technical Details](#technical-details)

## Architecture

### System Components

```
┌─────────────────────────────────────────────────────┐
│  QuestDB (Time-Series Database)                    │
│  ├─ kraken_l3_book_levels_agg (snapshots)          │
│  ├─ kraken_trades (individual trades)              │
│  ├─ kraken_l3_flow_features (OFI per level)        │
│  └─ OHLC bars (1-sec candles)                      │
└─────────────────┬─────────────────────────────────┘
                  │ PostgreSQL libpq
                  ▼
┌─────────────────────────────────────────────────────┐
│ QuestDBConnector                                    │
│ - Batch fetches 5-min chunks per asset             │
│ - Maintains 2-second context carryover             │
└─────────────────┬─────────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────────────────┐
│ FeatureCalculator                                   │
│ - LOB features (6)                                  │
│ - Flow features (4)                                 │
│ - Volatility features (4)                           │
│ - Contextual features (10)                          │
└─────────────────┬─────────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────────────────┐
│ CSV Output (Feature Matrix)                        │
│ timestamp, f0_BTC, f1_BTC, ..., f23_USDT           │
└─────────────────────────────────────────────────────┘
```

### Source Files

| File | Purpose |
|------|---------|
| [main.cpp](src/main.cpp) | Main orchestration engine, batch processing loop |
| [config.h](src/config.h) | Configuration parameters and hyperparameters |
| [data_structures.h](src/data_structures.h) | Data type definitions (BookSnapshot, Trade, FlowData) |
| [questdb_connector.h/cpp](src/questdb_connector.h) | Database interface for QuestDB via PostgreSQL protocol |
| [feature_calculator.h/cpp](src/feature_calculator.h) | Feature computation algorithms |
| [CMakeLists.txt](CMakeLists.txt) | Build configuration (C++20, PostgreSQL) |

## Data Sources

### Input Tables (QuestDB)

#### 1. kraken_l3_book_levels_agg
Level-by-level aggregated order book snapshots (8 levels stored, 5 used for features):
- **Per level**: `bid{1-8}_price`, `bid{1-8}_size`, `bid{1-8}_orders`
- **Per level**: `ask{1-8}_price`, `ask{1-8}_size`, `ask{1-8}_orders`
- **Concentration**: `bid{1-5}_hhi`, `ask{1-5}_hhi` (Herfindahl-Hirschman Index)
- **Order aging**: `bid{1-5}_toporderage_ms`, `ask{1-5}_toporderage_ms`

#### 2. kraken_trades
Individual trade events:
- `timestamp`, `price`, `size`, `side` (BUY/SELL)
- `ord_type` (market/limit) - distinguishes taker vs maker trades

#### 3. kraken_l3_flow_features
Order Flow Imbalance (OFI) metrics per level:
- `ofi_level1` through `ofi_level5`
- OFI = (Bid Volume Change - Ask Volume Change)
- Positive values indicate buying pressure, negative values indicate selling pressure

#### 4. OHLC Bars
1-second aggregated candles derived from raw trades:
- Open, High, Low, Close (mid-price), Volume
- Used for multi-timeframe volatility calculations (ATR, A/D)

### Data Processing Strategy

**Efficient Batch Loading:**
1. Fetches 5-minute data chunks per asset
2. Maintains 2-second context from previous batch to ensure feature continuity
3. Processes with 100ms resampling interval
4. Prunes history older than 6 minutes to control memory

## Features

The system generates **24 features per asset** across four categories:

### 1. Limit Order Book Features (6 features)

| Feature | Description | Formula |
|---------|-------------|---------|
| **f0: Mid-Price Log Return** | Instantaneous price change | log(mid_t / mid_{t-1}) |
| **f1: Relative Spread** | Normalized bid-ask spread | (ask[0] - bid[0]) / mid_price |
| **f2: VWAP (5 levels)** | Volume-weighted average price | Σ(price_i × size_i) / Σ(size_i) |
| **f3: BBO Imbalance** | Best bid/offer volume ratio | bid[0].size / (bid[0].size + ask[0].size) |
| **f4: 5-Level Book Imbalance** | Total bid vs ask volume | Σ bid_size / (Σ bid_size + Σ ask_size) |
| **f5: WAP Skew** | Weighted average price skewness | (bid_wap - ask_wap) / mid_price |
| **f6: Liquidity Asymmetry** | Slope difference | ask_slope - bid_slope |

### 2. Market Microstructure Features (4 features)

| Feature | Description | Interpretation |
|---------|-------------|----------------|
| **f7: Bid HHI** | Herfindahl-Hirschman Index (bid side) | Order concentration metric (higher = less fragmented) |
| **f8: Ask HHI** | HHI for ask side | Market depth quality indicator |
| **f9: Mean Order Age** | Average age of top orders | Order staleness (high age = stale quotes) |
| **f22: BBO Bid Age** | Top bid order age (log-scaled) | log(1 + top_bid_order_age_ms) |
| **f23: BBO Ask Age** | Top ask order age (log-scaled) | log(1 + top_ask_order_age_ms) |

### 3. Flow & Volume Features (4 features)

| Feature | Description | Calculation |
|---------|-------------|-------------|
| **f10: Weighted OFI** | Order Flow Imbalance | Σ (OFI_level_i × weight_i) across 5 levels |
| **f11: BBO Size Change** | Net change in BBO volume | Δbid[0].size - Δask[0].size |
| **f12: Taker Buy-Sell Volume** | Directional taker flow | taker_buy_volume - taker_sell_volume |
| **f13: Taker Participation** | Taker vs total volume ratio | (taker_buy + taker_sell) / total_volume |

**Taker Classification:** Trades with `ord_type == "market"` are classified as taker-initiated (aggressive).

### 4. Volatility Features (3 features)

| Feature | Description | Window |
|---------|-------------|--------|
| **f14: Realized Volatility** | Log return standard deviation | 100ms |
| **f15: Quote Update Volatility** | BBO update frequency | log(1 + update_count) per 100ms |
| **f16: Spread Volatility** | Spread variability | 1-minute rolling (Welford's algorithm) |

### 5. Contextual/Technical Indicators (5 features)

| Feature | Description | Period |
|---------|-------------|--------|
| **f17: Average True Range (ATR)** | Price range volatility | 20 seconds |
| **f18: Price to Long EMA Ratio** | Trend deviation | (mid / EMA_long) - 1, EMA_long = 5min |
| **f19: EMA Difference** | Short-term vs long-term trend | EMA_short (1min) - EMA_long (5min) |
| **f20: RSI (14 seconds)** | Relative Strength Index | 100 - 100/(1 + RS), 14s lookback |
| **f21: Accumulation/Distribution** | Money flow indicator | Cumulative (MFM × Volume), 1min |

### Weighting Scheme

Features across multiple LOB levels use **linear weighting**:
- Level 1: 5/15 (0.333)
- Level 2: 4/15 (0.267)
- Level 3: 3/15 (0.200)
- Level 4: 2/15 (0.133)
- Level 5: 1/15 (0.067)

Alternative: **Inverse Distance** weighting (1/(i+1) normalized).

## Matrix Autoregressive Models

### Why These Features for MAR?

Matrix Autoregressive models predict multivariate time series:

```
r_t = c + Σ A_i × r_{t-i} + ε_t
```

Where:
- **r_t** is the feature vector at time t (240-dimensional for 10 assets × 24 features)
- **A_i** are coefficient matrices capturing temporal dependencies
- **c** is a constant vector
- **ε_t** is the noise term

### Design Principles

1. **Multi-Horizon Information**
   - Features span 100ms to 5-minute windows
   - Captures microstructure (milliseconds) and macro regime changes (minutes)
   - MAR coefficients learn cross-timeframe relationships

2. **Cross-Asset Dimensionality**
   - Output format: `timestamp, f0_BTC, f1_BTC, ..., f23_BTC, f0_ETH, ..., f23_USDT`
   - Full 240-dimensional feature matrix per timestamp
   - Enables cross-asset dependency modeling

3. **Stationary Properties**
   - Log returns, imbalances, ratios → naturally stationary
   - RSI, spread volatility → bounded oscillators
   - Essential for stable VAR/MAR estimation

4. **Causal Information Structure**
   - OFI predicts price moves (order flow → execution)
   - Taker volumes reveal momentum
   - HHI/concentration shows market depth quality
   - Age metrics detect quote staleness
   - **No look-ahead bias**: All features use only data available at time t

### Example MAR Application

Predict 10-asset price moves over next 100ms window:

```
y_t = [r_BTC, r_ETH, ..., r_DOGE]  (10×1 returns vector)

y_t = A1 × features_{t-1} + A2 × features_{t-2} + A3 × features_{t-3} + ε_t

where A1, A2, A3 are (10×240) matrices learned from training data
```

**Use Cases:**
- High-frequency volatility forecasting
- Cross-asset return prediction
- Market impact modeling
- Alpha generation in crypto markets

## Installation

### Prerequisites

- **CMake** 3.15 or higher
- **C++20 compatible compiler** (MSVC 2022, GCC 10+, Clang 12+)
- **PostgreSQL 17+** (includes libpq development headers)
  - Windows: Install from [postgresql.org](https://www.postgresql.org/download/windows/)
  - Linux: `sudo apt-get install postgresql-server-dev-17`
- **QuestDB** instance with Kraken L3 data

### Build Instructions

#### Windows (Visual Studio 2022)

```bash
cd c:\dev\kraken_mar_engine
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug
```

Output: `build/Debug/feature_engine.exe`

#### Linux

```bash
cd /path/to/kraken_mar_engine
cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Output: `build/feature_engine`

### PostgreSQL Configuration

Ensure PostgreSQL client libraries are found by CMake:

**Windows:**
```cmake
# CMakeLists.txt automatically searches:
C:/Program Files/PostgreSQL/17/include
C:/Program Files/PostgreSQL/17/lib
```

**Linux:**
```bash
export PostgreSQL_ROOT=/usr/lib/postgresql/17
```

## Configuration

Edit [src/config.h](src/config.h) before building:

### Database Connection

```cpp
const std::string db_host = "localhost";
const int db_port = 8812;              // QuestDB PostgreSQL wire protocol
const std::string db_user = "admin";
const std::string db_pass = "quest";
const std::string db_name = "qdb";
```

### Assets to Process

```cpp
const std::vector<std::string> assets_to_process = {
    "BTC/USD", "ETH/USD", "SOL/USD", "XRP/USD", "DOGE/USD",
    "ADA/USD", "LTC/USD", "LINK/USD", "DOT/USD", "USDT/USD"
};
```

### Time Range

```cpp
const std::string start_time = "2025-08-12T07:37:59.000000Z";
const std::string end_time   = "2025-08-12T10:55:55.000000Z";
```

### Feature Parameters

```cpp
const int resampling_interval_ms = 100;     // Feature generation frequency
const int lob_depth_for_features = 5;       // Use levels 1-5 of order book

enum LevelWeightingScheme { LINEAR, INVERSE_DISTANCE };
const LevelWeightingScheme level_weighting_scheme = LINEAR;

// Technical indicator periods
const std::chrono::seconds contextual_ma_short_period{60};      // 1 minute
const std::chrono::seconds contextual_ma_long_period{300};      // 5 minutes
const std::chrono::seconds contextual_rsi_period_s{14};         // 14 seconds
const std::chrono::seconds contextual_ad_flow_period_s{60};     // 1 minute
const std::chrono::seconds contextual_bollinger_period_s{20};   // 20 seconds (ATR)
```

### Output

```cpp
const std::string output_csv_path = "data/features_output3.csv";
```

## Usage

### Running the Engine

```bash
# From project root directory
./build/Debug/feature_engine.exe       # Windows
./build/feature_engine                  # Linux
```

### Expected Output

**Console:**
```
Processing period: 2025-08-12T07:37:59.000000Z to 2025-08-12T10:55:55.000000Z
Resampling interval: 100 ms
Assets: BTC/USD, ETH/USD, SOL/USD, XRP/USD, DOGE/USD, ADA/USD, LTC/USD, LINK/USD, DOT/USD, USDT/USD

Fetching data for BTC/USD [07:37:59 - 07:42:59]...
Computing features: [=====>    ] 50%
...
CSV output written: data/features_output3.csv (532.9 MB)
```

### Debugging (VSCode)

Use [.vscode/launch.json](.vscode/launch.json):

```json
{
  "name": "Debug Feature Engine",
  "type": "cppvsdbg",
  "request": "launch",
  "program": "${workspaceFolder}/build/Debug/feature_engine.exe",
  "cwd": "${workspaceFolder}",
  "stopAtEntry": false
}
```

Press F5 to start debugging.

## Output Format

### CSV Structure

**Header:**
```csv
timestamp,BTC/USD_f0,BTC/USD_f1,...,BTC/USD_f23,ETH/USD_f0,...,USDT/USD_f23
```

**Data Rows (100ms intervals):**
```csv
2025-08-12T07:37:59.000000Z,0.00234,-0.00012,0.51234,0.48765,...
2025-08-12T07:37:59.100000Z,0.00198,-0.00018,0.51456,0.48921,...
```

**Dimensions:**
- **Rows**: `(end_time - start_time) / resampling_interval_ms`
- **Columns**: 1 (timestamp) + 10 assets × 24 features = **241 columns**

### Feature Column Naming

Format: `{ASSET}_{f#}`

Example columns:
- `BTC/USD_f0` → Mid-price log return (BTC)
- `BTC/USD_f10` → Weighted OFI (BTC)
- `ETH/USD_f20` → RSI (ETH)
- `USDT/USD_f23` → BBO Ask Age (USDT)

### Example Output Files

From `data/` directory:
- `features_output0.csv`: 532.9 MB (~5 hours, 10 assets)
- `features_output1.csv`: 333.8 MB
- `features_output2.csv`: 758.4 MB

## Technical Details

### Data Processing Pipeline

**Per 100ms Timestamp** ([main.cpp:225-240](src/main.cpp#L225-L240)):

1. **Locate Data Points**
   - Binary search (`std::lower_bound`) to find latest snapshot ≤ current_time
   - Scan trades/flows/OHLC within `[current_time - 100ms, current_time]`

2. **Compute Log Returns**
   ```cpp
   log_returns[i] = log(mid_price[t_i] / mid_price[t_{i-1}])
   ```

3. **Count BBO Updates**
   - Snapshots with changed `bid[0]` or `ask[0]`

4. **Calculate Features**
   - [calculate_lob_features()](src/feature_calculator.cpp#L60)
   - [calculate_flow_features()](src/feature_calculator.cpp#L200)
   - [calculate_volatility_features()](src/feature_calculator.cpp#L270)
   - [calculate_contextual_features()](src/feature_calculator.cpp#L305)

5. **Buffer Output**
   - Accumulate 4096 rows before flushing to CSV (reduces I/O overhead)

### Key Algorithms

| Algorithm | Location | Purpose |
|-----------|----------|---------|
| **Linear Regression** | [feature_calculator.cpp:172](src/feature_calculator.cpp#L172) | Compute bid/ask slopes for liquidity asymmetry |
| **Welford's Algorithm** | [feature_calculator.cpp:291](src/feature_calculator.cpp#L291) | Online variance (spread volatility) |
| **Binary Search** | [main.cpp:171](src/main.cpp#L171) | Find data boundaries in sorted buffers |
| **EMA** | [feature_calculator.cpp:360](src/feature_calculator.cpp#L360) | Exponential moving average for trend |
| **Money Flow Multiplier** | [feature_calculator.cpp:338](src/feature_calculator.cpp#L338) | A/D line calculation |
| **True Range** | [feature_calculator.cpp:315](src/feature_calculator.cpp#L315) | ATR component |

### Memory Management

**Efficient History Management:**
1. **2-Second Context Carryover**: Prevents feature discontinuities at batch boundaries
2. **Deque Pruning**: Removes data older than 6 minutes to control memory growth
3. **Pre-allocated Buffers**: `vector::reserve()` for 5-minute chunks
4. **Welford's Online Algorithm**: Avoids storing all values for std calculations

**State Per Asset:**
```cpp
struct AssetState {
  std::vector<BookSnapshot> snapshot_buffer;
  std::vector<Trade> trade_buffer;
  std::vector<FlowData> flow_buffer;

  size_t snapshot_buf_pos;  // Current read position
  size_t trade_buf_pos;
  size_t flow_buf_pos;

  bool buffer_initialized;
  std::chrono::system_clock::time_point buffer_end_tp;
};
```

### Timestamp Handling

**Cross-Platform UTC Parsing:**
- Config format: `"2025-08-12T07:37:59.000000Z"` (ISO 8601)
- Database format: `"2025-08-12 07:37:59.000000"` (SQL)

**Conversion** ([main.cpp:56-89](src/main.cpp#L56-L89)):
```cpp
1. Parse YYYY-MM-DD HH:MM:SS.ffffff
2. Adjust tm struct (year -= 1900, month -= 1)
3. Convert to time_t using platform-specific UTC function
   - Windows: _mkgmtime()
   - POSIX: timegm()
4. Add microsecond component
5. Convert to std::chrono::system_clock::time_point
```

### Optimization Strategies

1. **Batch Database Fetches**: 5-minute chunks vs. per-100ms queries
2. **CSV Output Buffering**: 4096 rows vs. per-row I/O
3. **Single-Pass Processing**: Position pointers only move forward
4. **Pre-computed Weights**: Level weights calculated once at startup
5. **In-Place Updates**: EMA/RSI use running state, no full recalculation

## Performance Characteristics

### Throughput

- **Processing speed**: ~10-50x real-time (depends on hardware)
- **Memory footprint**: ~500 MB for 10 assets with 6-minute history buffers
- **Database I/O**: ~200-500 queries per hour (5-min batches)

### Scalability

- **Assets**: Linear scaling (10 assets → 20 assets ≈ 2× time)
- **Time range**: Linear scaling with pruning (6 minutes → 6 hours ≈ 60× time, constant memory)
- **Feature count**: Minimal impact (most computation is data loading)

## Limitations & Future Work

### Current Limitations

1. **Hardcoded Asset List**: Requires recompilation to change assets
2. **CSV Output Only**: No direct database output or streaming interface
3. **No Real-Time Mode**: Batch processing only
4. **Windows/Linux Only**: Platform-specific timestamp functions

### Potential Improvements

1. **Configuration File**: Move parameters from `config.h` to JSON/YAML
2. **Streaming Output**: Direct QuestDB write or Apache Arrow format
3. **Parallel Asset Processing**: Multi-threading for independent assets
4. **Feature Selection**: Runtime feature subset selection
5. **Parameterized SQL**: Prevent potential injection (though currently safe)

## References

### Research Papers

1. **Order Flow Imbalance**: Cont, R., Kukanov, A., & Stoikov, S. (2014). "The Price Impact of Order Book Events". *Journal of Financial Econometrics*.

2. **Matrix Autoregressive Models**: Lütkepohl, H. (2005). *New Introduction to Multiple Time Series Analysis*. Springer.

3. **Market Microstructure**: Hasbrouck, J. (2007). *Empirical Market Microstructure: The Institutions, Economics, and Econometrics of Securities Trading*.

4. **HHI in Finance**: Brogaard, J., Hendershott, T., & Riordan, R. (2014). "High-Frequency Trading and Price Discovery". *Review of Financial Studies*.

### Related Tools

- **QuestDB**: [questdb.io](https://questdb.io) - Time-series database
- **Kraken API**: [docs.kraken.com](https://docs.kraken.com/websockets/) - L3 order book feed
- **MAR Libraries**: statsmodels (Python), vars (R), MFE Toolbox (MATLAB)

## License

[Specify your license here]

## Contact

[Your contact information or project repository]

---

**Generated by**: Kraken MAR Engine v1.0
**Last Updated**: 2025-10-24
