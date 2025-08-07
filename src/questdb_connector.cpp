// C:\dev\kraken_mar_engine\src\questdb_connector.cpp
#include "questdb_connector.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <algorithm>

extern std::chrono::system_clock::time_point parse_timestamp_from_string(const std::string &ts_str);

template <typename T>
T safe_get_value(PGresult *res, int row, int col)
{
    if (PQgetisnull(res, row, col))
        return T{0};
    try
    {
        if constexpr (std::is_same_v<T, double>)
            return std::stod(PQgetvalue(res, row, col));
        else if constexpr (std::is_same_v<T, long>)
            return std::stol(PQgetvalue(res, row, col));
    }
    catch (const std::exception &)
    {
        return T{0};
    }
    return T{0};
}

QuestDBConnector::QuestDBConnector(const AppConfig &config)
{
    std::string conn_str = "host=" + config.db_host + " port=" + std::to_string(config.db_port) + " user=" + config.db_user + " password=" + config.db_pass + " dbname=" + config.db_name;
    conn_ = PQconnectdb(conn_str.c_str());
    if (PQstatus(conn_) != CONNECTION_OK)
    {
        std::cerr << "QuestDB connection failed: " << PQerrorMessage(conn_) << std::endl;
        PQfinish(conn_);
        conn_ = nullptr;
    }
    else
    {
        std::cout << "Successfully connected to QuestDB via PostgreSQL." << std::endl;
        connected_ = true;
    }
}

QuestDBConnector::~QuestDBConnector()
{
    if (conn_)
        PQfinish(conn_);
}

bool QuestDBConnector::is_connected() const { return connected_; }

// Fetches RAW snapshots in a range
std::vector<BookSnapshot> QuestDBConnector::get_snapshots_for_range(const std::string &symbol, const std::string &start_time_str, const std::string &end_time_str)
{
    std::vector<BookSnapshot> snapshots;
    if (!is_connected())
        return snapshots;

    std::string query = "SELECT * FROM kraken_l3_book_levels_agg WHERE symbol = '" + symbol + "' AND timestamp >= '" + start_time_str + "' AND timestamp < '" + end_time_str + "' ORDER BY timestamp;";
    PGresult *res = PQexec(conn_, query.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        std::cerr << "Snapshot range query failed: " << PQresultErrorMessage(res) << std::endl;
        PQclear(res);
        return snapshots;
    }

    int rows = PQntuples(res);
    snapshots.reserve(rows);

    // --- OPTIMIZATION: Get all column indices before the loop ---
    int ts_col = PQfnumber(res, "timestamp");
    std::vector<int> bid_price_cols(MAX_LOB_LEVELS), ask_price_cols(MAX_LOB_LEVELS);
    std::vector<int> bid_size_cols(MAX_LOB_LEVELS), ask_size_cols(MAX_LOB_LEVELS);
    std::vector<int> bid_orders_cols(MAX_LOB_LEVELS), ask_orders_cols(MAX_LOB_LEVELS);
    std::vector<int> bid_hhi_cols(5), ask_hhi_cols(5);
    std::vector<int> bid_age_cols(5), ask_age_cols(5);

    for (int j = 0; j < MAX_LOB_LEVELS; ++j)
    {
        std::string level = std::to_string(j + 1);
        bid_price_cols[j] = PQfnumber(res, ("bid" + level + "_price").c_str());
        ask_price_cols[j] = PQfnumber(res, ("ask" + level + "_price").c_str());
        bid_size_cols[j] = PQfnumber(res, ("bid" + level + "_size").c_str());
        ask_size_cols[j] = PQfnumber(res, ("ask" + level + "_size").c_str());
        bid_orders_cols[j] = PQfnumber(res, ("bid" + level + "_orders").c_str());
        ask_orders_cols[j] = PQfnumber(res, ("ask" + level + "_orders").c_str());
    }
    // L3 features only exist for levels 1-5
    for (int j = 0; j < 5; ++j)
    {
        std::string level = std::to_string(j + 1);
        bid_hhi_cols[j] = PQfnumber(res, ("bid" + level + "_hhi").c_str());
        ask_hhi_cols[j] = PQfnumber(res, ("ask" + level + "_hhi").c_str());
        bid_age_cols[j] = PQfnumber(res, ("bid" + level + "_toporderage_ms").c_str());
        ask_age_cols[j] = PQfnumber(res, ("ask" + level + "_toporderage_ms").c_str());
    }

    for (int i = 0; i < rows; ++i)
    {
        BookSnapshot snapshot;
        snapshot.timestamp = parse_timestamp_from_string(PQgetvalue(res, i, ts_col));
        for (int j = 0; j < MAX_LOB_LEVELS; ++j)
        {
            double bid_hhi = 0.0, ask_hhi = 0.0;
            long bid_age = 0, ask_age = 0;

            if (j < 5)
            {
                bid_hhi = safe_get_value<double>(res, i, bid_hhi_cols[j]);
                ask_hhi = safe_get_value<double>(res, i, ask_hhi_cols[j]);
                bid_age = safe_get_value<long>(res, i, bid_age_cols[j]);
                ask_age = safe_get_value<long>(res, i, ask_age_cols[j]);
            }

            snapshot.bids[j] = {
                safe_get_value<double>(res, i, bid_price_cols[j]),
                safe_get_value<double>(res, i, bid_size_cols[j]),
                safe_get_value<long>(res, i, bid_orders_cols[j]),
                bid_hhi,
                bid_age};
            snapshot.asks[j] = {
                safe_get_value<double>(res, i, ask_price_cols[j]),
                safe_get_value<double>(res, i, ask_size_cols[j]),
                safe_get_value<long>(res, i, ask_orders_cols[j]),
                ask_hhi,
                ask_age};
        }
        snapshots.push_back(snapshot);
    }
    PQclear(res);
    return snapshots;
}

