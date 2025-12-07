#ifndef IRTREE_H
#define IRTREE_H

#include <iostream>
#include <memory>
#include <vector>
#include <queue>
#include <stack>
#include <functional>
#include <chrono>
#include "node.h"
#include "document.h"
#include "query.h"
#include "InvertedIndex.h"
#include "vocabulary.h"
#include "StorageInterface.h"
#include"ringoram.h"
#include <mutex>

//
// ===============================================
// TreeHeapEntry 结构体
// ===============================================
// 表示 IRTree 搜索过程中使用的中间结果项。
// 可以是一个节点(Node) 或 一个文档(Document)。
// 存放在优先队列中以支持 Top-k 查询。
// ===============================================
struct TreeHeapEntry {
    std::shared_ptr<Node> node;         ///< 指向节点对象（内部节点）
    std::shared_ptr<Document> document; ///< 指向文档对象（叶节点）
    double score;                       ///< 相关性得分，越高表示越相关
    int path;                           ///< 节点的物理路径

    /// 默认构造函数
    TreeHeapEntry() : node(nullptr), document(nullptr), score(0.0), path(-1) {}

    /// 节点构造函数
    TreeHeapEntry(std::shared_ptr<Node> n, int p = -1, double s = 0.0)
        : node(n), document(nullptr), score(s), path(p) {
    }

    /// 文档构造函数  
    TreeHeapEntry(std::shared_ptr<Document> doc, double s = 0.0)
        : node(nullptr), document(doc), score(s), path(-1) {
    }

    /// 判断该项是否为文档数据
    bool isData() const { return document != nullptr; }

    /// 判断该项是否为节点
    bool isNode() const { return node != nullptr; }

    /// 获取该项的空间范围（MBR）
    MBR getMBR() const {
        if (isData()) return document->getLocation();  // 文档位置
        if (isNode()) return node->getMBR();           // 节点包围框
        return MBR({ 0,0 }, { 0,0 });                  // 空范围
    }

    /// 获取该项的唯一标识符
    int getId() const {
        if (isData()) return document->getId();
        if (isNode()) return node->getId();
        return -1;
    }
};

//
// ===============================================
// TreeHeapComparator 比较器
// ===============================================
// 用于优先队列中按得分(score)排序，
// 得分高的项优先出队（大顶堆）。
// ===============================================
struct TreeHeapComparator {
    bool operator()(const TreeHeapEntry& a, const TreeHeapEntry& b) {
        return a.score < b.score; // 分数高者优先
    }
};

//
// ===============================================
// IRTree 类
// ===============================================
// Information Retrieval R-tree
// 信息检索树：融合空间索引(R-tree)与文本倒排索引的混合结构。
// 支持：
//   - 基于空间位置 + 关键字的联合查询
//   - Top-k 检索
//   - 基于 ORAM 的加密存储
// ===============================================
class IRTree {
public:
    // ====================================================
    // 基本状态成员
    // ====================================================
    std::shared_ptr<StorageInterface> storage;  ///< 底层存储接口（例如 ORAMStorage）
    int root_node_id;                           ///< 根节点ID
    Vocabulary vocab;                           ///< 词汇表（管理 term-ID 映射）
    InvertedIndex global_index;                 ///< 全局倒排索引（跨节点/文档）
    int next_node_id;                           ///< 下一可分配节点ID
    int next_doc_id;                            ///< 下一可分配文档ID

    // ====================================================
    // 树参数配置
    // ====================================================
    int min_capacity;   ///< 节点最小容量（分裂下限）
    int max_capacity;   ///< 节点最大容量（分裂上限）
    int dimensions;     ///< 空间维度（通常为2：经纬度）

    // ====================================================
    // 联合相关性计算
    // ====================================================

    /**
     * @brief 计算文本相关性（基于 TF-IDF）
     * @param doc 待评估文档
     * @param query_terms 查询关键字列表
     * @return 文本相关性得分
     */
    double computeTextRelevance(const Document& doc, const std::vector<std::string>& query_terms) const;

