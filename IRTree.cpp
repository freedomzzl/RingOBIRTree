#include"IRTree.h"
#include "NodeSerializer.h"  
#include <iostream>
#include <sstream>
#include <queue>
#include <stack>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <chrono>
#include "ringoram.h"
#include "RingoramStorage.h"
#include <iomanip>
#include <random>


// 修改构造函数
IRTree::IRTree(std::shared_ptr<StorageInterface> storage_impl,
    int dims, int min_cap, int max_cap)
    : storage(storage_impl), dimensions(dims), min_capacity(min_cap),
    max_capacity(max_cap), next_node_id(0), next_doc_id(0) {

    // 创建根节点 - 初始化为全零MBR的叶子节点
    MBR root_mbr(std::vector<double>(dims, 0.0), std::vector<double>(dims, 0.0));
    root_node_id = createNewNode(Node::LEAF, 0, root_mbr);

    std::cout << "IRTree initialized with storage interface" << std::endl;

    // 初始化递归位置映射
    initializeRecursivePositionMap();
}

// 节点管理方法
std::shared_ptr<Node> IRTree::loadNode(int node_id) const {
    // 从存储中读取节点数据
    auto node_data = storage->readNode(node_id);
    if (node_data.empty()) {
        std::cout << "No data found for node " << node_id << std::endl;

        return nullptr;
    }

    // 反序列化节点数据为节点对象
    auto node = NodeSerializer::deserialize(node_data);
    if (!node) {
        std::cerr << "Failed to deserialize node " << node_id << std::endl;
    }

    return node;
}

void IRTree::saveNode(int node_id, std::shared_ptr<Node> node) {
    if (!node) {
        std::cerr << "Cannot save null node with ID " << node_id << std::endl;
        return;
    }

    // 序列化节点对象为字节数据
    auto node_data = NodeSerializer::serialize(*node);
    if (node_data.empty()) {
        std::cerr << "Failed to serialize node " << node_id << std::endl;
        return;
    }

    // 存储序列化后的节点数据
    storage->storeNode(node_id, node_data);
}


int IRTree::createNewNode(Node::Type type, int level, const MBR& mbr) {
    // 分配新节点ID并创建节点对象
    int new_node_id = next_node_id++;

    // 验证参数合理性
    if (type == Node::LEAF && level != 0) {
        std::cerr << "WARNING: Creating leaf node with level " << level << " (should be 0)" << std::endl;
    }
    if (type == Node::INTERNAL && level == 0) {
        std::cerr << "WARNING: Creating internal node with level 0" << std::endl;
    }

    auto new_node = std::make_shared<Node>(new_node_id, type, level, mbr);

    // 立即验证节点类型
    if (new_node->getType() != type) {
        std::cerr << "CRITICAL ERROR: Node type mismatch after creation!" << std::endl;
    }

    // 保存新节点到存储
    saveNode(new_node_id, new_node);

    return new_node_id;
}



double IRTree::computeNodeRelevance(std::shared_ptr<Node> node, 
                                   const std::vector<std::string>& keywords,
                                   const MBR& spatial_scope, 
                                   double alpha) const
{
    if (!node) return 0.0;

    // === 1. 改进的空间相关性计算 ===
    double spatial_rel = computeSpatialRelevance(node->getMBR(), spatial_scope);
    if (spatial_rel == 0.0) return 0.0;

    // === 2. 改进的文本相关性上界 ===
    double text_upper_bound = 0.0;
    int total_docs = global_index.getTotalDocuments();
    int matched_keywords = 0;

    // 预计算查询词的全局文档频率
    std::vector<double> keyword_weights;
    for (const auto& keyword : keywords) {
        int term_id = vocab.getTermId(keyword);
        if (term_id == -1) {
            keyword_weights.push_back(0.0);
            continue;
        }
        
        int global_df = global_index.getDocumentFrequency(term_id);
        if (global_df == 0) {
            keyword_weights.push_back(0.0);
            continue;
        }
        
        // IDF权重（对数形式）
        double idf_weight = log((total_docs + 1.0) / (global_df + 1.0));
        keyword_weights.push_back(idf_weight);
    }

    // 计算节点中每个查询词的最大可能TF-IDF
    for (size_t i = 0; i < keywords.size(); i++) {
        const auto& keyword = keywords[i];
        double idf_weight = keyword_weights[i];
        if (idf_weight <= 0.0) continue; // 跳过无效关键词

        // 获取节点中该词的最大词频
        int tf_max = node->getMaxTermFrequency(keyword);
        if (tf_max == 0) continue;

        // 获取节点中该词的文档频率
        int node_df = node->getDocumentFrequency(keyword);
        
        // 改进：考虑节点内文档频率的影响
        // 如果节点中只有少量文档包含该词，降低权重
        double node_df_factor = (node_df > 0) ? 
            std::min(1.0, log(1.0 + node_df) / log(2.0)) : 0.0;
        
        // 计算更精确的TF-IDF上界
        double max_tfidf = tf_max * idf_weight * node_df_factor;
        text_upper_bound += max_tfidf;
        matched_keywords++;
    }

    // 归一化文本相关性上界
    if (matched_keywords > 0) {
        // 更保守的归一化：除以关键词数量和最大TF的平方根
        text_upper_bound = text_upper_bound / (keywords.size() * sqrt(10.0));
        text_upper_bound = std::min(1.0, text_upper_bound);
    } else {
        return 0.0; // 没有匹配的关键词
    }

    // === 3. 综合相关性 ===
    double joint_relevance = computeJointRelevance(text_upper_bound, spatial_rel, alpha);
    
    // 额外的剪枝：如果空间相关性很低，且alpha不偏向空间，降低分数
    if (spatial_rel < 0.1 && alpha < 0.3) {
        joint_relevance *= 0.5; // 惩罚低空间相关性
    }

    return joint_relevance;
}

