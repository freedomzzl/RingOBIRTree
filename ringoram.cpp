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
    READ_BUCKET = 1,
    WRITE_BUCKET = 2,
    READ_PATH = 3,
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
        auto total_start = std::chrono::high_resolution_clock::now();
        
        // 1. 准备请求头
        RequestHeader req_header;
        req_header.type = type;
        req_header.request_id = g_next_request_id++;
        req_header.data_len = static_cast<uint32_t>(request_len);
        req_header.reserved = 0;
        
        // === TCP优化：设置QUICKACK ===
        int quickack = 1;
        setsockopt(g_socket->native_handle(), IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
        
        // 2. 发送请求（一次性发送头和数据）
        auto send_start = std::chrono::high_resolution_clock::now();
        
        if (request_len == 0 || !request_data) {
            // 只有请求头
            asio::write(*g_socket, asio::buffer(&req_header, sizeof(req_header)));
        } else {
            // 合并发送头和数据
            std::vector<asio::const_buffer> buffers;
            buffers.push_back(asio::buffer(&req_header, sizeof(req_header)));
            buffers.push_back(asio::buffer(request_data, request_len));
            asio::write(*g_socket, buffers);
        }
        auto send_end = std::chrono::high_resolution_clock::now();
        
        // 3. 接收响应头
        auto recv_start = std::chrono::high_resolution_clock::now();
        
        // 接收前设置QUICKACK
        setsockopt(g_socket->native_handle(), IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
        ResponseHeader resp_header;
        asio::read(*g_socket, asio::buffer(&resp_header, sizeof(resp_header)));
        
        // 检查响应是否匹配请求
        if (resp_header.request_id != req_header.request_id) {
            error_msg = "Response ID mismatch: request=" + std::to_string(req_header.request_id)
                      + ", response=" + std::to_string(resp_header.request_id);
            return false;
        }
        
        if (resp_header.type != RESPONSE) {
            error_msg = "Invalid response type: " + std::to_string(resp_header.type);
            return false;
        }
        
        if (resp_header.result != 0) {
            error_msg = "Server operation failed, error code: " + std::to_string(resp_header.result);
            return false;
        }
        
        // 4. 接收响应数据
        response_data.clear();
        if (resp_header.data_len > 0) {
            // 接收前设置QUICKACK
            setsockopt(g_socket->native_handle(), IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
            response_data.resize(resp_header.data_len);
            asio::read(*g_socket, asio::buffer(response_data.data(), resp_header.data_len));
        }
        auto recv_end = std::chrono::high_resolution_clock::now();
        auto total_end = recv_end;
        
        // 输出客户端视角的耗时
        // std::cout << "[CLIENT] Type:" << type 
        //           << " Send:" << duration_us(send_end, send_start)
        //           << " Recv:" << duration_us(recv_end, recv_start)
        //           << " Total:" << duration_us(total_end, total_start) << "us" << std::endl;
        
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
// ringoram::~ringoram() {
//     if (positionmap) {
//         delete[] positionmap;
//     }
// }


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
    
    cout << "[ORAM] Server: " << server_ip_ << ":" << server_port_ << endl;
    cout << "[ORAM] Tree depth L = " << L << endl;
    cout << "[ORAM] Number of buckets = " << num_bucket << endl;
    cout << "[ORAM] Number of leaves = " << num_leaves << endl;
    cout << "[ORAM] Cache levels = " << cache_levels << endl;
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

void serialize_block(const block& blk, uint8_t* buffer, size_t& offset) {
  
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


std::vector<uint8_t> serialize_bucket(const bucket& bkt) {
    const size_t FIXED_SIZE = 65536;  // 固定大小
    
    try {
        // 分配固定大小的缓冲区
        std::vector<uint8_t> result(FIXED_SIZE, 0);
        
        // 序列化 bucket header
        SerializedBucketHeader* bucket_header = 
            reinterpret_cast<SerializedBucketHeader*>(result.data());
        bucket_header->Z = bkt.Z;
        bucket_header->S = bkt.S;
        bucket_header->count = bkt.count;
        bucket_header->num_blocks = static_cast<int32_t>(bkt.blocks.size());
        
        size_t offset = sizeof(SerializedBucketHeader);
        
        // 序列化 blocks
        for (int i = 0; i < bkt.blocks.size() && offset < FIXED_SIZE; i++) {
            serialize_block(bkt.blocks[i], result.data(), offset);
        }
        
        // 序列化 ptrs 和 valids
        int num_slots = bkt.Z + bkt.S;
        
        // 序列化 ptrs
        for (int i = 0; i < num_slots && offset < FIXED_SIZE; i++) {
            *reinterpret_cast<int32_t*>(result.data() + offset) = bkt.ptrs[i];
            offset += sizeof(int32_t);
        }
        
        // 序列化 valids
        for (int i = 0; i < num_slots && offset < FIXED_SIZE; i++) {
            *reinterpret_cast<int32_t*>(result.data() + offset) = bkt.valids[i];
            offset += sizeof(int32_t);
        }
        
        // 填充剩余空间为0（可选）
        // if (offset < FIXED_SIZE) {
        //     memset(result.data() + offset, 0, FIXED_SIZE - offset);
        // }
        
        return result;
        
    } catch (const std::exception& e) {
        std::cerr << "serialize_bucket failed: " << e.what() << std::endl;
        return std::vector<uint8_t>(FIXED_SIZE, 0);  // 返回固定大小的空数据
    }
}

bucket deserialize_bucket(const uint8_t* data, size_t size) {
    const size_t FIXED_SIZE = 65536;  // 固定大小
    
    // 验证数据大小
    if (size < FIXED_SIZE) {
        std::cerr << "WARNING: Data size " << size << " < " << FIXED_SIZE 
                  << ", using padded data" << std::endl;
        // 可以在这里创建一个填充数据的副本
    }
    
    if (size < sizeof(SerializedBucketHeader)) {
        std::cerr << "ERROR: Data too small for header" << std::endl;
        throw std::runtime_error("Invalid bucket data: too small");
    }
    
    const SerializedBucketHeader* bucket_header = 
        reinterpret_cast<const SerializedBucketHeader*>(data);
    
    // 创建空的bucket
    bucket result(0, 0);
    result.Z = bucket_header->Z;
    result.S = bucket_header->S;
    result.count = bucket_header->count;
    
    size_t offset = sizeof(SerializedBucketHeader);
    
    // 反序列化 blocks（使用实际块数，而不是header中的num_blocks）
    int num_blocks = std::min(bucket_header->num_blocks, 11);  // 假设最大11个块
    for (int i = 0; i < num_blocks && offset < size; i++) {
        result.blocks.push_back(deserialize_block(data, offset));
    }
    
    // 反序列化 ptrs 和 valids
    int num_slots = result.Z + result.S;
    result.ptrs.resize(num_slots, -1);
    result.valids.resize(num_slots, 0);
    
    // 反序列化 ptrs
    for (int i = 0; i < num_slots && offset < size; i++) {
        result.ptrs[i] = *reinterpret_cast<const int32_t*>(data + offset);
        offset += sizeof(int32_t);
    }
    
    // 反序列化 valids
    for (int i = 0; i < num_slots && offset < size; i++) {
        result.valids[i] = *reinterpret_cast<const int32_t*>(data + offset);
        offset += sizeof(int32_t);
    }
    
    return result;
}

void ringoram::ReadBucket(int pos)
{
    try
    {
        // 1. 准备请求数据（桶位置）
        std::vector<uint8_t> request_data(sizeof(int32_t));
        *reinterpret_cast<int32_t*>(request_data.data()) = static_cast<int32_t>(pos);
        
        // 2. 发送 READ_BUCKET 请求
        std::vector<uint8_t> response_data;
        std::string error_msg;
        
        if (!sendRequest(READ_BUCKET, request_data, response_data, error_msg)) {
            std::cerr << "Failed to read bucket (network): " << error_msg << std::endl;
            return;
        }
        
        // 3. 检查响应数据大小
        if (response_data.empty()) {
            std::cerr << "Empty response for bucket read" << std::endl;
            return;
        }

        // 4. 反序列化bucket
        bucket remote_bkt = deserialize_bucket(response_data.data(), response_data.size());

        // 5. 将real blocks添加到stash（解密数据）
        for (int j = 0; j < maxblockEachbkt; j++) {
            if (remote_bkt.ptrs[j] != -1 && remote_bkt.valids[j] && 
                !remote_bkt.blocks[j].IsDummy()) {
                
                block encrypted_block = remote_bkt.blocks[j];
                
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
    catch(const std::exception& e)
    {
        std::cerr << "Exception in ReadBucket: " << e.what() << std::endl;
    }
}

bucket ringoram::Read_bucket(int pos)
{
    try
    {
        // 1. 准备请求数据（桶位置）
        std::vector<uint8_t> request_data(sizeof(int32_t));
        *reinterpret_cast<int32_t*>(request_data.data()) = static_cast<int32_t>(pos);
        
        // 2. 发送 READ_BUCKET 请求
        std::vector<uint8_t> response_data;
        std::string error_msg;
        
        if (!sendRequest(READ_BUCKET, request_data, response_data, error_msg)) {
            std::cerr << "Failed to read bucket (network): " << error_msg << std::endl;
            return bucket();
        }
        
        // 3. 检查响应数据大小
        if (response_data.empty()) {
            std::cerr << "Empty response for bucket read" << std::endl;
            return bucket();
        }

        // 4. 反序列化bucket
        bucket remote_bkt = deserialize_bucket(response_data.data(), response_data.size());

        return remote_bkt;
    }
    catch(const std::exception& e)
    {
        std::cerr << "Exception in ReadBucket: " << e.what() << std::endl;
        return bucket();
    }   
}


void ringoram::WriteBucket(int position)
{
	try
    {
        int level = GetlevelFromPos(position);
        vector<block> blocksTobucket;

        // 从stash中选择可以放在这个bucket的块
        for (auto it = stash.begin(); it != stash.end() && blocksTobucket.size() < realBlockEachbkt; ) {
	        int target_leaf = it->GetLeafid();
	        int target_bucket_pos = Path_bucket(target_leaf, level);
	        if (target_bucket_pos == position) {
		        // 对要写回当前bucket的块进行加密
		        if (!it->IsDummy()) {
			        vector<char> plain_data = it->GetData();  // 当前是明文
			        vector<char> encrypted_data = encrypt_data(plain_data);

			        // 创建加密后的block
			        block encrypted_block(it->GetLeafid(), it->GetBlockindex(), encrypted_data);
			        blocksTobucket.push_back(encrypted_block);
		        }
		        it = stash.erase(it);
	        }
	        else {
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
        
        // 创建新的bucket
        bucket bktTowrite(realBlockEachbkt, dummyBlockEachbkt);
        bktTowrite.blocks = blocksTobucket;

        for (int i = 0; i < maxblockEachbkt; i++) {
            bktTowrite.ptrs[i] = bktTowrite.blocks[i].GetBlockindex();
            bktTowrite.valids[i] = 1;
        }
        bktTowrite.count = 0;

        // 序列化bucket
        std::vector<uint8_t> serialized_bkt = serialize_bucket(bktTowrite);
        
        const size_t FIXED_SIZE = 65536;
        if (serialized_bkt.size() != FIXED_SIZE) {
            std::cerr << "WARNING: Serialized bucket size is " 
                      << serialized_bkt.size() << ", expected " << FIXED_SIZE << std::endl;
            // 调整大小
            serialized_bkt.resize(FIXED_SIZE, 0);
        }

        // 准备请求数据：位置 + bucket数据
        std::vector<uint8_t> request_data(sizeof(int32_t) + FIXED_SIZE);
        *reinterpret_cast<int32_t*>(request_data.data()) = static_cast<int32_t>(position);
        memcpy(request_data.data() + sizeof(int32_t), 
               serialized_bkt.data(), FIXED_SIZE);
        
        // 发送请求
        std::vector<uint8_t> response_data;
        std::string error_msg;
        
        if (!sendRequest(WRITE_BUCKET, request_data, response_data, error_msg)) {
            std::cerr << "Failed to write bucket: " << error_msg << std::endl;
        }
    }
    catch(const std::exception& e) {
        std::cerr << "Exception in WriteBucket: " << e.what() << std::endl;
    }
}

block ringoram::ReadPath(int leafid, int blockindex)
{
	try
    {
        // 1. 准备请求数据
        std::vector<uint8_t> request_data(8); // 4字节leaf_id + 4字节block_index
        *reinterpret_cast<int32_t*>(request_data.data()) = leafid;
        *reinterpret_cast<int32_t*>(request_data.data() + 4) = blockindex;
        
        // 2. 发送 READ_PATH 请求
        std::vector<uint8_t> response_data;
        std::string error_msg;
        
        if (!sendRequest(READ_PATH, request_data, response_data, error_msg)) {
            std::cerr << "Failed to read path (network): " << error_msg << std::endl;
            return dummyBlock;
        }
        
        // 3. 解析响应
        if (response_data.size() < 1) {
            return dummyBlock;
        }

        // 第一个字节：是否是dummy块
        bool is_dummy = response_data[0] == 1;
        
        if (!is_dummy && response_data.size() > 1) {
            // 解析block数据
            std::vector<char> encrypted_data(response_data.begin() + 1, response_data.end());
            // std::vector<char> decrypted_data = decrypt_data(encrypted_data);
            
            // 创建block对象
            block result(leafid, blockindex, encrypted_data);
  
            return result;
        }
        return dummyBlock;
    }
    catch(const std::exception& e)
    {
        std::cerr << "Exception in ReadPath (network): " << e.what() << std::endl;
        return dummyBlock;
    }
    
}



void ringoram::EvictPath()
{
	int l = G % (1 << L);
	G += 1;

	int i;
	for (i = 0; i <= L; i++)
	{
		ReadBucket(Path_bucket(l, i));
	}

	for (i = L; i >= 0; i--)
	{
		WriteBucket(Path_bucket(l, i));
	}
}

void ringoram::EarlyReshuffle(int l)
{
	for (int i = 0; i <= L; i++)
	{
		int position = Path_bucket(l, i);
        bucket bkt=Read_bucket(position);

        if (bkt.count >= dummyBlockEachbkt)
        {
            for (int j = 0; j < maxblockEachbkt; j++) {
		        // 只读取真实且有效的块
		        if (bkt.ptrs[j] != -1 && bkt.valids[j] && !bkt.blocks[j].IsDummy()) {
			        // 读取时解密
			        block encrypted_block = bkt.blocks[j];
			        vector<char> decrypted_data = decrypt_data(encrypted_block.GetData());
			        block decrypted_block(encrypted_block.GetLeafid(), encrypted_block.GetBlockindex(), decrypted_data);
			        stash.push_back(decrypted_block);
		        }
	        }
            WriteBucket(position);
        }
	}
}

std::vector<char> ringoram::encrypt_data(const std::vector<char>& data)
{
	if (!crypto || data.empty()) return data;

	// 像ringoram一样直接转换和加密
	vector<uint8_t> data_u8(data.begin(), data.end());
	auto encrypted_u8 = crypto->encrypt(data_u8);
	return vector<char>(encrypted_u8.begin(), encrypted_u8.end());
}

std::vector<char> ringoram::decrypt_data(const std::vector<char>& encrypted_data)
{
	if (!crypto || encrypted_data.empty()) return encrypted_data;

	// 像ringoram一样检查数据大小
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
	if (blockindex < 0 || blockindex >= N) {

		return {};
	}

	int oldLeaf = positionmap[blockindex];
	positionmap[blockindex] = get_random();

	// 1. 读取路径获取目标块（加密状态）
	block interestblock = ReadPath(oldLeaf, blockindex);
	vector<char> blockdata;

	// 2. 处理读取到的块
	if (interestblock.GetBlockindex() == blockindex) {
		// 从路径读取到的目标块，需要解密
		if (!interestblock.IsDummy()) {
			blockdata = decrypt_data(interestblock.GetData());
		}
		else {
			blockdata = interestblock.GetData();
		}
	}
	else {
		// 3. 如果不在路径中，检查stash
		for (auto it = stash.begin(); it != stash.end(); ++it) {
			if (it->GetBlockindex() == blockindex) {
				blockdata = it->GetData();   // stash中已经是明文
				stash.erase(it);
				break;
			}
		}
	}

	// 4. 如果是WRITE操作，更新数据
	if (op == WRITE) {
		blockdata = data;
	}

	// 明文放入stash
	stash.emplace_back(positionmap[blockindex], blockindex, blockdata);

	// 5. 路径管理和驱逐
	round = (round + 1) % EvictRound;
	if (round == 0) EvictPath();

	EarlyReshuffle(oldLeaf);

	return blockdata;
}