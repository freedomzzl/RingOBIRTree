#ifndef STORAGE_INTERFACE_H
#define STORAGE_INTERFACE_H

#include <vector>
#include <memory>
#include <cstdint>

// ==========================
// StorageInterface 抽象接口
// ==========================
// 本接口定义了索引树(IRTree)或 ORAM 存储层的统一访问规范。
// 其核心思想是：IRTree 不直接操作底层文件或内存，而是通过此接口
// 实现节点(Node)与文档(Document)的存取、删除与批量操作，
// 从而支持多种不同的存储后端（如内存、磁盘、ORAM等）。
// ==========================

// 前向声明（避免循环依赖）
class Node;
class Document;

class StorageInterface {
public:
    virtual ~StorageInterface() = default;

    // ==========================
    // 节点(Node)相关操作接口
    // ==========================

    // 将一个节点数据存储到后端存储系统中。
    // 参数：
    // - node_id：节点的唯一标识符
    // - data：序列化后的节点数据（二进制形式）
    // 返回值：
    // - 成功存储返回 true，失败返回 false。
    virtual bool storeNode(int node_id, const std::vector<uint8_t>& data) = 0;

    // 读取指定节点的存储内容。
    // 参数：
    // - node_id：节点标识符
    // 返回值：
    // - 对应节点的二进制数据（若不存在，可返回空 vector）。
    virtual std::vector<uint8_t> readNode(int node_id) = 0;

    // 删除指定节点的数据。
    // 参数：
    // - node_id：待删除节点的标识符
    // 返回值：
    // - 删除成功返回 true，否则返回 false。
    virtual bool deleteNode(int node_id) = 0;

    // ==========================
    // 文档(Document)相关操作接口
    // ==========================

    // 存储一个文档的二进制数据。
    // 参数：
    // - doc_id：文档唯一标识符
    // - data：文档的序列化字节流
    // 返回值：
    // - 存储成功返回 true，否则 false。
    virtual bool storeDocument(int doc_id, const std::vector<uint8_t>& data) = 0;

    // 读取指定文档的内容。
    // 参数：
    // - doc_id：文档标识符
    // 返回值：
    // - 文档数据的二进制表示（若不存在，可返回空 vector）。
    virtual std::vector<uint8_t> readDocument(int doc_id) = 0;

    // ==========================
    // 批量操作接口（性能优化）
    // ==========================
    // 支持一次性存储多个节点数据，减少与底层存储交互次数。
    // 参数：
    // - nodes：包含若干 (node_id, data) 对的列表
    // 返回值：
    // - 批量存储全部成功返回 true，否则返回 false。
    virtual bool batchStoreNodes(const std::vector<std::pair<int, std::vector<uint8_t>>>& nodes) = 0;

    // ==========================
    // 统计信息接口
    // ==========================
    // 获取当前已存储的节点数量。
    virtual int getStoredNodeCount() const = 0;

    // 获取当前已存储的文档数量。
    virtual int getStoredDocumentCount() const = 0;
};

#endif // STORAGE_INTERFACE_H
