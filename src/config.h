// C:\dev\kraken_mar_engine\src\config.h
#pragma once
#include <string>
#include <vector>
#include <chrono>

enum class WeightingScheme
{
    LINEAR,
    INVERSE_DISTANCE
};

struct AppConfig
{
    // --- Database Configuration ---
    std::string db_host = "localhost";
    int db_port = 8812;
    std::string db_user = "admin";
    std::string db_pass = "quest";
    std::string db_name = "qdb";

    // --- Core Processing Configuration ---
    std::vector<std::string> assets_to_process = {
        "BTC/USD", "ETH/USD", "SOL/USD", "XRP/USD", "DOGE/USD",
        "ADA/USD", "LTC/USD", "LINK/USD", "DOT/USD", "USDT/USD"};
    long long resampling_interval_ms = 100; // Generate a feature matrix every 100ms
    std::string start_time = "2025-08-02T17:41:47.000000Z";
    std::string end_time = "2025-08-02T22:57:44.000000Z";
    // 0 - 5hr
    // std::string start_time = "2025-08-02T17:41:47.000000Z"; actual start time
    // std::string end_time = "2025-08-02T22:57:44.000000Z";
    // 1 - 8hr
    // std::string start_time = "2025-08-03T16:49:30.000000Z"; actual start time
    // std::string end_time = "2025-08-04T00:49:30.000000Z";

    // --- Feature Hyperparameters ---
    int lob_depth_for_features = 5; // Using all 8 levels available
    WeightingScheme level_weighting_scheme = WeightingScheme::LINEAR;

    // Window lengths for contextual features
    std::chrono::seconds contextual_ma_short_period = std::chrono::minutes(1);
    std::chrono::seconds contextual_ma_long_period = std::chrono::minutes(5);
    std::chrono::seconds contextual_rsi_period_s = std::chrono::seconds(14);
    std::chrono::seconds contextual_ad_flow_period_s = std::chrono::minutes(1);
    std::chrono::seconds contextual_bollinger_period_s = std::chrono::seconds(20);

    // --- Output Configuration ---
    std::string output_csv_path = "data/features_output3.csv";
    build
};