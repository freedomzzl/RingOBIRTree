#ifndef VECTOR_H
#define VECTOR_H

#include <unordered_map>
#include <vector>
#include <string>
#include <memory>

class Vocabulary;

class Vector {
private:
    int doc_id;
    std::unordered_map<int, double> term_weights; // term_id -> weight

public:
    Vector(int id = -1);

    // 向量操作
    void addTerm(int term_id, double weight);
    void setTermWeight(int term_id, double weight);
    double getTermWeight(int term_id) const;

    // 向量聚合（用于节点摘要）
    void aggregate(const Vector& other);

    // 向量运算
    double dotProduct(const Vector& other) const;
    double magnitude() const;
    double cosineSimilarity(const Vector& other) const;

    // 文本处理
    static void vectorize(Vector& vector, const std::string& text, Vocabulary& vocab);
    static double computeTFIDFWeight(int tf, int df, int total_docs);

    // Getter
    int getId() const { return doc_id; }
    void setId(int id) { doc_id = id; }
    const std::unordered_map<int, double>& getTermWeights() const { return term_weights; }
    size_t size() const { return term_weights.size(); }

    std::string toString(const Vocabulary& vocab) const;
};

#endif