#include <liburing.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <map>
#include <queue>
#include <iostream>
#include <openssl/sha.h>
#include <zlib.h>
#include <algorithm>
#include "rpc_protocol.h"

constexpr int QUEUE_DEPTH = 1024;
constexpr int BACKLOG = 1024;

enum class EventType {
    ACCEPT,
    READ,
    WRITE
};

// Store a pending request
struct PendingRequest {
    RPCHeader header;
    std::vector<uint8_t> payload;
};

struct Connection {
    int fd;
    std::vector<uint8_t> read_buffer;
    std::vector<uint8_t> write_buffer;
    size_t bytes_read;
    size_t bytes_to_write;
    size_t bytes_written;
    bool header_read;
    RPCHeader current_header;
    
    // Queue of requests to process
    std::queue<PendingRequest> request_queue;
    bool write_in_progress = false;
};

struct RequestContext {
    EventType type;
    Connection* conn;
    
    RequestContext(EventType t, Connection* c = nullptr) 
        : type(t), conn(c) {}
};

class RPCServer {
private:
    io_uring ring;
    int listen_fd;
    std::map<int, Connection*> connections;
    
    void process_request(Connection* conn, const PendingRequest& req);
    void handle_hash_compute(Connection* conn, const RPCHeader& header, 
                            const std::vector<uint8_t>& payload);
    void handle_sort_array(Connection* conn, const RPCHeader& header,
                          const std::vector<uint8_t>& payload);
    void handle_matrix_multiply(Connection* conn, const RPCHeader& header,
                               const std::vector<uint8_t>& payload);
    void handle_compress_data(Connection* conn, const RPCHeader& header,
                             const std::vector<uint8_t>& payload);
    
    void send_response(Connection* conn, uint64_t req_id, uint32_t operation,
                      const std::vector<uint8_t>& payload, RPCError error);
    void add_accept(io_uring* ring, int listen_fd);
    void add_read(io_uring* ring, Connection* conn);
    void add_write(io_uring* ring, Connection* conn);
    
public:
    RPCServer(uint16_t port);
    ~RPCServer();
    
    void run();
    void stop();
};

RPCServer::RPCServer(uint16_t port) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        throw std::runtime_error("Failed to create socket");
    }
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        throw std::runtime_error("Failed to bind socket");
    }
    
    if (listen(listen_fd, BACKLOG) < 0) {
        throw std::runtime_error("Failed to listen on socket");
    }
    
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        throw std::runtime_error("Failed to initialize io_uring");
    }
    
    std::cout << "RPC Server listening on port " << port << std::endl;
}

RPCServer::~RPCServer() {
    for (auto& [fd, conn] : connections) {
        close(fd);
        delete conn;
    }
    close(listen_fd);
    io_uring_queue_exit(&ring);
}

void RPCServer::add_accept(io_uring* ring, int listen_fd) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        fprintf(stderr, "[SERVER] Failed to get SQE for read, submitting first\n");
        io_uring_submit(ring);  // Submit pending operations
        sqe = io_uring_get_sqe(ring);  // Try again
        if (!sqe) {
            fprintf(stderr, "[SERVER] Still can't get SQE, dropping read!\n");
            return;
        }
    }
    io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, 0);
    
    auto* ctx = new RequestContext(EventType::ACCEPT);
    io_uring_sqe_set_data(sqe, ctx);
}

void RPCServer::add_read(io_uring* ring, Connection* conn) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);

    if (!sqe) {
        fprintf(stderr, "[SERVER] Failed to get SQE for read, submitting first\n");
        io_uring_submit(ring);  // Submit pending operations
        sqe = io_uring_get_sqe(ring);  // Try again
        if (!sqe) {
            fprintf(stderr, "[SERVER] Still can't get SQE, dropping read!\n");
            return;
        }
    }
    
    if (!conn->header_read) {
        conn->read_buffer.resize(sizeof(RPCHeader));
        io_uring_prep_read(sqe, conn->fd, conn->read_buffer.data(), 
                          sizeof(RPCHeader), 0);
    } else {
        size_t remaining = conn->current_header.payload_size - conn->bytes_read;
        io_uring_prep_read(sqe, conn->fd, 
                          conn->read_buffer.data() + conn->bytes_read,
                          remaining, 0);
    }
    
    auto* ctx = new RequestContext(EventType::READ, conn);
    io_uring_sqe_set_data(sqe, ctx);
}

void RPCServer::add_write(io_uring* ring, Connection* conn) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);

    if (!sqe) {
        fprintf(stderr, "[SERVER] Failed to get SQE for read, submitting first\n");
        io_uring_submit(ring);  // Submit pending operations
        sqe = io_uring_get_sqe(ring);  // Try again
        if (!sqe) {
            fprintf(stderr, "[SERVER] Still can't get SQE, dropping read!\n");
            return;
        }
    }
    
    size_t remaining = conn->bytes_to_write - conn->bytes_written;
    io_uring_prep_write(sqe, conn->fd,
                       conn->write_buffer.data() + conn->bytes_written,
                       remaining, 0);
    
    auto* ctx = new RequestContext(EventType::WRITE, conn);
    io_uring_sqe_set_data(sqe, ctx);
}