    /**
     * @brief 计算空间相关性（基于MBR与查询范围的重叠程度）
     * @param doc_location 文档的空间位置
     * @param query_scope 查询的空间范围
     * @return 空间相关性得分
     */
    double computeSpatialRelevance(const MBR& doc_location, const MBR& query_scope) const;

    /**
     * @brief 综合文本与空间相关性（通过权重α融合）
     * @param text_relevance 文本得分
     * @param spatial_relevance 空间得分
     * @param alpha 文本权重（0~1）
     * @return 综合得分
     */
    double computeJointRelevance(double text_relevance, double spatial_relevance, double alpha) const;

    // ====================================================
    // 树结构维护
    // ====================================================

    /**
     * @brief 选择插入叶节点（根据MBR扩展代价最小原则）
     * @param mbr 文档或对象的空间范围
     * @return 目标叶节点ID
     */
    int chooseLeaf(const MBR& mbr);

    /**
     * @brief 向上调整树结构，更新MBR与层级信息
     * @param node_id 被修改的节点ID
     */
    void adjustTree(int node_id);

    /**
     * @brief 分裂节点（当节点超过max_capacity时触发）
     * @param node_id 需分裂的节点ID
     */
    void splitNode(int node_id);

    // ====================================================
    // 递归位置映射初始化
    // ====================================================

    /**
     * @brief 为整个树分配随机路径并建立递归位置映射
     */
    void initializeRecursivePositionMap();

    /**
     * @brief 递归分配路径的辅助函数
     * @param node_id 当前节点ID
     * @return 当前节点的物理路径
     */
    int assignPathRecursively(int node_id);

    /**
     * @brief 获取随机叶子路径
     * @return 随机叶子路径 [0, num_leaves-1]
     */
    int getRandomLeafPath() const;

    // ====================================================
    // 递归查询支持
    // ====================================================

    /**
     * @brief 获取根节点的路径（从STASH中）
     * @return 根节点的物理路径
     */
    int getRootPath() const;

    /**
     * @brief 设置根节点的路径（到STASH中）
     * @param path 根节点的物理路径
     */
    void setRootPath(int path);

    /**
     * @brief 递归访问节点（使用路径而不是节点ID）
     * @param path 要访问的物理路径
     * @return 访问到的节点，如果失败返回nullptr
     */
    std::shared_ptr<Node> accessNodeByPath(int path);

    // ====================================================
    // 存储与序列化接口（通过 StorageInterface 实现）
    // ====================================================

    /// 从存储中加载节点（反序列化）
    std::shared_ptr<Node> loadNode(int node_id) const;

    /// 将节点对象写回存储（序列化）
    void saveNode(int node_id, std::shared_ptr<Node> node);

    /// 创建新节点（分配ID并初始化）
    int createNewNode(Node::Type type, int level, const MBR& mbr);

    // ====================================================
    // 内存缓存机制（提高访问效率）
    // ====================================================
    mutable std::unordered_map<int, std::shared_ptr<Node>> node_cache;  ///< 节点缓存
    mutable std::mutex cache_mutex;                                     ///< 缓存互斥锁
    const size_t MAX_CACHE_SIZE = 1000;                                 ///< 最大缓存节点数

    // ====================================================
    // 搜索辅助函数
    // ====================================================

    /**
     * @brief 计算节点与查询的相关性（用于内部节点剪枝）
     */
    double computeNodeRelevance(std::shared_ptr<Node> node,
        const std::vector<std::string>& keywords,
        const MBR& spatial_scope,
        double alpha) const;

    /**
     * @brief 处理叶节点：计算包含的文档的相关性并加入结果集
     */
    void processLeafNode(std::shared_ptr<Node> leaf_node,
        const std::vector<std::string>& keywords,
        const MBR& spatial_scope,
        double alpha,
        std::vector<TreeHeapEntry>& results) const;

    void processInternalNodeWithPath(std::shared_ptr<Node> internal_node,
        int parent_path,
        const std::vector<std::string>& keywords,
        const MBR& spatial_scope,
        double alpha,
        std::priority_queue<TreeHeapEntry, std::vector<TreeHeapEntry>, TreeHeapComparator>& queue);

