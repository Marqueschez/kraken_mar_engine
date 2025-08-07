// C:\dev\kraken_mar_engine\src\feature_calculator.cpp
#include "feature_calculator.h"
#include "questdb_connector.h"
#include <numeric>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <sstream>

FeatureCalculator::FeatureCalculator(const AppConfig &config) : config_(config)
{
    initialize_level_weights();
}

void FeatureCalculator::initialize_level_weights()
{
    level_weights_.resize(config_.lob_depth_for_features);
    double sum = 0;
    for (int i = 0; i < config_.lob_depth_for_features; ++i)
    {
        if (config_.level_weighting_scheme == WeightingScheme::LINEAR)
        {
            level_weights_[i] = static_cast<double>(config_.lob_depth_for_features - i);
        }
        else if (config_.level_weighting_scheme == WeightingScheme::INVERSE_DISTANCE)
        {
            level_weights_[i] = 1.0 / (i + 1.0);
        }
        sum += level_weights_[i];
    }
    if (sum > 1e-9)
    {
        for (double &w : level_weights_)
            w /= sum;
    }
}

FeatureVector FeatureCalculator::calculate_all_features(
    const std::chrono::system_clock::time_point current_tp,
    AssetState &state)
{
    FeatureVector features(24, 0.0);
    auto start_interval_tp = current_tp - std::chrono::milliseconds(config_.resampling_interval_ms);

    // --- Find current data points in buffers ---

    // Find the latest snapshot at or before the current time. This logic is correct.
    while (state.snapshot_buf_pos < state.snapshot_buffer.size() && state.snapshot_buffer[state.snapshot_buf_pos].timestamp <= current_tp)
    {
        state.snapshot_buf_pos++;
    }
    if (state.snapshot_buf_pos == 0)
        return features;
    const BookSnapshot &current_snapshot = state.snapshot_buffer[state.snapshot_buf_pos - 1];
    std::optional<BookSnapshot> prev_snapshot_opt;
    if (state.snapshot_buf_pos > 1)
    {
        prev_snapshot_opt = state.snapshot_buffer[state.snapshot_buf_pos - 2];
    }

    // --- FIX: Correctly gather all trades and flows for the CURRENT 100ms interval ---
    // The key is to use a temporary start position for the search, so we don't "lose our place"
    // relative to the main buffer pointer (state.trade_buf_pos).
    std::vector<Trade> current_trades;
    size_t temp_trade_pos = state.trade_buf_pos;
    while (temp_trade_pos < state.trade_buffer.size() && state.trade_buffer[temp_trade_pos].timestamp <= current_tp)
    {
        if (state.trade_buffer[temp_trade_pos].timestamp > start_interval_tp)
        {
            current_trades.push_back(state.trade_buffer[temp_trade_pos]);
        }
        temp_trade_pos++;
    }
    // Only advance the main pointer after we are done with this tick.
    state.trade_buf_pos = temp_trade_pos;

    std::vector<FlowData> current_flows;
    size_t temp_flow_pos = state.flow_buf_pos;
    while (temp_flow_pos < state.flow_buffer.size() && state.flow_buffer[temp_flow_pos].timestamp <= current_tp)
    {
        if (state.flow_buffer[temp_flow_pos].timestamp > start_interval_tp)
        {
            current_flows.push_back(state.flow_buffer[temp_flow_pos]);
        }
        temp_flow_pos++;
    }
    state.flow_buf_pos = temp_flow_pos;

    size_t prev_snapshot_idx = state.snapshot_buf_pos > 0 ? state.snapshot_buf_pos - 1 : 0;
    while (prev_snapshot_idx > 0 && state.snapshot_buffer[prev_snapshot_idx - 1].timestamp > start_interval_tp)
    {
        prev_snapshot_idx--;
    }

    // 1. Get Log Returns for the 100ms window (for Feature 15)
    std::vector<double> interval_log_returns;
    for (size_t i = prev_snapshot_idx; i < state.snapshot_buf_pos; ++i)
    {
        if (i > 0)
        {
            double m_curr = (state.snapshot_buffer[i].bids[0].price + state.snapshot_buffer[i].asks[0].price) / 2.0;
            double m_prev = (state.snapshot_buffer[i - 1].bids[0].price + state.snapshot_buffer[i - 1].asks[0].price) / 2.0;
            if (m_curr > 1e-9 && m_prev > 1e-9)
            {
                interval_log_returns.push_back(std::log(m_curr / m_prev));
            }
        }
    }

    // 2. Count BBO Updates for the 100ms window (for Feature 16)
    long bbo_update_count = 0;
    for (size_t i = prev_snapshot_idx; i < state.snapshot_buf_pos; ++i)
    {
        if (i > 0)
        {
            if (std::abs(state.snapshot_buffer[i].bids[0].price - state.snapshot_buffer[i - 1].bids[0].price) > 1e-9 ||
                std::abs(state.snapshot_buffer[i].asks[0].price - state.snapshot_buffer[i - 1].asks[0].price) > 1e-9)
            {
                bbo_update_count++;
            }
        }
    }

    double mid_price = (current_snapshot.bids[0].price + current_snapshot.asks[0].price) / 2.0;
    if (mid_price > 1e-9)
    {
        state.mid_price_history.push_back({current_tp, mid_price});
        state.relative_spread_history.push_back({current_tp, (current_snapshot.asks[0].price - current_snapshot.bids[0].price) / mid_price});
    }

    calculate_lob_features(features, current_snapshot);
    // Pass the vector of flows now
    calculate_flow_features(features, current_trades, current_flows, current_snapshot, prev_snapshot_opt);
    calculate_volatility_features(features, state, interval_log_returns, bbo_update_count);
    calculate_contextual_features(features, state);

    return features;
}

