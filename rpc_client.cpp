#include <liburing.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <functional>
#include <unordered_map>
#include <memory>
#include <thread>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <queue>
#include "rpc_protocol.h"

constexpr int CLIENT_QUEUE_DEPTH = 256;

// Forward declarations
class RPCClient;

// Callback types
using ResponseCallback = std::function<void(bool success, const RPCHeader&, const std::vector<uint8_t>&)>;
using HashCallback = std::function<void(bool success, const char* hash)>;
using SortCallback = std::function<void(bool success, const std::vector<int32_t>&)>;
using MatrixCallback = std::function<void(bool success, const std::vector<double>&, uint32_t n)>;
using CompressCallback = std::function<void(bool success, const std::vector<uint8_t>&)>;

// Pending request info - stores callback, doesn't do IO
struct PendingRequest {
    uint64_t request_id;
    ResponseCallback callback;
    
    PendingRequest(uint64_t id, ResponseCallback cb)
        : request_id(id), callback(std::move(cb)) {}
};

// Receiver state - handles reading from socket
enum class RecvState {
    READING_HEADER,
    READING_PAYLOAD
};

struct ReceiverContext {
    RecvState state = RecvState::READING_HEADER;
    std::vector<uint8_t> header_buf;
    RPCHeader current_header;
    std::vector<uint8_t> payload_buf;
    size_t bytes_read = 0;
    
    ReceiverContext() : header_buf(sizeof(RPCHeader)) {}
};

enum class OpType {
    SEND,
    RECV
};

class RPCClient {
private:
    io_uring ring;
    int sock_fd;
    std::atomic<uint64_t> next_request_id{1};
    std::atomic<bool> running{false};
    std::thread io_thread;
    
    // Map of request_id -> callback
    std::unordered_map<uint64_t, std::unique_ptr<PendingRequest>> pending_requests;
    std::mutex requests_mutex;
    
    // Send queue with flow control
    std::queue<std::vector<uint8_t>> send_queue;
    std::mutex send_mutex;
    bool send_in_progress = false;
    std::vector<uint8_t> current_send_buffer;
    size_t send_offset = 0;
    
    // Flow control: only send if we're not waiting for too many responses
    std::atomic<int> in_flight_requests{0};
    static constexpr int MAX_IN_FLIGHT = 100;  // Only 100 request in flight for now
    
    // Single receiver for the socket
    ReceiverContext receiver;
    
    // OpType markers - NOT static, instance members
    OpType send_op_marker = OpType::SEND;
    OpType recv_op_marker = OpType::RECV;
    
    void io_loop();
    void submit_send();
    void submit_recv();
    void handle_send_completion(int bytes_sent);
    void handle_recv_completion(int bytes_received);
    void dispatch_response(const RPCHeader& header, const std::vector<uint8_t>& payload);
    
    void async_request(const RPCHeader& header, 
                      const std::vector<uint8_t>& payload,
                      ResponseCallback callback);
    
public:
    RPCClient(const char* host, uint16_t port);
    ~RPCClient();
    
    // Async API with callbacks
    void hash_compute_async(const void* data, size_t size, HashCallback callback);
    void sort_array_async(const int32_t* array, size_t size, SortCallback callback);
    void matrix_multiply_async(const double* matA, const double* matB, 
                              uint32_t n, MatrixCallback callback);
    void compress_data_async(CompressionAlgo algo, const void* data,
                           size_t size, CompressCallback callback);
    
    // Synchronous wrappers
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
    
    // fprintf(stderr, "[CLIENT] Socket created: %d\n", sock_fd);
    
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
    
    // fprintf(stderr, "[CLIENT] Connected to server\n");
    
    if (io_uring_queue_init(CLIENT_QUEUE_DEPTH, &ring, 0) < 0) {
        close(sock_fd);
        throw std::runtime_error("Failed to initialize io_uring");
    }
    
