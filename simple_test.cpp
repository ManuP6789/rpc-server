#include "rpc_client.cpp"
#include <iostream>
#include <atomic>

int main() {
    RPCClient client("127.0.0.1", 8080);
    
    std::atomic<int> pending{0};
    std::atomic<int> success{0};
    std::atomic<int> failed{0};
    
    for (int i = 0; i < 100; i++) {
        auto data = std::make_shared<std::vector<uint8_t>>(100, 'A');
        pending++;
        
        client.hash_compute_async(data->data(), data->size(),
            [&, data](bool ok, const char* hash) {
                pending--;
                if (ok) success++; else failed++;
            });
    }
    
    while (pending > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "Pending: " << pending << " Success: " << success 
                  << " Failed: " << failed << "\n";
    }
    
    std::cout << "\nFinal - Success: " << success << " Failed: " << failed << "\n";
    return 0;
}