    /**
     * @brief 处理内部节点：计算子节点相关性并入队
     */
    void processInternalNode(std::shared_ptr<Node> internal_node,
        const std::vector<std::string>& keywords,
        const MBR& spatial_scope,
        double alpha,
        std::priority_queue<TreeHeapEntry,
        std::vector<TreeHeapEntry>,
        TreeHeapComparator>& queue) const;

    // ====================================================
    // 构造函数
    // ====================================================

    /**
     * @brief 构造函数
     * @param storage_impl 底层存储实现（例如 PathOramStorage）
     * @param dims 空间维度（默认2）
     * @param min_cap 节点最小容量
     * @param max_cap 节点最大容量
     */
    IRTree(std::shared_ptr<StorageInterface> storage_impl,
        int dims = 2, int min_cap = 2, int max_cap = 4);

    // ====================================================
    // 文档插入接口
    // ====================================================

    /// 插入单个文档（提供文本与空间位置）
    void insertDocument(const std::string& text, const MBR& location);

    /// 插入已构造的 Document 对象
    void insertDocument(std::shared_ptr<Document> document);

    // ====================================================
    // 查询接口
    // ====================================================

    /**
     * @brief 执行查询（Query对象方式）
     * @return 排序后的结果项
     */
    std::vector<TreeHeapEntry> search(const Query& query);

    /**
     * @brief 执行 Top-k 查询（关键词+空间范围）
     * @param keywords 查询关键词
     * @param spatial_scope 空间范围
     * @param k 返回的结果数量
     * @param alpha 文本权重参数
     * @return Top-k 结果项
     */
    std::vector<TreeHeapEntry> search(const std::vector<std::string>& keywords,
        const MBR& spatial_scope,
        int k = 10,
        double alpha = 0.5);

    // ====================================================
    // 批量插入接口（构建阶段）
    // ====================================================

    /// 从文件批量插入文档（文本 + 坐标）
    void bulkInsertFromFile(const std::string& filename);

    /// 从内存批量插入文档（三元组格式）
    void bulkInsertDocuments(const std::vector<std::tuple<std::string, double, double>>& documents);

    /// 优化后的批量插入（性能增强版）
    void optimizedBulkInsertFromFile(const std::string& filename);
    void optimizedBulkInsertDocuments(const std::vector<std::tuple<std::string, double, double>>& documents);

    /// 批量构建全局倒排索引
    void bulkBuildGlobalIndex(const std::vector<std::shared_ptr<Document>>& documents);

    /// 批量将文档插入树结构
    void bulkInsertToTree(const std::vector<std::shared_ptr<Document>>& documents);

    // ====================================================
    // 存储同步与缓存管理
    // ====================================================

    /// 将缓存中所有节点刷新写回存储
    void flushNodeCache();

    /// 从缓存加载节点（不存在则从存储加载）
    std::shared_ptr<Node> cachedLoadNode(int node_id) const;

    /// 将节点保存并更新缓存
    void cachedSaveNode(int node_id, std::shared_ptr<Node> node);

    /// 清空缓存
    void clearCache() {
        std::lock_guard<std::mutex> lock(cache_mutex);
        node_cache.clear();
        std::cout << "IRTree cache cleared - " << node_cache.size() << " nodes removed" << std::endl;
    }

    /// 打印缓存状态
    void printCacheStats() const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        std::cout << "IRTree cache stats - Size: " << node_cache.size() << std::endl;
    }

    // ====================================================
    // 树结构优化
    // ====================================================

    /// 底向上构建 IRTree（批量建树）
    void buildTreeBottomUp(const std::vector<std::shared_ptr<Document>>& documents);

    /*pathoram::CommunicationStats getOramStats() const;
    void resetOramStats() const;*/

    int search_blocks;
    // ====================================================
    // 性能评估接口
    // ====================================================

    /**
     * @brief 计算一次查询的运行时间（可选输出详细信息）
     * @param query_keywords 查询关键字
     * @param scope 空间范围
     * @param k 返回结果数量
     * @param show_details 是否显示执行细节
     * @return 查询耗时（纳秒）
     */
    std::chrono::nanoseconds getRunTime(const std::string& query_keywords, const MBR& scope, int k, bool show_details = true);
};

#endif // IRTREE_H