void IRTree::processLeafNode(std::shared_ptr<Node> leaf_node,
    const std::vector<std::string>& keywords,
    const MBR& spatial_scope,
    double alpha,
    std::vector<TreeHeapEntry>& results,
    int k,                    // 最大结果数
    double threshold) const { // 当前阈值
    
    if (!leaf_node || leaf_node->getType() != Node::LEAF) {
        return;
    }

    // 设置关键词匹配阈值（30%）
    const double KEYWORD_MATCH_THRESHOLD = 0.3;
    int min_keywords_needed = std::max(1, (int)(keywords.size() * KEYWORD_MATCH_THRESHOLD));
    
    // ============ 快速预检查 ============
    // 1. 检查叶子节点MBR是否与查询范围重叠
    if (!leaf_node->getMBR().overlaps(spatial_scope)) {
        return; // 整个叶子节点都不在范围内
    }
    
    // 2. 快速文本过滤：检查叶子节点是否至少包含30%的查询关键词
    int keyword_match_count = 0;
    for (const auto& keyword : keywords) {
        if (leaf_node->getDocumentFrequency(keyword) > 0) {
            keyword_match_count++;
            if (keyword_match_count >= min_keywords_needed) {
                break; // 达到阈值，无需继续检查
            }
        }
    }
    if (keyword_match_count < min_keywords_needed) {
        return; // 不满足最低关键词匹配要求
    }

    auto documents = leaf_node->getDocuments();
    int found_count = 0;
    int checked_count = 0;
    
    // ============ 预计算查询词的IDF ============
    std::vector<double> keyword_idf;
    int total_docs = global_index.getTotalDocuments();
    
    for (const auto& keyword : keywords) {
        int term_id = vocab.getTermId(keyword);
        if (term_id == -1) {
            keyword_idf.push_back(0.0);
            continue;
        }
        int df = global_index.getDocumentFrequency(term_id);
        if (df == 0) {
            keyword_idf.push_back(0.0);
            continue;
        }
        double idf = log((total_docs + 1.0) / (df + 1.0));
        keyword_idf.push_back(idf);
    }
    
    // ============ 收集文档和分数 ============
    std::vector<TreeHeapEntry> leaf_results; // 临时存储
    
    for (const auto& doc : documents) {
        checked_count++;
        
        // 1. 快速空间过滤（精确检查）
        if (!doc->getLocation().overlaps(spatial_scope)) {
            continue;
        }
        
        // 2. 计算匹配的关键词数量和分数
        int matched_keywords = 0;
        double text_score = 0.0;
        
        for (size_t i = 0; i < keywords.size(); i++) {
            const auto& keyword = keywords[i];
            int tf = doc->getTermFrequency(keyword);
            if (tf > 0 && keyword_idf[i] > 0) {
                matched_keywords++;
                text_score += tf * keyword_idf[i];
            }
        }
        
        // 检查是否满足最低匹配要求
        if (matched_keywords < min_keywords_needed) {
            continue; // 不满足30%匹配要求
        }
        
        // 3. 计算空间相关性
        double spatial_rel = computeSpatialRelevance(doc->getLocation(), spatial_scope);
        if (spatial_rel == 0.0) continue;
        
        // 4. 归一化文本分数
        double normalized_text = 0.0;
        if (matched_keywords > 0) {
            // 归一化到[0,1]范围，考虑匹配关键词比例
            double tfidf_max = text_score;
            double match_ratio = (double)matched_keywords / keywords.size();
            
            // 如果只匹配部分关键词，适当降低分数
            normalized_text = (tfidf_max / (keywords.size() * 10.0)) * match_ratio;
            normalized_text = std::min(1.0, normalized_text);
        }
        
        // 5. 计算综合分数
        double joint_rel = computeJointRelevance(normalized_text, spatial_rel, alpha);
        
        // 6. 阈值剪枝
        if (joint_rel < threshold && results.size() >= k) {
            continue; // 分数低于当前阈值，剪枝
        }
        
        // 7. 添加到临时结果
        leaf_results.push_back(TreeHeapEntry(doc, joint_rel));
        found_count++;
        
        // 可选：如果叶子节点内找到太多文档，可以提前终止
        if (found_count > 10 && results.size() >= k) {
            break; // 已经有一些结果，避免过度检查
        }
    }
    
    // ============ 合并结果 ============
    if (!leaf_results.empty()) {
        // 按分数排序（降序）
        std::sort(leaf_results.begin(), leaf_results.end(),
            [](const TreeHeapEntry& a, const TreeHeapEntry& b) {
                return a.score > b.score;
            });
        
        // 计算还能添加多少结果
        int remaining_slots = k - results.size();
        if (remaining_slots <= 0) return; // 结果已满
        
        // 添加高质量结果
        int to_add = std::min(remaining_slots, (int)leaf_results.size());
        for (int i = 0; i < to_add; i++) {
            results.push_back(leaf_results[i]);
        }
        
        // 如果添加了新结果，可能需要重新排序
        if (to_add > 0 && results.size() >= k) {
            std::sort(results.begin(), results.end(),
                [](const TreeHeapEntry& a, const TreeHeapEntry& b) {
                    return a.score > b.score;
                });
            // 如果超过k，只保留前k个
            if (results.size() > k) {
                results.resize(k);
            }
        }
    }
}

void IRTree::processInternalNodeWithPath(std::shared_ptr<Node> internal_node,
    int parent_path,
    const std::vector<std::string>& keywords,
    const MBR& spatial_scope,
    double alpha,
    std::priority_queue<TreeHeapEntry, std::vector<TreeHeapEntry>, TreeHeapComparator>& queue) {

    if (!internal_node || internal_node->getType() != Node::INTERNAL) {
        return;
    }

    // 预检查1：节点MBR是否与查询范围重叠
    if (!internal_node->getMBR().overlaps(spatial_scope)) {
        return; // 完全无空间重叠，跳过整个子树
    }

    // 预检查2：节点是否包含任何查询关键词
    bool has_potential_keywords = false;
    for (const auto& keyword : keywords) {
        if (internal_node->getDocumentFrequency(keyword) > 0) {
            has_potential_keywords = true;
            break;
        }
    }
    
    if (!has_potential_keywords) {
        return; // 完全不包含查询关键词，跳过
    }

    // 计算节点的相关性上界（用于排序和初步剪枝）
    double node_upper_bound = computeNodeRelevance(internal_node, keywords, spatial_scope, alpha);
   
    // 从父节点中获取子节点的位置映射
    const auto& child_position_map = internal_node->getChildPositionMap();
    
    // 子节点统计，用于智能排序
    struct ChildInfo {
        int child_id;
        int child_path;
        double estimated_relevance;
        int keyword_overlap; // 匹配的关键词数量
    };
    
    std::vector<ChildInfo> child_infos;
    
    // 第一轮：快速收集子节点信息
    for (const auto& pos_pair : child_position_map) {
        int child_id = pos_pair.first;
        int child_path = pos_pair.second;

        // 使用路径访问子节点
        auto child_node = accessNodeByPath(child_path);
        if (!child_node) continue;

        // 快速空间过滤
        if (!child_node->getMBR().overlaps(spatial_scope)) {
            continue;
        }

        // 快速文本过滤：统计匹配的关键词数量
        int keyword_overlap = 0;
        for (const auto& keyword : keywords) {
            if (child_node->getDocumentFrequency(keyword) > 0) {
                keyword_overlap++;
            }
        }
        
        if (keyword_overlap == 0) {
            continue; // 没有匹配的关键词
        }

        // 快速估计相关性（简化计算）
        double spatial_overlap = 0.0;
        if (child_node->getMBR().overlaps(spatial_scope)) {
            spatial_overlap = 0.5; // 保守估计
            if (spatial_scope.contains(child_node->getMBR())) {
                spatial_overlap = 1.0;
            }
        }
        
        double estimated_rel = alpha * (keyword_overlap / (double)keywords.size()) 
                             + (1 - alpha) * spatial_overlap;

        child_infos.push_back({child_id, child_path, estimated_rel, keyword_overlap});
    }

    // 按估计相关性排序子节点（高相关性先处理）
    std::sort(child_infos.begin(), child_infos.end(),
        [](const ChildInfo& a, const ChildInfo& b) {
            return a.estimated_relevance > b.estimated_relevance;
        });

    // 第二轮：计算精确相关性并加入队列
    for (const auto& child_info : child_infos) {
        // 重新加载子节点（因为可能被修改）
        auto child_node = accessNodeByPath(child_info.child_path);
        if (!child_node) continue;

        // 计算更精确的相关性
        double relevance = computeNodeRelevance(child_node, keywords, spatial_scope, alpha);
        
        if (relevance > 0) {
            queue.push(TreeHeapEntry(child_node, child_info.child_path, relevance));
        }
    }
}

