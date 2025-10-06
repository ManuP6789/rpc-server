#include "rpc_protocol.h"
#include <cstring>
#include <arpa/inet.h>

// HashComputeRequest implementations
std::vector<uint8_t> HashComputeRequest::serialize(const void* data, size_t size) {
    std::vector<uint8_t> result(sizeof(uint32_t) + size);
    uint32_t size_net = htonl(size);
    memcpy(result.data(), &size_net, sizeof(uint32_t));
    memcpy(result.data() + sizeof(uint32_t), data, size);
    return result;
}

bool HashComputeRequest::deserialize(const std::vector<uint8_t>& payload,
                                    std::vector<uint8_t>& data) {
    if (payload.size() < sizeof(uint32_t)) {
        return false;
    }
    
    uint32_t size_net;
    memcpy(&size_net, payload.data(), sizeof(uint32_t));
    uint32_t size = ntohl(size_net);
    
    if (payload.size() != sizeof(uint32_t) + size) {
        return false;
    }
    
    data.resize(size);
    memcpy(data.data(), payload.data() + sizeof(uint32_t), size);
    return true;
}

// HashComputeResponse implementations
std::vector<uint8_t> HashComputeResponse::serialize(const char* hash) {
    std::vector<uint8_t> result(65);
    memcpy(result.data(), hash, 65);
    return result;
}

bool HashComputeResponse::deserialize(const std::vector<uint8_t>& payload,
                                     char* hash) {
    if (payload.size() != 65) {
        return false;
    }
    
    memcpy(hash, payload.data(), 65);
    return true;
}

// SortArrayRequest implementations
std::vector<uint8_t> SortArrayRequest::serialize(const int32_t* array, size_t size) {
    std::vector<uint8_t> result(sizeof(uint32_t) + size * sizeof(int32_t));
    uint32_t size_net = htonl(size);
    memcpy(result.data(), &size_net, sizeof(uint32_t));
    
    // Convert array elements to network byte order
    int32_t* array_ptr = reinterpret_cast<int32_t*>(result.data() + sizeof(uint32_t));
    for (size_t i = 0; i < size; i++) {
        array_ptr[i] = htonl(array[i]);
    }
    
    return result;
}

bool SortArrayRequest::deserialize(const std::vector<uint8_t>& payload,
                                   std::vector<int32_t>& array) {
    if (payload.size() < sizeof(uint32_t)) {
        return false;
    }
    
    uint32_t size_net;
    memcpy(&size_net, payload.data(), sizeof(uint32_t));
    uint32_t size = ntohl(size_net);
    
    if (payload.size() != sizeof(uint32_t) + size * sizeof(int32_t)) {
        return false;
    }
    
    array.resize(size);
    const int32_t* array_ptr = reinterpret_cast<const int32_t*>(
        payload.data() + sizeof(uint32_t));
    
    for (size_t i = 0; i < size; i++) {
        array[i] = ntohl(array_ptr[i]);
    }
    
    return true;
}

// SortArrayResponse implementations
std::vector<uint8_t> SortArrayResponse::serialize(const int32_t* array, size_t size) {
    return SortArrayRequest::serialize(array, size);
}

bool SortArrayResponse::deserialize(const std::vector<uint8_t>& payload,
                                   std::vector<int32_t>& array) {
    return SortArrayRequest::deserialize(payload, array);
}

// MatrixMultiplyRequest implementations
std::vector<uint8_t> MatrixMultiplyRequest::serialize(const double* matA,
                                                     const double* matB,
                                                     uint32_t n) {
    size_t matrix_size = n * n * sizeof(double);
    std::vector<uint8_t> result(sizeof(uint32_t) + 2 * matrix_size);
    
    uint32_t n_net = htonl(n);
    memcpy(result.data(), &n_net, sizeof(uint32_t));
    memcpy(result.data() + sizeof(uint32_t), matA, matrix_size);
    memcpy(result.data() + sizeof(uint32_t) + matrix_size, matB, matrix_size);
    
    return result;
}

