#include "ringoram.h"
#include"CryptoUtil.h"
#include<random>
#include "param.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <memory>
#include <fstream>
#include <chrono>
#include <mutex>
#include <boost/asio.hpp>
#include <memory>
#include <sys/socket.h>
#include <netinet/tcp.h>  


using namespace std;
int ringoram::round = 0;
int ringoram::G = 0;

namespace asio = boost::asio;
using asio::ip::tcp;

// ================================
// 网络通信工具函数
// ================================

// 全局网络连接对象
static std::unique_ptr<tcp::socket> g_socket;
static std::mutex g_socket_mutex;
static uint32_t g_next_request_id = 1;

// 协议定义（和服务器一样）
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


// 初始化网络连接
bool initNetworkConnection(const std::string& server_address, int port) {
    try {
        asio::io_context io_context;  
        tcp::resolver resolver(io_context);
        
        // 解析服务器地址
        auto endpoints = resolver.resolve(server_address, std::to_string(port));
        
        // 创建socket并连接
        auto socket = std::make_unique<tcp::socket>(io_context);
        asio::connect(*socket, endpoints);
        
        std::lock_guard<std::mutex> lock(g_socket_mutex);
        g_socket = std::move(socket);
        
        std::cout << "Connected to server: " << server_address << ":" << port << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to connect to server: " << e.what() << std::endl;
        return false;
    }
}