// // 使用路径处理内部节点（新方法）
// void IRTree::processInternalNodeWithPath(std::shared_ptr<Node> internal_node,
//     int parent_path,
//     const std::vector<std::string>& keywords,
//     const MBR& spatial_scope,
//     double alpha,
//     std::priority_queue<TreeHeapEntry, std::vector<TreeHeapEntry>, TreeHeapComparator>& queue) {

//     if (!internal_node || internal_node->getType() != Node::INTERNAL) {
//         return;
//     }

//     // 从父节点中获取子节点的位置映射
//     const auto& child_position_map = internal_node->getChildPositionMap();

//     for (const auto& pos_pair : child_position_map) {
//         int child_id = pos_pair.first;
//         int child_path = pos_pair.second;

//         // 使用路径访问子节点
//         auto child_node = accessNodeByPath(child_path);
//         if (!child_node) {
//             std::cerr << "Failed to load child node " << child_id << " using path " << child_path << std::endl;
//             continue;
//         }

//         // 检查空间重叠
//         bool overlaps = child_node->getMBR().overlaps(spatial_scope);
//         if (!overlaps) {
//             continue;
//         }

//         // 检查文本相关性
//         bool has_relevant_keywords = false;
//         for (const auto& keyword : keywords) {
//             if (child_node->getDocumentFrequency(keyword) > 0) {
//                 has_relevant_keywords = true;
//                 break;
//             }
//         }
//         if (!has_relevant_keywords) {
//             continue;
//         }

//         // 计算子节点相关性并加入优先队列（包含路径信息）
//         double relevance = computeNodeRelevance(child_node, keywords, spatial_scope, alpha);
//         if (relevance > 0) {
//             queue.push(TreeHeapEntry(child_node, child_path, relevance));
//         }
//     }
// }

double IRTree::computeTextRelevance(const Document& doc, const std::vector<std::string>& query_terms) const
{
    double relevance = 0.0;
    int total_docs = global_index.getTotalDocuments();

    // 对每个查询词计算TF-IDF权重
    for (const auto& term : query_terms) {
        int term_id = vocab.getTermId(term);
        if (term_id == -1) continue;  // 词汇表中不存在的词跳过

        int tf = doc.getTermFrequency(term);  // 词频
        if (tf == 0) continue;

        int df = global_index.getDocumentFrequency(term_id);  // 文档频率
        if (df == 0) continue;

        // 使用 TF-IDF 计算权重
        double tf_idf = Vector::computeTFIDFWeight(tf, df, total_docs);
        relevance += tf_idf;
    }

    // 归一化到 [0, 1] 范围
    if (relevance > 0) {
        relevance = std::min(1.0, relevance / query_terms.size());
    }

    return relevance;
}

double IRTree::computeSpatialRelevance(const MBR& doc_location, const MBR& query_scope) const {
    // 1. 检查是否重叠（基本过滤）
    if (!doc_location.overlaps(query_scope)) {
        return 0.0;
    }

    // 2. 对于点位置文档（小MBR），使用距离倒数
    double doc_area = doc_location.area();
    double query_area = query_scope.area();
    
    // 如果文档位置近似为点
    if (doc_area < 0.0001) { // 很小的面积
        // 计算中心点距离
        std::vector<double> doc_center = doc_location.getCenter();
        std::vector<double> query_center = query_scope.getCenter();
        
        // 计算欧氏距离
        double distance = 0.0;
        for (size_t i = 0; i < doc_center.size(); i++) {
            double diff = doc_center[i] - query_center[i];
            distance += diff * diff;
        }
        distance = sqrt(distance);
        
        // 如果文档中心在查询范围内，返回1.0；否则返回距离倒数
        if (query_scope.contains(doc_location)) {
            return 1.0;
        } else {
            // 归一化距离：除以查询范围对角线长度
            double diag_length = 0.0;
            for (size_t i = 0; i < query_scope.getMin().size(); i++) {
                double diff = query_scope.getMax()[i] - query_scope.getMin()[i];
                diag_length += diff * diff;
            }
            diag_length = sqrt(diag_length);
            
            if (diag_length == 0) return 0.0;
            double normalized_dist = distance / diag_length;
            return 1.0 / (1.0 + normalized_dist);
        }
    }
    
    // 3. 对于区域文档，计算重叠比例（原来的方法）
    double overlap_area = 1.0;
    for (size_t i = 0; i < doc_location.getMin().size(); i++) {
        double overlap_min = std::max(doc_location.getMin()[i], query_scope.getMin()[i]);
        double overlap_max = std::min(doc_location.getMax()[i], query_scope.getMax()[i]);
        
        if (overlap_min >= overlap_max) {
            return 0.0;
        }
        overlap_area *= (overlap_max - overlap_min);
    }

    if (doc_area == 0) return 1.0;
    
    // 返回重叠比例，但对小重叠进行惩罚
    double overlap_ratio = overlap_area / doc_area;
    
    // 如果重叠比例很小，降低分数
    if (overlap_ratio < 0.1) {
        overlap_ratio *= 0.3; // 惩罚小重叠
    }
    
    return overlap_ratio;
}

