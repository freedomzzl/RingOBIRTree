// storage_server.cpp
#include <iostream>
#include <string>
#include <memory>
#include <cstdint>
#include <cstring>
#include<chrono>
#include <boost/asio.hpp>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include "ServerStorage.h" 
#include"param.h"
#include<cmath> 

using boost::asio::ip::tcp;

// 协议定义
enum RequestType {
    READ_BUCKET = 1,
    WRITE_BUCKET = 2,
    READ_PATH = 3,
    READ_PATH_FULL = 4,      
    WRITE_PATH_FULL = 5,
    CHECK_NEED_EARLY_SHUFFLE = 6,  
    CHECK_NEED_EARLY_SHUFFLE_WITH_DATA = 7, 
    WRITE_BUCKETS = 8,     
    RESPONSE = 100
};

struct RequestHeader {
    uint32_t type;
    uint32_t request_id;
    uint32_t data_len;
    uint32_t reserved;
};

struct ResponseHeader {
    uint32_t type;
    uint32_t request_id;
    uint32_t result;
    uint32_t data_len;
};


#pragma pack(push, 1)
struct SerializedBucketHeader {
    int32_t Z;
    int32_t S;
    int32_t count;
    int32_t num_blocks;
};

struct SerializedBlockHeader {
    int32_t leaf_id;
    int32_t block_index;
    int32_t data_size;
};
#pragma pack(pop)

// ================================
// 序列化工具函数
// ================================

size_t calculate_bucket_size(const bucket& bkt) {
    
    size_t size = sizeof(SerializedBucketHeader);
    
    // 计算blocks大小
    size_t blocks_size = 0;
    for (int i = 0; i < bkt.blocks.size(); i++) {
        auto& blk = bkt.blocks[i];
        size_t block_size = sizeof(SerializedBlockHeader) + blk.GetData().size();
        blocks_size += block_size;
      
    }
    size += blocks_size;
    
    // 计算ptrs和valids大小
    size_t ptrs_valids_size = (bkt.ptrs.size() + bkt.valids.size()) * sizeof(int32_t);
    size += ptrs_valids_size;
 
    return size;
}

void serialize_block( block& blk, uint8_t* buffer, size_t& offset) {
  
    SerializedBlockHeader* header = reinterpret_cast<SerializedBlockHeader*>(buffer + offset);
    header->leaf_id = blk.GetLeafid();
    header->block_index = blk.GetBlockindex();
    
    const auto& data = blk.GetData();
    header->data_size = static_cast<int32_t>(data.size());
 
    offset += sizeof(SerializedBlockHeader);
    
    if (!data.empty()) {
        memcpy(buffer + offset, data.data(), data.size());
        offset += data.size();
    }

}

block deserialize_block(const uint8_t* data, size_t& offset) {
    const SerializedBlockHeader* header = reinterpret_cast<const SerializedBlockHeader*>(data + offset);
    offset += sizeof(SerializedBlockHeader);
    
    std::vector<char> block_data;
    if (header->data_size > 0) {
        block_data.resize(header->data_size);
        memcpy(block_data.data(), data + offset, header->data_size);
        offset += header->data_size;
    }
    
    return block(header->leaf_id, header->block_index, block_data);
}

std::vector<uint8_t> serialize_bucket( bucket& bkt) {
   
    try {
    
        // 先计算大小

        size_t total_size = calculate_bucket_size(bkt);
     
        if (total_size == 0) {
            std::cerr << "ERROR: Calculated size is 0" << std::endl;
            return std::vector<uint8_t>();
        }
       
        std::vector<uint8_t> result(total_size);
      
        // 序列化 bucket header
      
        SerializedBucketHeader* bucket_header = reinterpret_cast<SerializedBucketHeader*>(result.data());
        bucket_header->Z = bkt.Z;
        bucket_header->S = bkt.S;
        bucket_header->count = bkt.count;
        bucket_header->num_blocks = static_cast<int32_t>(bkt.blocks.size());
       
        size_t offset = sizeof(SerializedBucketHeader);
       
        // 序列化 blocks
    
        for (int i = 0; i < bkt.blocks.size(); i++) {
          
            serialize_block(bkt.blocks[i], result.data(), offset);
       
        }
        
        // 序列化 ptrs 和 valids
   
        int num_slots = bkt.Z + bkt.S;
 
        // 检查边界
        if (offset + num_slots * 2 * sizeof(int32_t) > total_size) {
            std::cerr << "ERROR: Not enough space for ptrs and valids" << std::endl;
            return std::vector<uint8_t>();
        }
        
        // 序列化 ptrs
        for (int i = 0; i < num_slots; i++) {
            *reinterpret_cast<int32_t*>(result.data() + offset) = bkt.ptrs[i];
            offset += sizeof(int32_t);
        }
        
        // 序列化 valids
        for (int i = 0; i < num_slots; i++) {
            *reinterpret_cast<int32_t*>(result.data() + offset) = bkt.valids[i];
            offset += sizeof(int32_t);
        }
 
        return result;
        
    } catch (const std::exception& e) {
        std::cerr << "serialize_bucket failed with exception: " << e.what() << std::endl;
        return std::vector<uint8_t>();
    }
}