// Fetches RAW trades in a range
std::vector<Trade> QuestDBConnector::get_trades_for_range(const std::string &symbol, const std::string &start_time_str, const std::string &end_time_str)
{
    std::vector<Trade> trades;
    if (!is_connected())
        return trades;

    std::string query = "SELECT timestamp, price, size, side, ord_type FROM kraken_trades WHERE symbol = '" + symbol + "' AND timestamp >= '" + start_time_str + "' AND timestamp < '" + end_time_str + "' ORDER BY timestamp;";
    PGresult *res = PQexec(conn_, query.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PQclear(res);
        return trades;
    }
    int rows = PQntuples(res);
    trades.reserve(rows);
    for (int i = 0; i < rows; ++i)
    {
        trades.push_back({parse_timestamp_from_string(PQgetvalue(res, i, 0)), safe_get_value<double>(res, i, 1), safe_get_value<double>(res, i, 2), PQgetvalue(res, i, 3), PQgetvalue(res, i, 4)});
    }
    PQclear(res);
    return trades;
}

// Fetches RAW flow data in a range
std::vector<FlowData> QuestDBConnector::get_flow_for_range(const std::string &symbol, const std::string &start_time_str, const std::string &end_time_str)
{
    std::vector<FlowData> flows;
    if (!is_connected())
        return flows;

    std::string query = "SELECT timestamp, ofi_level1, ofi_level2, ofi_level3, ofi_level4, ofi_level5 FROM kraken_l3_flow_features WHERE symbol = '" + symbol + "' AND timestamp >= '" + start_time_str + "' AND timestamp < '" + end_time_str + "' ORDER BY timestamp;";
    PGresult *res = PQexec(conn_, query.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PQclear(res);
        return flows;
    }
    int rows = PQntuples(res);
    flows.reserve(rows);
    for (int i = 0; i < rows; ++i)
    {
        flows.push_back({parse_timestamp_from_string(PQgetvalue(res, i, 0)), safe_get_value<double>(res, i, 1), safe_get_value<double>(res, i, 2), safe_get_value<double>(res, i, 3), safe_get_value<double>(res, i, 4), safe_get_value<double>(res, i, 5)});
    }
    PQclear(res);
    return flows;
}

