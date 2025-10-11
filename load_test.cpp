#include "rpc_client.cpp"
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <queue>

enum class TestMode {
    SYNC,
    ASYNC
};

struct LatencyStats {
    std::vector<double> latencies;
    std::atomic<uint64_t> success_count{0};
    std::atomic<uint64_t> error_count{0};
    std::mutex latency_mutex;
    
    void add_latency(double latency_ms) {
        std::lock_guard<std::mutex> lock(latency_mutex);
        latencies.push_back(latency_ms);
        success_count++;
    }
    
    void add_error() {
        error_count++;
    }
    
    double get_percentile(double p) {
        if (latencies.empty()) return 0.0;
        
        std::vector<double> sorted = latencies;
        std::sort(sorted.begin(), sorted.end());
        
        size_t idx = static_cast<size_t>(p * sorted.size());
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        
        return sorted[idx];
    }
    
    double get_average() {
        if (latencies.empty()) return 0.0;
        
        double sum = 0.0;
        for (double lat : latencies) {
            sum += lat;
        }
        return sum / latencies.size();
    }
};

class LoadGenerator {
private:
    std::string host;
    uint16_t port;
    TestMode mode;
    
    void worker_thread_sync(int worker_id, double target_rate, int duration_sec,
                           LatencyStats& stats, std::atomic<bool>& running);
    void worker_thread_async(int worker_id, double target_rate, int duration_sec,
                            int max_in_flight, LatencyStats& stats, 
                            std::atomic<bool>& running);
    
    std::vector<uint8_t> generate_random_data(size_t size, int seed);
    
    // Sync operations
    void perform_hash_compute_sync(RPCClient& client, LatencyStats& stats, int seed);
    void perform_sort_array_sync(RPCClient& client, LatencyStats& stats, size_t size, int seed);
    void perform_matrix_multiply_sync(RPCClient& client, LatencyStats& stats, uint32_t n, int seed);
    void perform_compress_data_sync(RPCClient& client, LatencyStats& stats, size_t size, int seed);
    
public:
    LoadGenerator(const std::string& h, uint16_t p, TestMode m) 
        : host(h), port(p), mode(m) {}
    
    void run_load_test(double requests_per_sec, int duration_sec, 
                      int num_workers, int max_in_flight, LatencyStats& stats);
    void run_benchmark_suite(const std::string& output_file);
};

std::vector<uint8_t> LoadGenerator::generate_random_data(size_t size, int seed) {
    std::mt19937 gen(seed);
    std::uniform_int_distribution<uint8_t> dist(0, 255);
    
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; i++) {
        data[i] = dist(gen);
    }
    return data;
}

// ============ SYNC OPERATIONS ============

void LoadGenerator::perform_hash_compute_sync(RPCClient& client, LatencyStats& stats, int seed) {
    auto data = generate_random_data(1024, seed);
    char hash[65];
    
    auto start = std::chrono::high_resolution_clock::now();
    bool success = client.hash_compute(data.data(), data.size(), hash);
    auto end = std::chrono::high_resolution_clock::now();
    
    if (success) {
        double latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
        stats.add_latency(latency_ms);
    } else {
        stats.add_error();
    }
}

