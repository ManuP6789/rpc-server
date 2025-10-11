#include <liburing.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <map>
#include <iostream>
#include <openssl/sha.h>
#include <zlib.h>
#include <algorithm>
#include "rpc_protocol.h"

constexpr int QUEUE_DEPTH = 256;
constexpr int BACKLOG = 128;

enum class EventType {
    ACCEPT,
    READ,
    WRITE
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
    
    void handle_hash_compute(Connection* conn, const std::vector<uint8_t>& payload);
    void handle_sort_array(Connection* conn, const std::vector<uint8_t>& payload);
    void handle_matrix_multiply(Connection* conn, const std::vector<uint8_t>& payload);
    void handle_compress_data(Connection* conn, const std::vector<uint8_t>& payload);
    
    void send_error(Connection* conn, uint64_t req_id, RPCError error);
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
    // Create listening socket
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
    
    // Initialize io_uring
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
    io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, 0);
    
    auto* ctx = new RequestContext(EventType::ACCEPT);
    io_uring_sqe_set_data(sqe, ctx);
}

void RPCServer::add_read(io_uring* ring, Connection* conn) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    
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
    
    size_t remaining = conn->bytes_to_write - conn->bytes_written;
    io_uring_prep_write(sqe, conn->fd,
                       conn->write_buffer.data() + conn->bytes_written,
                       remaining, 0);
    
    auto* ctx = new RequestContext(EventType::WRITE, conn);
    io_uring_sqe_set_data(sqe, ctx);
}

void RPCServer::handle_hash_compute(Connection* conn, 
                                    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> data;
    if (!HashComputeRequest::deserialize(payload, data)) {
        send_error(conn, conn->current_header.request_id, 
                  RPCError::INVALID_PARAMS);
        return;
    }
    
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);
    
    char hex_hash[65];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hex_hash + i * 2, "%02x", hash[i]);
    }
    hex_hash[64] = '\0';
    
    // Prepare response
    RPCHeader resp_header;
    resp_header.request_id = conn->current_header.request_id;
    resp_header.operation = static_cast<uint32_t>(RPCOperation::HASH_COMPUTE);
    
    auto resp_payload = HashComputeResponse::serialize(hex_hash);
    resp_header.payload_size = resp_payload.size();
    resp_header.error_code = static_cast<uint32_t>(RPCError::SUCCESS);
    
    resp_header.to_network_order();
    
    conn->write_buffer.resize(sizeof(RPCHeader) + resp_payload.size());
    memcpy(conn->write_buffer.data(), &resp_header, sizeof(RPCHeader));
    memcpy(conn->write_buffer.data() + sizeof(RPCHeader), 
           resp_payload.data(), resp_payload.size());
    
    conn->bytes_to_write = conn->write_buffer.size();
    conn->bytes_written = 0;
    fprintf(stderr, "Server sending hash: '%s'\n", hex_hash);
    fprintf(stderr, "Response payload size: %zu\n", resp_payload.size());
    fprintf(stderr, "Response payload bytes: ");
    for (size_t i = 0; i < std::min(resp_payload.size(), size_t(65)); i++) {
        fprintf(stderr, "%02x ", resp_payload[i]);
    }
    fprintf(stderr, "\n");
    add_write(&ring, conn);
}

void RPCServer::handle_sort_array(Connection* conn,
                                  const std::vector<uint8_t>& payload) {
    std::vector<int32_t> array;
    if (!SortArrayRequest::deserialize(payload, array)) {
        send_error(conn, conn->current_header.request_id,
                  RPCError::INVALID_PARAMS);
        return;
    }
    
    std::sort(array.begin(), array.end());
    
    RPCHeader resp_header;
    resp_header.request_id = conn->current_header.request_id;
    resp_header.operation = static_cast<uint32_t>(RPCOperation::SORT_ARRAY);
    
    auto resp_payload = SortArrayResponse::serialize(array.data(), array.size());
    resp_header.payload_size = resp_payload.size();
    resp_header.error_code = static_cast<uint32_t>(RPCError::SUCCESS);
    
    resp_header.to_network_order();
    
    conn->write_buffer.resize(sizeof(RPCHeader) + resp_payload.size());
    memcpy(conn->write_buffer.data(), &resp_header, sizeof(RPCHeader));
    memcpy(conn->write_buffer.data() + sizeof(RPCHeader),
           resp_payload.data(), resp_payload.size());
    
    conn->bytes_to_write = conn->write_buffer.size();
    conn->bytes_written = 0;
    add_write(&ring, conn);
}