    // fprintf(stderr, "[CLIENT] io_uring initialized\n");
    
    running = true;
    io_thread = std::thread(&RPCClient::io_loop, this);
    
    // fprintf(stderr, "[CLIENT] IO thread started\n");
    
    // Start receiving immediately
    submit_recv();
    
    // fprintf(stderr, "[CLIENT] Initial recv submitted\n");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

RPCClient::~RPCClient() {
    running = false;
    if (io_thread.joinable()) {
        io_thread.join();
    }
    io_uring_queue_exit(&ring);
    close(sock_fd);
}

void RPCClient::io_loop() {
    while (running) {
        io_uring_cqe* cqe;
        
        __kernel_timespec ts = {0, 100000000}; // 100ms
        int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
        
        if (ret == -ETIME) {
            continue;
        }
        
        if (ret < 0) {
            if (running) {
                fprintf(stderr, "io_uring_wait_cqe error: %d\n", ret);
            }
            continue;
        }
        
        // Process ALL available completions
        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ring, head, cqe) {
            OpType* op_type = static_cast<OpType*>(io_uring_cqe_get_data(cqe));
            int res = cqe->res;
            
            if (res <= 0) {
                fprintf(stderr, "[CLIENT-%p] IO error: %d\n", this, res);
            } else {
                if (*op_type == OpType::SEND) {
                    handle_send_completion(res);
                } else {
                    handle_recv_completion(res);
                }
            }
            count++;
        }
        
        io_uring_cq_advance(&ring, count);
        
        if (count > 1) {
            fprintf(stderr, "[CLIENT-%p] Processed %u completions in batch\n", this, count);
        }
    }
}

void RPCClient::submit_send() {
    std::lock_guard<std::mutex> lock(send_mutex);
    
    // Flow control: don't send if too many in flight
    if (in_flight_requests.load() >= MAX_IN_FLIGHT) {
        fprintf(stderr, "[CLIENT] submit_send: too many in flight (%d), waiting\n", 
                in_flight_requests.load());
        return;
    }
    
    if (send_in_progress || send_queue.empty()) {
        return;
    }
    
    current_send_buffer = std::move(send_queue.front());
    send_queue.pop();
    send_offset = 0;
    send_in_progress = true;
    
    // fprintf(stderr, "[CLIENT] submit_send: sending %zu bytes\n", current_send_buffer.size());
    
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "Failed to get SQE for send\n");
        return;
    }
    
    io_uring_prep_send(sqe, sock_fd,
                      current_send_buffer.data(),
                      current_send_buffer.size(), 0);
    io_uring_sqe_set_data(sqe, &send_op_marker);
    io_uring_submit(&ring);
    
    // fprintf(stderr, "[CLIENT] submit_send: submitted\n");
}

void RPCClient::submit_recv() {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "Failed to get SQE for recv\n");
        return;
    }
    
    if (receiver.state == RecvState::READING_HEADER) {
        size_t remaining = sizeof(RPCHeader) - receiver.bytes_read;
        // fprintf(stderr, "[CLIENT] submit_recv: header, %zu bytes remaining\n", remaining);
        io_uring_prep_recv(sqe, sock_fd,
                          receiver.header_buf.data() + receiver.bytes_read,
                          remaining, 0);
    } else {
        size_t remaining = receiver.payload_buf.size() - receiver.bytes_read;
        // fprintf(stderr, "[CLIENT] submit_recv: payload, %zu bytes remaining\n", remaining);
        io_uring_prep_recv(sqe, sock_fd,
                          receiver.payload_buf.data() + receiver.bytes_read,
                          remaining, 0);
    }
    
    io_uring_sqe_set_data(sqe, &recv_op_marker);
    io_uring_submit(&ring);
}

