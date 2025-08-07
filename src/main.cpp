// C:\dev\kraken_mar_engine\src\main.cpp
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <cctype>
#include <algorithm>
#include <vector>
#include <map>

#include "config.h"
#include "questdb_connector.h"
#include "feature_calculator.h"
#include "data_structures.h"

std::string format_timestamp_for_sql(const std::chrono::system_clock::time_point &tp)
{
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    auto us_remainder = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()) % 1000000;
    std::stringstream ss;
    std::tm tm_utc;
#ifdef _WIN32
    gmtime_s(&tm_utc, &time_t_val);
#else
    gmtime_r(&time_t_val, &tm_utc);
#endif
    ss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(6) << us_remainder.count() << 'Z';
    return ss.str();
}

std::chrono::system_clock::time_point parse_timestamp_from_string(const std::string &ts_str)
{
    if (ts_str.empty())
    {
        return {}; // Return a zero/default time_point for empty strings
    }

    std::tm tm = {};
    double seconds_double = 0.0;
    int fields_read = 0;

    // Check the character at index 10 to determine the format.
    // This makes the parser robust to both inputs.
    if (ts_str.length() > 10 && ts_str[10] == 'T')
    {
        // Format from config file: "YYYY-MM-DDTHH:MM:SS.ffffffZ"
        fields_read = sscanf(ts_str.c_str(), "%d-%d-%dT%d:%d:%lf",
                             &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                             &tm.tm_hour, &tm.tm_min, &seconds_double);
    }
    else
    {
        // Format from database driver: "YYYY-MM-DD HH:MM:SS.ffffff"
        fields_read = sscanf(ts_str.c_str(), "%d-%d-%d %d:%d:%lf",
                             &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                             &tm.tm_hour, &tm.tm_min, &seconds_double);
    }

    // If parsing failed, return a zero time_point to indicate an error.
    if (fields_read < 6)
    {
        std::cerr << "WARNING: Failed to parse a valid timestamp from string: \"" << ts_str << "\"" << std::endl;
        return {};
    }

    tm.tm_sec = static_cast<int>(seconds_double);
    int microseconds = static_cast<int>(std::round((seconds_double - tm.tm_sec) * 1000000.0));

    // Adjust for the std::tm struct's format (years since 1900, months 0-11)
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;

    // Convert the UTC tm struct to a time_t
    time_t time_val;
#ifdef _WIN32
    time_val = _mkgmtime(&tm);
#else
    time_val = timegm(&tm);
#endif

    return std::chrono::system_clock::from_time_t(time_val) + std::chrono::microseconds(microseconds);
}

