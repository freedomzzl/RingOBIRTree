
#include "Vector.h"
#include "Vocabulary.h"
#include <sstream>
#include <cmath>
#include <algorithm>
#include <cctype>

Vector::Vector(int id) : doc_id(id) {}

void Vector::addTerm(int term_id, double weight) {
    if (term_weights.find(term_id) == term_weights.end()) {
        term_weights[term_id] = weight;
    }
    else {
        term_weights[term_id] += weight;
    }
}

void Vector::setTermWeight(int term_id, double weight) {
    term_weights[term_id] = weight;
}

double Vector::getTermWeight(int term_id) const {
    auto it = term_weights.find(term_id);
    return (it != term_weights.end()) ? it->second : 0.0;
}

void Vector::aggregate(const Vector& other) {
    for (const auto& pair : other.term_weights) {
        int term_id = pair.first;
        double weight = pair.second;

        // 对于聚合，我们取最大权重（用于节点摘要的TF_max）
        if (term_weights.find(term_id) == term_weights.end() || term_weights[term_id] < weight) {
            term_weights[term_id] = weight;
        }
    }
}

double Vector::dotProduct(const Vector& other) const {
    double result = 0.0;
    for (const auto& pair : term_weights) {
        int term_id = pair.first;
        result += pair.second * other.getTermWeight(term_id);
    }
    return result;
}

double Vector::magnitude() const {
    double sum = 0.0;
    for (const auto& pair : term_weights) {
        sum += pair.second * pair.second;
    }
    return std::sqrt(sum);
}

double Vector::cosineSimilarity(const Vector& other) const {
    double dot = dotProduct(other);
    double mag1 = magnitude();
    double mag2 = other.magnitude();

    if (mag1 == 0 || mag2 == 0) {
        return 0.0;
    }

    return dot / (mag1 * mag2);
}

void Vector::vectorize(Vector& vector, const std::string& text, Vocabulary& vocab) {
    std::stringstream ss(text);
    std::string word;
    std::unordered_map<std::string, int> term_freq;

    // 第一步：分词和统计词频
    while (ss >> word) {
        // 文本清理：转换为小写，移除标点
        std::transform(word.begin(), word.end(), word.begin(), ::tolower);
        word.erase(std::remove_if(word.begin(), word.end(), ::ispunct), word.end());

        if (!word.empty()) {
            term_freq[word]++;
        }
    }

    // 第二步：转换为向量表示
    for (const auto& pair : term_freq) {
        const std::string& term = pair.first;
        int tf = pair.second;

        int term_id = vocab.addTerm(term);
        if (term_id != -1) {
            // 使用原始TF值，后续可以应用TF-IDF
            vector.addTerm(term_id, static_cast<double>(tf));
        }
    }
}

double Vector::computeTFIDFWeight(int tf, int df, int total_docs) {
    if (tf == 0 || df == 0 || total_docs == 0) {
        return 0.0;
    }

    double tf_component = std::log(1 + tf); // 对数TF
    double idf_component = std::log(static_cast<double>(total_docs) / df);

    return tf_component * idf_component;
}

std::string Vector::toString(const Vocabulary& vocab) const {
    std::stringstream ss;
    ss << "Vector[doc_id=" << doc_id << ", terms=" << term_weights.size() << "] {";

    int count = 0;
    for (const auto& pair : term_weights) {
        if (count++ >= 5) break; // 只显示前5个术语
        if (count > 1) ss << ", ";

        std::string term_name;
        try {
            term_name = vocab.getTerm(pair.first);
        }
        catch (const std::out_of_range&) {
            term_name = "unknown(" + std::to_string(pair.first) + ")";
        }

        ss << term_name << ":" << pair.second;
    }
    if (term_weights.size() > 5) ss << ", ...";
    ss << "}";

    return ss.str();
}