void RPCClient::handle_send_completion(int bytes_sent) {
    fprintf(stderr, "[CLIENT-%p] handle_send_completion: %d bytes\n", this, bytes_sent);
    bool should_send_more = false;

    
    std::lock_guard<std::mutex> lock(send_mutex);
    
    send_offset += bytes_sent;
    
    if (send_offset < current_send_buffer.size()) {
        // More to send
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (!sqe) {
            fprintf(stderr, "Failed to get SQE for continued send\n");
            return;
        }
        
        io_uring_prep_send(sqe, sock_fd,
                          current_send_buffer.data() + send_offset,
                          current_send_buffer.size() - send_offset, 0);
        io_uring_sqe_set_data(sqe, &send_op_marker);
        io_uring_submit(&ring);
    } else {
        // Send complete - increment in-flight counter
        fprintf(stderr, "[CLIENT-%p] Send complete\n", this);

        
        send_in_progress = false;
        current_send_buffer.clear();
        should_send_more = !send_queue.empty();
    }
    if (should_send_more) {
        submit_send();
    }
}

void RPCClient::handle_recv_completion(int bytes_received) {
    fprintf(stderr, "[CLIENT] handle_recv_completion: %d bytes, state=%d\n", 
            bytes_received, (int)receiver.state);
    
    receiver.bytes_read += bytes_received;
    
    if (receiver.state == RecvState::READING_HEADER) {
        // fprintf(stderr, "[CLIENT] Reading header: %zu/%zu bytes\n", 
        //         receiver.bytes_read, sizeof(RPCHeader));
        
        if (receiver.bytes_read < sizeof(RPCHeader)) {
            // Need more header bytes
            submit_recv();
            return;
        }
        
        // Complete header received
        memcpy(&receiver.current_header, receiver.header_buf.data(), sizeof(RPCHeader));
        receiver.current_header.to_host_order();
        
        // fprintf(stderr, "[CLIENT] Header complete: magic=0x%08x, req_id=%lu, payload_size=%u\n",
        //         receiver.current_header.magic, receiver.current_header.request_id,
        //         receiver.current_header.payload_size);
        
        if (receiver.current_header.magic != RPC_MAGIC) {
            // fprintf(stderr, "Invalid magic: 0x%08x\n", receiver.current_header.magic);
            // Dispatch error to all pending requests
            std::lock_guard<std::mutex> lock(requests_mutex);
            for (auto& [id, req] : pending_requests) {
                req->callback(false, receiver.current_header, {});
            }
            pending_requests.clear();
            return;
        }
        
        if (receiver.current_header.payload_size > 0) {
            // Start reading payload
            receiver.state = RecvState::READING_PAYLOAD;
            receiver.payload_buf.resize(receiver.current_header.payload_size);
            receiver.bytes_read = 0;
            // fprintf(stderr, "[CLIENT] Starting payload read: %u bytes\n", 
                    // receiver.current_header.payload_size);
            submit_recv();
        } else {
            // No payload, dispatch immediately
            // fprintf(stderr, "[CLIENT] No payload, dispatching\n");
            dispatch_response(receiver.current_header, {});
            
            // Reset for next response
            receiver.state = RecvState::READING_HEADER;
            receiver.bytes_read = 0;
            submit_recv();
        }
    } else {
        // Reading payload
        // fprintf(stderr, "[CLIENT] Reading payload: %zu/%zu bytes\n",
                // receiver.bytes_read, receiver.payload_buf.size());
        
        if (receiver.bytes_read < receiver.payload_buf.size()) {
            // Need more payload bytes
            submit_recv();
            return;
        }
        
        // Complete response received
        // fprintf(stderr, "[CLIENT] Payload complete, dispatching\n");
        dispatch_response(receiver.current_header, receiver.payload_buf);
        
        // Reset for next response
        receiver.state = RecvState::READING_HEADER;
        receiver.bytes_read = 0;
        receiver.payload_buf.clear();
        submit_recv();
    }
}

