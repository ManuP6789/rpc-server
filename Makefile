CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -pthread
LDFLAGS = -luring -lssl -lcrypto -lz

# Source files
PROTOCOL_SRC = rpc_protocol.cpp
SERVER_SRC = rpc_server.cpp
CLIENT_SRC = rpc_client.cpp
LOAD_TEST_SRC = load_test.cpp

# Object files
PROTOCOL_OBJ = $(PROTOCOL_SRC:.cpp=.o)

# Binaries
SERVER_BIN = rpc_server
CLIENT_TEST_BIN = client_test
LOAD_TEST_BIN = load_test
UNIT_TEST_BIN = test_rpc

.PHONY: all clean test

all: $(SERVER_BIN) $(LOAD_TEST_BIN) $(UNIT_TEST_BIN)

$(PROTOCOL_OBJ): $(PROTOCOL_SRC) rpc_protocol.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SERVER_BIN): $(SERVER_SRC) $(PROTOCOL_OBJ) rpc_protocol.h
	$(CXX) $(CXXFLAGS) $< $(PROTOCOL_OBJ) -o $@ $(LDFLAGS)

$(LOAD_TEST_BIN): $(LOAD_TEST_SRC) $(PROTOCOL_OBJ) rpc_protocol.h
	$(CXX) $(CXXFLAGS) $< $(PROTOCOL_OBJ) -o $@ $(LDFLAGS)

$(UNIT_TEST_BIN): test_rpc.cpp $(PROTOCOL_OBJ) rpc_protocol.h
	$(CXX) $(CXXFLAGS) $< $(PROTOCOL_OBJ) -o $@ $(LDFLAGS)

clean:
	rm -f $(SERVER_BIN) $(LOAD_TEST_BIN) $(PROTOCOL_OBJ) *.o
	rm -f load_test_results.csv

test: all
	@echo "Starting RPC server in background..."
	./$(SERVER_BIN) 8080 &
	@SERVER_PID=$!; \
	sleep 2; \
	echo "Running load tests..."; \
	./$(LOAD_TEST_BIN) 127.0.0.1 8080 load_test_results.csv; \
	echo "Stopping server..."; \
	kill $SERVER_PID

# Install dependencies (Ubuntu/Debian)
deps:
	sudo apt-get update
	sudo apt-get install -y liburing-dev libssl-dev zlib1g-dev build-essential

# For analysis and plotting
analyze: load_test_results.csv
	python3 plot_results.py load_test_results.csv