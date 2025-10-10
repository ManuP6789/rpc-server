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

struct LatencyStats {
    std::vector<double> latencies;
    std::atomic<uint64_t> success_count{0};
    std::atomic<uint64_t> error_count{0};
    
    void add_latency(double latency_ms) {
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
    
    void worker_thread(int worker_id, double target_rate, int duration_sec,
                      LatencyStats& stats, std::atomic<bool>& running);
    
    std::vector<uint8_t> generate_random_data(size_t size);
    void perform_hash_compute(RPCClient& client, LatencyStats& stats);
    void perform_sort_array(RPCClient& client, LatencyStats& stats, size_t size);
    void perform_matrix_multiply(RPCClient& client, LatencyStats& stats, uint32_t n);
    void perform_compress_data(RPCClient& client, LatencyStats& stats, size_t size);
    
public:
    LoadGenerator(const std::string& h, uint16_t p) : host(h), port(p) {}
    
    void run_load_test(double requests_per_sec, int duration_sec, 
                               int num_workers, LatencyStats& stats);
    void run_benchmark_suite(const std::string& output_file);
};

std::vector<uint8_t> LoadGenerator::generate_random_data(size_t size) {
    static std::mt19937 gen(42);
    static std::uniform_int_distribution<uint8_t> dist(0, 255);
    
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; i++) {
        data[i] = dist(gen);
    }
    return data;
}

void LoadGenerator::perform_hash_compute(RPCClient& client, LatencyStats& stats) {
    auto data = generate_random_data(1024);
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

void LoadGenerator::perform_sort_array(RPCClient& client, LatencyStats& stats, 
                                       size_t size) {
    std::vector<int32_t> array(size);
    std::mt19937 gen(42);
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

void LoadGenerator::perform_matrix_multiply(RPCClient& client, LatencyStats& stats,
                                            uint32_t n) {
    std::vector<double> matA(n * n);
    std::vector<double> matB(n * n);
    std::vector<double> result(n * n);
    
    std::mt19937 gen(42);
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

void LoadGenerator::perform_compress_data(RPCClient& client, LatencyStats& stats,
                                          size_t size) {
    auto data = generate_random_data(size);
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

void LoadGenerator::worker_thread(int worker_id, double target_rate, 
                                  int duration_sec, LatencyStats& stats,
                                  std::atomic<bool>& running) {
    try {
        // std::cout << "Inside running the worker_thread" << worker_id << "\n";
        RPCClient client(host.c_str(), port);
        
        double inter_request_time_ms = 1000.0 / target_rate;
        auto next_send_time = std::chrono::high_resolution_clock::now();
        
        std::mt19937 gen(worker_id);
        std::uniform_int_distribution<int> op_dist(0, 3);
        
        while (running.load()) {
            auto now = std::chrono::high_resolution_clock::now();
            if (now >= next_send_time) {
                // Choose operation randomly
                int op = op_dist(gen);
                
                switch (op) {
                case 0:
                    perform_hash_compute(client, stats);
                    break;
                case 1:
                    perform_sort_array(client, stats, 1000);
                    break;
                case 2:
                    perform_matrix_multiply(client, stats, 16);
                    break;
                case 3:
                    perform_compress_data(client, stats, 4096);
                    break;
                }
                
                next_send_time += std::chrono::microseconds(
                    static_cast<long>(inter_request_time_ms * 1000));
            } else {
                // Sleep until next request time
                std::this_thread::sleep_for(
                    std::chrono::microseconds(100));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Worker " << worker_id << " error: " << e.what() << std::endl;
    }
}

void LoadGenerator::run_load_test(double requests_per_sec, int duration_sec, 
                                          int num_workers, LatencyStats& stats) {
    std::atomic<bool> running{true};
    std::vector<std::thread> workers;
    
    double rate_per_worker = requests_per_sec / num_workers;
    
    std::cout << "Starting load test: " << requests_per_sec << " req/s, "
              << duration_sec << " seconds, " << num_workers << " workers\n";
    
    // Start worker threads
    for (int i = 0; i < num_workers; i++) {
        workers.emplace_back(&LoadGenerator::worker_thread, this, 
                           i, rate_per_worker, duration_sec, 
                           std::ref(stats), std::ref(running));
    }
    std::cout << "made it past init worker threads\n";
    
    // Run for specified duration
    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    
    // Stop workers
    running.store(false);
    for (auto& worker : workers) {
        worker.join();
    }
}

void LoadGenerator::run_benchmark_suite(const std::string& output_file) {
    std::vector<double> load_levels = {3000};
    int duration_sec = 10;
    int num_workers = 1;
    
    std::ofstream csv(output_file);
    csv << "RequestsPerSec,AvgLatency,P50,P95,P99,P999,SuccessCount,ErrorCount,Throughput\n";
    
    for (double load : load_levels) {
        std::cout << "\n=== Testing load: " << load << " req/s ===\n";
        LatencyStats stats;
        run_load_test(load, duration_sec, num_workers, stats);        
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
        
        csv << load << "," << avg << "," << p50 << "," << p95 << "," 
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
    std::string output = "load_test_results.csv";
    
    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);
    if (argc > 3) output = argv[3];
    
    LoadGenerator gen(host, port);
    gen.run_benchmark_suite(output);
    
    return 0;
}