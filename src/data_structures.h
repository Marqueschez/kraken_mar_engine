// C:\dev\kraken_mar_engine\src\data_structures.h

#pragma once
#include <string>
#include <array>
#include <vector>
#include <map>
#include <chrono>
#include <deque>

constexpr int MAX_LOB_LEVELS = 8;

struct OHLCBar
{
    std::chrono::system_clock::time_point timestamp;
    double open = 0.0, high = 0.0, low = 0.0, close = 0.0, volume = 0.0;
};

struct BookLevel
{
    double price = 0.0, size = 0.0;
    long orders = 0;
    double hhi = 0.0;
    long topOrderAge_ms = 0;
};

struct BookSnapshot
{
    std::chrono::system_clock::time_point timestamp;
    std::array<BookLevel, MAX_LOB_LEVELS> bids;
    std::array<BookLevel, MAX_LOB_LEVELS> asks;
};

struct Trade
{
    std::chrono::system_clock::time_point timestamp;
    double price, size;
    std::string side;
    std::string ord_type; // Added for maker/taker analysis
};

struct FlowData
{
    std::chrono::system_clock::time_point timestamp;
    double ofi_l1 = 0.0, ofi_l2 = 0.0, ofi_l3 = 0.0, ofi_l4 = 0.0, ofi_l5 = 0.0;
};

struct AssetState
{
    // Deques for feature calculation windows
    std::deque<std::pair<std::chrono::system_clock::time_point, double>> mid_price_history;
    std::deque<std::pair<std::chrono::system_clock::time_point, double>> relative_spread_history;
    std::deque<std::pair<std::chrono::system_clock::time_point, double>> wap_history_5L;
    std::deque<OHLCBar> ohlc_history_1s;
    std::deque<std::pair<std::chrono::system_clock::time_point, double>> ad_line_history;
    std::deque<double> true_range_history;

    double last_atr = 0.0;

    // --- ADD THESE LINES for efficient EMA calculation ---
    double last_ema_short = 0.0;
    double last_ema_long = 0.0;

    // --- ADD THESE LINES for efficient window management ---
    size_t hist_search_start_pos_rsi = 0;
    size_t hist_search_start_pos_ad = 0;

    // --- ADD THESE LINES for rolling spread volatility ---
    long spread_vol_count = 0;
    double spread_vol_mean = 0.0;
    double spread_vol_m2 = 0.0; // Sum of squares of differences from the mean

    // Buffers for batch-fetched raw data
    std::vector<BookSnapshot> snapshot_buffer;
    std::vector<Trade> trade_buffer;
    std::vector<FlowData> flow_buffer;
    std::vector<OHLCBar> ohlc_buffer;

    // State management for buffers
    size_t snapshot_buf_pos = 0;
    size_t trade_buf_pos = 0;
    size_t flow_buf_pos = 0;
    size_t ohlc_buf_pos = 0;
    std::chrono::system_clock::time_point buffer_end_tp;
    bool buffer_initialized = false;
};

using FeatureVector = std::vector<double>;
using FeatureMatrix = std::map<std::string, FeatureVector>;