bool MatrixMultiplyRequest::deserialize(const std::vector<uint8_t>& payload,
                                       std::vector<double>& matA,
                                       std::vector<double>& matB,
                                       uint32_t& n) {
    if (payload.size() < sizeof(uint32_t)) {
        return false;
    }
    
    uint32_t n_net;
    memcpy(&n_net, payload.data(), sizeof(uint32_t));
    n = ntohl(n_net);
    
    size_t matrix_size = n * n * sizeof(double);
    if (payload.size() != sizeof(uint32_t) + 2 * matrix_size) {
        return false;
    }
    
    matA.resize(n * n);
    matB.resize(n * n);
    
    memcpy(matA.data(), payload.data() + sizeof(uint32_t), matrix_size);
    memcpy(matB.data(), payload.data() + sizeof(uint32_t) + matrix_size, matrix_size);
    
    return true;
}

// MatrixMultiplyResponse implementations
std::vector<uint8_t> MatrixMultiplyResponse::serialize(const double* result, uint32_t n) {
    size_t matrix_size = n * n * sizeof(double);
    std::vector<uint8_t> payload(sizeof(uint32_t) + matrix_size);
    
    uint32_t n_net = htonl(n);
    memcpy(payload.data(), &n_net, sizeof(uint32_t));
    memcpy(payload.data() + sizeof(uint32_t), result, matrix_size);
    
    return payload;
}

bool MatrixMultiplyResponse::deserialize(const std::vector<uint8_t>& payload,
                                        std::vector<double>& result,
                                        uint32_t& n) {
    if (payload.size() < sizeof(uint32_t)) {
        return false;
    }
    
    uint32_t n_net;
    memcpy(&n_net, payload.data(), sizeof(uint32_t));
    n = ntohl(n_net);
    
    size_t matrix_size = n * n * sizeof(double);
    if (payload.size() != sizeof(uint32_t) + matrix_size) {
        return false;
    }
    
    result.resize(n * n);
    memcpy(result.data(), payload.data() + sizeof(uint32_t), matrix_size);
    
    return true;
}

// CompressDataRequest implementations
std::vector<uint8_t> CompressDataRequest::serialize(CompressionAlgo algo,
                                                    const void* data,
                                                    size_t size) {
    std::vector<uint8_t> result(sizeof(uint8_t) + sizeof(uint32_t) + size);
    
    result[0] = static_cast<uint8_t>(algo);
    uint32_t size_net = htonl(size);
    memcpy(result.data() + sizeof(uint8_t), &size_net, sizeof(uint32_t));
    memcpy(result.data() + sizeof(uint8_t) + sizeof(uint32_t), data, size);
    
    return result;
}

bool CompressDataRequest::deserialize(const std::vector<uint8_t>& payload,
                                     CompressionAlgo& algo,
                                     std::vector<uint8_t>& data) {
    if (payload.size() < sizeof(uint8_t) + sizeof(uint32_t)) {
        return false;
    }
    
    algo = static_cast<CompressionAlgo>(payload[0]);
    
    uint32_t size_net;
    memcpy(&size_net, payload.data() + sizeof(uint8_t), sizeof(uint32_t));
    uint32_t size = ntohl(size_net);
    
    if (payload.size() != sizeof(uint8_t) + sizeof(uint32_t) + size) {
        return false;
    }
    
    data.resize(size);
    memcpy(data.data(), payload.data() + sizeof(uint8_t) + sizeof(uint32_t), size);
    
    return true;
}

// CompressDataResponse implementations
std::vector<uint8_t> CompressDataResponse::serialize(const void* data, size_t size) {
    std::vector<uint8_t> result(sizeof(uint32_t) + size);
    uint32_t size_net = htonl(size);
    memcpy(result.data(), &size_net, sizeof(uint32_t));
    memcpy(result.data() + sizeof(uint32_t), data, size);
    return result;
}

bool CompressDataResponse::deserialize(const std::vector<uint8_t>& payload,
                                      std::vector<uint8_t>& data) {
    if (payload.size() < sizeof(uint32_t)) {
        return false;
    }
    
    uint32_t size_net;
    memcpy(&size_net, payload.data(), sizeof(uint32_t));
    uint32_t size = ntohl(size_net);
    
    if (payload.size() != sizeof(uint32_t) + size) {
        return false;
    }
    
    data.resize(size);
    memcpy(data.data(), payload.data() + sizeof(uint32_t), size);
    
    return true;
}