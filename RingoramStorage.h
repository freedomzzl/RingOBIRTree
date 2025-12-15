#ifndef Ring_ORAM_STORAGE_H
#define Ring_ORAM_STORAGE_H

#include "StorageInterface.h"
#include "ringoram.h"
#include "ServerStorage.h"
#include "CryptoUtil.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <iostream>

using namespace std;

/**
 * @class RingOramStorage
 * @brief 基于 Ring ORAM 的安全存储实现类
 *
 * 该类实现了 StorageInterface 接口，用于在服务器端通过 ORAM 技术
 * 安全地存储、读取、删除节点与文档数据。可选择递归版 ORAM 实现以
 * 提升隐私保护层次，同时支持数据加密与内存缓存。
 */
class RingOramStorage : public StorageInterface {
private:
    // ==============================
    // 内部成员变量
    // ==============================

    /// 非递归 Path ORAM 实例
    std::unique_ptr<ringoram> oram;


    /// 加密工具类（用于数据加解密）
    std::shared_ptr<CryptoUtils> crypto;



    /// 节点 ID -> ORAM 块 ID 映射表
    std::unordered_map<int, int> node_id_to_block;

    /// 文档 ID -> ORAM 块 ID 映射表
    std::unordered_map<int, int> doc_id_to_block;



    /// 下一个可分配的块 ID
    int next_block_id;

    /// ORAM 容量（块数量）
    int capacity;

    std::string server_ip_;
    int server_port_;


    // ==============================
    // 内部辅助函数
    // ==============================

    /**
     * @brief 获取下一个未使用的 ORAM 块 ID
     * @return int 新的块 ID
     */
    int getNextBlockId();

    /// 根节点路径存储
    int root_path;

    /// 根节点路径的块索引（用于在ORAM中存储根路径）
    int root_path_block_index;

    /// 路径到节点ID的映射（用于调试和验证）
    std::unordered_map<int, int> path_to_node_id;

    /// 路径到块索引的映射
    std::unordered_map<int, int> path_to_block_index;

    /// 块索引到路径的映射  
    std::unordered_map<int, int> block_index_to_path;

public:
    // ==============================
    // 构造与初始化
    // ==============================

    /**
     * @brief 构造函数
     * @param cap ORAM 容量（块数）
     * @param block_size 每个块大小（字节）
     */
    RingOramStorage(int cap, int block_size, 
                    const std::string& server_ip = "127.0.0.1", 
                    int server_port = 12345);


    // ==============================
    // StorageInterface 接口实现
    // ==============================

    /**
     * @brief 将节点数据存入 ORAM
     * @param node_id 节点 ID
     * @param data 节点数据（二进制）
     * @return 存储是否成功
     */
    bool storeNode(int node_id, const std::vector<uint8_t>& data) override;

    /**
     * @brief 从 ORAM 读取节点数据
     * @param node_id 节点 ID
     * @return 节点数据（若不存在返回空向量）
     */
    std::vector<uint8_t> readNode(int node_id) override;

    /**
     * @brief 删除指定节点数据
     * @param node_id 节点 ID
     * @return 删除是否成功
     */
    bool deleteNode(int node_id) override;

    /**
     * @brief 将文档数据存入 ORAM
     * @param doc_id 文档 ID
     * @param data 文档数据
     * @return 存储是否成功
     */
    bool storeDocument(int doc_id, const std::vector<uint8_t>& data) override;

    /**
     * @brief 从 ORAM 读取文档数据
     * @param doc_id 文档 ID
     * @return 文档数据（若不存在返回空向量）
     */
    std::vector<uint8_t> readDocument(int doc_id) override;

    /**
     * @brief 批量存储多个节点（减少通信轮次）
     * @param nodes 节点 ID 与数据的键值对列表
     * @return 是否全部存储成功
     */
    bool batchStoreNodes(const std::vector<std::pair<int, std::vector<uint8_t>>>& nodes) override;

    // ==============================
    // 递归访问支持
    // ==============================

    /**
     * @brief 通过路径访问节点（递归ORAM访问）
     * @param path 物理路径
     * @return 节点数据
     */
    std::vector<uint8_t> accessByPath(int path);

    /**
     * @brief 设置根节点路径
     * @param path 根节点路径
     */
    void setRootPath(int path);

    /**
     * @brief 获取根节点路径
     * @return 根节点路径
     */
    int getRootPath() const;

    /**
     * @brief 持久化根路径到ORAM
     */
    void persistRootPath();

    /**
     * @brief 从ORAM中加载根路径
     */
    void loadRootPath();

    /**
     * @brief 建立路径到节点ID的映射（用于初始化）
     * @param path 物理路径
     * @param node_id 节点ID
     */
    void mapPathToNode(int path, int node_id) {
        path_to_node_id[path] = node_id;

    }

    /**
     * @brief 获取路径对应的节点ID（用于调试）
     * @param path 物理路径
     * @return 节点ID，如果不存在返回-1
     */
    int getNodeIdByPath(int path) const {
        auto it = path_to_node_id.find(path);
        return (it != path_to_node_id.end()) ? it->second : -1;
    }

    /**
      * @brief 分配块索引给路径
      * @param path 物理路径
      * @return 分配的块索引
      */
    int allocateBlockForPath(int path);

    /**
     * @brief 通过路径获取块索引
     * @param path 物理路径
     * @return 块索引
     */
    int getBlockIndexByPath(int path) const;

    // ==============================
    // 存储统计信息
    // ==============================

    /**
     * @brief 获取当前存储的节点数量
     * @return 节点数量
     */
    int getStoredNodeCount() const override;

    /**
     * @brief 获取当前存储的文档数量
     * @return 文档数量
     */
    int getStoredDocumentCount() const override;


    // ==============================
    // 调试与信息输出
    // ==============================

    /**
     * @brief 打印 ORAM 存储统计信息
     *
     * 包括节点/文档数量、ORAM 层数、缓存大小等。
     */
    void printStorageStats() const;


};

#endif // Ring_ORAM_STORAGE_H