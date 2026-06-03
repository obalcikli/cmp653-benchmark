
cmake_minimum_required(VERSION 3.16)
project(VectorBenchmark CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Donanıma özel SIMD optimizasyon bayrakları
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -pthread")

# Kütüphaneleri PkgConfig üzerinden daha güvenilir şekilde bulma
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED libpqxx)

find_package(CURL REQUIRED)

# Include dizinleri
include_directories(${PQXX_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS})

# Çalıştırılabilir dosyanın tanımlanması
add_executable(benchmark_harness main.cpp)

# Kütüphanelerin bağlanması (Linker)
target_link_libraries(benchmark_harness ${PQXX_LIBRARIES} ${CURL_LIBRARIES} pthread)
