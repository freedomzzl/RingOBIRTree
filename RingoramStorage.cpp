#include "RingoramStorage.h"
#include <iostream>
#include <stdexcept>

RingOramStorage::RingOramStorage(int cap, int block_size)
    : next_block_id(0), capacity(cap) {

    std::cout << "Initializing RingOramStorage with capacity: " << capacity
        << ", block_size: " << block_size
        << std::endl;

    // 创建 ServerStorage
    server_storage = std::make_unique<ServerStorage>();

    oram = std::make_unique<ringoram>(capacity, server_storage.get());

    // 尝试加载已存储的根路径
    loadRootPath();
}

int RingOramStorage::getNextBlockId() {
    int block_id = next_block_id++;

    // 关键修复：确保block ID在有效范围内
    if (block_id >= capacity) {
        std::cerr << "ERROR: Block ID " << block_id << " exceeds ORAM capacity " << capacity << std::endl;
        throw std::runtime_error("ORAM capacity exceeded");
    }

    return block_id;
}


bool RingOramStorage::storeNode(int node_id, const std::vector<uint8_t>& data) {
    try {

        int block_id = getNextBlockId();
        node_id_to_block[node_id] = block_id;

        std::vector<char> data_vec(data.begin(), data.end());



        oram->access(block_id, ringoram::WRITE, data_vec);



        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Error storing node " << node_id << ": " << e.what() << std::endl;
        return false;
    }
}

std::vector<uint8_t> RingOramStorage::readNode(int node_id) {
    try {

        auto block_it = node_id_to_block.find(node_id);
        if (block_it == node_id_to_block.end()) {
            return {};
        }

        int block_id = block_it->second;



        std::vector<char> result_data;


        result_data = oram->access(block_id, ringoram::READ, {});



        if (result_data.empty()) {
            return {};
        }

        std::vector<uint8_t> vec_uint8(result_data.begin(), result_data.end());
        return vec_uint8;
    }
    catch (const std::exception& e) {
        std::cerr << "Error reading node " << node_id << ": " << e.what() << std::endl;
        return {};
    }
}

bool RingOramStorage::deleteNode(int node_id) {
    try {
        auto it = node_id_to_block.find(node_id);
        if (it != node_id_to_block.end()) {
            int block_id = it->second;

            // 从ORAM中删除：写入空数据
            std::vector<char> empty_data;

            oram->access(block_id, ringoram::WRITE, empty_data);


            // 清理映射和缓存
            node_id_to_block.erase(it);

            return true;
        }
        return false;
    }
    catch (const std::exception& e) {
        std::cerr << "Error deleting node " << node_id << ": " << e.what() << std::endl;
        return false;
    }
}

bool RingOramStorage::storeDocument(int doc_id, const std::vector<uint8_t>& data) {
    try {

        std::vector<char> processed_data(data.begin(), data.end());
        int block_id;
        auto it = doc_id_to_block.find(doc_id);
        if (it != doc_id_to_block.end()) {
            block_id = it->second;
        }
        else {
            block_id = getNextBlockId();
            doc_id_to_block[doc_id] = block_id;
        }

        if (block_id >= capacity) {
            std::cerr << "ERROR: Block ID " << block_id << " exceeds ORAM capacity " << capacity << std::endl;
            return false;
        }

        std::vector<char> oram_data(processed_data.begin(), processed_data.end());
        std::vector<char> result;



        result = oram->access(block_id, ringoram::WRITE, oram_data);


        return !result.empty();
    }
    catch (const std::exception& e) {
        std::cerr << "Error storing document " << doc_id << ": " << e.what() << std::endl;
        return false;
    }
}

std::vector<uint8_t> RingOramStorage::readDocument(int doc_id) {
    try {
        auto it = doc_id_to_block.find(doc_id);
        if (it == doc_id_to_block.end()) {
            return {};
        }

        int block_id = it->second;

        if (block_id >= capacity) {
            std::cerr << "ERROR: Invalid block ID " << block_id << " for document " << doc_id << std::endl;
            return {};
        }

        std::vector<char> result;


        result = oram->access(block_id, ringoram::READ,{});


        if (result.empty()) {
            return {};
        }

        std::vector<uint8_t> data(result.begin(), result.end());


        return data;
    }
    catch (const std::exception& e) {
        std::cerr << "Error reading document " << doc_id << ": " << e.what() << std::endl;
        return {};
    }
}

