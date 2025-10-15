# High-Performance RPC System with io_uring

A complete Remote Procedure Call (RPC) implementation demonstrating compute disaggregation using Linux io_uring.

## Overview

This project implements a high-performance RPC system with:

- **io_uring** for asynchronous I/O operations
- Binary protocol with efficient serialization
- Open-loop load testing
- Support for four compute operations: hash, sort, matrix multiply, and compression

## Features

### Supported Operations

1. **hash_compute**: SHA-256 hashing of arbitrary byte data
2. **sort_array**: Ascending sort of int32 arrays
3. **matrix_multiply**: Square matrix multiplication (float64, row-major)
4. **compress_data**: Data compression using zlib

### Performance Characteristics

- **Zero-copy I/O** where possible using io_uring
- **Asynchronous request handling**
- **Connection pooling** support

## Architecture

### Protocol Design

The RPC protocol uses a simple binary format:

```
[Header: 24 bytes] [Payload: variable]

Header:
  - magic: 4 bytes (0x52504301)
  - request_id: 8 bytes
  - operation: 4 bytes
  - payload_size: 4 bytes
  - error_code: 4 bytes
  - reserved: 4 bytes
```

All multi-byte integers are in network byte order (big-endian).

### Components

- **rpc_protocol.h/cpp**: Protocol definitions and serialization
- **rpc_server.cpp**: io_uring-based server implementation
- **rpc_client.cpp**: Client library with synchronous API
- **load_test.cpp**: Open-loop load generator

## Building

### Dependencies

Ubuntu/Debian:

```bash
sudo apt-get update
sudo apt-get install -y liburing-dev libssl-dev zlib1g-dev build-essential g++
```

Or use the Makefile target:

```bash
make deps
```

### Compilation

```bash
make all
```

This produces:

- `rpc_server`: The RPC server binary
- `load_test`: Load testing tool

## Usage

### Starting the Server

```bash
./rpc_server [port]
```

Default port is 8080.

Example:

```bash
./rpc_server 9000
```

### Running Load Tests

```bash
./load_test [host] [port] [output_csv]
```

Example:

```bash
./load_test 127.0.0.1 8080 results.csv
```

This will run a comprehensive benchmark suite testing various load levels from 10 to 5000 requests/second.

### Using the Client API

```cpp
#include "rpc_client.cpp"

// Connect to server
RPCClient client("localhost", 8080);

// Hash computation
char hash[65];
const char* data = "Hello, World!";
client.hash_compute(data, strlen(data), hash);
printf("Hash: %s\n", hash);

// Sort array
int32_t array[] = {5, 2, 8, 1, 9};
client.sort_array(array, 5);

// Matrix multiplication
double matA[16], matB[16], result[16];
client.matrix_multiply(matA, matB, 4, result);

// Compress data
std::vector<uint8_t> compressed;
client.compress_data(CompressionAlgo::ZLIB, data, size, compressed);
```

## Load Testing Methodology

### Open-Loop Design

The load generator uses an **open-loop** approach:

- Requests are sent at precise intervals regardless of response times
- Multiple worker threads maintain target request rate
- Each worker independently tracks send times
- More realistic simulation of real-world traffic patterns

### Metrics Collected

- **Per-request latency**: Time from send to response received
- **Percentiles**: P50, P95, P99, P99.9
- **Success/error counts**
- **Actual throughput**: Achieved requests per second

### Test Configuration

Default benchmark suite tests these load levels (req/s):

```
1000, 2000, 3000, 5000
```

Each test runs for 30 seconds with 4 worker threads.

### Plotting Results

A Python script is provided to visualize results:

```bash
python3 plot_results.py load_test_results.csv
```

This generates:

- Load vs. Latency curves (avg, P95, P99)
- Throughput saturation analysis


## Testing

### Unit Tests

Basic functionality tests for each operation:

```bash
# Test rpc operations
./test_rpc
```

## Limitations and Future Work

### Current Limitations

1. **Single-threaded server**: Uses single io_uring instance
2. **No authentication**: No security layer
3. **No encryption**: Plain text protocol
4. **TCP only**: No UDP support
5. **No load balancing**: Single server instance

### Future Enhancements

1. **Multi-threaded server**: One io_uring per core
3. **Load balancing**: Client-side or proxy-based
4. **Async client API**: Non-blocking operations (needs work)
5. **Request batching**: Group small requests

## References

- [io_uring documentation](https://kernel.dk/io_uring.pdf)
- [io_uring man page](https://man7.org/linux/man-pages/man7/io_uring.7.html)
- [gRPC Design Principles](https://grpc.io/blog/principles/)

## Contributors

Course Assignment Implementation - CS Disaggregated Computing