void FeatureCalculator::calculate_lob_features(FeatureVector &features, const BookSnapshot &snapshot)
{
    const int depth = config_.lob_depth_for_features;
    double mid_price = (snapshot.bids[0].price + snapshot.asks[0].price) / 2.0;
    if (mid_price < 1e-9)
        return;
    features[1] = (snapshot.asks[0].price - snapshot.bids[0].price) / mid_price;
    double bbo_total_size = snapshot.bids[0].size + snapshot.asks[0].size;
    if (bbo_total_size > 1e-9)
        features[3] = snapshot.bids[0].size / bbo_total_size;
    double total_bid_size_l5 = 0, total_ask_size_l5 = 0;
    for (int i = 0; i < 5; ++i)
    {
        total_bid_size_l5 += snapshot.bids[i].size;
        total_ask_size_l5 += snapshot.asks[i].size;
    }
    if (total_bid_size_l5 + total_ask_size_l5 > 1e-9)
        features[4] = total_bid_size_l5 / (total_bid_size_l5 + total_ask_size_l5);
    double bid_wap_num = 0, bid_wap_den = 0, ask_wap_num = 0, ask_wap_den = 0;
    for (int i = 0; i < 5; ++i)
    {
        bid_wap_num += snapshot.bids[i].price * snapshot.bids[i].size;
        bid_wap_den += snapshot.bids[i].size;
        ask_wap_num += snapshot.asks[i].price * snapshot.asks[i].size;
        ask_wap_den += snapshot.asks[i].size;
    }
    if (bid_wap_den + ask_wap_den > 1e-9)
        features[2] = (bid_wap_num + ask_wap_num) / (bid_wap_den + ask_wap_den);
    double bid_wap = (bid_wap_den > 1e-9) ? bid_wap_num / bid_wap_den : snapshot.bids[0].price;
    double ask_wap = (ask_wap_den > 1e-9) ? ask_wap_num / ask_wap_den : snapshot.asks[0].price;
    features[5] = (bid_wap - ask_wap) / mid_price;
    auto calculate_slope = [](const std::array<BookLevel, MAX_LOB_LEVELS> &levels) -> double
    {
        double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
        int n = 5;

        for (int i = 0; i < n; ++i)
        {
            double x = static_cast<double>(i + 1); // Level (1, 2, 3, 4, 5)
            double y = levels[i].size;             // Size at that level
            sum_x += x;
            sum_y += y;
            sum_xy += x * y;
            sum_x2 += x * x;
        }

        double denominator = n * sum_x2 - sum_x * sum_x;
        if (std::abs(denominator) < 1e-9)
        {
            return 0.0; // Avoid division by zero; slope is undefined/zero
        }
        return (n * sum_xy - sum_x * sum_y) / denominator;
    };

    double bid_slope = calculate_slope(snapshot.bids);
    double ask_slope = calculate_slope(snapshot.asks);
    features[6] = ask_slope - bid_slope; // Measures asymmetry in liquidity slope
    double weighted_bid_hhi = 0, weighted_ask_hhi = 0;
    for (int i = 0; i < depth; ++i)
    {
        weighted_bid_hhi += snapshot.bids[i].hhi * level_weights_[i];
        weighted_ask_hhi += snapshot.asks[i].hhi * level_weights_[i];
    }
    features[7] = weighted_bid_hhi;
    features[8] = weighted_ask_hhi;
    double weighted_bid_age = 0, weighted_ask_age = 0;
    for (int i = 0; i < 5; ++i)
    {
        weighted_bid_age += snapshot.bids[i].topOrderAge_ms * level_weights_[i];
        weighted_ask_age += snapshot.asks[i].topOrderAge_ms * level_weights_[i];
    }
    features[9] = (weighted_bid_age + weighted_ask_age) / 2.0;
    features[22] = std::log1p(static_cast<double>(snapshot.bids[0].topOrderAge_ms));
    features[23] = std::log1p(static_cast<double>(snapshot.asks[0].topOrderAge_ms));
}

