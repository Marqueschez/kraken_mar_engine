// C:\dev\kraken_mar_engine\src\feature_calculator.h
#pragma once
#include "config.h"
#include "data_structures.h"
#include <vector>
#include <map>
#include <optional>

class QuestDBConnector; // Forward declaration is enough

class FeatureCalculator
{
public:
    explicit FeatureCalculator(const AppConfig &config);

    FeatureVector calculate_all_features(
        const std::chrono::system_clock::time_point current_tp,
        AssetState &state);

    void process_new_ohlc_bar(AssetState &state, const OHLCBar &bar);

private:
    const AppConfig &config_;
    std::vector<double> level_weights_;

    void initialize_level_weights();

    void calculate_lob_features(FeatureVector &features, const BookSnapshot &snapshot);
    void calculate_flow_features(FeatureVector &features, const std::vector<Trade> &trades, const std::vector<FlowData> &flows, const BookSnapshot &current_snapshot, const std::optional<BookSnapshot> &prev_snapshot);

    // --- UPDATE THIS SIGNATURE ---
    void calculate_volatility_features(FeatureVector &features, AssetState &state, const std::vector<double> &interval_log_returns, long bbo_update_count);

    void calculate_contextual_features(FeatureVector &features, AssetState &state);
};
