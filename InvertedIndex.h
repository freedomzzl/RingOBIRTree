
#ifndef INVERTED_INDEX_H
#define INVERTED_INDEX_H

#include <unordered_map>
#include <vector>
#include <memory>
#include "Vector.h"

struct Posting {
    int doc_id;
    double weight;

    Posting(int id, double w) : doc_id(id), weight(w) {}
};

class InvertedIndex {
private:
    std::unordered_map<int, std::vector<Posting>> index; // term_id -> postings list
    int total_documents;

public:
    InvertedIndex();

    // 添加文档到索引
    void addDocument(int doc_id, Vector& vector);

    // 查询处理
    std::vector<Posting> getPostings(int term_id) const;
    std::vector<int> getDocumentsWithTerm(int term_id) const;

    // 统计信息
    int getDocumentFrequency(int term_id) const;
    int getTotalDocuments() const { return total_documents; }

    // 索引管理
    void clear();
    void merge(const InvertedIndex& other);

    std::string toString(Vocabulary& vocab);
};

#endif