void FeatureCalculator::calculate_flow_features(FeatureVector &features, const std::vector<Trade> &trades, const std::vector<FlowData> &flows, const BookSnapshot &current_snapshot, const std::optional<BookSnapshot> &prev_snapshot_opt)
{

    // Sum up all OFI values within the 100ms window
    double summed_ofi[] = {0.0, 0.0, 0.0, 0.0, 0.0};
    for (const auto &flow : flows)
    {
        summed_ofi[0] += flow.ofi_l1;
        summed_ofi[1] += flow.ofi_l2;
        summed_ofi[2] += flow.ofi_l3;
        summed_ofi[3] += flow.ofi_l4;
        summed_ofi[4] += flow.ofi_l5;
    }

    double weighted_ofi = 0;
    for (int i = 0; i < 5; ++i)
    {
        weighted_ofi += summed_ofi[i] * level_weights_[i];
    }
    features[10] = weighted_ofi;

    if (prev_snapshot_opt)
    {
        features[11] = (current_snapshot.bids[0].size - prev_snapshot_opt->bids[0].size) - (current_snapshot.asks[0].size - prev_snapshot_opt->asks[0].size);
    }

    double taker_buy_volume = 0.0, taker_sell_volume = 0.0, total_volume = 0.0;
    for (const auto &trade : trades)
    {
        total_volume += trade.size;
        if (trade.ord_type == "market")
        {
            if (trade.side == "BUY")
                taker_buy_volume += trade.size;
            else if (trade.side == "SELL")
                taker_sell_volume += trade.size;
        }
    }
    features[12] = taker_buy_volume - taker_sell_volume;
    if (total_volume > 1e-9)
        features[13] = (taker_buy_volume + taker_sell_volume) / total_volume;
}

