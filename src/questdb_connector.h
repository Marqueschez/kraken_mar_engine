// C:\dev\kraken_mar_engine\src\questdb_connector.h
#pragma once
#include "config.h"
#include <string>
#include <vector>
#include <optional>
#include <libpq-fe.h>
#include "data_structures.h"

class QuestDBConnector
{
public:
    explicit QuestDBConnector(const AppConfig &config);
    ~QuestDBConnector();
    bool is_connected() const;

    // Batch fetching methods for raw data
    std::vector<BookSnapshot> get_snapshots_for_range(const std::string &symbol, const std::string &start_time_str, const std::string &end_time_str);
    std::vector<Trade> get_trades_for_range(const std::string &symbol, const std::string &start_time_str, const std::string &end_time_str);
    std::vector<FlowData> get_flow_for_range(const std::string &symbol, const std::string &start_time_str, const std::string &end_time_str);
    std::vector<OHLCBar> get_ohlc_bars_for_range(const std::string &symbol, const std::string &start_time_str, const std::string &end_time_str);

    // Robust method to find the last available data point in a specific table
    std::optional<std::chrono::system_clock::time_point> get_latest_timestamp_in_range(
        const std::string &table_name, const std::string &symbol,
        const std::string &start_time_str, const std::string &end_time_str);

private:
    PGconn *conn_;
    bool connected_ = false;
};