void RPCClient::dispatch_response(const RPCHeader& header, const std::vector<uint8_t>& payload) {
    std::unique_ptr<PendingRequest> req;
    
    {
        std::lock_guard<std::mutex> lock(requests_mutex);
        auto it = pending_requests.find(header.request_id);
        
        if (it == pending_requests.end()) {
            fprintf(stderr, "[CLIENT-%p] Received response for unknown request_id: %lu\n", 
                    this, header.request_id);
            return;
        }
        
        req = std::move(it->second);
        pending_requests.erase(it);
    }
    
    // Decrement in-flight counter
    int prev = in_flight_requests.fetch_sub(1);
    fprintf(stderr, "[CLIENT-%p] Response dispatched for req_id=%lu, in_flight: %d -> %d\n",
            this, header.request_id, prev, prev - 1);
    
    // Try to send next queued request
    submit_send();
    
    // Invoke callback outside lock
    bool success = (header.error_code == static_cast<uint32_t>(RPCError::SUCCESS));
    req->callback(success, header, payload);
}

void RPCClient::async_request(const RPCHeader& header,
                              const std::vector<uint8_t>& payload,
                              ResponseCallback callback) {
    
    // Register callback
    fprintf(stderr, "[CLIENT-%p] async_request: req_id=%lu\n", this, header.request_id);

    {
        std::lock_guard<std::mutex> lock(requests_mutex);
        pending_requests[header.request_id] = std::make_unique<PendingRequest>(header.request_id, std::move(callback));
    }
    in_flight_requests++;
    fprintf(stderr, "[CLIENT-%p] in_flight: %d\n", this, in_flight_requests.load());


    // Prepare send buffer
    std::vector<uint8_t> send_buf(sizeof(RPCHeader) + payload.size());
    
    RPCHeader net_header = header;
    net_header.to_network_order();
    
    memcpy(send_buf.data(), &net_header, sizeof(RPCHeader));
    if (!payload.empty()) {
        memcpy(send_buf.data() + sizeof(RPCHeader), payload.data(), payload.size());
    }
    
    // fprintf(stderr, "[CLIENT] Queuing send buffer: %zu bytes\n", send_buf.size());
    
    // Queue for sending
    {
        std::lock_guard<std::mutex> lock(send_mutex);
        send_queue.push(std::move(send_buf));
        // fprintf(stderr, "[CLIENT] Send queue size: %zu\n", send_queue.size());
    }
    
    // Kick off send if not in progress
    submit_send();
}

void RPCClient::hash_compute_async(const void* data, size_t size, HashCallback callback) {
    RPCHeader header;
    header.magic = RPC_MAGIC;
    header.request_id = next_request_id++;
    header.operation = static_cast<uint32_t>(RPCOperation::HASH_COMPUTE);
    header.error_code = 0;
    
    auto payload = HashComputeRequest::serialize(data, size);
    header.payload_size = payload.size();
    
    async_request(header, payload,
        [callback](bool success, const RPCHeader&, const std::vector<uint8_t>& resp_payload) {
            if (!success) {
                callback(false, nullptr);
                return;
            }
            
            char hash[65] = {0};
            if (HashComputeResponse::deserialize(resp_payload, hash)) {
                callback(true, hash);
            } else {
                callback(false, nullptr);
            }
        });
}

void RPCClient::sort_array_async(const int32_t* array, size_t size, SortCallback callback) {
    RPCHeader header;
    header.magic = RPC_MAGIC;
    header.request_id = next_request_id++;
    header.operation = static_cast<uint32_t>(RPCOperation::SORT_ARRAY);
    header.error_code = 0;
    
    auto payload = SortArrayRequest::serialize(array, size);
    header.payload_size = payload.size();
    
    async_request(header, payload,
        [callback](bool success, const RPCHeader&, const std::vector<uint8_t>& resp_payload) {
            if (!success) {
                callback(false, {});
                return;
            }
            
            std::vector<int32_t> sorted;
            if (SortArrayResponse::deserialize(resp_payload, sorted)) {
                callback(true, sorted);
            } else {
                callback(false, {});
            }
        });
}