int main()
{
    AppConfig config;
    QuestDBConnector db_connector(config);
    if (!db_connector.is_connected())
    {
        std::cerr << "CRITICAL: Could not connect to database. Exiting." << std::endl;
        return 1;
    }
    FeatureCalculator feature_calculator(config);

    std::ofstream output_file(config.output_csv_path);
    output_file << "timestamp";
    for (const auto &asset : config.assets_to_process)
    {
        std::string header_asset = asset;
        std::replace(header_asset.begin(), header_asset.end(), '/', '_');
        for (int i = 0; i < 24; ++i)
            output_file << "," << header_asset << "_f" << i;
    }
    output_file << "\n";

    std::stringstream csv_buffer;
    const int buffer_flush_size = 4096;

    auto configured_start_tp = parse_timestamp_from_string(config.start_time);
    auto configured_end_tp = parse_timestamp_from_string(config.end_time);
    std::string sql_start = format_timestamp_for_sql(configured_start_tp);
    std::string sql_end = format_timestamp_for_sql(configured_end_tp);

    std::cout << "Intended run window: " << sql_start << " to " << sql_end << std::endl;

    auto effective_end_tp = configured_end_tp;
    std::cout << "Querying database to find the last available book snapshot for each asset..." << std::endl;

    for (const auto &asset : config.assets_to_process)
    {
        std::string db_symbol = asset;
        std::replace(db_symbol.begin(), db_symbol.end(), '/', '_');
        auto last_book = db_connector.get_latest_timestamp_in_range("kraken_l3_book_levels_agg", db_symbol, sql_start, sql_end);
        if (!last_book)
        {
            std::cerr << "CRITICAL: Asset '" << asset << "' has NO book data within the intended window. Exiting." << std::endl;
            return 1;
        }
        effective_end_tp = std::min(effective_end_tp, *last_book);
    }

    if (configured_start_tp >= effective_end_tp)
    {
        std::cerr << "CRITICAL: Effective window is invalid. No overlapping book data found for all assets. Exiting." << std::endl;
        return 1;
    }

    std::cout << "Effective processing window: " << format_timestamp_for_sql(configured_start_tp) << " to " << format_timestamp_for_sql(effective_end_tp) << std::endl;

    std::map<std::string, AssetState> asset_states;
    for (const auto &asset : config.assets_to_process)
        asset_states[asset] = AssetState{};

    auto current_tp = configured_start_tp;
    int loop_count = 0;
    while (current_tp < effective_end_tp)
    {
        for (const auto &asset : config.assets_to_process)
        {
            auto &state = asset_states.at(asset);

            // Refill condition: Use >= to refill AT the boundary.
            if (!state.buffer_initialized || current_tp >= state.buffer_end_tp)
            {
                // --- THE ROBUST FIX: CARRY-OVER CONTEXT ---
                auto fetch_boundary = state.buffer_initialized ? state.buffer_end_tp : configured_start_tp;
                auto end_fetch = fetch_boundary + std::chrono::minutes(5);
                const auto context_duration = std::chrono::seconds(2); // 2 seconds of context is very safe
                auto context_start_tp = fetch_boundary - context_duration;

                // 1. Save context from the end of the old buffers.
                std::vector<BookSnapshot> snapshot_context;
                std::vector<Trade> trade_context;
                std::vector<FlowData> flow_context;

                if (state.buffer_initialized)
                {
                    // Efficiently find the start of the context window
                    auto snap_it = std::lower_bound(state.snapshot_buffer.begin(), state.snapshot_buffer.end(), context_start_tp,
                                                    [](const BookSnapshot &s, const auto &t)
                                                    { return s.timestamp < t; });
                    snapshot_context.assign(snap_it, state.snapshot_buffer.end());

                    auto trade_it = std::lower_bound(state.trade_buffer.begin(), state.trade_buffer.end(), context_start_tp,
                                                     [](const Trade &t, const auto &tp)
                                                     { return t.timestamp < tp; });
                    trade_context.assign(trade_it, state.trade_buffer.end());

                    auto flow_it = std::lower_bound(state.flow_buffer.begin(), state.flow_buffer.end(), context_start_tp,
                                                    [](const FlowData &f, const auto &tp)
                                                    { return f.timestamp < tp; });
                    flow_context.assign(flow_it, state.flow_buffer.end());
                }
                std::vector<OHLCBar> ohlc_context;
                if (state.buffer_initialized)
                {
                    auto ohlc_it = std::lower_bound(state.ohlc_buffer.begin(), state.ohlc_buffer.end(), context_start_tp,
                                                    [](const OHLCBar &o, const auto &t)
                                                    { return o.timestamp < t; });
                    ohlc_context.assign(ohlc_it, state.ohlc_buffer.end());
                }
                // 2. Fetch ONLY the new data from the database.
                std::string db_symbol = asset;
                std::replace(db_symbol.begin(), db_symbol.end(), '/', '_');
                std::cout << "Refilling buffers for " << asset << " from " << format_timestamp_for_sql(fetch_boundary) << " to " << format_timestamp_for_sql(end_fetch) << std::endl;

                auto new_snapshots = db_connector.get_snapshots_for_range(db_symbol, format_timestamp_for_sql(fetch_boundary), format_timestamp_for_sql(end_fetch));
                auto new_trades = db_connector.get_trades_for_range(db_symbol, format_timestamp_for_sql(fetch_boundary), format_timestamp_for_sql(end_fetch));
                auto new_flows = db_connector.get_flow_for_range(db_symbol, format_timestamp_for_sql(fetch_boundary), format_timestamp_for_sql(end_fetch));
                auto new_ohlc_bars = db_connector.get_ohlc_bars_for_range(db_symbol, format_timestamp_for_sql(fetch_boundary), format_timestamp_for_sql(end_fetch));

                // 3. Combine context and new data.
                state.snapshot_buffer = std::move(snapshot_context);
                state.snapshot_buffer.insert(state.snapshot_buffer.end(), std::make_move_iterator(new_snapshots.begin()), std::make_move_iterator(new_snapshots.end()));

                state.trade_buffer = std::move(trade_context);
                state.trade_buffer.insert(state.trade_buffer.end(), std::make_move_iterator(new_trades.begin()), std::make_move_iterator(new_trades.end()));

                state.flow_buffer = std::move(flow_context);
                state.flow_buffer.insert(state.flow_buffer.end(), std::make_move_iterator(new_flows.begin()), std::make_move_iterator(new_flows.end()));
                state.ohlc_buffer = std::move(ohlc_context);
                state.ohlc_buffer.insert(state.ohlc_buffer.end(), std::make_move_iterator(new_ohlc_bars.begin()), std::make_move_iterator(new_ohlc_bars.end()));
                // 4. Reset buffer pointers and update state.
                state.snapshot_buf_pos = 0;
                state.trade_buf_pos = 0;
                state.flow_buf_pos = 0;
                state.ohlc_buf_pos = 0;
                state.buffer_end_tp = end_fetch;
                state.buffer_initialized = true;
            }
        }

        csv_buffer << format_timestamp_for_sql(current_tp);
        for (const auto &asset : config.assets_to_process)
        {
            auto &state = asset_states.at(asset);
            while (state.ohlc_buf_pos < state.ohlc_buffer.size() && state.ohlc_buffer[state.ohlc_buf_pos].timestamp <= current_tp)
            {
                feature_calculator.process_new_ohlc_bar(state, state.ohlc_buffer[state.ohlc_buf_pos]);
                state.ohlc_buf_pos++;
            }
            FeatureVector fv = feature_calculator.calculate_all_features(current_tp, asset_states.at(asset));
            for (const double &val : fv)
            {
                csv_buffer << "," << std::fixed << std::setprecision(8) << val;
            }
        }
        csv_buffer << "\n";

        current_tp += std::chrono::milliseconds(config.resampling_interval_ms);

        if (++loop_count % buffer_flush_size == 0)
        {
            output_file << csv_buffer.rdbuf();
            csv_buffer.str("");
            csv_buffer.clear();
            std::cout << "Processed up to timestamp: " << format_timestamp_for_sql(current_tp) << std::endl;
        }
    }

    output_file << csv_buffer.rdbuf();

    std::cout << "\nProcessing finished. Output written to " << config.output_csv_path << std::endl;
    output_file.close();
    return 0;
}