bucket deserialize_bucket(const uint8_t* data, size_t size) {
 
    if (size < sizeof(SerializedBucketHeader)) {
        std::cerr << "  ERROR: Data too small for header" << std::endl;
        throw std::runtime_error("Invalid bucket data: too small");
    }
    
    const SerializedBucketHeader* bucket_header = reinterpret_cast<const SerializedBucketHeader*>(data);
  
    // 创建空的bucket
    bucket result(0, 0);
    result.Z = bucket_header->Z;
    result.S = bucket_header->S;
    result.count = bucket_header->count;
    
    size_t offset = sizeof(SerializedBucketHeader);
 
    // 反序列化 blocks
  
    for (int i = 0; i < bucket_header->num_blocks && offset < size; i++) {
        result.blocks.push_back(deserialize_block(data, offset));
    }
 
    //从序列化数据中恢复ptrs和valids
    int num_slots = result.Z + result.S;
    result.ptrs.resize(num_slots, -1);
    result.valids.resize(num_slots, 0);
    
    // 检查是否有足够的空间来读取ptrs和valids
    if (offset + num_slots * 2 * sizeof(int32_t) <= size) {
     
        // 反序列化 ptrs
        for (int i = 0; i < num_slots; i++) {
            int32_t ptr = *reinterpret_cast<const int32_t*>(data + offset);
            result.ptrs[i] = ptr;
            offset += sizeof(int32_t);
        }
        
        // 反序列化 valids
        for (int i = 0; i < num_slots; i++) {
            int32_t valid = *reinterpret_cast<const int32_t*>(data + offset);
            result.valids[i] = valid;
            offset += sizeof(int32_t);
        }
   
    } else {
        std::cout << "  WARNING: No ptrs and valids data in serialized bucket" << std::endl;
    }
   
    return result;
}


// 全局的 ServerStorage 对象
std::unique_ptr<ServerStorage> g_storage;

// 处理 READ_BUCKET 请求
std::vector<uint8_t> handleReadBucket(const uint8_t* request_data, uint32_t data_len) {
    if (data_len < 4) {
        std::cout << "Error: READ_BUCKET request data too short" << std::endl;
        // 返回空的65536字节数据
        return std::vector<uint8_t>(65536, 0);
    }
    
    // 解析桶位置
    int32_t position = *reinterpret_cast<const int32_t*>(request_data);
  
    try {
        // 调用 ServerStorage 读取桶
        bucket bkt = g_storage->GetBucket(position);
   
        // 使用真正的序列化
        std::vector<uint8_t> serialized = serialize_bucket(bkt);
   
        return serialized;
        
    } catch (const std::exception& e) {
        std::cerr << "Failed to read bucket: " << e.what() << std::endl;
        // 返回空的65536字节数据
        return std::vector<uint8_t>(65536, 0);
    }
}