double IRTree::computeJointRelevance(double text_relevance, double spatial_relevance, double alpha) const
{
    // 线性加权组合文本相关性和空间相关性
    return alpha * text_relevance + (1 - alpha) * spatial_relevance;
}

int IRTree::chooseLeaf(const MBR& mbr) {
    int current_id = root_node_id;
    auto current = loadNode(current_id);

    if (!current) {
        std::cerr << "Failed to load root node " << current_id << std::endl;
        return -1;
    }

    // 从根节点开始向下遍历，直到找到叶子节点
    while (current->getType() != Node::LEAF) {
        auto children = current->getChildNodes();
        if (children.empty()) break;

        // 选择需要最小扩展的子树（R树插入策略）
        int best_child_id = -1;
        double min_expansion = std::numeric_limits<double>::max();

        for (const auto& child : children) {
            MBR expanded = child->getMBR();
            expanded.expand(mbr);  // 扩展MBR以包含新文档
            double expansion = expanded.area() - child->getMBR().area();  // 计算扩展面积

            // 选择扩展面积最小的子节点，面积相同时选择面积较小的子节点
            if (best_child_id == -1 || expansion < min_expansion ||
                (expansion == min_expansion && child->getMBR().area() <
                    loadNode(best_child_id)->getMBR().area())) {
                min_expansion = expansion;
                best_child_id = child->getId();
            }
        }

        if (best_child_id == -1) break;

        // 移动到选择的子节点
        current_id = best_child_id;
        current = loadNode(current_id);
        if (!current) break;
    }

    return current_id;  // 返回选择的叶子节点ID
}

void IRTree::adjustTree(int node_id)
{
    // 首先加载节点
    auto node = loadNode(node_id);
    if (!node) {
        std::cerr << "Failed to load node " << node_id << " for adjustment" << std::endl;
        return;
    }

    // 更新节点摘要信息（MBR和文本摘要）
    node->updateSummary();

    // 保存更新后的节点
    saveNode(node_id, node);

    // 如果节点溢出，需要分裂
    if ((node->getType() == Node::LEAF && node->getDocuments().size() > max_capacity) ||
        (node->getType() == Node::INTERNAL && node->getChildNodes().size() > max_capacity)) {
        splitNode(node_id);
    }
}



void IRTree::splitNode(int node_id) {

    auto node = loadNode(node_id);
    if (node == nullptr) {
        std::cerr << "Failed to load node " << node_id << " for splitting" << std::endl;
        return;
    }

    if (node->getType() == Node::LEAF) {
        auto documents = node->getDocuments();


        if (documents.size() <= max_capacity) {
            std::cout << "  No need to split - within capacity" << std::endl;
            return;
        }


        // 按X坐标排序文档，然后分成两部分（线性分裂策略）

        std::sort(documents.begin(), documents.end(),
            [](const std::shared_ptr<Document>& a, const std::shared_ptr<Document>& b) {
                double center_a = a->getLocation().getCenter()[0];
                double center_b = b->getLocation().getCenter()[0];

                return center_a < center_b;
            });



        int split_index = documents.size() / 2;  // 中间位置分裂


        // 创建两个新节点 - 修复1：计算正确的MBR
        MBR new_mbr1 = documents[0]->getLocation();
        MBR new_mbr2 = documents[split_index]->getLocation();

        // 扩展MBR以包含所有分配的文档
        for (int i = 1; i < split_index; i++) {
            new_mbr1.expand(documents[i]->getLocation());
        }
        for (int i = split_index + 1; i < documents.size(); i++) {
            new_mbr2.expand(documents[i]->getLocation());
        }

        int new_node_id1 = createNewNode(Node::LEAF, node->getLevel(), new_mbr1);
        int new_node_id2 = createNewNode(Node::LEAF, node->getLevel(), new_mbr2);

        auto new_node1 = loadNode(new_node_id1);
        auto new_node2 = loadNode(new_node_id2);

        if (new_node1 == nullptr || new_node2 == nullptr) {
            std::cerr << "? Failed to create new nodes for splitting" << std::endl;
            return;
        }


        // 分配文档到两个新节点
        for (int i = 0; i < split_index; i++) {
            new_node1->addDocument(documents[i]);

        }
        for (int i = split_index; i < documents.size(); i++) {
            new_node2->addDocument(documents[i]);

        }


        new_node1->updateSummary();
        new_node2->updateSummary();



        // 保存新节点
        saveNode(new_node_id1, new_node1);
        saveNode(new_node_id2, new_node2);

        // 如果是根节点，创建新的根节点
        if (node_id == root_node_id) {

            MBR root_mbr = new_mbr1;
            root_mbr.expand(new_mbr2);

            int new_root_id = createNewNode(Node::INTERNAL, node->getLevel() + 1, root_mbr);
            auto new_root = loadNode(new_root_id);

            if (new_root != nullptr) {
                // 重新加载实际的子节点对象
                auto child1 = loadNode(new_node_id1);
                auto child2 = loadNode(new_node_id2);

                if (child1 != nullptr && child2 != nullptr) {
                    new_root->addChild(child1);
                    new_root->addChild(child2);
                    saveNode(new_root_id, new_root);
                    root_node_id = new_root_id;  // 更新根节点ID


                    // 删除旧的根节点
                    storage->deleteNode(node_id);

                }
                else {
                    std::cerr << "Failed to load child nodes for new root" << std::endl;
                }
            }
        }

    }
    else {
        // 内部节点的分裂 - 类似修复
        auto children = node->getChildNodes();


        if (children.size() <= max_capacity) {

            return;
        }



        // 按X坐标排序子节点
        std::sort(children.begin(), children.end(),
            [](const std::shared_ptr<Node>& a, const std::shared_ptr<Node>& b) {
                return a->getMBR().getCenter()[0] < b->getMBR().getCenter()[0];
            });

        int split_index = children.size() / 2;

        // 创建新节点 - 修复MBR计算
        MBR new_mbr1 = children[0]->getMBR();
        MBR new_mbr2 = children[split_index]->getMBR();

        for (int i = 1; i < split_index; i++) {
            new_mbr1.expand(children[i]->getMBR());
        }
        for (int i = split_index + 1; i < children.size(); i++) {
            new_mbr2.expand(children[i]->getMBR());
        }

        int new_node_id1 = createNewNode(Node::INTERNAL, node->getLevel(), new_mbr1);
        int new_node_id2 = createNewNode(Node::INTERNAL, node->getLevel(), new_mbr2);

        auto new_node1 = loadNode(new_node_id1);
        auto new_node2 = loadNode(new_node_id2);

        if (new_node1 == nullptr || new_node2 == nullptr) {
            std::cerr << " Failed to create new internal nodes for splitting" << std::endl;
            return;
        }

        // 分配子节点
        for (int i = 0; i < split_index; i++) {
            new_node1->addChild(children[i]);
        }
        for (int i = split_index; i < children.size(); i++) {
            new_node2->addChild(children[i]);
        }

        // 保存新节点
        saveNode(new_node_id1, new_node1);
        saveNode(new_node_id2, new_node2);

        // 如果是根节点，创建新的根节点
        if (node_id == root_node_id) {
            MBR root_mbr = new_mbr1;
            root_mbr.expand(new_mbr2);

            int new_root_id = createNewNode(Node::INTERNAL, node->getLevel() + 1, root_mbr);
            auto new_root = loadNode(new_root_id);

            if (new_root != nullptr) {
                auto child1 = loadNode(new_node_id1);
                auto child2 = loadNode(new_node_id2);

                if (child1 != nullptr && child2 != nullptr) {
                    new_root->addChild(child1);
                    new_root->addChild(child2);
                    saveNode(new_root_id, new_root);
                    root_node_id = new_root_id;

                    // 删除旧的根节点
                    storage->deleteNode(node_id);
                }
                else {
                    std::cerr << "Failed to load child nodes for new root" << std::endl;
                }
            }
        }
    }

}
// 初始化递归位置映射
void IRTree::initializeRecursivePositionMap() {
    // 从根节点开始递归分配路径
    int root_path = assignPathRecursively(root_node_id);

    if (root_path != -1) {
        // 存储根节点路径到 STASH（移除调试打印）
        setRootPath(root_path);
    }
    else {
        std::cerr << "Failed to assign path to root node" << std::endl;
    }

}