void LoadGenerator::perform_sort_array_sync(RPCClient& client, LatencyStats& stats, 
                                            size_t size, int seed) {
    std::vector<int32_t> array(size);
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int32_t> dist(-1000000, 1000000);
    
    for (size_t i = 0; i < size; i++) {
        array[i] = dist(gen);
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    bool success = client.sort_array(array.data(), size);
    auto end = std::chrono::high_resolution_clock::now();
    
    if (success) {
        double latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
        stats.add_latency(latency_ms);
    } else {
        stats.add_error();
    }
}

void LoadGenerator::perform_matrix_multiply_sync(RPCClient& client, LatencyStats& stats,
                                                 uint32_t n, int seed) {
    std::vector<double> matA(n * n);
    std::vector<double> matB(n * n);
    std::vector<double> result(n * n);
    
    std::mt19937 gen(seed);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    
    for (uint32_t i = 0; i < n * n; i++) {
        matA[i] = dist(gen);
        matB[i] = dist(gen);
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    bool success = client.matrix_multiply(matA.data(), matB.data(), n, result.data());
    auto end = std::chrono::high_resolution_clock::now();
    
    if (success) {
        double latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
        stats.add_latency(latency_ms);
    } else {
        stats.add_error();
    }
}

void LoadGenerator::perform_compress_data_sync(RPCClient& client, LatencyStats& stats,
                                               size_t size, int seed) {
    auto data = generate_random_data(size, seed);
    std::vector<uint8_t> compressed;
    
    auto start = std::chrono::high_resolution_clock::now();
    bool success = client.compress_data(CompressionAlgo::ZLIB, 
                                       data.data(), data.size(), compressed);
    auto end = std::chrono::high_resolution_clock::now();
    
    if (success) {
        double latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
        stats.add_latency(latency_ms);
    } else {
        stats.add_error();
    }
}

// ============ WORKER THREADS ============

void LoadGenerator::worker_thread_sync(int worker_id, double target_rate, 
                                       int duration_sec, LatencyStats& stats,
                                       std::atomic<bool>& running) {
    try {
        RPCClient client(host.c_str(), port);
        
        double inter_request_time_ms = 1000.0 / target_rate;
        auto next_send_time = std::chrono::high_resolution_clock::now();
        
        std::mt19937 gen(worker_id);
        std::uniform_int_distribution<int> op_dist(0, 3);
        int req_count = 0;
        
        while (running.load()) {
            auto now = std::chrono::high_resolution_clock::now();
            if (now >= next_send_time) {
                int op = op_dist(gen);
                int seed = worker_id * 1000000 + req_count++;
                
                switch (op) {
                case 0:
                    perform_hash_compute_sync(client, stats, seed);
                    break;
                case 1:
                    perform_sort_array_sync(client, stats, 1000, seed);
                    break;
                case 2:
                    perform_matrix_multiply_sync(client, stats, 16, seed);
                    break;
                case 3:
                    perform_compress_data_sync(client, stats, 4096, seed);
                    break;
                }
                
                next_send_time += std::chrono::microseconds(
                    static_cast<long>(inter_request_time_ms * 1000));
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Worker " << worker_id << " error: " << e.what() << std::endl;
    }
}

// TRUE ASYNC - Multiple requests in flight!
void LoadGenerator::worker_thread_async(int worker_id, double target_rate, 
                                       int duration_sec, int max_in_flight,
                                       LatencyStats& stats,
                                       std::atomic<bool>& running) {
    try {
        RPCClient client(host.c_str(), port);
        
        std::atomic<int> in_flight{0};
        std::mt19937 gen(worker_id);
        std::uniform_int_distribution<int> op_dist(0, 3);
        int req_count = 0;
        
        double inter_request_time_ms = 1000.0 / target_rate;
        auto next_send_time = std::chrono::high_resolution_clock::now();
        
        while (running.load() || in_flight.load() > 0) {
            auto now = std::chrono::high_resolution_clock::now();
            
            // Issue new requests if we have capacity and it's time
            if (running.load() && now >= next_send_time && in_flight.load() < max_in_flight) {
                int op = op_dist(gen);
                int seed = worker_id * 1000000 + req_count++;
                
                in_flight++;
                auto start = std::chrono::high_resolution_clock::now();
                
                switch (op) {
                case 0: {
                    auto data = generate_random_data(1024, seed);
                    client.hash_compute_async(data.data(), data.size(), 
                        [&stats, &in_flight, start](bool success, const char* hash) {
                            auto end = std::chrono::high_resolution_clock::now();
                            if (success) {
                                double latency = std::chrono::duration<double, std::milli>(end - start).count();
                                stats.add_latency(latency);
                            } else {
                                stats.add_error();
                            }
                            in_flight--;
                        });
                    break;
                }
                case 1: {
                    std::vector<int32_t> array(1000);
                    std::mt19937 arr_gen(seed);
                    std::uniform_int_distribution<int32_t> arr_dist(-1000000, 1000000);
                    for (size_t i = 0; i < 1000; i++) {
                        array[i] = arr_dist(arr_gen);
                    }
                    client.sort_array_async(array.data(), 1000,
                        [&stats, &in_flight, start](bool success, const std::vector<int32_t>& sorted) {
                            auto end = std::chrono::high_resolution_clock::now();
                            if (success) {
                                double latency = std::chrono::duration<double, std::milli>(end - start).count();
                                stats.add_latency(latency);
                            } else {
                                stats.add_error();
                            }
                            in_flight--;
                        });
                    break;
                }
                case 2: {
                    std::vector<double> matA(16 * 16);
                    std::vector<double> matB(16 * 16);
                    std::mt19937 mat_gen(seed);
                    std::uniform_real_distribution<double> mat_dist(0.0, 1.0);
                    for (uint32_t i = 0; i < 16 * 16; i++) {
                        matA[i] = mat_dist(mat_gen);
                        matB[i] = mat_dist(mat_gen);
                    }
                    client.matrix_multiply_async(matA.data(), matB.data(), 16,
                        [&stats, &in_flight, start](bool success, const std::vector<double>& result, uint32_t n) {
                            auto end = std::chrono::high_resolution_clock::now();
                            if (success) {
                                double latency = std::chrono::duration<double, std::milli>(end - start).count();
                                stats.add_latency(latency);
                            } else {
                                stats.add_error();
                            }
                            in_flight--;
                        });
                    break;
                }
                case 3: {
                    auto data = generate_random_data(4096, seed);
                    client.compress_data_async(CompressionAlgo::ZLIB, data.data(), data.size(),
                        [&stats, &in_flight, start](bool success, const std::vector<uint8_t>& compressed) {
                            auto end = std::chrono::high_resolution_clock::now();
                            if (success) {
                                double latency = std::chrono::duration<double, std::milli>(end - start).count();
                                stats.add_latency(latency);
                            } else {
                                stats.add_error();
                            }
                            in_flight--;
                        });
                    break;
                }
                }
                
                next_send_time += std::chrono::microseconds(
                    static_cast<long>(inter_request_time_ms * 1000));
            } else {
                // Sleep briefly if we can't issue more requests
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Worker " << worker_id << " error: " << e.what() << std::endl;
    }
}

void LoadGenerator::run_load_test(double requests_per_sec, int duration_sec, 
                                  int num_workers, int max_in_flight, LatencyStats& stats) {
    std::atomic<bool> running{true};
    std::vector<std::thread> workers;
    
    double rate_per_worker = requests_per_sec / num_workers;
    
    const char* mode_str = (mode == TestMode::SYNC) ? "SYNC" : "ASYNC";
    
    std::cout << "Starting load test (" << mode_str << "): " 
              << requests_per_sec << " req/s, "
              << duration_sec << " seconds, " << num_workers << " workers";
    
    if (mode == TestMode::ASYNC) {
        std::cout << ", max " << max_in_flight << " in-flight per worker";
    }
    std::cout << "\n";
    
    // Start worker threads
    for (int i = 0; i < num_workers; i++) {
        if (mode == TestMode::SYNC) {
            workers.emplace_back(&LoadGenerator::worker_thread_sync, this, 
                               i, rate_per_worker, duration_sec, 
                               std::ref(stats), std::ref(running));
        } else {
            workers.emplace_back(&LoadGenerator::worker_thread_async, this, 
                               i, rate_per_worker, duration_sec, max_in_flight,
                               std::ref(stats), std::ref(running));
        }
    }
    
    // Run for specified duration
    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    
    // Stop workers
    running.store(false);
    for (auto& worker : workers) {
        worker.join();
    }
}

void LoadGenerator::run_benchmark_suite(const std::string& output_file) {
    std::vector<double> load_levels = {1000, 5000, 10000, 20000, 30000};
    int duration_sec = 30;
    int num_workers = 4;
    int max_in_flight = 100;  // For async mode
    
    std::ofstream csv(output_file);
    csv << "Mode,RequestsPerSec,AvgLatency,P50,P95,P99,P999,SuccessCount,ErrorCount,Throughput\n";
    
    for (double load : load_levels) {
        std::cout << "\n=== Testing load: " << load << " req/s ===\n";
        LatencyStats stats;
        run_load_test(load, duration_sec, num_workers, max_in_flight, stats);
        
        double avg = stats.get_average();
        double p50 = stats.get_percentile(0.50);
        double p95 = stats.get_percentile(0.95);
        double p99 = stats.get_percentile(0.99);
        double p999 = stats.get_percentile(0.999);
        
        double actual_throughput = stats.success_count.load() / 
                                  static_cast<double>(duration_sec);
        
        std::cout << "  Average latency: " << std::fixed << std::setprecision(2) 
                  << avg << " ms\n";
        std::cout << "  P50: " << p50 << " ms\n";
        std::cout << "  P95: " << p95 << " ms\n";
        std::cout << "  P99: " << p99 << " ms\n";
        std::cout << "  P99.9: " << p999 << " ms\n";
        std::cout << "  Success: " << stats.success_count.load() << "\n";
        std::cout << "  Errors: " << stats.error_count.load() << "\n";
        std::cout << "  Actual throughput: " << actual_throughput << " req/s\n";
        
        const char* mode_str = (mode == TestMode::SYNC) ? "SYNC" : "ASYNC";
        
        csv << mode_str << ","
            << load << "," << avg << "," << p50 << "," << p95 << "," 
            << p99 << "," << p999 << "," << stats.success_count.load() << ","
            << stats.error_count.load() << "," << actual_throughput << "\n";
        
        // Brief pause between tests
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    
    csv.close();
    std::cout << "\nResults written to " << output_file << "\n";
}

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    std::string mode_str = "async";
    std::string output = "load_test_results.csv";
    
    if (argc > 1) mode_str = argv[1];  // "sync" or "async"
    if (argc > 2) host = argv[2];
    if (argc > 3) port = std::atoi(argv[3]);
    if (argc > 4) output = argv[4];
    
    TestMode mode = (mode_str == "sync") ? TestMode::SYNC : TestMode::ASYNC;
    
    std::cout << "Running in " << mode_str << " mode\n";
    
    LoadGenerator gen(host, port, mode);
    gen.run_benchmark_suite(output);
    
    return 0;
}