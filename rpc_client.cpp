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
#include "rpc_protocol.h"

constexpr int CLIENT_QUEUE_DEPTH = 64;
class RPCClient;

// Callback types
using ResponseCallback = std::function<void(bool success, const RPCHeader&, const std::vector<uint8_t>&)>;
using HashCallback = std::function<void(bool success, const char* hash)>;
using SortCallback = std::function<void(bool success, const std::vector<int32_t>&)>;
using MatrixCallback = std::function<void(bool success, const std::vector<double>&, uint32_t n)>;
using CompressCallback = std::function<void(bool success, const std::vector<uint8_t>&)>;

enum class IOState {
    SEND_HEADER_PAYLOAD,
    RECV_HEADER,
    RECV_PAYLOAD,
    COMPLETE
};

struct RequestContext {
    uint64_t request_id;
    IOState state;
    
    // Send buffers
    std::vector<uint8_t> send_buffer;
    size_t send_offset;
    
    // Receive buffers
    std::vector<uint8_t> recv_header_buf;
    size_t recv_offset;
    std::vector<uint8_t> recv_payload_buf;
    
    RPCHeader response_header;
    ResponseCallback callback;
    
    RequestContext(uint64_t id) 
        : request_id(id), state(IOState::SEND_HEADER_PAYLOAD),
          send_offset(0), recv_header_buf(sizeof(RPCHeader)), recv_offset(0) {}
};

class RPCClient {
private:
    io_uring ring;
    int sock_fd;
    std::atomic<uint64_t> next_request_id{1};
    std::atomic<bool> running{false};
    std::thread io_thread;
    
    std::unordered_map<uint64_t, std::unique_ptr<RequestContext>> pending_requests;
    std::mutex requests_mutex;

    void io_loop();
    void submit_send(RequestContext* ctx);
    void submit_recv_header(RequestContext* ctx);
    void submit_recv_payload(RequestContext* ctx);
    void handle_completion(io_uring_cqe* cqe);
    
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

    running = true;
    io_thread = std::thread(&RPCClient::io_loop, this);

    // Give IO thread time to start
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
        
        // Wait for completion with timeout
        __kernel_timespec ts = {0, 100000000}; // 100ms
        int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
        
        if (ret == -ETIME) {
            continue; // Timeout, check running flag
        }
        
        if (ret < 0) {
            if (running) {
                fprintf(stderr, "io_uring_wait_cqe error: %d\n", ret);
            }
            continue;
        }
        
        handle_completion(cqe);
        io_uring_cqe_seen(&ring, cqe);
    }
}

void RPCClient::submit_send(RequestContext* ctx) {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        std::cerr << "Failed to get SQE\n";
        return;
    }

    size_t remaining = ctx->send_buffer.size() - ctx->send_offset;
    io_uring_prep_send(sqe, sock_fd, 
                      ctx->send_buffer.data() + ctx->send_offset,
                      remaining, 0);

    io_uring_sqe_set_data(sqe, ctx);
    io_uring_submit(&ring);
}

void RPCClient::submit_recv_header(RequestContext* ctx) {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        std::cerr << "Failed to get SQE\n";
        return;
    }

    size_t remaining = sizeof(RPCHeader) - ctx->recv_offset;
    io_uring_prep_recv(sqe, sock_fd,
                       ctx->recv_header_buf.data() + ctx->recv_offset,
                       remaining, 0);
                    
    io_uring_sqe_set_data(sqe, ctx);
    io_uring_submit(&ring);
}

void RPCClient::submit_recv_payload(RequestContext* ctx) {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        std::cerr << "Failed to get SQE\n";
        return;
    }

    size_t remaining = ctx->recv_payload_buf.size() - ctx->recv_offset;
    void* buf_ptr = ctx->recv_payload_buf.data() + ctx->recv_offset;
    
    io_uring_prep_recv(sqe, sock_fd, buf_ptr, remaining, 0);
    io_uring_sqe_set_data(sqe, ctx);
    io_uring_submit(&ring);
}