// 递归分配路径
int IRTree::assignPathRecursively(int node_id) {
    auto node = loadNode(node_id);
    if (!node) {
        std::cerr << "Failed to load node " << node_id << " for path assignment" << std::endl;
        return -1;
    }

    // 为当前节点分配随机路径
    int current_path = getRandomLeafPath();

    // 为路径分配块索引
    auto path_oram_storage = std::dynamic_pointer_cast<RingOramStorage>(storage);
    if (path_oram_storage) {
        int block_index = path_oram_storage->allocateBlockForPath(current_path);
        path_oram_storage->mapPathToNode(current_path, node_id);
    }

    if (node->getType() == Node::INTERNAL) {
        // 递归为子节点分配路径
        auto child_nodes = node->getChildNodes();
        for (const auto& child : child_nodes) {
            int child_id = child->getId();
            int child_path = assignPathRecursively(child_id);

            if (child_path != -1) {
                // 将子节点路径记录到父节点中
                node->setChildPosition(child_id, child_path);
            }
        }
    }

    // 保存更新后的节点（包含子节点位置映射）
    saveNode(node_id, node);

    return current_path;
}

// 获取随机叶子路径
int IRTree::getRandomLeafPath() const {
    static random_device rd;
    static mt19937 gen(rd());
    uniform_int_distribution<int> dist(0, numLeaves - 1);
    return dist(gen);
}

// 根节点路径管理（简化实现，后面需要集成到RingOramStorage）
int IRTree::getRootPath() const {
    if (!storage) {
        std::cerr << "Storage not available for root path access" << std::endl;
        return -1;
    }

    // 动态转换到RingOramStorage来获取根路径
    auto path_oram_storage = std::dynamic_pointer_cast<RingOramStorage>(storage);
    if (path_oram_storage) {
        return path_oram_storage->getRootPath();
    }
    else {
        std::cerr << "Storage is not RingOramStorage, cannot get root path" << std::endl;
        return -1;
    }
}

void IRTree::setRootPath(int path) {
    if (!storage) {
        std::cerr << "Storage not available for root path setting" << std::endl;
        return;
    }

    // 动态转换到RingOramStorage来设置根路径
    auto path_oram_storage = std::dynamic_pointer_cast<RingOramStorage>(storage);
    if (path_oram_storage) {
        path_oram_storage->setRootPath(path);
        // 移除调试打印，让 RingOramStorage 处理日志
    }
    else {
        std::cerr << "Storage is not RingOramStorage, cannot set root path" << std::endl;
    }
}

// 递归访问节点（使用路径而不是节点ID）
std::shared_ptr<Node> IRTree::accessNodeByPath(int path) {
    if (!storage) {
        std::cerr << "Storage not available for path access" << std::endl;
        return nullptr;
    }

    // 动态转换到RingOramStorage来使用路径访问
    auto path_oram_storage = std::dynamic_pointer_cast<RingOramStorage>(storage);
    if (path_oram_storage) {
        auto node_data = path_oram_storage->accessByPath(path);
        if (node_data.empty()) {
            std::cerr << "Failed to access node data for path " << path << std::endl;
            return nullptr;
        }

        // 反序列化节点数据
        auto node = NodeSerializer::deserialize(node_data);
        if (!node) {
            std::cerr << "Failed to deserialize node from path " << path << std::endl;
            return nullptr;
        }

        return node;
    }
    else {
        std::cerr << "Storage is not RingOramStorage, cannot use path-based access" << std::endl;
        return nullptr;
    }
}

void IRTree::insertDocument(const std::string& text, const MBR& location)
{
    // 创建文档对象并分配ID
    auto document = std::make_shared<Document>(next_doc_id++, location, text);
    insertDocument(document);
}

//修改 insertDocument 方法
void IRTree::insertDocument(std::shared_ptr<Document> document) {
    // 添加到全局索引 - 构建文本索引
    Vector doc_vector(document->getId());
    Vector::vectorize(doc_vector, document->getText(), vocab);
    global_index.addDocument(document->getId(), doc_vector);

    // 选择插入的叶子节点
    int leaf_id = chooseLeaf(document->getLocation());
    if (leaf_id == -1) {
        std::cerr << "Failed to choose leaf for document insertion" << std::endl;
        return;
    }

    auto leaf_node = loadNode(leaf_id);
    if (!leaf_node) {
        std::cerr << "Failed to load leaf node " << leaf_id << std::endl;
        return;
    }

    // 插入文档到叶子节点
    leaf_node->addDocument(document);
    saveNode(leaf_id, leaf_node);

    // 调整树结构（更新MBR，可能分裂）
    adjustTree(leaf_id);

    // 检查根节点是否需要分裂
    auto root_node = loadNode(root_node_id);
    if (!root_node) return;

    if ((root_node->getType() == Node::LEAF && root_node->getDocuments().size() > max_capacity) ||
        (root_node->getType() == Node::INTERNAL && root_node->getChildNodes().size() > max_capacity)) {
        splitNode(root_node_id);
    }
}



