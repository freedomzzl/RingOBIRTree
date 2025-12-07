#include "Query.h"
#include <sstream>

Query::Query(const std::vector<std::string>& kw, const MBR& scope, int top_k, double a)
    : keywords(kw), spatial_scope(scope), k(top_k), alpha(a) {
    if (k <= 0) {
        throw std::invalid_argument("k must be positive");
    }
    if (alpha < 0 || alpha > 1) {
        throw std::invalid_argument("alpha must be between 0 and 1");
    }
}

std::string Query::toString() const {
    std::stringstream ss;
    ss << "Query[keywords=(";
    for (size_t i = 0; i < keywords.size(); i++) {
        if (i > 0) ss << ", ";
        ss << keywords[i];
    }
    ss << "), scope=" << spatial_scope.toString()
        << ", k=" << k << ", alpha=" << alpha << "]";
    return ss.str();
}
