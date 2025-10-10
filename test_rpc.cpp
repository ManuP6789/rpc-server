#include "rpc_client.cpp"
#include <cassert>
#include <iostream>
#include <cmath>
#include <cstring>

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " #name "... "; \
    test_##name(); \
    std::cout << "PASSED\n"; \
} while(0)

// Helper function to compare floating point arrays
bool arrays_equal(const double* a, const double* b, size_t n, double epsilon = 1e-6) {
    for (size_t i = 0; i < n; i++) {
        if (std::abs(a[i] - b[i]) > epsilon) {
            return false;
        }
    }
    return true;
}

TEST(hash_compute_simple) {
    RPCClient client("127.0.0.1", 8080);
    
    const char* data = "Hello, World!";
    char hash[65];
    
    bool success = client.hash_compute(data, strlen(data), hash);
    assert(success && "Hash computation failed");
    
    // Expected SHA-256 of "Hello, World!"
    const char* expected = "dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f";
    assert(strcmp(hash, expected) == 0 && "Hash mismatch");
}

TEST(hash_compute_empty) {
    RPCClient client("127.0.0.1", 8080);
    
    const char* data = "";
    char hash[65];
    
    bool success = client.hash_compute(data, 0, hash);
    assert(success && "Hash computation of empty data failed");
    
    // SHA-256 of empty string
    const char* expected = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    assert(strcmp(hash, expected) == 0 && "Empty hash mismatch");
}

TEST(hash_compute_large) {
    RPCClient client("127.0.0.1", 8080);
    
    // Create 1MB of data
    std::vector<uint8_t> data(1024 * 1024, 'A');
    char hash[65];
    
    bool success = client.hash_compute(data.data(), data.size(), hash);
    assert(success && "Hash computation of large data failed");
    assert(strlen(hash) == 64 && "Hash has wrong length");
}

TEST(sort_array_simple) {
    RPCClient client("127.0.0.1", 8080);
    
    int32_t array[] = {5, 2, 8, 1, 9, 3, 7};
    size_t size = sizeof(array) / sizeof(array[0]);
    
    bool success = client.sort_array(array, size);
    assert(success && "Sort failed");
    
    int32_t expected[] = {1, 2, 3, 5, 7, 8, 9};
    for (size_t i = 0; i < size; i++) {
        assert(array[i] == expected[i] && "Array not sorted correctly");
    }
}

TEST(sort_array_negative) {
    RPCClient client("127.0.0.1", 8080);
    
    int32_t array[] = {-5, 10, -3, 0, -8, 2};
    size_t size = sizeof(array) / sizeof(array[0]);
    
    bool success = client.sort_array(array, size);
    assert(success && "Sort with negatives failed");
    
    int32_t expected[] = {-8, -5, -3, 0, 2, 10};
    for (size_t i = 0; i < size; i++) {
        assert(array[i] == expected[i] && "Negative array not sorted correctly");
    }
}

TEST(sort_array_single) {
    RPCClient client("127.0.0.1", 8080);
    
    int32_t array[] = {42};
    
    bool success = client.sort_array(array, 1);
    assert(success && "Sort of single element failed");
    assert(array[0] == 42 && "Single element changed");
}

TEST(sort_array_already_sorted) {
    RPCClient client("127.0.0.1", 8080);
    
    int32_t array[] = {1, 2, 3, 4, 5};
    size_t size = 5;
    
    bool success = client.sort_array(array, size);
    assert(success && "Sort of sorted array failed");
    
    for (size_t i = 0; i < size; i++) {
        assert(array[i] == static_cast<int32_t>(i + 1) && "Sorted array modified");
    }
}

TEST(matrix_multiply_identity) {
    RPCClient client("127.0.0.1", 8080);
    
    uint32_t n = 3;
    double matA[] = {
        1, 2, 3,
        4, 5, 6,
        7, 8, 9
    };
    
    double identity[] = {
        1, 0, 0,
        0, 1, 0,
        0, 0, 1
    };
    
    double result[9];
    
    bool success = client.matrix_multiply(matA, identity, n, result);
    assert(success && "Matrix multiply with identity failed");
    assert(arrays_equal(result, matA, 9) && "Identity multiplication incorrect");
}

TEST(matrix_multiply_2x2) {
    RPCClient client("127.0.0.1", 8080);
    
    uint32_t n = 2;
    double matA[] = {1, 2, 3, 4};
    double matB[] = {5, 6, 7, 8};
    double result[4];
    
    bool success = client.matrix_multiply(matA, matB, n, result);
    assert(success && "2x2 matrix multiply failed");
    
    // [1 2] * [5 6] = [19 22]
    // [3 4]   [7 8]   [43 50]
    double expected[] = {19, 22, 43, 50};
    assert(arrays_equal(result, expected, 4) && "2x2 multiplication incorrect");
}

