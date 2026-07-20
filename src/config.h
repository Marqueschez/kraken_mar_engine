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
    std::string start_time = "2025-08-12T07:37:59.000000Z";
    std::string end_time = "2025-08-12T10:55:55.000000Z";

    // cd C:\dev\kraken_mar_engine
    // C:\dev\kraken_mar_engine\build\Debug\feature_engine.exe

    // 0 - 5 hr 15 min
    // std::string start_time = "2025-08-02T17:41:47.000000Z"; actual start time
    // std::string end_time = "2025-08-02T22:57:44.000000Z";
    // 1 - 3 hr 20 min
    // std::string start_time = "2025-08-12T07:37:59.000000Z"; actual start time
    // std::string end_time = "2025-08-12T10:55:55.000000Z";
    // 2 - 7 hr 30 min
    // std::string start_time = "2025-08-21T23:42:00.000000Z"; actual start time
    // std::string end_time = "2025-08-22T07:12:00.000000Z";

    // 2025-08-21T23:42:00 - 14hr - 2025-08-22T07:12:00

    // --- Feature Hyperparameters ---
    int lob_depth_for_features = 5; // Not using all 8 levels available
    WeightingScheme level_weighting_scheme = WeightingScheme::LINEAR;

    // Window lengths for contextual features
    std::chrono::seconds contextual_ma_short_period = std::chrono::minutes(1);
    std::chrono::seconds contextual_ma_long_period = std::chrono::minutes(5);
    std::chrono::seconds contextual_rsi_period_s = std::chrono::seconds(14);
    std::chrono::seconds contextual_ad_flow_period_s = std::chrono::minutes(1);
    std::chrono::seconds contextual_bollinger_period_s = std::chrono::seconds(20);

    // --- Output Configuration ---
    std::string output_csv_path = "data/features_output3.csv";
    // BUILD THE PROJECT BEFORE RUNNING !!!!!
};