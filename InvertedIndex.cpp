#include "InvertedIndex.h"
#include "Vocabulary.h"  
#include <sstream>
#include <algorithm>

InvertedIndex::InvertedIndex() : total_documents(0) {}

void InvertedIndex::addDocument(int doc_id, Vector& vector) {
    total_documents++;

    for (const auto& pair : vector.getTermWeights()) {
        int term_id = pair.first;
        double weight = pair.second;

        // 添加到倒排列表
        auto& postings = index[term_id];
        postings.emplace_back(doc_id, weight);
    }
}

std::vector<Posting> InvertedIndex::getPostings(int term_id) const {
    auto it = index.find(term_id);
    if (it != index.end()) {
        return it->second;
    }
    return {};
}

std::vector<int> InvertedIndex::getDocumentsWithTerm(int term_id) const {
    std::vector<int> doc_ids;
    auto it = index.find(term_id);
    if (it != index.end()) {
        for (const auto& posting : it->second) {
            doc_ids.push_back(posting.doc_id);
        }
    }
    return doc_ids;
}

int InvertedIndex::getDocumentFrequency(int term_id) const {
    auto it = index.find(term_id);
    if (it != index.end()) {
        return it->second.size();
    }
    return 0;
}

void InvertedIndex::clear() {
    index.clear();
    total_documents = 0;
}

void InvertedIndex::merge(const InvertedIndex& other) {
    for (const auto& pair : other.index) {
        int term_id = pair.first;
        const auto& other_postings = pair.second;

        auto& this_postings = index[term_id];
        this_postings.insert(this_postings.end(), other_postings.begin(), other_postings.end());
    }
    total_documents += other.total_documents;
}

std::string InvertedIndex::toString(Vocabulary& vocab) {
    std::stringstream ss;
    ss << "InvertedIndex[total_docs=" << total_documents << ", terms=" << index.size() << "]";

    // 显示前5个术语的倒排列表
    int term_count = 0;
    for (const auto& pair : index) {
        if (term_count++ >= 3) break; // 只显示前3个术语

        int term_id = pair.first;
        const auto& postings = pair.second;

        std::string term_name;
        try {
            term_name = vocab.getTerm(term_id);
        }
        catch (const std::out_of_range&) {
            term_name = "unknown(" + std::to_string(term_id) + ")";
        }

        ss << "\n  " << term_name << " (df=" << postings.size() << "): [";

        int posting_count = 0;
        for (const auto& posting : postings) {
            if (posting_count++ >= 3) break; // 只显示前3个posting
            if (posting_count > 1) ss << ", ";
            ss << posting.doc_id << ":" << posting.weight;
        }
        if (postings.size() > 3) ss << ", ...";
        ss << "]";
    }
    if (index.size() > 3) ss << "\n  ...";

    return ss.str();
}