void RPCClient::handle_completion(io_uring_cqe* cqe) {
    RequestContext* ctx = static_cast<RequestContext*>(io_uring_cqe_get_data(cqe));
    if (!ctx) return;

    int res = cqe->res;
    if (res <= 0) {
        // Error or connection was closed
        ctx->callback(false, ctx->response_header, {});
        std::lock_guard<std::mutex> lock(requests_mutex);
        pending_requests.erase(ctx->request_id);
        return;
    } 

    switch (ctx->state) {
        case IOState::SEND_HEADER_PAYLOAD:
            ctx->send_offset += res;
            if (ctx->send_offset < ctx->send_buffer.size()) {
                // Continue sending
                submit_send(ctx);
            } else {
                // Send completed, starting receiving header
                ctx->state = IOState::RECV_HEADER;
                ctx->recv_offset = 0;
                submit_recv_header(ctx);
            }
            break;

        case IOState::RECV_HEADER:
            ctx->recv_offset += res;
            if (ctx->recv_offset < sizeof(RPCHeader)) {
                // Still receiving header
                submit_recv_header(ctx);
            } else {
                // Header succesfully received, now parsing...
                memcpy(&ctx->response_header, ctx->recv_header_buf.data(), sizeof(RPCHeader));
                ctx->response_header.to_host_order();

                if (ctx->response_header.magic != RPC_MAGIC) {
                    RPCHeader header_copy = ctx->response_header;
                    uint64_t req_id = ctx->request_id;
                    
                    {
                        std::lock_guard<std::mutex> lock(requests_mutex);
                        auto callback_copy = std::move(ctx->callback);
                        pending_requests.erase(req_id);
                        callback_copy(false, header_copy, {});
                    }
                    return;
                }

                if (ctx->response_header.payload_size > 0) {
                    // Receiving payload...
                    ctx->state = IOState::RECV_PAYLOAD;
                    ctx->recv_offset = 0;
                    ctx->recv_payload_buf.resize(ctx->response_header.payload_size);
                    submit_recv_payload(ctx);
                } else {
                    // Finished receiving payload
                    ctx->state = IOState::COMPLETE;
                    bool success = (ctx->response_header.error_code == 
                                    static_cast<uint32_t>(RPCError::SUCCESS));

                    RPCHeader header_copy = ctx->response_header;
                    uint64_t req_id = ctx->request_id;
                    
                    {
                        std::lock_guard<std::mutex> lock(requests_mutex);
                        auto callback_copy = std::move(ctx->callback);
                        pending_requests.erase(req_id);
                        callback_copy(success, header_copy, {});
                    }
                }
            }
            break;

        case IOState::RECV_PAYLOAD:
            ctx->recv_offset += res;

            if (ctx->recv_offset < ctx->recv_payload_buf.size()) {
                // Continue receiving payload
                submit_recv_payload(ctx);
            } else {
                // Payload complete
                ctx->state = IOState::COMPLETE;
                bool success = (ctx->response_header.error_code ==
                                static_cast<uint32_t>(RPCError::SUCCESS));
                
                // Copy data before erasing context 
                RPCHeader header_copy = ctx->response_header;
                std::vector<uint8_t> payload_copy = ctx->recv_payload_buf;
                uint64_t req_id = ctx->request_id;

                // Invoke callback AFTER removing from map to avoid use-after-free
                {
                    std::lock_guard<std::mutex> lock(requests_mutex);
                    auto callback_copy = std::move(ctx->callback);
                    pending_requests.erase(req_id);
                    
                    // Now invoke with copied data
                    callback_copy(success, header_copy, payload_copy);
                }
            }
            break;

        case IOState::COMPLETE:
            // Should not happen
            break;
    }
}

void RPCClient::async_request(const RPCHeader& header, 
                              const std::vector<uint8_t>& payload,
                              ResponseCallback callback) 
    {
        auto ctx = std::make_unique<RequestContext>(header.request_id);
        ctx->callback = std::move(callback);

        // Send buffer
        ctx->send_buffer.resize(sizeof(RPCHeader) + payload.size());

        RPCHeader net_header = header;
        net_header.to_network_order();

        memcpy(ctx->send_buffer.data(), &net_header, sizeof(RPCHeader));
        if (!payload.empty()) {
            memcpy(ctx->send_buffer.data() + sizeof(RPCHeader), payload.data(),
                    payload.size());                
        }

        RequestContext* ctx_ptr = ctx.get();
        
        // Explain this line cause wth
        {
            std::lock_guard<std::mutex> lock(requests_mutex);
            pending_requests[header.request_id] = std::move(ctx);
        }

        submit_send(ctx_ptr);
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
        [callback](bool success, const RPCHeader& resp_header, 
        const std::vector<uint8_t>& resp_payload) {
            if (!success) {
                callback(false, nullptr);
                return;
            }

            // Allocate on stack - callback happens synchronously before lambda exits
            char hash[65] = {0};
            bool deser_result = HashComputeResponse::deserialize(resp_payload, hash);
            
            if (deser_result) {
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
        [callback](bool success, const RPCHeader& resp_header, 
        const std::vector<uint8_t>& resp_payload) {
            if (!success) {
                callback(false, {});
                return;
            }

            std::vector<int32_t> sorted;
            if (SortArrayRequest::deserialize(resp_payload, sorted)) {
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
        [callback](bool success, const RPCHeader& resp_header, 
        const std::vector<uint8_t>& resp_payload) {
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
        [callback](bool success, const RPCHeader& resp_header, 
        const std::vector<uint8_t>& resp_payload) {
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

bool RPCClient::hash_compute(const void* data, size_t size, char* hash_out) {
    std::mutex mtx;
    std:: condition_variable cv;
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
    std:: condition_variable cv;
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
    std:: condition_variable cv;
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