bool RingOramStorage::batchStoreNodes(const std::vector<std::pair<int, std::vector<uint8_t>>>& nodes) {
    bool all_success = true;

    for (const auto& node_pair : nodes) {
        int node_id = node_pair.first;
        const std::vector<uint8_t>& data = node_pair.second;

        if (!storeNode(node_id, data)) {
            all_success = false;
            std::cerr << "Failed to store node " << node_id << " in batch operation" << std::endl;
        }
    }
    return all_success;
}

std::vector<uint8_t> RingOramStorage::accessByPath(int path) {
    try {


        // 通过路径获取块索引
        int block_index = getBlockIndexByPath(path);
        if (block_index == -1) {
            std::cerr << "No block index allocated for path " << path << std::endl;
            return {};
        }

        // 使用原有的节点访问机制 - 这会调用 oram->access(block_index, ...)
        // 查找节点ID
        int node_id = getNodeIdByPath(path);
        if (node_id == -1) {
            std::cerr << "No node ID mapped to path " << path << std::endl;
            return {};
        }

        return readNode(node_id);

    }
    catch (const std::exception& e) {
        std::cerr << "Error accessing path " << path << ": " << e.what() << std::endl;
        return {};
    }
}

// 设置根节点路径
void RingOramStorage::setRootPath(int path) {
    root_path = path;

    // 持久化到ORAM
    persistRootPath();
}

// 获取根节点路径
int RingOramStorage::getRootPath() const {
    // 如果内存中有值，直接返回
    if (root_path != -1) {
        return root_path;
    }

    // 否则从ORAM加载
    const_cast<RingOramStorage*>(this)->loadRootPath();
    return root_path;
}

// 持久化根路径到ORAM
void RingOramStorage::persistRootPath() {
    try {
        // 为根路径分配专用块（如果还没有）
        if (root_path_block_index == -1) {
            root_path_block_index = getNextBlockId();

        }

        // 序列化根路径数据
        std::vector<uint8_t> root_path_data(sizeof(int));
        memcpy(root_path_data.data(), &root_path, sizeof(int));

        // 存储到ORAM
        std::vector<char> data_vec(root_path_data.begin(), root_path_data.end());
        oram->access(root_path_block_index, ringoram::WRITE, data_vec);

    }
    catch (const std::exception& e) {
        std::cerr << "Error persisting root path: " << e.what() << std::endl;
    }
}

// 从ORAM中加载根路径
void RingOramStorage::loadRootPath() {
    try {
        // 如果还没有分配块，说明根路径还未存储
        if (root_path_block_index == -1) {
            root_path = 0; // 默认值
            return;
        }

        // 从ORAM读取根路径数据
        std::vector<char> result_data = oram->access(root_path_block_index, ringoram::READ, {});
        if (result_data.size() >= sizeof(int)) {
            memcpy(&root_path, result_data.data(), sizeof(int));

        }
        else {
            /*std::cerr << "Invalid root path data size: " << result_data.size() << std::endl;*/
            root_path = 0;
        }

    }
    catch (const std::exception& e) {
        std::cerr << "Error loading root path: " << e.what() << std::endl;
        root_path = 0;
    }
}

int RingOramStorage::allocateBlockForPath(int path) {
    int block_index = getNextBlockId();
    path_to_block_index[path] = block_index;
    block_index_to_path[block_index] = path;

    return block_index;
}

int RingOramStorage::getBlockIndexByPath(int path) const {
    auto it = path_to_block_index.find(path);
    return (it != path_to_block_index.end()) ? it->second : -1;
}

int RingOramStorage::getStoredNodeCount() const {
    return node_id_to_block.size();
}

int RingOramStorage::getStoredDocumentCount() const {
    return doc_id_to_block.size();
}

void RingOramStorage::printStorageStats() const {
    std::cout << "=== RingOramStorage Statistics ===" << std::endl;
    std::cout << "Total nodes stored: " << getStoredNodeCount() << std::endl;
    std::cout << "Total documents stored: " << getStoredDocumentCount() << std::endl;
    std::cout << "Next block ID: " << next_block_id << std::endl;
    std::cout << "ORAM capacity: " << capacity << std::endl;



    // 打印节点到块的映射
    std::cout << "Node to block mapping:" << std::endl;
    for (const auto& pair : node_id_to_block) {
        std::cout << "  Node " << pair.first << " -> Block " << pair.second << std::endl;
    }
}
