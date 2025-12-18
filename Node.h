#ifndef NODE_H
#define NODE_H

#include <vector>
#include <memory>
#include <stdexcept> 
#include <unordered_map>
#include "MBR.h"

// 前向声明，避免循环依赖
class Document;

/**
 * @class Node
 * @brief IRTree（或R-Tree）中的节点类。
 *
 * 每个 Node 代表索引树中的一个节点。
 * - 叶子节点（LEAF）存储具体的 Document 对象；
 * - 内部节点（INTERNAL）存储指向子节点的指针；
 * 每个节点都包含一个最小外接矩形（MBR），用于描述节点所覆盖的空间范围。
 * 此外，节点还维护了文档摘要信息（如文档频率 DF、最大词频 TFmax），
 * 用于在查询阶段进行快速剪枝。
 */
class Node {
public:
    /// 节点类型：叶子节点或内部节点
    enum Type { LEAF, INTERNAL };

private:
    int node_id;       ///< 节点ID，用于唯一标识节点
    Type type;         ///< 节点类型（LEAF 或 INTERNAL）
    MBR mbr;           ///< 节点的最小外接矩形（Minimum Bounding Rectangle）
    int level;         ///< 节点所在的层级（根为最高层）

    // 子节点或文档集合（根据节点类型不同而使用）
    std::vector<std::shared_ptr<Node>> child_nodes;   ///< 内部节点的子节点列表
    std::vector<std::shared_ptr<Document>> documents; ///< 叶子节点存储的文档列表

    // 文档摘要信息（用于索引加速）
    int document_count; ///< 当前节点下的文档总数（包含子节点递归统计）
    std::unordered_map<std::string, int> df;      ///< 文档频率（Document Frequency）：包含某词项的文档数
    std::unordered_map<std::string, int> tf_max;  ///< 最大词频（Max Term Frequency）：某词项在该节点下的最大出现次数

    // 递归位置映射：存储子节点的 (node_id, path) 对
    std::unordered_map<int, int> child_position_map;  // node_id -> path

public:
    /**
     * @brief 构造函数
     * @param id 节点ID
     * @param node_type 节点类型（LEAF 或 INTERNAL）
     * @param node_level 节点层级
     * @param node_mbr 节点的MBR范围
     */
    Node(int id, Type node_type, int node_level, const MBR& node_mbr);

    /* ======================== 节点结构操作 ======================== */

    /**
     * @brief 向内部节点添加一个子节点。
     * @param child 子节点指针
     */
    void addChild(std::shared_ptr<Node> child);

    /**
     * @brief 向叶子节点添加一个文档。
     * @param doc 文档指针
     */
    void addDocument(std::shared_ptr<Document> doc);

    /**
     * @brief 更新节点的摘要信息。
     *
     * 包括：
     * - 重新计算文档数量；
     * - 更新 DF（词项出现的文档数）；
     * - 更新 TFmax（节点下该词项的最大频率）。
     */
    void updateSummary();

    /* ======================== Getter 接口 ======================== */

    int getId() const { return node_id; }               ///< 获取节点ID
    Type getType() const { return type; }               ///< 获取节点类型
    const MBR& getMBR() const { return mbr; }           ///< 获取节点MBR
    int getLevel() const { return level; }              ///< 获取节点层级
    int getDocumentCount() const { return document_count; } ///< 获取文档数量

    const std::vector<std::shared_ptr<Node>>& getChildNodes() const { return child_nodes; } ///< 获取子节点列表
    const std::vector<std::shared_ptr<Document>>& getDocuments() const { return documents; } ///< 获取文档列表

    /* ======================== 文档摘要访问 ======================== */

    /**
     * @brief 获取某个词项在该节点下的文档频率。
     * @param term 词项字符串
     * @return 出现该词项的文档数量
     */
    int getDocumentFrequency(const std::string& term) const;

    /**
     * @brief 获取某个词项的最大词频（TFmax）。
     * @param term 词项字符串
     * @return 该词项在节点范围内的最大出现次数
     */
    int getMaxTermFrequency(const std::string& term) const;

    /**
     * @brief 获取 DF 哈希表的引用。
     */
    const std::unordered_map<std::string, int>& getDF() const { return df; }

    /**
     * @brief 获取 TFmax 哈希表的引用。
     */
    const std::unordered_map<std::string, int>& getTFMax() const { return tf_max; }

    /* ======================== 调试与输出 ======================== */

    /**
     * @brief 将节点信息转换为字符串（用于调试打印）。
     */
    std::string toString() const;

    /* ======================== 辅助接口 ======================== */

    /**
     * @brief 设置节点的摘要信息（用于反序列化或恢复索引）。
     * @param new_df 新的 DF 数据
     * @param new_tf_max 新的 TFmax 数据
     */
    void setDocumentSummary(const std::unordered_map<std::string, int>& new_df,
        const std::unordered_map<std::string, int>& new_tf_max) {
        df = new_df;
        tf_max = new_tf_max;
    }

    /**
     * @brief 清空节点的文档（常用于测试或节点重建）。
     */
    void clearDocuments() {
        documents.clear();
        updateSummary();
    }

    /**
     * @brief 设置内部节点的子节点列表（用于反序列化）。
     * @param children 子节点向量
     * @throws std::logic_error 若当前节点是叶子节点则抛出异常
     */
    void setChildNodes(const std::vector<std::shared_ptr<Node>>& children) {
        if (type != INTERNAL) {
            throw std::logic_error("Cannot set child nodes on leaf node");
        }
        child_nodes = children;
        updateSummary();
    }

    /**
     * @brief 清空节点的所有子节点。
     */
    void clearChildNodes() {
        child_nodes.clear();
        updateSummary();
    }

    /**
     * @brief 获取所有子节点的 ID 列表。
     * @return 子节点ID向量
     */
    std::vector<int> getChildNodeIds() const {
        std::vector<int> ids;
        for (const auto& child : child_nodes) {
            ids.push_back(child->getId());
        }
        return ids;
    }

    /* ======================== 位置映射操作 ======================== */

    /**
     * @brief 设置子节点的位置映射
     * @param child_id 子节点ID
     * @param path 子节点的物理路径
     */
    void setChildPosition(int child_id, int path) {
        child_position_map[child_id] = path;
    }

    /**
     * @brief 获取子节点的位置映射
     * @param child_id 子节点ID
     * @return 子节点的物理路径，如果不存在返回-1
     */
    int getChildPosition(int child_id) const {
        auto it = child_position_map.find(child_id);
        return (it != child_position_map.end()) ? it->second : -1;
    }

    /**
     * @brief 获取所有子节点的位置映射
     * @return 子节点位置映射的引用
     */
    const std::unordered_map<int, int>& getChildPositionMap() const {
        return child_position_map;
    }

    /**
     * @brief 清空子节点位置映射
     */
    void clearChildPositionMap() {
        child_position_map.clear();
    }

    /**
     * @brief 设置整个子节点位置映射（用于反序列化）
     * @param new_position_map 新的位置映射
     */
    void setChildPositionMap(const std::unordered_map<int, int>& new_position_map) {
        child_position_map = new_position_map;
    }
};

#endif // NODE_H