std::vector<TreeHeapEntry> IRTree::search(const Query& query)
{
    // 委托给参数化搜索方法
    return search(query.getKeywords(), query.getSpatialScope(), query.getK(), query.getAlpha());
}


int IRTree::getChildPath(std::shared_ptr<Node> parent_node, int child_id) const {
    if (!parent_node) {
        return -1; // 无效路径
    }
    
    // 方法1: 从节点的子节点位置映射中查找
    const auto& child_position_map = parent_node->getChildPositionMap();
    auto it = child_position_map.find(child_id);
    if (it != child_position_map.end()) {
        return it->second; // 返回存储的路径
    }
    
    return -1; // 未找到路径
}


std::vector<TreeHeapEntry> IRTree::search(const std::vector<std::string>& keywords,
    const MBR& spatial_scope,
    int k,
    double alpha) {

    search_blocks = 0;
    std::vector<TreeHeapEntry> results;

    if (!storage || keywords.empty() || k <= 0) {
        return results;
    }

    // 获取根节点路径
    int root_path = getRootPath();
    if (root_path == -1) {
        std::cerr << "Failed to get root path for search" << std::endl;
        return results;
    }

    // 加载根节点
    auto root_node = accessNodeByPath(root_path);
    if (!root_node) {
        std::cerr << "Failed to load root node using path " << root_path << std::endl;
        return results;
    }

    // 使用优先队列（只存储节点，不存储文档）
    std::priority_queue<TreeHeapEntry, std::vector<TreeHeapEntry>, TreeHeapComparator> queue;

    // 计算根节点相关性
    double root_relevance = computeNodeRelevance(root_node, keywords, spatial_scope, alpha);
    if (root_relevance > 0) {
        queue.push(TreeHeapEntry(root_node, root_path, root_relevance));
    }

    int nodes_visited = 0;
    int documents_checked = 0;
    double threshold = 0.0;
    int pruned_nodes = 0;
    int pruned_leaves = 0;

    // ============ 关键修改：更灵活的循环条件 ============
    while (!queue.empty() && results.size() < k) {
        TreeHeapEntry current = queue.top();
        queue.pop();
        nodes_visited++;

        auto node = current.node; // 确保current总是节点
        
        // ============ 节点剪枝 ============
        if (results.size() > 0 && current.score < threshold * 0.9) {
            pruned_nodes++;
            continue;
        }

        if (node->getType() == Node::LEAF) {
            // ============ 叶子节点处理 ============
            // 快速预检查
            if (!node->getMBR().overlaps(spatial_scope)) {
                continue; // 无空间重叠
            }
            
            // 检查关键词匹配（30%阈值）
            bool has_potential = false;
            int keyword_match = 0;
            const double KEYWORD_THRESHOLD = 0.3;
            int min_needed = std::max(1, (int)(keywords.size() * KEYWORD_THRESHOLD));
            
            for (const auto& keyword : keywords) {
                if (node->getDocumentFrequency(keyword) > 0) {
                    keyword_match++;
                    if (keyword_match >= min_needed) {
                        has_potential = true;
                        break;
                    }
                }
            }
            
            if (!has_potential) {
                pruned_leaves++;
                continue;
            }
            
            // 计算叶子上界
            double leaf_upper_bound = computeNodeRelevance(node, keywords, spatial_scope, alpha);
            if (results.size() > 0 && leaf_upper_bound < threshold * 0.8) {
                pruned_leaves++;
                continue;
            }
            
            // 处理叶子节点中的文档
            int prev_results = results.size();
            processLeafNode(node, keywords, spatial_scope, alpha, results, k, threshold);
            documents_checked += (results.size() - prev_results);
            
            // 如果达到k个结果，更新阈值
            if (results.size() >= k) {
                // 使用最小堆维护top-k
                std::make_heap(results.begin(), results.end(), 
                    [](const TreeHeapEntry& a, const TreeHeapEntry& b) {
                        return a.score > b.score; // 最小堆
                    });
                threshold = results.front().score; // 最小分数
                
                // 注意：可以在此考虑提前终止
                // 如果队列中最高分 < threshold，可以提前结束
                if (!queue.empty() && queue.top().score < threshold) {
                    break;
                }
            }
        }
        else {
            // ============ 内部节点处理 ============
            processInternalNodeWithPath(node, current.path, keywords, 
                                       spatial_scope, alpha, queue);
        }
    }

    search_blocks = nodes_visited * (OramL - cacheLevel);
    
    // 最终排序（降序）
    std::sort(results.begin(), results.end(),
        [](const TreeHeapEntry& a, const TreeHeapEntry& b) {
            return a.score > b.score;
        });

    // ============ 统计输出 ============
    if (nodes_visited > 0) {
        std::cout << "=== SEARCH STATISTICS ===" << std::endl;
        std::cout << "  Total nodes visited: " << nodes_visited << std::endl;
        std::cout << "  Final results: " << results.size() << std::endl;
    }

    return results;
}



void IRTree::bulkInsertFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open file " << filename << std::endl;
        return;
    }

    std::vector<std::tuple<std::string, double, double>> documents;
    std::string line;
    int loaded_count = 0;

    // 逐行解析文件
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string text;
        double lon, lat;
        std::string id_str;

        // 解析格式: 文本|经度|纬度|ID(可选)
        if (std::getline(iss, text, '|') &&
            iss >> lon &&
            iss.ignore() && // 忽略分隔符
            iss >> lat) {

            documents.emplace_back(text, lon, lat);
            loaded_count++;
        }
    }

    file.close();

    // 批量插入解析后的文档数据
    bulkInsertDocuments(documents);
}