void RPCServer::process_request(Connection* conn, const PendingRequest& req) {
    auto op = static_cast<RPCOperation>(req.header.operation);
    
    switch (op) {
    case RPCOperation::HASH_COMPUTE:
        handle_hash_compute(conn, req.header, req.payload);
        break;
    case RPCOperation::SORT_ARRAY:
        handle_sort_array(conn, req.header, req.payload);
        break;
    case RPCOperation::MATRIX_MULTIPLY:
        handle_matrix_multiply(conn, req.header, req.payload);
        break;
    case RPCOperation::COMPRESS_DATA:
        handle_compress_data(conn, req.header, req.payload);
        break;
    default:
        send_response(conn, req.header.request_id, 0, {}, RPCError::INVALID_OPERATION);
    }
}

void RPCServer::handle_hash_compute(Connection* conn, const RPCHeader& header,
                                    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> data;
    if (!HashComputeRequest::deserialize(payload, data)) {
        send_response(conn, header.request_id, header.operation, {}, RPCError::INVALID_PARAMS);
        return;
    }
    
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);
    
    char hex_hash[65];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hex_hash + i * 2, "%02x", hash[i]);
    }
    hex_hash[64] = '\0';
    
    auto resp_payload = HashComputeResponse::serialize(hex_hash);
    send_response(conn, header.request_id, header.operation, resp_payload, RPCError::SUCCESS);
}

void RPCServer::handle_sort_array(Connection* conn, const RPCHeader& header,
                                  const std::vector<uint8_t>& payload) {
    std::vector<int32_t> array;
    if (!SortArrayRequest::deserialize(payload, array)) {
        send_response(conn, header.request_id, header.operation, {}, RPCError::INVALID_PARAMS);
        return;
    }
    
    std::sort(array.begin(), array.end());
    
    auto resp_payload = SortArrayResponse::serialize(array.data(), array.size());
    send_response(conn, header.request_id, header.operation, resp_payload, RPCError::SUCCESS);
}

void RPCServer::handle_matrix_multiply(Connection* conn, const RPCHeader& header,
                                       const std::vector<uint8_t>& payload) {
    std::vector<double> matA, matB;
    uint32_t n;
    
    if (!MatrixMultiplyRequest::deserialize(payload, matA, matB, n)) {
        send_response(conn, header.request_id, header.operation, {}, RPCError::INVALID_PARAMS);
        return;
    }
    
    std::vector<double> result(n * n, 0.0);
    
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < n; j++) {
            for (uint32_t k = 0; k < n; k++) {
                result[i * n + j] += matA[i * n + k] * matB[k * n + j];
            }
        }
    }
    
    auto resp_payload = MatrixMultiplyResponse::serialize(result.data(), n);
    send_response(conn, header.request_id, header.operation, resp_payload, RPCError::SUCCESS);
}

void RPCServer::handle_compress_data(Connection* conn, const RPCHeader& header,
                                     const std::vector<uint8_t>& payload) {
    CompressionAlgo algo;
    std::vector<uint8_t> data;
    
    if (!CompressDataRequest::deserialize(payload, algo, data)) {
        send_response(conn, header.request_id, header.operation, {}, RPCError::INVALID_PARAMS);
        return;
    }
    
    std::vector<uint8_t> compressed;
    
    if (algo == CompressionAlgo::ZLIB) {
        uLongf compressed_size = compressBound(data.size());
        compressed.resize(compressed_size);
        
        if (compress(compressed.data(), &compressed_size,
                    data.data(), data.size()) != Z_OK) {
            send_response(conn, header.request_id, header.operation, {}, RPCError::COMPUTATION_ERROR);
            return;
        }
        compressed.resize(compressed_size);
    }
    
    auto resp_payload = CompressDataResponse::serialize(compressed.data(), compressed.size());
    send_response(conn, header.request_id, header.operation, resp_payload, RPCError::SUCCESS);
}

void RPCServer::send_response(Connection* conn, uint64_t req_id, uint32_t operation,
                              const std::vector<uint8_t>& payload, RPCError error) {
    RPCHeader resp_header;
    resp_header.magic = RPC_MAGIC;
    resp_header.request_id = req_id;
    resp_header.operation = operation;
    resp_header.payload_size = payload.size();
    resp_header.error_code = static_cast<uint32_t>(error);
    
    resp_header.to_network_order();
    
    conn->write_buffer.resize(sizeof(RPCHeader) + payload.size());
    memcpy(conn->write_buffer.data(), &resp_header, sizeof(RPCHeader));
    if (!payload.empty()) {
        memcpy(conn->write_buffer.data() + sizeof(RPCHeader), 
               payload.data(), payload.size());
    }
    
    conn->bytes_to_write = conn->write_buffer.size();
    conn->bytes_written = 0;
    conn->write_in_progress = true;
    
    add_write(&ring, conn);
}