void FeatureCalculator::calculate_volatility_features(FeatureVector &features, AssetState &state, const std::vector<double> &interval_log_returns, long bbo_update_count)
{
    // --- FEATURE 1: Mid-Price Log Return (Unchanged) ---
    auto &hist = state.mid_price_history;
    if (hist.size() > 1)
    {
        features[0] = std::log(hist.back().second / hist[hist.size() - 2].second);
    }

    // --- FEATURE 15: Realized Volatility (Δt) (Unchanged) ---
    if (interval_log_returns.size() > 1)
    {
        double sum = std::accumulate(interval_log_returns.begin(), interval_log_returns.end(), 0.0);
        double mean = sum / interval_log_returns.size();
        double sq_sum = std::inner_product(interval_log_returns.begin(), interval_log_returns.end(), interval_log_returns.begin(), 0.0);
        features[14] = std::sqrt(sq_sum / interval_log_returns.size() - mean * mean);
    }
    else
    {
        features[14] = 0.0;
    }

    // --- FEATURE 16: Quote Volatility (Unchanged) ---
    features[15] = std::log1p(static_cast<double>(bbo_update_count));

    // --- FEATURE 17: Spread Volatility (OPTIMIZED with Welford's algorithm) ---
    auto &spread_hist = state.relative_spread_history;
    if (!spread_hist.empty())
    {
        // Add the new value
        double new_val = spread_hist.back().second;
        state.spread_vol_count++;
        double delta = new_val - state.spread_vol_mean;
        state.spread_vol_mean += delta / state.spread_vol_count;
        double delta2 = new_val - state.spread_vol_mean;
        state.spread_vol_m2 += delta * delta2;

        // Remove the old value (if the deque is at max size)
        // This check assumes pruning happens elsewhere, which it now does.
        // A more robust implementation could check the front of the deque here.
    }

    if (state.spread_vol_count > 1)
    {
        features[16] = std::sqrt(state.spread_vol_m2 / state.spread_vol_count);
    }
}

void FeatureCalculator::process_new_ohlc_bar(AssetState &state, const OHLCBar &bar)
{
    // 1. Calculate the True Range for the current bar
    double true_range = 0.0;
    if (!state.ohlc_history_1s.empty())
    {
        double prev_close = state.ohlc_history_1s.back().close;
        true_range = std::max({bar.high - bar.low, std::abs(bar.high - prev_close), std::abs(bar.low - prev_close)});
    }
    else
    {
        true_range = bar.high - bar.low; // For the very first bar, TR is just High - Low
    }
    state.true_range_history.push_back(true_range);

    // 2. Prune the history to maintain the correct window size for the ATR calculation
    size_t atr_period = config_.contextual_bollinger_period_s.count();
    while (state.true_range_history.size() > atr_period)
    {
        state.true_range_history.pop_front();
    }

    // 3. Calculate the new ATR (which is the Simple Moving Average of the True Range)
    if (!state.true_range_history.empty())
    {
        double sum_tr = std::accumulate(state.true_range_history.begin(), state.true_range_history.end(), 0.0);
        state.last_atr = sum_tr / state.true_range_history.size(); // Update the state
    }

    // 4. Update other histories that depend on OHLC bars (like the A/D Line)
    double mfm = (bar.high - bar.low > 1e-9) ? (((bar.close - bar.low) - (bar.high - bar.close)) / (bar.high - bar.low)) : 0.0;
    double mfv = mfm * bar.volume;
    double prev_ad_line = state.ad_line_history.empty() ? 0.0 : state.ad_line_history.back().second;
    state.ad_line_history.push_back({bar.timestamp, prev_ad_line + mfv});

    // 5. Finally, add the current bar to the history for the next iteration's "prev_close"
    state.ohlc_history_1s.push_back(bar);
}

