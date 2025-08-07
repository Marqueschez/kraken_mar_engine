# C:\dev\kraken_mar_engine\CMakeLists.txt
cmake_minimum_required(VERSION 3.15)
project(KrakenFeatureEngine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find the PostgreSQL library (libpq)
find_package(PostgreSQL REQUIRED)

# Add our source code to be compiled into an executable
add_executable(
    feature_engine
    src/main.cpp
    src/questdb_connector.cpp
    src/feature_calculator.cpp
)

# Link the executable against the PostgreSQL library
target_include_directories(feature_engine
    PUBLIC #<-- Using PUBLIC is often more robust
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${PostgreSQL_INCLUDE_DIRS}
)
target_link_libraries(feature_engine PRIVATE ${PostgreSQL_LIBRARIES})