void IRTree::bulkInsertDocuments(const std::vector<std::tuple<std::string, double, double>>& documents) {
    auto start_time = std::chrono::high_resolution_clock::now();
    int inserted_count = 0;

    // 逐个插入文档（顺序插入，性能较低）
    for (const auto& doc_data : documents) {
        const auto& text = std::get<0>(doc_data);
        double lon = std::get<1>(doc_data);
        double lat = std::get<2>(doc_data);

        // 创建MBR（使用点位置，设置小的边界框）
        double epsilon = 0.001; // 小偏移量，使MBR不为点
        MBR location({ lon - epsilon, lat - epsilon }, { lon + epsilon, lat + epsilon });

        // 插入文档
        insertDocument(text, location);
        inserted_count++;

    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    // 注意：这里没有输出耗时信息
}

// 优化的批量插入方法
void IRTree::optimizedBulkInsertFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open file " << filename << std::endl;
        return;
    }

    std::vector<std::tuple<std::string, double, double>> documents;
    std::string line;
    int loaded_count = 0;
    const int REPORT_INTERVAL = 1000;  // 进度报告间隔

    auto load_start = std::chrono::high_resolution_clock::now();

    // 快速文件解析 - 使用字符串操作替代stringstream
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        // 使用更快的字符串解析
        size_t pos1 = line.find('|');
        if (pos1 == std::string::npos) continue;

        size_t pos2 = line.find('|', pos1 + 1);
        if (pos2 == std::string::npos) continue;

        std::string text = line.substr(0, pos1);
        std::string lon_str = line.substr(pos1 + 1, pos2 - pos1 - 1);
        std::string lat_str = line.substr(pos2 + 1);

        try {
            double lon = std::stod(lon_str);
            double lat = std::stod(lat_str);
            documents.emplace_back(text, lon, lat);
            loaded_count++;

            // 定期报告加载进度
            if (loaded_count % REPORT_INTERVAL == 0) {
                std::cout << "Loaded " << loaded_count << " records..." << std::endl;
            }
        }
        catch (const std::exception& e) {
            continue; // 跳过解析错误的数据
        }
    }

    file.close();

    auto load_end = std::chrono::high_resolution_clock::now();
    auto load_duration = std::chrono::duration_cast<std::chrono::milliseconds>(load_end - load_start);
    std::cout << "File loading completed: " << loaded_count << " records in "
        << load_duration.count() << " ms" << std::endl;

    // 使用优化的批量插入
    optimizedBulkInsertDocuments(documents);
}

void IRTree::optimizedBulkInsertDocuments(const std::vector<std::tuple<std::string, double, double>>& documents) {
    if (documents.empty()) return;

    auto total_start = std::chrono::high_resolution_clock::now();
    std::cout << "Starting bulk insertion of " << documents.size() << " documents..." << std::endl;

    // 阶段1：批量创建文档对象（并行化）
    std::vector<std::shared_ptr<Document>> doc_objects;
    doc_objects.reserve(documents.size());

    auto create_start = std::chrono::high_resolution_clock::now();

    // 使用更高效的方式创建文档 - OpenMP并行化
#pragma omp parallel for
    for (int i = 0; i < documents.size(); i++) {
        const auto& doc_data = documents[i];
        const auto& text = std::get<0>(doc_data);
        double lon = std::get<1>(doc_data);
        double lat = std::get<2>(doc_data);

        double epsilon = 0.001;
        MBR location({ lon - epsilon, lat - epsilon }, { lon + epsilon, lat + epsilon });

#pragma omp critical
        {
            // 临界区保护共享资源
            auto document = std::make_shared<Document>(next_doc_id++, location, text);
            doc_objects.push_back(document);
        }
    }

    auto create_end = std::chrono::high_resolution_clock::now();
    auto create_duration = std::chrono::duration_cast<std::chrono::milliseconds>(create_end - create_start);
    std::cout << "Document objects created: " << create_duration.count() << " ms" << std::endl;

    // 阶段2：批量构建全局索引（优化版本）
    bulkBuildGlobalIndex(doc_objects);

    // 阶段3：使用自底向上的方式构建树（关键优化）
    buildTreeBottomUp(doc_objects);

    auto total_end = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(total_end - total_start);

    std::cout << "bulk insertion completed: " << documents.size()
        << " documents in " << total_duration.count() << " ms" << std::endl;
}

void IRTree::bulkBuildGlobalIndex(const std::vector<std::shared_ptr<Document>>& documents) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // 批量词汇表构建 - 先收集所有词汇
    std::unordered_map<std::string, int> term_frequencies;

    // 先收集所有词汇及其频率
    for (const auto& doc : documents) {
        std::istringstream iss(doc->getText());
        std::string word;
        while (iss >> word) {
            term_frequencies[word]++;
        }
    }

    // 批量添加到词汇表 
    for (const auto& term : term_frequencies) {
        int term_id = vocab.addTerm(term.first);
    
        // 验证添加是否成功
        int verify_id = vocab.getTermId(term.first);
        if (verify_id != term_id) {
            std::cerr << "ERROR: Vocabulary inconsistency for term '" << term.first
                << "'. Added as " << term_id << " but found as " << verify_id << std::endl;
        }
    }

    // 批量构建文档向量
    for (const auto& doc : documents) {
        Vector doc_vector(doc->getId());
        Vector::vectorize(doc_vector, doc->getText(), vocab);
        global_index.addDocument(doc->getId(), doc_vector);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::cout << "Optimized global index built: " << duration.count() << " ms" << std::endl;
}

void IRTree::bulkInsertToTree(const std::vector<std::shared_ptr<Document>>& documents) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // 按空间位置排序，提高局部性
    std::vector<std::shared_ptr<Document>> sorted_docs = documents;
    std::sort(sorted_docs.begin(), sorted_docs.end(),
        [](const std::shared_ptr<Document>& a, const std::shared_ptr<Document>& b) {
            return a->getLocation().getCenter()[0] < b->getLocation().getCenter()[0];
        });

    // 分组插入到叶子节点
    std::unordered_map<int, std::shared_ptr<Node>> leaf_nodes;
    std::unordered_map<int, std::vector<std::shared_ptr<Document>>> leaf_docs;

    // 第一步：收集每个叶子节点要插入的文档
    int choose_leaf_count = 0;
    for (const auto& doc : sorted_docs) {
        int leaf_id = chooseLeaf(doc->getLocation());
        if (leaf_id != -1) {
            leaf_docs[leaf_id].push_back(doc);
            choose_leaf_count++;

            if (choose_leaf_count % 1000 == 0) {
                std::cout << "Processed " << choose_leaf_count << " documents for leaf assignment..." << std::endl;
            }
        }
    }

    // 第二步：批量加载叶子节点
    for (const auto& entry : leaf_docs) {
        int leaf_id = entry.first;
        auto node = loadNode(leaf_id);
        if (node) {
            leaf_nodes[leaf_id] = node;
        }
    }

    // 第三步：批量添加文档到节点
    int insert_count = 0;
    for (const auto& entry : leaf_docs) {
        int leaf_id = entry.first;
        auto node = leaf_nodes[leaf_id];
        if (node) {
            for (const auto& doc : entry.second) {
                node->addDocument(doc);
                insert_count++;
            }

            saveNode(leaf_id, node);
        }
    }

    // 第四步：批量调整树结构（延迟调整）
    int adjust_count = 0;
    for (const auto& entry : leaf_nodes) {
        adjustTree(entry.first);
        adjust_count++;

        if (adjust_count % 100 == 0) {
            std::cout << "Adjusted " << adjust_count << " nodes..." << std::endl;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::cout << "Tree insertion completed: " << insert_count << " documents in "
        << duration.count() << " ms" << std::endl;
}

// 刷新缓存 - 将所有缓存节点写回存储
void IRTree::flushNodeCache() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    for (const auto& entry : node_cache) {
        storage->storeNode(entry.first, NodeSerializer::serialize(*entry.second));
    }
    node_cache.clear();
}