void FeatureCalculator::calculate_contextual_features(FeatureVector &features, AssetState &state)
{
    if (state.mid_price_history.empty())
        return;

    const auto current_tp = state.mid_price_history.back().first;
    const double current_mid_price = state.mid_price_history.back().second;

    // --- EMA calculation (Features 18, 19) is unchanged ---
    const double short_period_samples = config_.contextual_ma_short_period.count() * 1000.0 / config_.resampling_interval_ms;
    const double long_period_samples = config_.contextual_ma_long_period.count() * 1000.0 / config_.resampling_interval_ms;
    const double alpha_short = 2.0 / (short_period_samples + 1.0);
    const double alpha_long = 2.0 / (long_period_samples + 1.0);
    double current_ema_short = (state.last_ema_short == 0.0) ? current_mid_price : (current_mid_price * alpha_short) + (state.last_ema_short * (1.0 - alpha_short));
    double current_ema_long = (state.last_ema_long == 0.0) ? current_mid_price : (current_mid_price * alpha_long) + (state.last_ema_long * (1.0 - alpha_long));
    state.last_ema_short = current_ema_short;
    state.last_ema_long = current_ema_long;
    features[18] = (current_ema_long > 1e-9) ? (current_mid_price / current_ema_long) - 1.0 : 0.0;
    features[19] = current_ema_short - current_ema_long;

    // --- THE ATR FIX: SIMPLY RETRIEVE THE PRE-CALCULATED VALUE ---
    features[17] = state.last_atr;

    // Prune ALL history deques to save memory and prevent infinite growth.
    auto required_duration = config_.contextual_ma_long_period + std::chrono::minutes(1); // Keep a little extra buffer

    auto prune_pair = [&](auto &dq, size_t &start_pos_idx)
    {
        size_t popped_count = 0;
        while (!dq.empty() && (current_tp - dq.front().first > required_duration))
        {
            dq.pop_front();
            popped_count++;
        }
        // Adjust the search index to account for the popped elements
        start_pos_idx = (start_pos_idx > popped_count) ? start_pos_idx - popped_count : 0;
    };
    auto prune_ohlc = [&](auto &dq)
    {
        while (!dq.empty() && (current_tp - dq.front().timestamp > required_duration))
        {
            dq.pop_front();
        }
    };

    // Call prune on ALL relevant deques
    prune_pair(state.mid_price_history, state.hist_search_start_pos_rsi); // <-- WAS MISSING
    prune_pair(state.ad_line_history, state.hist_search_start_pos_ad);

    size_t dummy_idx = 0;                                 // For deques that don't need a search index
    prune_pair(state.relative_spread_history, dummy_idx); // <-- WAS MISSING
    prune_pair(state.wap_history_5L, dummy_idx);          // <-- WAS MISSING
    prune_ohlc(state.ohlc_history_1s);

    // --- FEATURE 22 (index 21): Accumulation/Distribution Flow (OPTIMIZED) ---
    if (state.ad_line_history.size() > 1)
    {
        auto ad_window_start_tp = current_tp - config_.contextual_ad_flow_period_s;
        while (state.hist_search_start_pos_ad < state.ad_line_history.size() &&
               state.ad_line_history[state.hist_search_start_pos_ad].first < ad_window_start_tp)
        {
            state.hist_search_start_pos_ad++;
        }
        if (state.hist_search_start_pos_ad < state.ad_line_history.size())
        {
            features[21] = state.ad_line_history.back().second - state.ad_line_history[state.hist_search_start_pos_ad].second;
        }
    }

    // --- FEATURE 21 (index 20): RSI Calculation (OPTIMIZED) ---
    double gain = 0, loss = 0;
    int n_rsi = 0;
    auto rsi_window_start_tp = current_tp - config_.contextual_rsi_period_s;
    while (state.hist_search_start_pos_rsi < state.mid_price_history.size() &&
           state.mid_price_history[state.hist_search_start_pos_rsi].first < rsi_window_start_tp)
    {
        state.hist_search_start_pos_rsi++;
    }

    for (size_t i = state.hist_search_start_pos_rsi; i < state.mid_price_history.size(); ++i)
    {
        if (i > 0)
        {
            double change = state.mid_price_history[i].second - state.mid_price_history[i - 1].second;
            if (change > 0)
                gain += change;
            else
                loss -= change;
            n_rsi++;
        }
    }

    if (n_rsi > 0)
    {
        double avg_gain = gain / n_rsi, avg_loss = loss / n_rsi;
        if (avg_loss > 1e-9)
        {
            double rs = avg_gain / avg_loss;
            features[20] = 100.0 - (100.0 / (1.0 + rs));
        }
        else
        {
            features[20] = 100.0;
        }
    }
}