void RPCServer::handle_matrix_multiply(Connection* conn,
                                       const std::vector<uint8_t>& payload) {
    std::vector<double> matA, matB;
    uint32_t n;
    
    if (!MatrixMultiplyRequest::deserialize(payload, matA, matB, n)) {
        send_error(conn, conn->current_header.request_id,
                  RPCError::INVALID_PARAMS);
        return;
    }
    
    std::vector<double> result(n * n, 0.0);
    
    // Standard matrix multiplication
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < n; j++) {
            for (uint32_t k = 0; k < n; k++) {
                result[i * n + j] += matA[i * n + k] * matB[k * n + j];
            }
        }
    }
    
    RPCHeader resp_header;
    resp_header.request_id = conn->current_header.request_id;
    resp_header.operation = static_cast<uint32_t>(RPCOperation::MATRIX_MULTIPLY);
    
    auto resp_payload = MatrixMultiplyResponse::serialize(result.data(), n);
    resp_header.payload_size = resp_payload.size();
    resp_header.error_code = static_cast<uint32_t>(RPCError::SUCCESS);
    
    resp_header.to_network_order();
    
    conn->write_buffer.resize(sizeof(RPCHeader) + resp_payload.size());
    memcpy(conn->write_buffer.data(), &resp_header, sizeof(RPCHeader));
    memcpy(conn->write_buffer.data() + sizeof(RPCHeader),
           resp_payload.data(), resp_payload.size());
    
    conn->bytes_to_write = conn->write_buffer.size();
    conn->bytes_written = 0;
    add_write(&ring, conn);
}

void RPCServer::handle_compress_data(Connection* conn,
                                     const std::vector<uint8_t>& payload) {
    CompressionAlgo algo;
    std::vector<uint8_t> data;
    
    if (!CompressDataRequest::deserialize(payload, algo, data)) {
        send_error(conn, conn->current_header.request_id,
                  RPCError::INVALID_PARAMS);
        return;
    }
    
    std::vector<uint8_t> compressed;
    
    if (algo == CompressionAlgo::ZLIB) {
        uLongf compressed_size = compressBound(data.size());
        compressed.resize(compressed_size);
        
        if (compress(compressed.data(), &compressed_size,
                    data.data(), data.size()) != Z_OK) {
            send_error(conn, conn->current_header.request_id,
                      RPCError::COMPUTATION_ERROR);
            return;
        }
        compressed.resize(compressed_size);
    }
    
    RPCHeader resp_header;
    resp_header.request_id = conn->current_header.request_id;
    resp_header.operation = static_cast<uint32_t>(RPCOperation::COMPRESS_DATA);
    
    auto resp_payload = CompressDataResponse::serialize(compressed.data(),
                                                        compressed.size());
    resp_header.payload_size = resp_payload.size();
    resp_header.error_code = static_cast<uint32_t>(RPCError::SUCCESS);
    
    resp_header.to_network_order();
    
    conn->write_buffer.resize(sizeof(RPCHeader) + resp_payload.size());
    memcpy(conn->write_buffer.data(), &resp_header, sizeof(RPCHeader));
    memcpy(conn->write_buffer.data() + sizeof(RPCHeader),
           resp_payload.data(), resp_payload.size());
    
    conn->bytes_to_write = conn->write_buffer.size();
    conn->bytes_written = 0;
    add_write(&ring, conn);
}

void RPCServer::send_error(Connection* conn, uint64_t req_id, RPCError error) {
    RPCHeader resp_header;
    resp_header.request_id = req_id;
    resp_header.operation = 0;
    resp_header.payload_size = 0;
    resp_header.error_code = static_cast<uint32_t>(error);
    
    resp_header.to_network_order();
    
    conn->write_buffer.resize(sizeof(RPCHeader));
    memcpy(conn->write_buffer.data(), &resp_header, sizeof(RPCHeader));
    
    conn->bytes_to_write = conn->write_buffer.size();
    conn->bytes_written = 0;
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
                // Connection closed or error
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
                            add_read(&ring, ctx->conn);
                        }
                    } else {
                        add_read(&ring, ctx->conn);
                    }
                } else {
                    ctx->conn->bytes_read += bytes_read;
                    if (ctx->conn->bytes_read == 
                        ctx->conn->current_header.payload_size) {
                        // Process request
                        auto op = static_cast<RPCOperation>(
                            ctx->conn->current_header.operation);
                        
                        switch (op) {
                        case RPCOperation::HASH_COMPUTE:
                            handle_hash_compute(ctx->conn, 
                                              ctx->conn->read_buffer);
                            break;
                        case RPCOperation::SORT_ARRAY:
                            handle_sort_array(ctx->conn, 
                                            ctx->conn->read_buffer);
                            break;
                        case RPCOperation::MATRIX_MULTIPLY:
                            handle_matrix_multiply(ctx->conn,
                                                 ctx->conn->read_buffer);
                            break;
                        case RPCOperation::COMPRESS_DATA:
                            handle_compress_data(ctx->conn,
                                               ctx->conn->read_buffer);
                            break;
                        default:
                            send_error(ctx->conn, 
                                     ctx->conn->current_header.request_id,
                                     RPCError::INVALID_OPERATION);
                        }
                        
                        ctx->conn->header_read = false;
                        ctx->conn->bytes_read = 0;
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
                    // Response sent, ready for next request
                    add_read(&ring, ctx->conn);
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