// Fetches OHLC bars (this one is already an aggregate and is fine)
std::vector<OHLCBar> QuestDBConnector::get_ohlc_bars_for_range(const std::string &symbol, const std::string &start_time_str, const std::string &end_time_str)
{
    std::vector<OHLCBar> bars;
    if (!is_connected())
        return bars;
    std::string query = "WITH mid_price AS ( SELECT timestamp AS ts, first((bid1_price+ask1_price)/2) AS open, max((bid1_price+ask1_price)/2) AS high, min((bid1_price+ask1_price)/2) AS low, last((bid1_price+ask1_price)/2) AS close FROM kraken_l3_book_levels_agg WHERE symbol = '" + symbol + "' AND timestamp >= '" + start_time_str + "' AND timestamp < '" + end_time_str + "' SAMPLE BY 1s FILL(prev)), trade_vol AS ( SELECT timestamp AS ts, sum(size) AS volume FROM kraken_trades WHERE symbol = '" + symbol + "' AND timestamp >= '" + start_time_str + "' AND timestamp < '" + end_time_str + "' SAMPLE BY 1s FILL(none) ) SELECT m.ts, m.open, m.high, m.low, m.close, coalesce(t.volume,0) FROM mid_price m LEFT JOIN trade_vol t ON m.ts = t.ts ORDER BY m.ts;";
    PGresult *res = PQexec(conn_, query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        std::cerr << "OHLC batch query failed: " << PQresultErrorMessage(res) << std::endl;
        PQclear(res);
        return bars;
    }
    int rows = PQntuples(res);
    bars.reserve(rows);
    for (int i = 0; i < rows; ++i)
    {
        bars.push_back({parse_timestamp_from_string(PQgetvalue(res, i, 0)), safe_get_value<double>(res, i, 1), safe_get_value<double>(res, i, 2), safe_get_value<double>(res, i, 3), safe_get_value<double>(res, i, 4), safe_get_value<double>(res, i, 5)});
    }
    PQclear(res);
    return bars;
}
extern std::string format_timestamp_for_sql(const std::chrono::system_clock::time_point &tp);
// Robust timestamp finder
std::optional<std::chrono::system_clock::time_point> QuestDBConnector::get_latest_timestamp_in_range(const std::string &table_name, const std::string &symbol, const std::string &start_time_str, const std::string &end_time_str)
{
    if (!is_connected())
        return std::nullopt;
    std::string query = "SELECT timestamp FROM " + table_name + " WHERE symbol = '" + symbol + "' AND timestamp >= '" + start_time_str + "' AND timestamp < '" + end_time_str + "' ORDER BY timestamp DESC LIMIT 1;";
    std::cout << "[connector] Executing SQL: " << query << std::endl;
    PGresult *res = PQexec(conn_, query.c_str());
    ExecStatusType status = PQresultStatus(res);
    int rows = PQntuples(res);
    std::cout << "[connector] Query for '" << table_name << "' returned status " << PQresStatus(status) << " with " << rows << " rows." << std::endl;
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0 || PQgetisnull(res, 0, 0))
    {
        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            std::cerr << "Latest timestamp query failed for " << symbol << " on table " << table_name << ": " << PQresultErrorMessage(res) << std::endl;
        }
        PQclear(res);
        return std::nullopt;
    }
    const char *raw_ts_from_db = PQgetvalue(res, 0, 0);
    std::cout << "[connector]   Raw timestamp string from DB: " << raw_ts_from_db << std::endl;

    // 2. Parse it into a C++ time_point object.
    auto ts = parse_timestamp_from_string(raw_ts_from_db);

    // 3. Format the C++ object back into a string to see what it parsed.
    std::cout << "[connector]   Parsed and reformatted timestamp:   " << format_timestamp_for_sql(ts) << std::endl;
    // --- END DIAGNOSTIC STEP ---

    PQclear(res);
    return ts;
}