// 缓存优化的节点加载 - 先查缓存，没有再加载
std::shared_ptr<Node> IRTree::cachedLoadNode(int node_id) const {
    std::lock_guard<std::mutex> lock(cache_mutex);

    auto it = node_cache.find(node_id);
    if (it != node_cache.end()) {
        return it->second;  // 缓存命中
    }

    // 缓存未命中，从存储加载
    auto node = loadNode(node_id);
    if (node) {
        // 简单的缓存管理：如果缓存太大，清除一些（LRU近似）
        if (node_cache.size() >= MAX_CACHE_SIZE) {
            node_cache.erase(node_cache.begin());
        }
        node_cache[node_id] = node;
    }

    return node;
}

// 缓存优化的节点保存 - 更新缓存并异步保存到存储
void IRTree::cachedSaveNode(int node_id, std::shared_ptr<Node> node) {
    if (!node) return;

    std::lock_guard<std::mutex> lock(cache_mutex);
    node_cache[node_id] = node;

    // 异步保存到存储
    storage->storeNode(node_id, NodeSerializer::serialize(*node));
}

// 关键优化：自底向上构建树 - 避免频繁的树调整操作
void IRTree::buildTreeBottomUp(const std::vector<std::shared_ptr<Document>>& documents) {
    auto start_time = std::chrono::high_resolution_clock::now();

    if (documents.empty()) return;

    // 按空间位置聚类 - 提高局部性
    std::vector<std::shared_ptr<Document>> sorted_docs = documents;
    std::sort(sorted_docs.begin(), sorted_docs.end(),
        [](const std::shared_ptr<Document>& a, const std::shared_ptr<Document>& b) {
            return a->getLocation().getCenter()[0] < b->getLocation().getCenter()[0];
        });

    // 直接创建叶子节点，避免频繁的chooseLeaf调用
    std::vector<std::shared_ptr<Node>> leaf_nodes;
    const int LEAF_CAPACITY = max_capacity;

    // 批量创建叶子节点
    for (size_t i = 0; i < sorted_docs.size(); i += LEAF_CAPACITY) {
        size_t end_index = std::min(i + LEAF_CAPACITY, sorted_docs.size());

        // 计算叶子节点的MBR - 包含该批次所有文档
        MBR leaf_mbr = sorted_docs[i]->getLocation();
        for (size_t j = i + 1; j < end_index; j++) {
            leaf_mbr.expand(sorted_docs[j]->getLocation());
        }

        // 创建叶子节点
        int leaf_id = createNewNode(Node::LEAF, 0, leaf_mbr);
        auto leaf_node = cachedLoadNode(leaf_id);

        if (leaf_node) {
            // 批量添加文档
            for (size_t j = i; j < end_index; j++) {
                leaf_node->addDocument(sorted_docs[j]);
            }
            cachedSaveNode(leaf_id, leaf_node);
            leaf_nodes.push_back(leaf_node);
        }


        //// 定期报告进度
        //if (leaf_nodes.size() % 100 == 0) {
        //    std::cout << "Created " << leaf_nodes.size() << " leaf nodes..." << std::endl;
        //}
    }

    std::cout << "Created " << leaf_nodes.size() << " leaf nodes total" << std::endl;

    // 自底向上构建树 - 从叶子节点开始构建上层节点
    std::vector<std::shared_ptr<Node>> current_level = leaf_nodes;
    int level = 1;

    while (current_level.size() > 1) {
        std::vector<std::shared_ptr<Node>> next_level;

        // 按空间位置排序当前层节点
        std::sort(current_level.begin(), current_level.end(),
            [](const std::shared_ptr<Node>& a, const std::shared_ptr<Node>& b) {
                return a->getMBR().getCenter()[0] < b->getMBR().getCenter()[0];
            });

        // 创建父节点
        for (size_t i = 0; i < current_level.size(); i += max_capacity) {
            size_t end_index = std::min(i + max_capacity, current_level.size());

            // 计算父节点的MBR - 包含所有子节点
            MBR parent_mbr = current_level[i]->getMBR();
            for (size_t j = i + 1; j < end_index; j++) {
                parent_mbr.expand(current_level[j]->getMBR());
            }

            // 创建内部节点
            int parent_id = createNewNode(Node::INTERNAL, level, parent_mbr);
            auto parent_node = cachedLoadNode(parent_id);

            if (parent_node) {
                // 添加子节点
                for (size_t j = i; j < end_index; j++) {
                    parent_node->addChild(current_level[j]);
                }
                cachedSaveNode(parent_id, parent_node);
                next_level.push_back(parent_node);
            }
        }


        current_level = next_level;
        level++;
    }

    // 更新根节点
    if (!current_level.empty()) {
        root_node_id = current_level[0]->getId();
    }

    // 刷新所有缓存到存储
    flushNodeCache();

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::cout << "Bottom-up tree construction completed in " << duration.count() << " ms" << std::endl;
    initializeRecursivePositionMap();
}




std::chrono::nanoseconds IRTree::getRunTime(const std::string& query_keywords, const MBR& scope, int k, bool show_details) {
    // 分割关键词
    std::vector<std::string> keywords;
    std::istringstream iss(query_keywords);
    std::string keyword;
    while (iss >> keyword) {
        keywords.push_back(keyword);
    }



    auto start_time = std::chrono::high_resolution_clock::now();

    // 执行搜索
    auto results = search(keywords, scope, k, 0.5);

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time);



    // 打印关键词和搜索结果
    if (show_details) {
        std::cout << "\n" << std::string(40, '=') << std::endl;
        std::cout << "QUERY: '" << query_keywords << "'" << std::endl;
        std::cout << std::string(40, '=') << std::endl;

        std::cout << "RESULTS: " << results.size() << " documents found" << std::endl;
        std::cout << "Time: " << duration.count() / 1000000.0 << " ms" << std::endl;


        if (!results.empty()) {
            for (size_t i = 0; i < results.size(); i++) {
                const auto& result = results[i];
                if (result.isData()) {
                    auto doc = result.document;
                    std::cout << "  " << (i + 1) << ". Doc " << doc->getId()
                        << " - Score: " << result.score
                        << " - '" << doc->getText() << "'" << std::endl;
                }
            }
        }
        else {
            std::cout << "  No results found" << std::endl;
        }
    }


    return duration;
}