inline int64_t duration_us(
    const std::chrono::high_resolution_clock::time_point& end,
    const std::chrono::high_resolution_clock::time_point& start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

// 发送请求并接收响应
bool sendRequest(uint32_t type, const uint8_t* request_data, size_t request_len,
                 std::vector<uint8_t>& response_data, std::string& error_msg) {
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    
    if (!g_socket || !g_socket->is_open()) {
        error_msg = "Network connection is not established or disconnected";
        return false;
    }
    
    try {
        // 1. 准备请求头
        RequestHeader req_header;
        req_header.type = type;
        req_header.request_id = g_next_request_id++;
        req_header.data_len = static_cast<uint32_t>(request_len);
        req_header.reserved = 0;
        
        // === 统计发送的数据量 ===
        size_t sent_bytes = sizeof(req_header) + request_len;
        bandwidth += sent_bytes;  // 累加发送字节数
        roundtrip++;  // 每次请求-响应算一个往返
        
        // === TCP优化：设置QUICKACK ===
        int quickack = 1;
        setsockopt(g_socket->native_handle(), IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
        
        // 2. 发送请求
        if (request_len == 0 || !request_data) {
            asio::write(*g_socket, asio::buffer(&req_header, sizeof(req_header)));
        } else {
            std::vector<asio::const_buffer> buffers;
            buffers.push_back(asio::buffer(&req_header, sizeof(req_header)));
            buffers.push_back(asio::buffer(request_data, request_len));
            asio::write(*g_socket, buffers);
        }
        
        // 3. 接收响应头
        setsockopt(g_socket->native_handle(), IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
        ResponseHeader resp_header;
        asio::read(*g_socket, asio::buffer(&resp_header, sizeof(resp_header)));
        
        // 统计接收的头数据
        bandwidth += sizeof(resp_header);  // 累加接收字节数
        
        // 检查响应是否匹配请求
        if (resp_header.request_id != req_header.request_id) {
            error_msg = "Response ID mismatch";
            return false;
        }
        
        if (resp_header.type != RESPONSE) {
            error_msg = "Invalid response type";
            return false;
        }
        
        if (resp_header.result != 0) {
            error_msg = "Server operation failed";
            return false;
        }
        
        // 4. 接收响应数据
        response_data.clear();
        if (resp_header.data_len > 0) {
            setsockopt(g_socket->native_handle(), IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
            response_data.resize(resp_header.data_len);
            asio::read(*g_socket, asio::buffer(response_data.data(), resp_header.data_len));
            
            // 统计接收的数据
            bandwidth += resp_header.data_len;  // 累加接收数据字节数
        }
        
        return true;
    }
    catch (const std::exception& e) {
        error_msg = "Network communication error: " + std::string(e.what());
        return false;
    }
}


bool sendRequest(uint32_t type, const std::vector<uint8_t>& request_data,
                 std::vector<uint8_t>& response_data, std::string& error_msg) {
    return sendRequest(type, request_data.data(), request_data.size(), 
                      response_data, error_msg);
}


// 析构函数
ringoram::~ringoram() {
    if (positionmap) {
        delete[] positionmap;
    }
}


ringoram::ringoram(int n, const std::string& server_ip, int server_port, int cache_levels)
    : N(n), 
      L(static_cast<int>(ceil(log2(N)))), 
      num_bucket((1 << (L + 1)) - 1), 
      num_leaves(1 << L),
      server_ip_(server_ip),
      server_port_(server_port),
      cache_levels(cache_levels)
{
    c = 0;
    
    // 1. 初始化位置映射
    positionmap = new int[N];
    for (int i = 0; i < N; i++) {
        positionmap[i] = get_random();
    }
    
    // 2. 初始化加密
    encryption_key = CryptoUtils::generateRandomKey(16);
    crypto = make_shared<CryptoUtils>(encryption_key);

    // 3. 初始化网络连接
    try {
        initNetwork();
    } catch (const std::exception& e) {
        delete[] positionmap;  // 清理资源
        throw;  // 重新抛出异常
    }
}


// 网络初始化
void ringoram::initNetwork() {

    if (!initNetworkConnection(server_ip_, server_port_)) {
        throw std::runtime_error("Failed to connect to server at " + 
                                 server_ip_ + ":" + std::to_string(server_port_));
    }
}


int ringoram::get_random()
{
	static std::random_device rd;      // 真随机数种子
	static std::mt19937 gen(rd());     // 高质量随机数引擎
	std::uniform_int_distribution<int> dist(0, num_leaves - 1);
	return dist(gen);
}

int ringoram::Path_bucket(int leaf, int level)
{

	int result = (1 << level) - 1 + (leaf >> (this->L - level));

	// 添加边界检查
	if (result < 0 || result >= num_bucket) {
		std::cerr << "ERROR: Path_bucket calculated invalid position: " << result
			<< " (leaf=" << leaf << ", level=" << level
			<< ", num_bucket=" << num_bucket << ")" << std::endl;
		// 返回一个安全的默认值
		return 0;
	}

	return result;

}

int ringoram::GetlevelFromPos(int pos)
{
	return (int)floor(log2(pos + 1));
}

block ringoram::FindBlock(bucket bkt, int offset)
{
	return bkt.blocks[offset];
}

int ringoram::GetBlockOffset(bucket bkt, int blockindex)
{

	int i;
	for (i = 0; i < (realBlockEachbkt + dummyBlockEachbkt); i++)
	{
		if (bkt.ptrs[i] == blockindex && bkt.valids[i] == 1) return i;
	}

	return bkt.GetDummyblockOffset();
}


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
        // std::cerr << "  ERROR: Data too small for header" << std::endl;
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
        // std::cout << "  WARNING: No ptrs and valids data in serialized bucket" << std::endl;
    }
   
    return result;
}



ringoram::ReadPathResult ringoram::ReadPath(int leafid, int blockindex)
{
    ReadPathResult result;
    result.target_block = dummyBlock;
    
    try
    {
       
       
        
        // === 第一步通信：获取路径上所有bucket的元数据 ===
        std::vector<uint8_t> request_data(8); // 4字节leaf_id + 4字节block_index
        *reinterpret_cast<int32_t*>(request_data.data()) = leafid;
        *reinterpret_cast<int32_t*>(request_data.data() + 4) = blockindex;
        
        // 发送 READ_PATH 请求获取元数据
        std::vector<uint8_t> metadata_response;
        std::string error_msg;
        
        if (!sendRequest(READ_PATH, request_data, metadata_response, error_msg)) {
            std::cerr << "Failed to read path metadata: " << error_msg << std::endl;
            return result;
        }
        
        // 解析元数据响应
        size_t offset = 0;
        
        // 解析block_index（用于验证）
        if (offset + 4 > metadata_response.size()) {
            std::cerr << "Invalid metadata response" << std::endl;
            return result;
        }
        int32_t resp_blockindex = *reinterpret_cast<const int32_t*>(metadata_response.data() + offset);
        offset += 4;
        
        if (resp_blockindex != blockindex) {
            std::cerr << "Block index mismatch in metadata response" << std::endl;
            return result;
        }
        
        // 解析需要洗牌的bucket数量
        if (offset + 4 > metadata_response.size()) {
            std::cerr << "Invalid metadata response (no shuffle count)" << std::endl;
            return result;
        }
        uint32_t shuffle_count = *reinterpret_cast<const uint32_t*>(metadata_response.data() + offset);
        offset += 4;
        
        // 存储每个bucket的元数据，用于查找目标块位置
        struct BucketMeta {
            int position;
            int count;
            std::vector<int> ptrs;
            std::vector<int> valids;
        };
        
        std::vector<BucketMeta> path_metadata;
        std::vector<int> need_shuffle_positions;
        
        // 解析路径上每个bucket的元数据
        while (offset < metadata_response.size()) {
            if (offset + 8 > metadata_response.size()) break;
            
            // 读取位置
            int32_t position = *reinterpret_cast<const int32_t*>(metadata_response.data() + offset);
            offset += sizeof(int32_t);
            
            // 读取元数据大小
            uint32_t meta_size = *reinterpret_cast<const uint32_t*>(metadata_response.data() + offset);
            offset += sizeof(uint32_t);
            
            if (offset + meta_size > metadata_response.size()) break;
            
            // 解析bucket元数据
            BucketMeta meta;
            meta.position = position;
            
            size_t meta_offset = 0;
            
            // 解析count
            meta.count = *reinterpret_cast<const int32_t*>(metadata_response.data() + offset + meta_offset);
            meta_offset += sizeof(int32_t);
            
            int num_slots = realBlockEachbkt + dummyBlockEachbkt;
            
            // 解析ptrs
            meta.ptrs.resize(num_slots);
            for (int i = 0; i < num_slots && meta_offset + 4 <= meta_size; i++) {
                meta.ptrs[i] = *reinterpret_cast<const int32_t*>(metadata_response.data() + offset + meta_offset);
                meta_offset += sizeof(int32_t);
            }
            
            // 解析valids
            meta.valids.resize(num_slots);
            for (int i = 0; i < num_slots && meta_offset + 4 <= meta_size; i++) {
                meta.valids[i] = *reinterpret_cast<const int32_t*>(metadata_response.data() + offset + meta_offset);
                meta_offset += sizeof(int32_t);
            }
            
            path_metadata.push_back(meta);
            
            // 检查是否需要洗牌
            if (meta.count >= dummyBlockEachbkt) {
                need_shuffle_positions.push_back(position);
            }
            
            offset += meta_size;
        }
        
        // === 客户端处理：找到目标块的位置和需要洗牌的bucket ===
        int target_position = -1;
        int target_offset = -1;
        
        for (const auto& meta : path_metadata) {
            for (int i = 0; i < meta.ptrs.size(); i++) {
                if (meta.ptrs[i] == blockindex && meta.valids[i] == 1) {
                    target_position = meta.position;
                    target_offset = i;
                    break;
                }
            }
            if (target_position != -1) break;
        }
        
        if (target_position == -1) {
            // 没找到目标块，选择一个dummy块的位置
            for (const auto& meta : path_metadata) {
                for (int i = 0; i < meta.ptrs.size(); i++) {
                    if (meta.ptrs[i] == -1 && meta.valids[i] == 1) {
                        target_position = meta.position;
                        target_offset = i;
                        break;
                    }
                }
                if (target_position != -1) break;
            }
        }
        
        // === 第二步通信：获取实际数据 ===
        std::vector<uint8_t> fetch_request;
        
        // 添加目标块位置
        fetch_request.insert(fetch_request.end(),
                           reinterpret_cast<uint8_t*>(&target_position),
                           reinterpret_cast<uint8_t*>(&target_position) + sizeof(int32_t));
        fetch_request.insert(fetch_request.end(),
                           reinterpret_cast<uint8_t*>(&target_offset),
                           reinterpret_cast<uint8_t*>(&target_offset) + sizeof(int32_t));
        
        // 添加需要洗牌的bucket列表
        uint32_t fetch_shuffle_count = static_cast<uint32_t>(need_shuffle_positions.size());
        fetch_request.insert(fetch_request.end(),
                           reinterpret_cast<uint8_t*>(&fetch_shuffle_count),
                           reinterpret_cast<uint8_t*>(&fetch_shuffle_count) + sizeof(uint32_t));
        
        for (int pos : need_shuffle_positions) {
            fetch_request.insert(fetch_request.end(),
                               reinterpret_cast<uint8_t*>(&pos),
                               reinterpret_cast<uint8_t*>(&pos) + sizeof(int32_t));
        }
        
        // 发送 FETCH_BLOCKS 请求获取实际数据
        std::vector<uint8_t> data_response;
        if (!sendRequest(FETCH_BLOCKS, fetch_request, data_response, error_msg)) {
            std::cerr << "Failed to fetch blocks: " << error_msg << std::endl;
            return result;
        }
        
        // 解析数据响应
        offset = 0;
        
        // 解析目标块
        if (data_response.size() < 1) {
            return result;
        }
        
        bool target_is_dummy = data_response[offset] == 1;
        offset += 1;
        
        if (!target_is_dummy && offset + 4 <= data_response.size()) {
            uint32_t target_data_size = *reinterpret_cast<const uint32_t*>(data_response.data() + offset);
            offset += 4;
            
            if (offset + target_data_size <= data_response.size()) {
                std::vector<char> encrypted_data(
                    data_response.begin() + offset, 
                    data_response.begin() + offset + target_data_size
                );
                offset += target_data_size;
                
                result.target_block = block(leafid, blockindex, encrypted_data);
            }
        }
        
        // 解析需要洗牌的buckets
        while (offset < data_response.size()) {
            if (offset + 8 > data_response.size()) break;
            
            int32_t position = *reinterpret_cast<const int32_t*>(data_response.data() + offset);
            offset += sizeof(int32_t);
            
            uint32_t bucket_size = *reinterpret_cast<const uint32_t*>(data_response.data() + offset);
            offset += sizeof(uint32_t);
            
            if (offset + bucket_size > data_response.size()) break;
            
            bucket bkt = deserialize_bucket(data_response.data() + offset, bucket_size);
            offset += bucket_size;
            
            result.need_shuffle_buckets.emplace_back(position, bkt);
        }
        
        return result;
    }
    catch(const std::exception& e)
    {
        // std::cerr << "Exception in ReadPath: " << e.what() << std::endl;
        return result;
    }
}

std::vector<bucket> ringoram::ReadPathFull(int leaf_id) {
    try {
        // 准备请求数据
        std::vector<uint8_t> request_data(sizeof(int32_t));
        *reinterpret_cast<int32_t*>(request_data.data()) = leaf_id;
        
        // 发送 READ_PATH_FULL 请求
        std::vector<uint8_t> response_data;
        std::string error_msg;
        
        if (!sendRequest(READ_PATH_FULL, request_data, response_data, error_msg)) {
            std::cerr << "Failed to read path full (network): " << error_msg << std::endl;
            return {};
        }
        
        // 解析响应
        std::vector<bucket> result;
        size_t offset = 0;
        
        while (offset < response_data.size()) {
            if (offset + 8 > response_data.size()) break;
            
            // 读取位置信息
            int32_t position = *reinterpret_cast<const int32_t*>(response_data.data() + offset);
            offset += sizeof(int32_t);
            
            // 读取bucket大小
            uint32_t bucket_size = *reinterpret_cast<const uint32_t*>(response_data.data() + offset);
            offset += sizeof(uint32_t);
            
            if (offset + bucket_size > response_data.size()) break;
            
            // 反序列化bucket
            bucket bkt = deserialize_bucket(response_data.data() + offset, bucket_size);
            offset += bucket_size;
            
            result.push_back(bkt);
        }
        
        return result;
        
    } catch (const std::exception& e) {
        std::cerr << "Exception in ReadPathFull: " << e.what() << std::endl;
        return {};
    }
}

// 批量写入路径上的所有bucket
bool ringoram::WritePathFull(int leaf_id, const std::vector<bucket>& buckets_to_write) {
    try {
        // 准备请求数据
        std::vector<uint8_t> request_data;
        
        // 将每个bucket序列化并添加到请求数据
        for (size_t i = 0; i <= L; i++) {
            int position = Path_bucket(leaf_id, i);
            
            if (i < buckets_to_write.size()) {
                // 序列化bucket
                std::vector<uint8_t> serialized_bkt = serialize_bucket(
                    const_cast<bucket&>(buckets_to_write[i])
                );
                
                if (!serialized_bkt.empty()) {
                    // 添加位置信息
                    request_data.insert(request_data.end(),
                                      reinterpret_cast<uint8_t*>(&position),
                                      reinterpret_cast<uint8_t*>(&position) + sizeof(int32_t));
                    
                    // 添加bucket大小
                    uint32_t bkt_size = static_cast<uint32_t>(serialized_bkt.size());
                    request_data.insert(request_data.end(),
                                      reinterpret_cast<uint8_t*>(&bkt_size),
                                      reinterpret_cast<uint8_t*>(&bkt_size) + sizeof(uint32_t));
                    
                    // 添加bucket数据
                    request_data.insert(request_data.end(),
                                      serialized_bkt.begin(),
                                      serialized_bkt.end());
                }
            }
        }
        
        // 发送 WRITE_PATH_FULL 请求
        std::vector<uint8_t> response_data;
        std::string error_msg;
        
        return sendRequest(WRITE_PATH_FULL, request_data, response_data, error_msg);
        
    } catch (const std::exception& e) {
        std::cerr << "Exception in WritePathFull: " << e.what() << std::endl;
        return false;
    }
}

void ringoram::EvictPath() {
    int l = G % (1 << L);
    G += 1;

    try {
        // 1. 一次性读取路径上的所有bucket
        std::vector<bucket> path_buckets = ReadPathFull(l);
        
        // 2. 处理读取到的所有bucket
        for (auto& bkt : path_buckets) {
            // 将real blocks添加到stash（解密数据）
            for (int j = 0; j < maxblockEachbkt; j++) {
                if (bkt.ptrs[j] != -1 && bkt.valids[j] && !bkt.blocks[j].IsDummy()) {
                    block encrypted_block = bkt.blocks[j];
                    
                    // 解密数据
                    vector<char> decrypted_data = decrypt_data(encrypted_block.GetData());
                    
                    // 创建解密后的block对象
                    block decrypted_block(encrypted_block.GetLeafid(),
                                          encrypted_block.GetBlockindex(),
                                          decrypted_data);
                    
                    // 添加到stash
                    stash.push_back(decrypted_block);
                }
            }
        }
        
        // 3. 准备要写回的bucket
        std::vector<bucket> buckets_to_write;
        
        for (int i = 0; i <= L; i++) {
            int position = Path_bucket(l, i);
            int level = GetlevelFromPos(position);
            vector<block> blocksTobucket;
            
            // 从stash中选择可以放在这个bucket的块
            for (auto it = stash.begin(); it != stash.end() && blocksTobucket.size() < realBlockEachbkt; ) {
                int target_leaf = it->GetLeafid();
                int target_bucket_pos = Path_bucket(target_leaf, level);
                if (target_bucket_pos == position) {
                    // 对要写回当前bucket的块进行加密
                    if (!it->IsDummy()) {
                        vector<char> plain_data = it->GetData();
                        vector<char> encrypted_data = encrypt_data(plain_data);
                        
                        block encrypted_block(it->GetLeafid(), it->GetBlockindex(), encrypted_data);
                        blocksTobucket.push_back(encrypted_block);
                    }
                    it = stash.erase(it);
                } else {
                    ++it;
                }
            }
            
            // 填充dummy块
            while (blocksTobucket.size() < realBlockEachbkt + dummyBlockEachbkt) {
                blocksTobucket.push_back(dummyBlock);
            }
            
            // 随机排列
            std::random_device rd;
            std::mt19937 g(rd());
            std::shuffle(blocksTobucket.begin(), blocksTobucket.end(), g);
            
            // 创建bucket
            bucket bktTowrite(realBlockEachbkt, dummyBlockEachbkt);
            bktTowrite.blocks = blocksTobucket;
            
            for (int j = 0; j < maxblockEachbkt; j++) {
                bktTowrite.ptrs[j] = bktTowrite.blocks[j].GetBlockindex();
                bktTowrite.valids[j] = 1;
            }
            bktTowrite.count = 0;
            
            buckets_to_write.push_back(bktTowrite);
        }
        
        // 4. 一次性写入所有bucket
        WritePathFull(l, buckets_to_write);
        
    } catch (const std::exception& e) {
        std::cerr << "Exception in EvictPath: " << e.what() << std::endl;
    }
}




void ringoram::EarlyReshuffle(int l, const std::vector<std::pair<int, bucket>>& buckets_to_process)
{
    
    // 2. 如果没有传入需要处理的服务器端buckets，直接返回
    if (buckets_to_process.empty()) {
        return;
    }
  
    // 3. 处理传入的需要洗牌的buckets
    std::vector<std::pair<int, bucket>> buckets_to_write;
    
    for (auto& [position, bkt] : buckets_to_process) {
        int level = GetlevelFromPos(position);
        
        // 将有效块添加到stash（解密）
        for (int j = 0; j < maxblockEachbkt; j++) {
            if (bkt.ptrs[j] != -1 && bkt.valids[j] && !bkt.blocks[j].IsDummy()) {
                block encrypted_block = bkt.blocks[j];
                
                // 解密数据
                vector<char> decrypted_data = decrypt_data(encrypted_block.GetData());
                
                // 创建解密后的block对象
                block decrypted_block(encrypted_block.GetLeafid(),
                                        encrypted_block.GetBlockindex(),
                                        decrypted_data);
                
                // 添加到stash
                stash.push_back(decrypted_block);
            }
        }
        
        // 准备要写回的blocks
        vector<block> blocksToBucket;
        
        // 从stash中选择可以放在这个bucket的块
        for (auto it = stash.begin(); it != stash.end() && blocksToBucket.size() < realBlockEachbkt; ) {
            int target_leaf = it->GetLeafid();
            int target_bucket_pos = Path_bucket(target_leaf, level);
            
            if (target_bucket_pos == position) {
                // 对要写回当前bucket的块进行加密
                if (!it->IsDummy()) {
                    vector<char> plain_data = it->GetData();
                    vector<char> encrypted_data = encrypt_data(plain_data);
                    
                    block encrypted_block(it->GetLeafid(), it->GetBlockindex(), encrypted_data);
                    blocksToBucket.push_back(encrypted_block);
                } else {
                    blocksToBucket.push_back(*it);
                }
                it = stash.erase(it);
            } else {
                ++it;
            }
        }
        
        // 填充dummy块
        while (blocksToBucket.size() < realBlockEachbkt + dummyBlockEachbkt) {
            blocksToBucket.push_back(dummyBlock);
        }
        
        // 随机排列
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(blocksToBucket.begin(), blocksToBucket.end(), g);
        
        // 创建新的bucket
        bucket new_bkt(realBlockEachbkt, dummyBlockEachbkt);
        new_bkt.blocks = blocksToBucket;
        
        for (int j = 0; j < maxblockEachbkt; j++) {
            new_bkt.ptrs[j] = new_bkt.blocks[j].GetBlockindex();
            new_bkt.valids[j] = 1;
        }
        new_bkt.count = 0;
        
        buckets_to_write.emplace_back(position, new_bkt);
    }
    
    // 4. 批量写回所有bucket
    if (!buckets_to_write.empty()) {
        std::vector<uint8_t> write_request_data;
        
        for (auto& [position, bkt] : buckets_to_write) {
            std::vector<uint8_t> serialized_bkt = serialize_bucket(bkt);
            
            if (!serialized_bkt.empty()) {
                write_request_data.insert(write_request_data.end(),
                                        reinterpret_cast<uint8_t*>(&position),
                                        reinterpret_cast<uint8_t*>(&position) + sizeof(int32_t));
                
                uint32_t bkt_size = static_cast<uint32_t>(serialized_bkt.size());
                write_request_data.insert(write_request_data.end(),
                                            reinterpret_cast<uint8_t*>(&bkt_size),
                                            reinterpret_cast<uint8_t*>(&bkt_size) + sizeof(uint32_t));
                
                write_request_data.insert(write_request_data.end(),
                                            serialized_bkt.begin(),
                                            serialized_bkt.end());
            }
        }
        
        std::string error_msg;
        std::vector<uint8_t> write_response;
        if (!sendRequest(WRITE_BUCKETS, write_request_data, write_response, error_msg)) {
            std::cerr << "Failed to write buckets: " << error_msg << std::endl;
        }
    }
}


std::vector<char> ringoram::encrypt_data(const std::vector<char>& data)
{
	if (!crypto || data.empty()) return data;

	vector<uint8_t> data_u8(data.begin(), data.end());
	auto encrypted_u8 = crypto->encrypt(data_u8);
	return vector<char>(encrypted_u8.begin(), encrypted_u8.end());
}

std::vector<char> ringoram::decrypt_data(const std::vector<char>& encrypted_data)
{
	if (!crypto || encrypted_data.empty()) return encrypted_data;

	if (encrypted_data.size() % 16 != 0) {
		cerr << "[DECRYPT] ERROR: Size " << encrypted_data.size() << " not multiple of 16" << endl;
		return encrypted_data;
	}

	try {
		vector<uint8_t> encrypted_u8(encrypted_data.begin(), encrypted_data.end());
		auto decrypted_u8 = crypto->decrypt(encrypted_u8);
		return vector<char>(decrypted_u8.begin(), decrypted_u8.end());
	}
	catch (const exception& e) {
		cerr << "[DECRYPT] ERROR: " << e.what() << endl;
		return encrypted_data;
	}
}



vector<char> ringoram::access(int blockindex, Operation op, vector<char> data)
{
    
    auto access_start = std::chrono::high_resolution_clock::now();
    if (blockindex < 0 || blockindex >= N) {
        return {};
    }

    int oldLeaf = positionmap[blockindex];
    positionmap[blockindex] = get_random();

    vector<char> blockdata;
    bool found_in_stash = false;

    // 1. 先在 stash 中查找
    for (auto it = stash.begin(); it != stash.end(); ++it) {
        if (it->GetBlockindex() == blockindex) {
            blockdata = it->GetData();   // stash中已经是明文
            stash.erase(it);
            found_in_stash = true;
            break;
        }
    }

    // 2. 如果 stash 中没有找到，再从路径中读取
    if (!found_in_stash) {
        // 读取路径，同时获取需要洗牌的buckets
        ReadPathResult path_result = ReadPath(oldLeaf, blockindex);

        // 处理从路径读取到的块
        if (path_result.target_block.GetBlockindex() == blockindex) {
            // 从路径读取到的目标块，需要解密
            if (!path_result.target_block.IsDummy()) {
                blockdata = decrypt_data(path_result.target_block.GetData());
            }
            else {
                blockdata = path_result.target_block.GetData();
            }
        }

        // 使用ReadPath中获取的需要洗牌的buckets进行EarlyReshuffle
        EarlyReshuffle(oldLeaf, path_result.need_shuffle_buckets);
    }

    // 3. 如果是WRITE操作，更新数据
    if (op == WRITE) {
        blockdata = data;
    }

    // 明文放入stash
    stash.emplace_back(positionmap[blockindex], blockindex, blockdata);

    // 4. 路径管理和驱逐
    round = (round + 1) % EvictRound;
    if (round == 0) EvictPath();

    auto access_end = std::chrono::high_resolution_clock::now();
    // std::cout << "access time: " 
    //           << std::chrono::duration<double, std::milli>(access_end - access_start).count()
    //           << " ms" << std::endl;

    return blockdata;
}