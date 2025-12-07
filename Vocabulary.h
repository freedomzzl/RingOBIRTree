#ifndef VOCABULARY_H
#define VOCABULARY_H

#include <string>
#include <unordered_map>
#include <vector>

class Vocabulary {
private:
    std::unordered_map<std::string, int> term_to_id;
    std::vector<std::string> id_to_term;
    int next_id;

public:
    Vocabulary();

    // 添加术语并返回ID，如果已存在则返回现有ID
    int addTerm(const std::string& term);

    // 获取术语ID，如果不存在返回-1
    int getTermId(const std::string& term) const;

    // 根据ID获取术语
    std::string getTerm(int term_id) const;

    // 获取词汇表大小
    size_t size() const { return id_to_term.size(); }

    // 清空词汇表
    void clear();

    std::string toString() const;
};

#endif