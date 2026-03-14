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


enum RequestType {
    READ_PATH = 3,
    READ_PATH_FULL = 4,      
    WRITE_PATH_FULL = 5,
    WRITE_BUCKETS = 8,     
    FETCH_BLOCKS = 9,
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



size_t calculate_bucket_size(const bucket& bkt) {
    
    size_t size = sizeof(SerializedBucketHeader);
    
    
    size_t blocks_size = 0;
    for (int i = 0; i < bkt.blocks.size(); i++) {
        auto& blk = bkt.blocks[i];
        size_t block_size = sizeof(SerializedBlockHeader) + blk.GetData().size();
        blocks_size += block_size;
      
    }
    size += blocks_size;
    
  
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
    
      
        size_t total_size = calculate_bucket_size(bkt);
     
        if (total_size == 0) {
            std::cerr << "ERROR: Calculated size is 0" << std::endl;
            return std::vector<uint8_t>();
        }
       
        std::vector<uint8_t> result(total_size);
      
        SerializedBucketHeader* bucket_header = reinterpret_cast<SerializedBucketHeader*>(result.data());
        bucket_header->Z = bkt.Z;
        bucket_header->S = bkt.S;
        bucket_header->count = bkt.count;
        bucket_header->num_blocks = static_cast<int32_t>(bkt.blocks.size());
       
        size_t offset = sizeof(SerializedBucketHeader);
     
        for (int i = 0; i < bkt.blocks.size(); i++) {
          
            serialize_block(bkt.blocks[i], result.data(), offset);
       
        }
      
        int num_slots = bkt.Z + bkt.S;
 
      
        if (offset + num_slots * 2 * sizeof(int32_t) > total_size) {
            std::cerr << "ERROR: Not enough space for ptrs and valids" << std::endl;
            return std::vector<uint8_t>();
        }
        
        for (int i = 0; i < num_slots; i++) {
            *reinterpret_cast<int32_t*>(result.data() + offset) = bkt.ptrs[i];
            offset += sizeof(int32_t);
        }
     
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
  
    bucket result(0, 0);
    result.Z = bucket_header->Z;
    result.S = bucket_header->S;
    result.count = bucket_header->count;
    
    size_t offset = sizeof(SerializedBucketHeader);
 
    for (int i = 0; i < bucket_header->num_blocks && offset < size; i++) {
        result.blocks.push_back(deserialize_block(data, offset));
    }
 
    int num_slots = result.Z + result.S;
    result.ptrs.resize(num_slots, -1);
    result.valids.resize(num_slots, 0);
    
    
    if (offset + num_slots * 2 * sizeof(int32_t) <= size) {
     
        for (int i = 0; i < num_slots; i++) {
            int32_t ptr = *reinterpret_cast<const int32_t*>(data + offset);
            result.ptrs[i] = ptr;
            offset += sizeof(int32_t);
        }
        
        
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



std::vector<uint8_t> serialize_bucket_metadata(bucket& bkt) {
    try {
       
        int num_slots = bkt.Z + bkt.S;
        size_t total_size = sizeof(int32_t) + num_slots * 2 * sizeof(int32_t);
        
        std::vector<uint8_t> result(total_size);
        size_t offset = 0;
    
        *reinterpret_cast<int32_t*>(result.data() + offset) = bkt.count;
        offset += sizeof(int32_t);
        
   
        for (int i = 0; i < num_slots; i++) {
            *reinterpret_cast<int32_t*>(result.data() + offset) = bkt.ptrs[i];
            offset += sizeof(int32_t);
        }
     
        for (int i = 0; i < num_slots; i++) {
            *reinterpret_cast<int32_t*>(result.data() + offset) = bkt.valids[i];
            offset += sizeof(int32_t);
        }
        
        return result;
    } catch (const std::exception& e) {
        std::cerr << "serialize_bucket_metadata failed: " << e.what() << std::endl;
        return std::vector<uint8_t>();
    }
}


std::unique_ptr<ServerStorage> g_storage;



std::vector<uint8_t> handleReadPath(const uint8_t* request_data, uint32_t data_len) {
    if (!g_storage) {
        std::cout << "Error: ServerStorage not initialized" << std::endl;
        return {};
    }
    
    if (data_len < 8) {
        std::cout << "Error: READ_PATH request data too short" << std::endl;
        return {};
    }
    
    int32_t leaf_id = *reinterpret_cast<const int32_t*>(request_data);
    int32_t block_index = *reinterpret_cast<const int32_t*>(request_data + 4);
    
    try {
        std::vector<uint8_t> response_data;
        
        std::vector<int> need_shuffle_positions;
       
        for (int i = 0; i <= OramL; i++) {
            int position = (1 << i) - 1 + (leaf_id >> (OramL - i));
            
            if (position < 0 || position >= capacity) {
                std::cout << "Warning: Invalid bucket position: " << position 
                         << " (leaf_id=" << leaf_id << ", level=" << i << ")" << std::endl;
                continue;
            }
          
            bucket& bkt = g_storage->GetBucket(position);
           
            if (bkt.count >= dummyBlockEachbkt) {
                need_shuffle_positions.push_back(position);
            }
           
            std::vector<uint8_t> metadata = serialize_bucket_metadata(bkt);
            
            if (!metadata.empty()) {
  
                response_data.insert(response_data.end(),
                                   reinterpret_cast<uint8_t*>(&position),
                                   reinterpret_cast<uint8_t*>(&position) + sizeof(int32_t));
 
                uint32_t meta_size = static_cast<uint32_t>(metadata.size());
                response_data.insert(response_data.end(),
                                   reinterpret_cast<uint8_t*>(&meta_size),
                                   reinterpret_cast<uint8_t*>(&meta_size) + sizeof(uint32_t));
   
                response_data.insert(response_data.end(),
                                   metadata.begin(), metadata.end());
            }
        }
  
        uint32_t shuffle_count = static_cast<uint32_t>(need_shuffle_positions.size());
        response_data.insert(response_data.begin(),
                           reinterpret_cast<uint8_t*>(&shuffle_count),
                           reinterpret_cast<uint8_t*>(&shuffle_count) + sizeof(uint32_t));
   
        response_data.insert(response_data.begin(),
                           reinterpret_cast<uint8_t*>(&block_index),
                           reinterpret_cast<uint8_t*>(&block_index) + sizeof(int32_t));
        
        return response_data;
        
    } catch (const std::exception& e) {
        std::cerr << "Read path failed: " << e.what() << std::endl;
        return {};
    }
}


std::vector<uint8_t> handleFetchBlocks(const uint8_t* request_data, uint32_t data_len) {
    if (!g_storage) {
        std::cout << "Error: ServerStorage not initialized" << std::endl;
        return {};
    }
    
    try {
        std::vector<uint8_t> response_data;
        size_t offset = 0;
        
        // ???????
        if (offset + 8 > data_len) {
            std::cout << "Error: Incomplete target block info" << std::endl;
            return {};
        }
        
        int32_t target_position = *reinterpret_cast<const int32_t*>(request_data + offset);
        offset += sizeof(int32_t);
        int32_t target_offset = *reinterpret_cast<const int32_t*>(request_data + offset);
        offset += sizeof(int32_t);
        
        // ?????
        if (target_position >= 0 && target_position < capacity && target_offset >= 0) {
            bucket& bkt = g_storage->GetBucket(target_position);
            if (target_offset < bkt.blocks.size()) {
                block& target_block = bkt.blocks[target_offset];
                
                // ?????
                bkt.valids[target_offset] = 0;
                bkt.count += 1;
                
                // ??????
                const auto& data = target_block.GetData();
                uint32_t data_size = static_cast<uint32_t>(data.size());
                
                // ????????is_dummy?? + ???? + ???
                response_data.push_back(target_block.GetBlockindex() == -1 ? 1 : 0);
                response_data.insert(response_data.end(),
                                   reinterpret_cast<uint8_t*>(&data_size),
                                   reinterpret_cast<uint8_t*>(&data_size) + sizeof(uint32_t));
                if (!data.empty()) {
                    response_data.insert(response_data.end(), data.begin(), data.end());
                }
            }
        }
        
        // ???????bucket??
        uint32_t shuffle_count = 0;
        if (offset + 4 <= data_len) {
            shuffle_count = *reinterpret_cast<const uint32_t*>(request_data + offset);
            offset += sizeof(uint32_t);
        }
        
        // ???????buckets?????
        for (uint32_t i = 0; i < shuffle_count && offset + 4 <= data_len; i++) {
            int32_t position = *reinterpret_cast<const int32_t*>(request_data + offset);
            offset += sizeof(int32_t);
            
            if (position >= 0 && position < capacity) {
                bucket& bkt = g_storage->GetBucket(position);
                std::vector<uint8_t> serialized_bkt = serialize_bucket(bkt);
                
                if (!serialized_bkt.empty()) {
                    // ??????
                    response_data.insert(response_data.end(),
                                       reinterpret_cast<uint8_t*>(&position),
                                       reinterpret_cast<uint8_t*>(&position) + sizeof(int32_t));
                    
                    // ??bucket??
                    uint32_t bkt_size = static_cast<uint32_t>(serialized_bkt.size());
                    response_data.insert(response_data.end(),
                                       reinterpret_cast<uint8_t*>(&bkt_size),
                                       reinterpret_cast<uint8_t*>(&bkt_size) + sizeof(uint32_t));
                    
                    // ??bucket??
                    response_data.insert(response_data.end(),
                                       serialized_bkt.begin(), serialized_bkt.end());
                }
            }
        }
        
        return response_data;
        
    } catch (const std::exception& e) {
        std::cerr << "Fetch blocks failed: " << e.what() << std::endl;
        return {};
    }
}

// ?? READ_PATH_FULL ??
std::vector<uint8_t> handleReadPathFull(const uint8_t* request_data, uint32_t data_len) {
    if (!g_storage) {
        std::cout << "Error: ServerStorage not initialized" << std::endl;
        return {};
    }
    
    if (data_len < 4) {
        std::cout << "Error: READ_PATH_FULL request data too short" << std::endl;
        return {};
    }
   
    int32_t leaf_id = *reinterpret_cast<const int32_t*>(request_data);
    
    try {
        std::vector<uint8_t> response_data;
      
        for (int i = 0; i <= OramL; i++) {
            int position = (1 << i) - 1 + (leaf_id >> (OramL - i));
            
            if (position < 0 || position >= capacity) {
                std::cout << "Warning: Invalid bucket position: " << position 
                         << " (leaf_id=" << leaf_id << ", level=" << i << ")" << std::endl;
                continue;
            }
           
            bucket& bkt = g_storage->GetBucket(position);
            std::vector<uint8_t> serialized_bkt = serialize_bucket(bkt);
            
            if (!serialized_bkt.empty()) {
             
                uint32_t pos = static_cast<uint32_t>(position);
                response_data.insert(response_data.end(), 
                                   reinterpret_cast<uint8_t*>(&pos), 
                                   reinterpret_cast<uint8_t*>(&pos) + sizeof(uint32_t));
                
                uint32_t bkt_size = static_cast<uint32_t>(serialized_bkt.size());
                response_data.insert(response_data.end(),
                                   reinterpret_cast<uint8_t*>(&bkt_size),
                                   reinterpret_cast<uint8_t*>(&bkt_size) + sizeof(uint32_t));
                
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

bool handleWritePathFull(const uint8_t* request_data, uint32_t data_len) {
    if (!g_storage) {
        std::cout << "Error: ServerStorage not initialized" << std::endl;
        return false;
    }
    
    size_t offset = 0;
    
    try {
      
        while (offset < data_len) {
            if (offset + 8 > data_len) {
                std::cout << "Error: Incomplete bucket header in WRITE_PATH_FULL" << std::endl;
                return false;
            }
           
            int32_t position = *reinterpret_cast<const int32_t*>(request_data + offset);
            offset += sizeof(int32_t);
            
            uint32_t bucket_size = *reinterpret_cast<const uint32_t*>(request_data + offset);
            offset += sizeof(uint32_t);
            
            if (offset + bucket_size > data_len) {
                std::cout << "Error: Incomplete bucket data in WRITE_PATH_FULL" << std::endl;
                return false;
            }
            
            bucket bkt = deserialize_bucket(request_data + offset, bucket_size);
            offset += bucket_size;
            
            g_storage->SetBucket(position, bkt);
        }
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Write path full failed: " << e.what() << std::endl;
        return false;
    }
}



bool handleWriteBuckets(const uint8_t* request_data, uint32_t data_len) {
    if (!g_storage) {
        std::cout << "Error: ServerStorage not initialized" << std::endl;
        return false;
    }
    
    size_t offset = 0;
    
    try {
        // ????bucket???
        while (offset < data_len) {
            if (offset + 8 > data_len) {
                std::cout << "Error: Incomplete bucket header in WRITE_BUCKETS" << std::endl;
                return false;
            }
            
            // ??????
            int32_t position = *reinterpret_cast<const int32_t*>(request_data + offset);
            offset += sizeof(int32_t);
            
            // ??bucket??
            uint32_t bucket_size = *reinterpret_cast<const uint32_t*>(request_data + offset);
            offset += sizeof(uint32_t);
            
            // ??????????
            if (offset + bucket_size > data_len) {
                std::cout << "Error: Incomplete bucket data in WRITE_BUCKETS" << std::endl;
                return false;
            }
            
            // ????bucket
            bucket bkt = deserialize_bucket(request_data + offset, bucket_size);
            offset += bucket_size;
            
            // ??bucket
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

// ?????????
void handleClient(tcp::socket socket) {
    try {
       
        tcp::no_delay no_delay_opt(true);
        socket.set_option(no_delay_opt);
        
        // 2. ??TCP???? (TCP_QUICKACK)
        int quickack = 1;
        setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
        
        // 3. ??TCP???
        int buf_size = 65536;
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
        
        while (true) {
            // ??1: ?????
            auto t1 = std::chrono::high_resolution_clock::now();
            RequestHeader header;
            
            // ????????QUICKACK
            setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
            boost::asio::read(socket, boost::asio::buffer(&header, sizeof(header)));
            auto t2 = std::chrono::high_resolution_clock::now();
            
            // ??2: ??????
            std::vector<uint8_t> request_data(header.data_len);
            if (header.data_len > 0) {
                setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
                boost::asio::read(socket, boost::asio::buffer(request_data.data(), header.data_len));
            }
            auto t3 = std::chrono::high_resolution_clock::now();
            
            // ??3: ????
            auto t4 = std::chrono::high_resolution_clock::now();
            std::vector<uint8_t> response_data;
            bool success = false;
            
            switch (header.type) {
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
                case WRITE_BUCKETS:
                    success = handleWriteBuckets(request_data.data(), header.data_len);
                    break;
                case FETCH_BLOCKS:    // ??
                    response_data = handleFetchBlocks(request_data.data(), header.data_len);
                    success = !response_data.empty();
                    break;
            }
            auto t5 = std::chrono::high_resolution_clock::now();
            
            // ??4: ????
            ResponseHeader response;
            response.type = RESPONSE;
            response.request_id = header.request_id;
            response.result = success ? 0 : 1;
            response.data_len = response_data.size();
            
            // ?????????????????
            if (response_data.empty()) {
                boost::asio::write(socket, boost::asio::buffer(&response, sizeof(response)));
            } else {
                // ????????
                std::vector<boost::asio::const_buffer> buffers;
                buffers.push_back(boost::asio::buffer(&response, sizeof(response)));
                buffers.push_back(boost::asio::buffer(response_data.data(), response_data.size()));
                boost::asio::write(socket, buffers);
            }
            auto t7 = std::chrono::high_resolution_clock::now();
            
            // ???????
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
    
    // 1. ??? ServerStorage
    try {
        g_storage = std::make_unique<ServerStorage>();
        
        g_storage->setCapacity(capacity);
    }
    catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
    
    // 2. ???????
    try {
        boost::asio::io_context io_context;
        tcp::endpoint endpoint(boost::asio::ip::address::from_string("127.0.0.1"), 12345);
        tcp::acceptor acceptor(io_context, endpoint);
        

        std::cout << "Waiting for client connection..." << std::endl;
        
        tcp::socket socket(io_context);
        acceptor.accept(socket);
        std::cout << "\n=== client connected ===" << std::endl;

        // ?????? handleClient ??
        handleClient(std::move(socket));

        std::cout << "Client disconnected. Server exit." << std::endl;
    }
    catch (std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}