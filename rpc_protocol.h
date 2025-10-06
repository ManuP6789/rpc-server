#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Protocol constants
constexpr uint32_t RPC_MAGIC = 0x52504301; // "RPC\x01"
constexpr uint32_t MAX_PAYLOAD_SIZE = 16 * 1024 * 1024; // 16MB

// Operation codes
enum class RPCOperation : uint32_t {
    HASH_COMPUTE = 1,
    SORT_ARRAY = 2,
    MATRIX_MULTIPLY = 3,
    COMPRESS_DATA = 4
};

// Compression algorithms
enum class CompressionAlgo : uint8_t {
    ZLIB = 0,
    LZ4 = 1
};

// Error codes
enum class RPCError : uint32_t {
    SUCCESS = 0,
    INVALID_OPERATION = 1,
    INVALID_PARAMS = 2,
    COMPUTATION_ERROR = 3,
    NETWORK_ERROR = 4,
    TIMEOUT = 5
};

// RPC Message Header (fixed size, network byte order)
struct __attribute__((packed)) RPCHeader {
    uint32_t magic;           // Protocol magic number
    uint64_t request_id;      // Unique request identifier
    uint32_t operation;       // Operation code
    uint32_t payload_size;    // Size of payload in bytes
    uint32_t error_code;      // Error code (0 = success)
    uint32_t reserved;        // Reserved for future use
    
    RPCHeader() : magic(RPC_MAGIC), request_id(0), operation(0), 
                  payload_size(0), error_code(0), reserved(0) {}
    
    void to_network_order();
    void to_host_order();
};

// Request/Response payload structures
struct HashComputeRequest {
    uint32_t data_size;
    // Followed by data_size bytes of data
    
    static std::vector<uint8_t> serialize(const void* data, size_t size);
    static bool deserialize(const std::vector<uint8_t>& payload, 
                           std::vector<uint8_t>& data);
};

struct HashComputeResponse {
    char hash[65]; // 64 hex chars + null terminator
    
    static std::vector<uint8_t> serialize(const char* hash);
    static bool deserialize(const std::vector<uint8_t>& payload, char* hash);
};

struct SortArrayRequest {
    uint32_t array_size;
    // Followed by array_size * sizeof(int32_t) bytes
    
    static std::vector<uint8_t> serialize(const int32_t* array, size_t size);
    static bool deserialize(const std::vector<uint8_t>& payload,
                           std::vector<int32_t>& array);
};

struct SortArrayResponse {
    uint32_t array_size;
    // Followed by sorted array data
    
    static std::vector<uint8_t> serialize(const int32_t* array, size_t size);
    static bool deserialize(const std::vector<uint8_t>& payload,
                           std::vector<int32_t>& array);
};

struct MatrixMultiplyRequest {
    uint32_t n; // Matrix dimension (n x n)
    // Followed by 2 * n * n * sizeof(double) bytes (matA, matB)
    
    static std::vector<uint8_t> serialize(const double* matA, const double* matB, 
                                         uint32_t n);
    static bool deserialize(const std::vector<uint8_t>& payload,
                           std::vector<double>& matA, std::vector<double>& matB,
                           uint32_t& n);
};

struct MatrixMultiplyResponse {
    uint32_t n;
    // Followed by n * n * sizeof(double) bytes (result matrix)
    
    static std::vector<uint8_t> serialize(const double* result, uint32_t n);
    static bool deserialize(const std::vector<uint8_t>& payload,
                           std::vector<double>& result, uint32_t& n);
};

struct CompressDataRequest {
    uint8_t algorithm; // CompressionAlgo
    uint32_t data_size;
    // Followed by data_size bytes
    
    static std::vector<uint8_t> serialize(CompressionAlgo algo, 
                                         const void* data, size_t size);
    static bool deserialize(const std::vector<uint8_t>& payload,
                           CompressionAlgo& algo, std::vector<uint8_t>& data);
};

struct CompressDataResponse {
    uint32_t compressed_size;
    // Followed by compressed data
    
    static std::vector<uint8_t> serialize(const void* data, size_t size);
    static bool deserialize(const std::vector<uint8_t>& payload,
                           std::vector<uint8_t>& data);
};

// Helper functions
inline void RPCHeader::to_network_order() {
    magic = htonl(magic);
    request_id = htobe64(request_id);
    operation = htonl(operation);
    payload_size = htonl(payload_size);
    error_code = htonl(error_code);
}

inline void RPCHeader::to_host_order() {
    magic = ntohl(magic);
    request_id = be64toh(request_id);
    operation = ntohl(operation);
    payload_size = ntohl(payload_size);
    error_code = ntohl(error_code);
}