// 处理 WRITE_BUCKET 请求
bool handleWriteBucket(const uint8_t* request_data, uint32_t data_len) {
    if (!g_storage) {
        std::cout << "Error: ServerStorage not initialized" << std::endl;
        return false;
    }
    
    if (data_len < 4) {
        std::cout << "Error: WRITE_BUCKET request data too short" << std::endl;
        return false;
    }
    
    // 解析桶位置
    int32_t position = *reinterpret_cast<const int32_t*>(request_data);
   
    try {
        // 反序列化bucket数据（从第5字节开始）
        if (data_len >= 4 + sizeof(SerializedBucketHeader)) {
            const uint8_t* bucket_data = request_data + 4;
            uint32_t bucket_data_len = data_len - 4;
            
            bucket bkt_to_write = deserialize_bucket(bucket_data, bucket_data_len);
          
            // 执行写入
            g_storage->SetBucket(position, bkt_to_write);
          
            return true;
        } else {
            std::cout << "Error: Incomplete bucket data" << std::endl;
            return false;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Write bucket failed: " << e.what() << std::endl;
        return false;
    }
}

std::vector<uint8_t> handleReadPath(const uint8_t* request_data, uint32_t data_len) {
    if (!g_storage) {
        std::cout << "Error: ServerStorage not initialized" << std::endl;
        return {};
    }
    
    if (data_len < 8) {
        std::cout << "Error: READ_PATH request data too short" << std::endl;
        return {};
    }
    
    // 解析 leaf_id 和 block_index
    int32_t leaf_id = *reinterpret_cast<const int32_t*>(request_data);
    int32_t block_index = *reinterpret_cast<const int32_t*>(request_data + 4);
    
 
    try {
       
        block interestblock = dummyBlock;
        
        // 遍历路径上的所有层级
        for (int i = 0; i <= OramL; i++) {
            int position = (1 << i) - 1 + (leaf_id >> (OramL - i));
            
            if (position < 0 || position >= capacity) {
                std::cout << "Warning: Invalid bucket position: " << position 
                         << " (leaf_id=" << leaf_id << ", level=" << i << ")" << std::endl;
                continue;
            }
            
            // 读取bucket
            bucket& bkt = g_storage->GetBucket(position);
            
            // 查找目标块
            int offset = -1;
            for (int j = 0; j < (realBlockEachbkt + dummyBlockEachbkt); j++) {
                if (bkt.ptrs[j] == block_index && bkt.valids[j] == 1) {
                    offset = j;
                    break;
                }
            }
            
            if (offset == -1) {
                offset = bkt.GetDummyblockOffset();
            }
            
            block blk = bkt.blocks[offset];
            
            // 标记为无效
            bkt.valids[offset] = 0;
            bkt.count += 1;
            
            if (blk.GetBlockindex() == block_index) {
                interestblock = blk;
            }
        }
        
        // 准备响应
        std::vector<uint8_t> response;
        
        // 第一个字节：是否是dummy块
        response.push_back(interestblock.GetBlockindex() == -1 ? 1 : 0);
        
        // 如果不是dummy块，添加block数据
        if (interestblock.GetBlockindex() != -1) {
            const auto& data = interestblock.GetData();
            if (!data.empty()) {
                // // 检查数据大小
                // if (data.size() > 4095) {  // 减去1字节的is_dummy标志
                //     std::cerr << "Warning: Block data too large: " << data.size() 
                //               << " bytes, truncated" << std::endl;
                //     response.insert(response.end(), data.begin(), data.begin() + 4095);
                // } else {
                //     response.insert(response.end(), data.begin(), data.end());
                // }
                response.insert(response.end(), data.begin(), data.end());
            }
        }

        return response;
        
    } catch (const std::exception& e) {
        std::cerr << "Read path failed: " << e.what() << std::endl;
        return {};
    }
}


// 处理 READ_PATH_FULL 请求
std::vector<uint8_t> handleReadPathFull(const uint8_t* request_data, uint32_t data_len) {
    if (!g_storage) {
        std::cout << "Error: ServerStorage not initialized" << std::endl;
        return {};
    }
    
    if (data_len < 4) {
        std::cout << "Error: READ_PATH_FULL request data too short" << std::endl;
        return {};
    }
    
    // 解析 leaf_id
    int32_t leaf_id = *reinterpret_cast<const int32_t*>(request_data);
    
    try {
        std::vector<uint8_t> response_data;
        
        // 遍历路径上的所有层级
        for (int i = 0; i <= OramL; i++) {
            int position = (1 << i) - 1 + (leaf_id >> (OramL - i));
            
            if (position < 0 || position >= capacity) {
                std::cout << "Warning: Invalid bucket position: " << position 
                         << " (leaf_id=" << leaf_id << ", level=" << i << ")" << std::endl;
                continue;
            }
            
            // 读取bucket并序列化
            bucket& bkt = g_storage->GetBucket(position);
            std::vector<uint8_t> serialized_bkt = serialize_bucket(bkt);
            
            if (!serialized_bkt.empty()) {
                // 添加位置信息（4字节）
                uint32_t pos = static_cast<uint32_t>(position);
                response_data.insert(response_data.end(), 
                                   reinterpret_cast<uint8_t*>(&pos), 
                                   reinterpret_cast<uint8_t*>(&pos) + sizeof(uint32_t));
                
                // 添加bucket大小信息（4字节）
                uint32_t bkt_size = static_cast<uint32_t>(serialized_bkt.size());
                response_data.insert(response_data.end(),
                                   reinterpret_cast<uint8_t*>(&bkt_size),
                                   reinterpret_cast<uint8_t*>(&bkt_size) + sizeof(uint32_t));
                
                // 添加bucket数据
                response_data.insert(response_data.end(),
                                   serialized_bkt.begin(),
                                   serialized_bkt.end());
            }
        }
        
        return response_data;
        
    } catch (const std::exception& e) {
        std::cerr << "Read path full failed: " << e.what() << std::endl;
        return {};
    }
}

// 处理 WRITE_PATH_FULL 请求
bool handleWritePathFull(const uint8_t* request_data, uint32_t data_len) {
    if (!g_storage) {
        std::cout << "Error: ServerStorage not initialized" << std::endl;
        return false;
    }
    
    size_t offset = 0;
    
    try {
        // 解析并写入每个bucket
        while (offset < data_len) {
            if (offset + 8 > data_len) {
                std::cout << "Error: Incomplete bucket header in WRITE_PATH_FULL" << std::endl;
                return false;
            }
            
            // 读取位置信息
            int32_t position = *reinterpret_cast<const int32_t*>(request_data + offset);
            offset += sizeof(int32_t);
            
            // 读取bucket大小
            uint32_t bucket_size = *reinterpret_cast<const uint32_t*>(request_data + offset);
            offset += sizeof(uint32_t);
            
            // 检查是否有足够的数据
            if (offset + bucket_size > data_len) {
                std::cout << "Error: Incomplete bucket data in WRITE_PATH_FULL" << std::endl;
                return false;
            }
            
            // 反序列化bucket
            bucket bkt = deserialize_bucket(request_data + offset, bucket_size);
            offset += bucket_size;
            
            // 写入bucket
            g_storage->SetBucket(position, bkt);
        }
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Write path full failed: " << e.what() << std::endl;
        return false;
    }
}


// 处理 CHECK_NEED_EARLY_SHUFFLE 请求
std::vector<uint8_t> handleCheckNeedEarlyShuffle(const uint8_t* request_data, uint32_t data_len) {
    if (!g_storage) {
        std::cout << "Error: ServerStorage not initialized" << std::endl;
        return {};
    }
    
    if (data_len < 4) {
        std::cout << "Error: CHECK_NEED_EARLY_SHUFFLE request data too short" << std::endl;
        return {};
    }
    
    // 解析 leaf_id
    int32_t leaf_id = *reinterpret_cast<const int32_t*>(request_data);
    std::vector<uint8_t> response_data;
    
    try {
        // 遍历路径上的所有层级
        for (int i = 0; i <= OramL; i++) {
            int position = (1 << i) - 1 + (leaf_id >> (OramL - i));
            
            if (position < 0 || position >= capacity) {
                continue;
            }
            
            // 读取bucket的count值（不需要读取整个bucket）
            bucket& bkt = g_storage->GetBucket(position);
            
            // 检查是否需要reshuffle
            if (bkt.count >= dummyBlockEachbkt) {
                // 添加需要reshuffle的bucket位置
                response_data.insert(response_data.end(),
                                   reinterpret_cast<const uint8_t*>(&position),
                                   reinterpret_cast<const uint8_t*>(&position) + sizeof(int32_t));
                
                // std::cout << "[Server] Bucket at position " << position 
                //          << " needs earlyshuffle, count=" << bkt.count 
                //          << " >= " << dummyBlockEachbkt << std::endl;
            }
        }
        
        return response_data;
        
    } catch (const std::exception& e) {
        std::cerr << "Check need earlyshuffle failed: " << e.what() << std::endl;
        return {};
    }
}


std::vector<uint8_t> handleCheckNeedEarlyShuffleWithData(const uint8_t* request_data, uint32_t data_len) {
    if (!g_storage) {
        std::cout << "Error: ServerStorage not initialized" << std::endl;
        return {};
    }
    
    if (data_len < 4) {
        std::cout << "Error: CHECK_NEED_EARLY_SHUFFLE_WITH_DATA request data too short" << std::endl;
        return {};
    }
    
    // 解析 leaf_id
    int32_t leaf_id = *reinterpret_cast<const int32_t*>(request_data);
    std::vector<uint8_t> response_data;
    
    try {
        // 遍历路径上的所有层级
        for (int i = 0; i <= OramL; i++) {
            int position = (1 << i) - 1 + (leaf_id >> (OramL - i));
            
            if (position < 0 || position >= capacity) {
                continue;
            }
            
            // 读取bucket
            bucket& bkt = g_storage->GetBucket(position);
            
            // 检查是否需要reshuffle
            if (bkt.count >= dummyBlockEachbkt) {
                // 序列化bucket
                std::vector<uint8_t> serialized_bkt = serialize_bucket(bkt);
                
                if (!serialized_bkt.empty()) {
                    // 添加位置信息（4字节）
                    int32_t pos = static_cast<int32_t>(position);
                    response_data.insert(response_data.end(),
                                       reinterpret_cast<uint8_t*>(&pos),
                                       reinterpret_cast<uint8_t*>(&pos) + sizeof(int32_t));
                    
                    // 添加bucket大小（4字节）
                    uint32_t bkt_size = static_cast<uint32_t>(serialized_bkt.size());
                    response_data.insert(response_data.end(),
                                       reinterpret_cast<uint8_t*>(&bkt_size),
                                       reinterpret_cast<uint8_t*>(&bkt_size) + sizeof(uint32_t));
                    
                    // 添加bucket数据
                    response_data.insert(response_data.end(),
                                       serialized_bkt.begin(),
                                       serialized_bkt.end());
                }
            }
        }
        
        return response_data;
        
    } catch (const std::exception& e) {
        std::cerr << "Check need earlyshuffle with data failed: " << e.what() << std::endl;
        return {};
    }
}

// 处理 WRITE_BUCKETS 请求（批量写入多个bucket）
bool handleWriteBuckets(const uint8_t* request_data, uint32_t data_len) {
    if (!g_storage) {
        std::cout << "Error: ServerStorage not initialized" << std::endl;
        return false;
    }
    
    size_t offset = 0;
    
    try {
        // 解析每个bucket并写入
        while (offset < data_len) {
            if (offset + 8 > data_len) {
                std::cout << "Error: Incomplete bucket header in WRITE_BUCKETS" << std::endl;
                return false;
            }
            
            // 读取位置信息
            int32_t position = *reinterpret_cast<const int32_t*>(request_data + offset);
            offset += sizeof(int32_t);
            
            // 读取bucket大小
            uint32_t bucket_size = *reinterpret_cast<const uint32_t*>(request_data + offset);
            offset += sizeof(uint32_t);
            
            // 检查是否有足够的数据
            if (offset + bucket_size > data_len) {
                std::cout << "Error: Incomplete bucket data in WRITE_BUCKETS" << std::endl;
                return false;
            }
            
            // 反序列化bucket
            bucket bkt = deserialize_bucket(request_data + offset, bucket_size);
            offset += bucket_size;
            
            // 写入bucket
            g_storage->SetBucket(position, bkt);
        }
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Write buckets failed: " << e.what() << std::endl;
        return false;
    }
}

inline int64_t duration_us(
    const std::chrono::high_resolution_clock::time_point& end,
    const std::chrono::high_resolution_clock::time_point& start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

// 处理单个客户端连接
void handleClient(tcp::socket socket) {
    try {
        // === TCP优化设置 ===
        // 1. 禁用Nagle算法
        tcp::no_delay no_delay_opt(true);
        socket.set_option(no_delay_opt);
        
        // 2. 禁用TCP延迟确认 (TCP_QUICKACK)
        int quickack = 1;
        setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
        
        // 3. 增大TCP缓冲区
        int buf_size = 65536;
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
        
        while (true) {
            // 阶段1: 接收请求头
            auto t1 = std::chrono::high_resolution_clock::now();
            RequestHeader header;
            
            // 确保在读取前设置QUICKACK
            setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
            boost::asio::read(socket, boost::asio::buffer(&header, sizeof(header)));
            auto t2 = std::chrono::high_resolution_clock::now();
            
            // 阶段2: 接收请求数据
            std::vector<uint8_t> request_data(header.data_len);
            if (header.data_len > 0) {
                setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
                boost::asio::read(socket, boost::asio::buffer(request_data.data(), header.data_len));
            }
            auto t3 = std::chrono::high_resolution_clock::now();
            
            // 阶段3: 处理请求
            auto t4 = std::chrono::high_resolution_clock::now();
            std::vector<uint8_t> response_data;
            bool success = false;
            
            switch (header.type) {
                case READ_BUCKET:
                    response_data = handleReadBucket(request_data.data(), header.data_len);
                    success = !response_data.empty();
                    break;
                case WRITE_BUCKET:
                    success = handleWriteBucket(request_data.data(), header.data_len);
                    break;
                case READ_PATH:
                    response_data = handleReadPath(request_data.data(), header.data_len);
                    success = !response_data.empty();
                    break;
                case READ_PATH_FULL:
                    response_data = handleReadPathFull(request_data.data(), header.data_len);
                    success = !response_data.empty();
                    break;
                case WRITE_PATH_FULL:
                    success = handleWritePathFull(request_data.data(), header.data_len);
                    break;
                case CHECK_NEED_EARLY_SHUFFLE:
                    response_data = handleCheckNeedEarlyShuffle(request_data.data(), header.data_len);
                    success = true;
                    break;
                case CHECK_NEED_EARLY_SHUFFLE_WITH_DATA:
                    response_data = handleCheckNeedEarlyShuffleWithData(request_data.data(), header.data_len);
                    success = true;
                    break;
                case WRITE_BUCKETS:
                    success = handleWriteBuckets(request_data.data(), header.data_len);
                    break;
            }
            auto t5 = std::chrono::high_resolution_clock::now();
            
            // 阶段4: 发送响应
            ResponseHeader response;
            response.type = RESPONSE;
            response.request_id = header.request_id;
            response.result = success ? 0 : 1;
            response.data_len = response_data.size();
            
            // 一次性发送响应头和数据（如果可能）
            if (response_data.empty()) {
                boost::asio::write(socket, boost::asio::buffer(&response, sizeof(response)));
            } else {
                // 合并发送头和数据
                std::vector<boost::asio::const_buffer> buffers;
                buffers.push_back(boost::asio::buffer(&response, sizeof(response)));
                buffers.push_back(boost::asio::buffer(response_data.data(), response_data.size()));
                boost::asio::write(socket, buffers);
            }
            auto t7 = std::chrono::high_resolution_clock::now();
            
            // 输出各阶段耗时
            // std::cout << "[SERVER] Type:" << header.type 
            //           << " RecvHeader:" << duration_us(t2,t1)
            //           << " RecvData:" << duration_us(t3,t2)
            //           << " Process:" << duration_us(t5,t4)
            //           << " SendTotal:" << duration_us(t7,t5)
            //           << " Total:" << duration_us(t7,t1) << "us" << std::endl;
        }
    }
    catch (std::exception& e) {
        std::cout << "Over: " << e.what() << std::endl;
    }
}


int main(int argc, char* argv[]) {
    std::cout << "=== Storage Server  ===" << std::endl;
    
    // 1. 初始化 ServerStorage
    try {
        g_storage = std::make_unique<ServerStorage>();
        
        g_storage->setCapacity(capacity);
    }
    catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
    
    // 2. 启动网络服务器
    try {
        boost::asio::io_context io_context;
        tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), 12345));
        
        std::cout << "Listening on port: 12345" << std::endl;
        std::cout << "Waiting for client connection..." << std::endl;
        
        tcp::socket socket(io_context);
        acceptor.accept(socket);
        std::cout << "\n=== client connected ===" << std::endl;

        // 当前线程等待 handleClient 结束
        handleClient(std::move(socket));

        std::cout << "Client disconnected. Server exit." << std::endl;
    }
    catch (std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}