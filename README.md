# CMP653: Vector Database Benchmarking Harness
**Comparing PostgreSQL (pgvector) vs. Pinecone (Serverless) under Hardware Constraints**

This repository contains the natively compiled C++17 benchmarking harness developed for the CMP653 course. It evaluates the architectural bottlenecks of executing "Hybrid Queries" on local shared-memory databases versus decoupled cloud-native vector engines.

## System Architecture

```mermaid
graph TD
    subgraph Local Constraint Environment [WSL2 / Docker 8-vCPU]
        Client[C++17 Benchmarking Harness<br/>Linux epoll Event Loop]
        PG[(PostgreSQL 16<br/>pgvector)]
        WAL[Relational UPDATE<br/>Contention Thread]
    end
    
    subgraph Cloud Environment [AWS us-east-1]
        PC[(Pinecone Serverless<br/>Vector Index)]
    end

    Client -- "libpq (Native TCP/IP)<br/>Sub-millisecond" ---> PG
    WAL -. "Forces WAL Stress" .-> PG
    Client -- "libcurl (HTTP/2 REST)<br/>Network Latency & 429 Rate Limits" ---> PC
```

## Prerequisites

To ensure absolute reproducibility, the host environment requires:

* Ubuntu 24.04 (WSL2 or Native)
* C++17 Compiler (GCC) and CMake
* Required Libraries: `libpqxx-dev`, `libcurl4-openssl-dev`
* `nlohmann/json` (Header-only, already included in `/src`)

## Build and Run Instructions

**1. Clone the repository:**
```bash
git clone https://github.com/obalcikli/cmp653-benchmark.git
cd cmp653-benchmark/src
```

**2. Compile the Harness:**
```bash
rm -rf CMakeCache.txt CMakeFiles/
cmake .
make
```

**3. Execute the Benchmark:**
```bash
./benchmark_harness
```