TEST(matrix_multiply_zero) {
    RPCClient client("127.0.0.1", 8080);
    
    uint32_t n = 2;
    double matA[] = {1, 2, 3, 4};
    double zeros[] = {0, 0, 0, 0};
    double result[4];
    
    bool success = client.matrix_multiply(matA, zeros, n, result);
    assert(success && "Matrix multiply with zeros failed");
    
    double expected[] = {0, 0, 0, 0};
    assert(arrays_equal(result, expected, 4) && "Zero multiplication incorrect");
}

TEST(compress_data_simple) {
    RPCClient client("127.0.0.1", 8080);
    
    const char* data = "This is a test string for compression. "
                      "This is a test string for compression. "
                      "This is a test string for compression.";
    size_t size = strlen(data);
    
    std::vector<uint8_t> compressed;
    bool success = client.compress_data(CompressionAlgo::ZLIB, data, size, compressed);
    
    assert(success && "Compression failed");
    assert(!compressed.empty() && "Compressed data is empty");
    assert(compressed.size() < size && "Compressed size not smaller");
}

TEST(compress_data_empty) {
    RPCClient client("127.0.0.1", 8080);
    
    const char* data = "";
    std::vector<uint8_t> compressed;
    
    bool success = client.compress_data(CompressionAlgo::ZLIB, data, 0, compressed);
    assert(success && "Empty compression failed");
}

TEST(compress_data_random) {
    RPCClient client("127.0.0.1", 8080);
    
    // Random data doesn't compress well
    std::vector<uint8_t> data(1000);
    for (size_t i = 0; i < data.size(); i++) {
        data[i] = static_cast<uint8_t>(rand() % 256);
    }
    
    std::vector<uint8_t> compressed;
    bool success = client.compress_data(CompressionAlgo::ZLIB, 
                                       data.data(), data.size(), compressed);
    
    assert(success && "Random data compression failed");
    // Random data might not compress much, just check it didn't fail
}

TEST(multiple_operations_sequence) {
    RPCClient client("127.0.0.1", 8080);
    
    // Hash
    char hash[65];
    client.hash_compute("test", 4, hash);
    assert(strlen(hash) == 64 && "Hash operation in sequence failed");
    
    // Sort
    int32_t array[] = {3, 1, 2};
    client.sort_array(array, 3);
    assert(array[0] == 1 && array[1] == 2 && array[2] == 3 && 
           "Sort operation in sequence failed");
    
    // Matrix multiply
    double matA[] = {1, 0, 0, 1};
    double matB[] = {2, 3, 4, 5};
    double result[4];
    client.matrix_multiply(matA, matB, 2, result);
    assert(result[0] == 2 && result[3] == 5 && 
           "Matrix operation in sequence failed");
    
    // Compress
    std::vector<uint8_t> compressed;
    client.compress_data(CompressionAlgo::ZLIB, "data", 4, compressed);
    assert(!compressed.empty() && "Compress operation in sequence failed");
}

TEST(stress_many_requests) {
    RPCClient client("127.0.0.1", 8080);
    
    const int NUM_REQUESTS = 100;
    
    for (int i = 0; i < NUM_REQUESTS; i++) {
        char hash[65];
        std::string data = "request_" + std::to_string(i);
        bool success = client.hash_compute(data.c_str(), data.length(), hash);
        assert(success && "Stress test request failed");
    }
}

int main(int argc, char** argv) {
    if (argc > 1) {
        std::cout << "Usage: " << argv[0] << "\n";
        std::cout << "Make sure the RPC server is running on port 8080\n";
        return 1;
    }
    
    std::cout << "=== Running RPC Unit Tests ===\n\n";
    
    try {
        // Hash tests
        RUN_TEST(hash_compute_simple);
        RUN_TEST(hash_compute_empty);
        RUN_TEST(hash_compute_large);
        
        // Sort tests
        RUN_TEST(sort_array_simple);
        RUN_TEST(sort_array_negative);
        RUN_TEST(sort_array_single);
        RUN_TEST(sort_array_already_sorted);
        
        // Matrix tests
        RUN_TEST(matrix_multiply_identity);
        RUN_TEST(matrix_multiply_2x2);
        RUN_TEST(matrix_multiply_zero);
        
        // Compression tests
        RUN_TEST(compress_data_simple);
        RUN_TEST(compress_data_empty);
        RUN_TEST(compress_data_random);
        
        // Integration tests
        RUN_TEST(multiple_operations_sequence);
        RUN_TEST(stress_many_requests);
        
        std::cout << "\n=== All Tests Passed! ===\n";
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "\nTest failed with exception: " << e.what() << "\n";
        return 1;
    }
}