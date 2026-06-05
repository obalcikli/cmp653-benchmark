#include <iostream>
#include <chrono>
#include <vector>
#include <thread>
#include <random>
#include <algorithm>
#include <pqxx/pqxx>
#include <sys/epoll.h>
#include <unistd.h>
#include <curl/curl.h>
#include "json.hpp"

// Gecikme testleri icin iki ayri dizi (Yerel vs Bulut)
std::vector<double> pg_latencies;
std::vector<double> pinecone_latencies;

// Hocanın rapor üzerinde belirttiği donanım spesifikasyon parametreleri
const std::string DB_CONN = "host=localhost port=5432 dbname=benchmark_db user=postgres password=password";
const int MAX_EVENTS = 64;
const int CONCURRENT_THREADS = 8; // Raporlanan 8 vCPU sınırı

// Basit bir rastgele sayı üretici (Zipfian dağılımı taklidi filtreler için)
int getRandomId(int min, int max) {
    static std::random_device rd;
    static std::mt19937 eng(rd());
    std::uniform_int_distribution<> distr(min, max);
    return distr(eng);
}

// Stres Testi: Paralel ilişkisel UPDATE çekişmesi (Contention Simulation)
void run_contention_workload() {
    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work tx(conn);
        // Rastgele satırların meta verilerini güncelleyerek write-ahead log (WAL) yükü yaratma
        for(int i = 0; i < 500; ++i) {
            int target_id = getRandomId(1, 100000);
            int new_category = getRandomId(1, 100);
            tx.exec0("UPDATE vector_benchmark SET category_id = " + std::to_string(new_category) + " WHERE id = " + std::to_string(target_id) + ";");
        }
        tx.commit();
    } catch (const std::exception &e) {
        // Test esnasında kilitlenmeleri yutmamak için sessizce geçebiliriz
    }
}

// Hibrit Sorgu Çalıştırma (Vektör benzerliği + Strict Relational Metadata Filtering)
void execute_hybrid_query(pqxx::work &tx) {
    int target_tenant = getRandomId(1, 5000);   // Yüksek kardinalite
    int target_category = getRandomId(1, 100);  // Orta kardinalite
    
    // Rastgele bir sorgu vektörü simülasyonu
    std::string query_vector = "[0.01";
    for(int i=1; i<1536; ++i) query_vector += ",0.01";
    query_vector += "]";

    // Cosine uzaklığı hesabı ile hibrit ilişkisel filtreleme
    std::string sql = 
        "SELECT id FROM vector_benchmark "
        "WHERE tenant_id = " + std::to_string(target_tenant) + " "
        "AND category_id = " + std::to_string(target_category) + " "
        "ORDER BY embedding <=> '" + query_vector + "'::vector LIMIT 10;";
    
    pqxx::result r = tx.exec(sql);
}

using json = nlohmann::json;

// Pinecone API Ayarları 
const std::string PINECONE_API_KEY = "pcsk_5bppga_3BpeU975AsymY5jnzqz6xNet3RvrjKLXas8AhWhykFovfTfRJzQf8v8hoDC5LPX";
const std::string PINECONE_HOST = "https://cmp653-benchmark-q4lnyjz.svc.aped-4627-b74a.pinecone.io/query";

