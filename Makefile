CXX = g++
CXXFLAGS = -O3  -funroll-loops -flto -pthread -std=c++14
LDFLAGS = -flto
LIBS = /usr/local/lib/libcryptopp.a -lpthread -lm

# 客户端源码
CLIENT_CPP = client.cpp ringoram.cpp block.cpp bucket.cpp \
             param.cpp CryptoUtil.cpp Vocabulary.cpp Vector.cpp \
             Node.cpp InvertedIndex.cpp Document.cpp MBR.cpp \
             NodeSerializer.cpp Query.cpp RingoramStorage.cpp IRTree.cpp

# 服务器源码
SERVER_CPP = storage_server.cpp block.cpp bucket.cpp \
             ServerStorage.cpp param.cpp CryptoUtil.cpp

# 自动生成对应的 .o 文件列表
CLIENT_OBJ = $(CLIENT_CPP:.cpp=.o)
SERVER_OBJ = $(SERVER_CPP:.cpp=.o)

# 默认任务
all: client server

client: $(CLIENT_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

server: $(SERVER_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS) -lboost_system

# 通用编译规则
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f client server $(CLIENT_OBJ) $(SERVER_OBJ)

run_test: client server
	@echo "Starting server in background..."
	@./server &
	@SERVER_PID=$$!; \
	sleep 2; \
	echo "Starting client..."; \
	./client 127.0.0.1 12345; \
	echo "Stopping server..."; \
	kill $$SERVER_PID 2>/dev/null || true

rebuild: clean all

help:
	@echo "可用命令:"
	@echo "  make all        - 编译客户端和服务器"
	@echo "  make client     - 编译客户端"
	@echo "  make server     - 编译服务器"
	@echo "  make clean      - 清理编译文件"
	@echo "  make rebuild    - 重新编译"
	@echo "  make run_test   - 运行客户端测试"
	@echo "  make help       - 显示此帮助"

.PHONY: all clean run_test rebuild help
