
#include "node.h"
#include "document.h"
#include <sstream>
#include <algorithm>

Node::Node(int id, Type node_type, int node_level, const MBR& node_mbr)
    : node_id(id), type(node_type), mbr(node_mbr), level(node_level), document_count(0) {
}

void Node::addChild(std::shared_ptr<Node> child) {
    if (type != INTERNAL) {
        throw std::logic_error("Cannot add child to leaf node");
    }

    child_nodes.push_back(child);
    mbr.expand(child->getMBR());
    updateSummary();
}

void Node::addDocument(std::shared_ptr<Document> doc) {
    if (type != LEAF) {
        throw std::logic_error("Cannot add document to internal node");
    }

    documents.push_back(doc);
    mbr.expand(doc->getLocation());
    updateSummary();
}

void Node::updateSummary() {
    document_count = 0;  // 重置计数
    df.clear();
    tf_max.clear();

    if (type == LEAF) {
        // 叶子节点：统计文档中的词频
        for (const auto& doc : documents) {
            document_count++;  // 每个文档只计数一次
            const auto& term_freq_map = doc->getTermFreq();
            for (const auto& pair : term_freq_map) {
                const std::string& term = pair.first;
                int freq = pair.second;

                df[term]++; // 文档频率
                if (tf_max[term] < freq) {
                    tf_max[term] = freq; // 最大词频
                }
            }
        }
    }
    else {
        // 内部节点：聚合子节点的统计信息
        for (const auto& child : child_nodes) {
            document_count += child->getDocumentCount();

            // 合并文档频率
            const auto& child_df = child->getDF();
            for (const auto& pair : child_df) {
                df[pair.first] += pair.second;
            }

            // 取最大词频
            const auto& child_tf_max = child->getTFMax();
            for (const auto& pair : child_tf_max) {
                if (tf_max.find(pair.first) == tf_max.end() || tf_max[pair.first] < pair.second) {
                    tf_max[pair.first] = pair.second;
                }
            }
        }
    }
}

int Node::getDocumentFrequency(const std::string& term) const {
    auto it = df.find(term);
    return (it != df.end()) ? it->second : 0;
}

int Node::getMaxTermFrequency(const std::string& term) const {
    auto it = tf_max.find(term);
    return (it != tf_max.end()) ? it->second : 0;
}

std::string Node::toString() const {
    std::stringstream ss;
    ss << "Node[id=" << node_id
        << ", type=" << (type == LEAF ? "LEAF" : "INTERNAL")
        << ", level=" << level
        << ", documents=" << document_count
        << ", " << mbr.toString() << "]";

    // 显示前5个最常见的术语（用于调试）
    int count = 0;
    ss << " {df=";
    for (const auto& pair : df) {
        if (count++ >= 5) break;
        if (count > 1) ss << ", ";
        ss << pair.first << ":" << pair.second;
    }
    if (df.size() > 5) ss << ", ...";
    ss << "}";

    return ss.str();
}