void RPCServer::run() {
    add_accept(&ring, listen_fd);
    io_uring_submit(&ring);
    
    while (true) {
        io_uring_cqe* cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        
        if (ret < 0) {
            std::cerr << "Error waiting for completion: " << strerror(-ret) << std::endl;
            continue;
        }
        
        auto* ctx = static_cast<RequestContext*>(io_uring_cqe_get_data(cqe));
        
        if (ctx->type == EventType::ACCEPT) {
            int client_fd = cqe->res;
            if (client_fd >= 0) {
                auto* conn = new Connection();
                conn->fd = client_fd;
                conn->bytes_read = 0;
                conn->header_read = false;
                connections[client_fd] = conn;
                
                add_read(&ring, conn);
                add_accept(&ring, listen_fd);
            }
        } else if (ctx->type == EventType::READ) {
            int bytes_read = cqe->res;
            
            if (bytes_read <= 0) {
                close(ctx->conn->fd);
                connections.erase(ctx->conn->fd);
                delete ctx->conn;
            } else {
                if (!ctx->conn->header_read) {
                    ctx->conn->bytes_read += bytes_read;
                    if (ctx->conn->bytes_read == sizeof(RPCHeader)) {
                        memcpy(&ctx->conn->current_header, 
                               ctx->conn->read_buffer.data(),
                               sizeof(RPCHeader));
                        ctx->conn->current_header.to_host_order();
                        
                        if (ctx->conn->current_header.magic != RPC_MAGIC) {
                            close(ctx->conn->fd);
                            connections.erase(ctx->conn->fd);
                            delete ctx->conn;
                        } else {
                            ctx->conn->header_read = true;
                            ctx->conn->bytes_read = 0;
                            ctx->conn->read_buffer.resize(
                                ctx->conn->current_header.payload_size);
                            
                            if (ctx->conn->current_header.payload_size == 0) {
                                // No payload, queue immediately
                                PendingRequest req;
                                req.header = ctx->conn->current_header;
                                ctx->conn->request_queue.push(std::move(req));
                                
                                ctx->conn->header_read = false;
                                ctx->conn->bytes_read = 0;
                                
                                // Process if not writing
                                if (!ctx->conn->write_in_progress && !ctx->conn->request_queue.empty()) {
                                    auto pending = std::move(ctx->conn->request_queue.front());
                                    ctx->conn->request_queue.pop();
                                    process_request(ctx->conn, pending);
                                }
                                
                                add_read(&ring, ctx->conn);
                            } else {
                                add_read(&ring, ctx->conn);
                            }
                        }
                    } else {
                        add_read(&ring, ctx->conn);
                    }
                } else {
                    ctx->conn->bytes_read += bytes_read;
                    if (ctx->conn->bytes_read == ctx->conn->current_header.payload_size) {
                        // Complete request received, queue it
                        PendingRequest req;
                        req.header = ctx->conn->current_header;
                        req.payload = ctx->conn->read_buffer;
                        ctx->conn->request_queue.push(std::move(req));
                        
                        ctx->conn->header_read = false;
                        ctx->conn->bytes_read = 0;
                        
                        // Process if not writing
                        if (!ctx->conn->write_in_progress && !ctx->conn->request_queue.empty()) {
                            auto pending = std::move(ctx->conn->request_queue.front());
                            ctx->conn->request_queue.pop();
                            process_request(ctx->conn, pending);
                        }
                        
                        // Keep reading more requests
                        add_read(&ring, ctx->conn);
                    } else {
                        add_read(&ring, ctx->conn);
                    }
                }
            }
        } else if (ctx->type == EventType::WRITE) {
            int bytes_written = cqe->res;
            
            if (bytes_written <= 0) {
                close(ctx->conn->fd);
                connections.erase(ctx->conn->fd);
                delete ctx->conn;
            } else {
                ctx->conn->bytes_written += bytes_written;
                if (ctx->conn->bytes_written == ctx->conn->bytes_to_write) {
                    // Response sent
                    ctx->conn->write_in_progress = false;
                    
                    // Process next queued request if any
                    if (!ctx->conn->request_queue.empty()) {
                        auto pending = std::move(ctx->conn->request_queue.front());
                        ctx->conn->request_queue.pop();
                        process_request(ctx->conn, pending);
                    }
                } else {
                    add_write(&ring, ctx->conn);
                }
            }
        }
        
        io_uring_cqe_seen(&ring, cqe);
        delete ctx;
        io_uring_submit(&ring);
    }
}

int main(int argc, char** argv) {
    uint16_t port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    
    try {
        RPCServer server(port);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}