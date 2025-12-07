#include "Document.h"
#include <sstream>
#include <algorithm>
#include <cctype>

Document::Document(int id, const MBR& loc, const std::string& text)
    : doc_id(id), location(loc), raw_text(text) {
    if (!text.empty()) {
        processText(text);
    }
}

void Document::processText(const std::string& text) {
    term_freq.clear();

    std::stringstream ss(text);
    std::string word;

    while (ss >> word) {
        // 文本清理：转换为小写，移除标点
        std::transform(word.begin(), word.end(), word.begin(), ::tolower);
        word.erase(std::remove_if(word.begin(), word.end(), ::ispunct), word.end());

        if (!word.empty()) {
            addTerm(word);
        }
    }
}

void Document::addTerm(const std::string& term, int freq) {
    if (term_freq.find(term) == term_freq.end()) {
        term_freq[term] = freq;
    }
    else {
        term_freq[term] += freq;
    }
}

int Document::getTermFrequency(const std::string& term) const {
    auto it = term_freq.find(term);
    return (it != term_freq.end()) ? it->second : 0;
}

std::string Document::toString() const {
    std::stringstream ss;
    ss << "Document[id=" << doc_id
        << ", location=" << location.toString()
        << ", terms=" << term_freq.size() << "]";

    // 显示前5个词频最高的术语（用于调试）
    int count = 0;
    ss << " {";
    for (const auto& pair : term_freq) {
        if (count++ >= 5) break;
        if (count > 1) ss << ", ";
        ss << pair.first << ":" << pair.second;
    }
    if (term_freq.size() > 5) ss << ", ...";
    ss << "}";

    return ss.str();
}
