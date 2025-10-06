#include <liburing.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include "rpc_protocol.h"

constexpr int CLIENT_QUEUE_DEPTH = 64;

class RPCClient {
private:
    io_uring ring;
    int sock_fd;
    std::atomic<uint64_t> next_request_id{1};
    
    bool send_request(const RPCHeader& header, 
                     const std::vector<uint8_t>& payload);
    bool receive_response(RPCHeader& header, std::vector<uint8_t>& payload);
    
public:
    RPCClient(const char* host, uint16_t port);
    ~RPCClient();
    
    bool hash_compute(const void* data, size_t size, char* hash_out);
    bool sort_array(int32_t* array, size_t size);
    bool matrix_multiply(const double* matA, const double* matB, 
                        uint32_t n, double* result);
    bool compress_data(CompressionAlgo algo, const void* data, 
                      size_t size, std::vector<uint8_t>& compressed);
};

RPCClient::RPCClient(const char* host, uint16_t port) {
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        throw std::runtime_error("Failed to create socket");
    }
    
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        close(sock_fd);
        throw std::runtime_error("Invalid address");
    }
    
    if (connect(sock_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sock_fd);
        throw std::runtime_error("Connection failed");
    }
    
    if (io_uring_queue_init(CLIENT_QUEUE_DEPTH, &ring, 0) < 0) {
        close(sock_fd);
        throw std::runtime_error("Failed to initialize io_uring");
    }
}

RPCClient::~RPCClient() {
    io_uring_queue_exit(&ring);
    close(sock_fd);
}

bool RPCClient::send_request(const RPCHeader& header,
                             const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> buffer(sizeof(RPCHeader) + payload.size());
    
    RPCHeader net_header = header;
    net_header.to_network_order();
    
    memcpy(buffer.data(), &net_header, sizeof(RPCHeader));
    if (!payload.empty()) {
        memcpy(buffer.data() + sizeof(RPCHeader), 
               payload.data(), payload.size());
    }
    
    // Send entire message
    size_t total_sent = 0;
    while (total_sent < buffer.size()) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        io_uring_prep_send(sqe, sock_fd, 
                          buffer.data() + total_sent,
                          buffer.size() - total_sent, 0);
        io_uring_submit(&ring);
        
        io_uring_cqe* cqe;
        if (io_uring_wait_cqe(&ring, &cqe) < 0) {
            return false;
        }
        
        int sent = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        
        if (sent <= 0) {
            return false;
        }
        
        total_sent += sent;
    }
    
    return true;
}

bool RPCClient::receive_response(RPCHeader& header, 
                                 std::vector<uint8_t>& payload) {
    // Receive header
    std::vector<uint8_t> header_buf(sizeof(RPCHeader));
    size_t total_read = 0;
    
    while (total_read < sizeof(RPCHeader)) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        io_uring_prep_recv(sqe, sock_fd,
                          header_buf.data() + total_read,
                          sizeof(RPCHeader) - total_read, 0);
        io_uring_submit(&ring);
        
        io_uring_cqe* cqe;
        if (io_uring_wait_cqe(&ring, &cqe) < 0) {
            return false;
        }
        
        int received = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        
        if (received <= 0) {
            return false;
        }
        
        total_read += received;
    }
    
    memcpy(&header, header_buf.data(), sizeof(RPCHeader));
    header.to_host_order();
    
    if (header.magic != RPC_MAGIC) {
        return false;
    }
    
    // Receive payload if present
    if (header.payload_size > 0) {
        payload.resize(header.payload_size);
        total_read = 0;
        
        while (total_read < header.payload_size) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring);
            io_uring_prep_recv(sqe, sock_fd,
                              payload.data() + total_read,
                              header.payload_size - total_read, 0);
            io_uring_submit(&ring);
            
            io_uring_cqe* cqe;
            if (io_uring_wait_cqe(&ring, &cqe) < 0) {
                return false;
            }
            
            int received = cqe->res;
            io_uring_cqe_seen(&ring, cqe);
            
            if (received <= 0) {
                return false;
            }
            
            total_read += received;
        }
    }
    
    return header.error_code == static_cast<uint32_t>(RPCError::SUCCESS);
}

bool RPCClient::hash_compute(const void* data, size_t size, char* hash_out) {
    RPCHeader req_header;
    req_header.request_id = next_request_id++;
    req_header.operation = static_cast<uint32_t>(RPCOperation::HASH_COMPUTE);
    
    auto payload = HashComputeRequest::serialize(data, size);
    req_header.payload_size = payload.size();
    
    if (!send_request(req_header, payload)) {
        return false;
    }
    
    RPCHeader resp_header;
    std::vector<uint8_t> resp_payload;
    
    if (!receive_response(resp_header, resp_payload)) {
        return false;
    }
    
    return HashComputeResponse::deserialize(resp_payload, hash_out);
}

bool RPCClient::sort_array(int32_t* array, size_t size) {
    RPCHeader req_header;
    req_header.request_id = next_request_id++;
    req_header.operation = static_cast<uint32_t>(RPCOperation::SORT_ARRAY);
    
    auto payload = SortArrayRequest::serialize(array, size);
    req_header.payload_size = payload.size();
    
    if (!send_request(req_header, payload)) {
        return false;
    }
    
    RPCHeader resp_header;
    std::vector<uint8_t> resp_payload;
    
    if (!receive_response(resp_header, resp_payload)) {
        return false;
    }
    
    std::vector<int32_t> sorted;
    if (!SortArrayResponse::deserialize(resp_payload, sorted)) {
        return false;
    }
    
    memcpy(array, sorted.data(), size * sizeof(int32_t));
    return true;
}

bool RPCClient::matrix_multiply(const double* matA, const double* matB,
                                uint32_t n, double* result) {
    RPCHeader req_header;
    req_header.request_id = next_request_id++;
    req_header.operation = static_cast<uint32_t>(RPCOperation::MATRIX_MULTIPLY);
    
    auto payload = MatrixMultiplyRequest::serialize(matA, matB, n);
    req_header.payload_size = payload.size();
    
    if (!send_request(req_header, payload)) {
        return false;
    }
    
    RPCHeader resp_header;
    std::vector<uint8_t> resp_payload;
    
    if (!receive_response(resp_header, resp_payload)) {
        return false;
    }
    
    std::vector<double> result_vec;
    uint32_t result_n;
    
    if (!MatrixMultiplyResponse::deserialize(resp_payload, result_vec, result_n)) {
        return false;
    }
    
    memcpy(result, result_vec.data(), result_n * result_n * sizeof(double));
    return true;
}

bool RPCClient::compress_data(CompressionAlgo algo, const void* data,
                              size_t size, std::vector<uint8_t>& compressed) {
    RPCHeader req_header;
    req_header.request_id = next_request_id++;
    req_header.operation = static_cast<uint32_t>(RPCOperation::COMPRESS_DATA);
    
    auto payload = CompressDataRequest::serialize(algo, data, size);
    req_header.payload_size = payload.size();
    
    if (!send_request(req_header, payload)) {
        return false;
    }
    
    RPCHeader resp_header;
    std::vector<uint8_t> resp_payload;
    
    if (!receive_response(resp_header, resp_payload)) {
        return false;
    }
    
    return CompressDataResponse::deserialize(resp_payload, compressed);
}