void RPCClient::matrix_multiply_async(const double* matA, const double* matB,
                                      uint32_t n, MatrixCallback callback) {
    RPCHeader header;
    header.magic = RPC_MAGIC;
    header.request_id = next_request_id++;
    header.operation = static_cast<uint32_t>(RPCOperation::MATRIX_MULTIPLY);
    header.error_code = 0;
    
    auto payload = MatrixMultiplyRequest::serialize(matA, matB, n);
    header.payload_size = payload.size();
    
    async_request(header, payload,
        [callback](bool success, const RPCHeader&, const std::vector<uint8_t>& resp_payload) {
            if (!success) {
                callback(false, {}, 0);
                return;
            }
            
            std::vector<double> result;
            uint32_t result_n;
            if (MatrixMultiplyResponse::deserialize(resp_payload, result, result_n)) {
                callback(true, result, result_n);
            } else {
                callback(false, {}, 0);
            }
        });
}

void RPCClient::compress_data_async(CompressionAlgo algo, const void* data,
                                    size_t size, CompressCallback callback) {
    RPCHeader header;
    header.magic = RPC_MAGIC;
    header.request_id = next_request_id++;
    header.operation = static_cast<uint32_t>(RPCOperation::COMPRESS_DATA);
    header.error_code = 0;
    
    auto payload = CompressDataRequest::serialize(algo, data, size);
    header.payload_size = payload.size();
    
    async_request(header, payload,
        [callback](bool success, const RPCHeader&, const std::vector<uint8_t>& resp_payload) {
            if (!success) {
                callback(false, {});
                return;
            }
            
            std::vector<uint8_t> compressed;
            if (CompressDataResponse::deserialize(resp_payload, compressed)) {
                callback(true, compressed);
            } else {
                callback(false, {});
            }
        });
}

// Synchronous wrappers
bool RPCClient::hash_compute(const void* data, size_t size, char* hash_out) {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool result = false;
    char hash_buffer[65] = {0};
    
    hash_compute_async(data, size, [&](bool success, const char* hash) {
        std::lock_guard<std::mutex> lock(mtx);
        result = success;
        if (success && hash) {
            memcpy(hash_buffer, hash, 65);
        }
        done = true;
        cv.notify_one();
    });
    
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&] { return done; });
    
    if (result && hash_out) {
        memcpy(hash_out, hash_buffer, 65);
    }
    return result;
}

bool RPCClient::sort_array(int32_t* array, size_t size) {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool result = false;
    
    sort_array_async(array, size, [&](bool success, const std::vector<int32_t>& sorted) {
        std::lock_guard<std::mutex> lock(mtx);
        result = success;
        if (success) {
            memcpy(array, sorted.data(), size * sizeof(int32_t));
        }
        done = true;
        cv.notify_one();
    });
    
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&] { return done; });
    return result;
}

bool RPCClient::matrix_multiply(const double* matA, const double* matB,
                                uint32_t n, double* result) {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool success_flag = false;
    
    matrix_multiply_async(matA, matB, n, [&](bool success, const std::vector<double>& res, uint32_t result_n) {
        std::lock_guard<std::mutex> lock(mtx);
        success_flag = success;
        if (success) {
            memcpy(result, res.data(), result_n * result_n * sizeof(double));
        }
        done = true;
        cv.notify_one();
    });
    
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&] { return done; });
    return success_flag;
}

bool RPCClient::compress_data(CompressionAlgo algo, const void* data,
                              size_t size, std::vector<uint8_t>& compressed) {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    bool result = false;
    
    compress_data_async(algo, data, size, [&](bool success, const std::vector<uint8_t>& comp) {
        std::lock_guard<std::mutex> lock(mtx);
        result = success;
        if (success) {
            compressed = comp;
        }
        done = true;
        cv.notify_one();
    });
    
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&] { return done; });
    return result;
}