// libcurl için veri okuma callback fonksiyonu
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Pinecone Üstel Geri Çekilme (Exponential Backoff) Destekli Hibrit Sorgu Fonksiyonu
void execute_pinecone_query() {
    int target_tenant = getRandomId(1, 5000);
    int target_category = getRandomId(1, 100);
    
    std::vector<float> query_vector(1536, 0.01f);

    json payload = {
        {"vector", query_vector},
        {"topK", 10},
        {"includeMetadata", false},
        {"filter", {
            {"tenant_id", {"$eq", target_tenant}},
            {"category_id", {"$eq", target_category}}
        }}
    };

    std::string payload_str = payload.dump();

    CURL* curl = curl_easy_init();
    if (curl) {
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, ("Api-Key: " + PINECONE_API_KEY).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, PINECONE_HOST.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
        
        std::string response_string;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

        int max_retries = 5;
        int current_retry = 0;
        int base_delay_ms = 100;

        while (current_retry < max_retries) {
            CURLcode res = curl_easy_perform(curl);
            
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            if (http_code == 429) { 
                std::cout << "[Pinecone] HTTP 429 Engeli! Backoff uygulaniyor: " << base_delay_ms << " ms" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(base_delay_ms));
                base_delay_ms *= 2; 
                current_retry++;
            } else if (http_code == 200) {
                break; 
            } else {
                break; 
            }
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

int main() {
    std::cout << "=== CMP653 Asynchronous Vector Benchmark Harness ===" << std::endl;

    try {
        pqxx::connection conn(DB_CONN);
        pqxx::work tx(conn);

        // 1. ADIM: PostgreSQL üzerinde HNSW indeksini oluşturma ve süresini ölçme
        std::cout << "\n[1/3] PostgreSQL HNSW indeksi oluşturuluyor (m=16, ef=64)..." << std::endl;
        auto start_idx = std::chrono::high_resolution_clock::now();
        
        tx.exec0("CREATE INDEX IF NOT EXISTS vector_idx ON vector_benchmark USING hnsw (embedding vector_cosine_ops) WITH (m = 16, ef_construction = 64);");
        tx.commit();
        
        auto end_idx = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed_idx = end_idx - start_idx;
        std::cout << ">> Indeks olusturma suresi: " << elapsed_idx.count() << " saniye." << std::endl;

        // 2. ADIM: Linux epoll I/O Kurulumu
        std::cout << "\n[2/3] Linux epoll alt yapisi hazirlaniyor..." << std::endl;
        int epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) {
            std::cerr << "epoll_create1 hatasi!" << std::endl;
            return 1;
        }
        std::cout << ">> epoll instance basariyla olusturuldu." << std::endl;

        // 3. ADIM: Eşzamanlı Hibrit Sorgular ve Stres Testi
        std::cout << "\n[3/3] Hibrit Arama ve Eszamanli Stres Testi baslatiliyor..." << std::endl;
        
        std::thread contention_thread(run_contention_workload);

        auto start_query = std::chrono::high_resolution_clock::now();
        
        pqxx::connection query_conn(DB_CONN);
        pqxx::work q_tx(query_conn);
        
        int total_queries = 1000;
        
        // ==========================================
        // POSTGRESQL DONGUSU VE SURE OLCUMU
        // ==========================================
        for (int i = 0; i < total_queries; ++i) {
            auto t1 = std::chrono::high_resolution_clock::now();
            
            execute_hybrid_query(q_tx);
            
            auto t2 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> ms = t2 - t1;
            pg_latencies.push_back(ms.count());
        }
        // ==========================================

        q_tx.commit();
        contention_thread.join();
        auto end_query = std::chrono::high_resolution_clock::now();
        
        std::chrono::duration<double> elapsed_query = end_query - start_query;
        std::cout << "\n=== BENCHMARK SONUCLARI (PostgreSQL) ===" << std::endl;
        std::cout << "Toplam " << total_queries << " Hibrit Sorgu Suresi: " << elapsed_query.count() << " saniye." << std::endl;
        std::cout << "Ortalama QPS: " << (total_queries / elapsed_query.count()) << std::endl;

        close(epoll_fd);

        // === PINECONE BULUT TESTI ===
        std::cout << "\n[Pinecone] Bulut uzerinden Hibrit Sorgu Testi baslatiliyor..." << std::endl;
        auto start_pinecone = std::chrono::high_resolution_clock::now();
        
        int pinecone_queries = 50; 
        
        // ==========================================
        // PINECONE DONGUSU VE SURE OLCUMU
        // ==========================================
        for (int i = 0; i < pinecone_queries; ++i) {
            auto t1 = std::chrono::high_resolution_clock::now();
            
            execute_pinecone_query();
            
            auto t2 = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> ms = t2 - t1;
            pinecone_latencies.push_back(ms.count());
        }
        // ==========================================

        auto end_pinecone = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed_pinecone = end_pinecone - start_pinecone;
        
        std::cout << "\n=== BENCHMARK SONUCLARI (Pinecone) ===" << std::endl;
        std::cout << "Toplam " << pinecone_queries << " Hibrit Sorgu Suresi: " << elapsed_pinecone.count() << " saniye." << std::endl;
        std::cout << "Ortalama QPS: " << (pinecone_queries / elapsed_pinecone.count()) << std::endl;

        // === POSTGRESQL YUZDELIK GECIKMELER (PERCENTILES) ===
        if (!pg_latencies.empty()) {
            std::sort(pg_latencies.begin(), pg_latencies.end());
            size_t p50_idx = pg_latencies.size() * 0.50;
            size_t p90_idx = pg_latencies.size() * 0.90;
            size_t p99_idx = pg_latencies.size() * 0.99;

            std::cout << "\n=== PostgreSQL Latency Percentiles ===\n";
            std::cout << "p50 (Median) : " << pg_latencies[p50_idx] << " ms\n";
            std::cout << "p90          : " << pg_latencies[p90_idx] << " ms\n";
            std::cout << "p99 (Tail)   : " << pg_latencies[p99_idx] << " ms\n";
        }

        // === PINECONE YUZDELIK GECIKMELER (PERCENTILES) ===
        if (!pinecone_latencies.empty()) {
            std::sort(pinecone_latencies.begin(), pinecone_latencies.end());
            size_t p50_idx = pinecone_latencies.size() * 0.50;
            size_t p90_idx = pinecone_latencies.size() * 0.90;
            size_t p99_idx = pinecone_latencies.size() * 0.99;

            std::cout << "\n=== Pinecone Latency Percentiles ===\n";
            std::cout << "p50 (Median) : " << pinecone_latencies[p50_idx] << " ms\n";
            std::cout << "p90          : " << pinecone_latencies[p90_idx] << " ms\n";
            std::cout << "p99 (Tail)   : " << pinecone_latencies[p99_idx] << " ms\n";
            std::cout << "============================================\n";
        }

    } catch (const std::exception &e) {
        std::cerr << "Hata olustu: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}