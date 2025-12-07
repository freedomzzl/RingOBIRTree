
#include "Vocabulary.h"
#include <sstream>
#include <stdexcept>

Vocabulary::Vocabulary() : next_id(0) {}

int Vocabulary::addTerm(const std::string& term) {
    if (term.empty()) {
        return -1;
    }

    auto it = term_to_id.find(term);
    if (it != term_to_id.end()) {
        return it->second;
    }

    int term_id = next_id++;
    term_to_id[term] = term_id;
    id_to_term.push_back(term);
    return term_id;
}

int Vocabulary::getTermId(const std::string& term) const {
    auto it = term_to_id.find(term);
    return (it != term_to_id.end()) ? it->second : -1;
}

std::string Vocabulary::getTerm(int term_id) const {
    if (term_id < 0 || term_id >= static_cast<int>(id_to_term.size())) {
        throw std::out_of_range("Term ID out of range: " + std::to_string(term_id));
    }
    return id_to_term[term_id];
}

void Vocabulary::clear() {
    term_to_id.clear();
    id_to_term.clear();
    next_id = 0;
}

std::string Vocabulary::toString() const {
    std::stringstream ss;
    ss << "Vocabulary[size=" << size() << "] {";

    int count = 0;
    for (const auto& term : id_to_term) {
        if (count++ >= 10) break; // Ö»ÏÔÊ¾Ç°10¸öÊõÓï
        if (count > 1) ss << ", ";
        ss << term << ":" << term_to_id.at(term);
    }
    if (size() > 10) ss << ", ...";
    ss << "}";

    return ss.str();
}

