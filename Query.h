#ifndef QUERY_H
#define QUERY_H

#include <vector>
#include <string>
#include "MBR.h"

class Query {
private:
    std::vector<std::string> keywords;
    MBR spatial_scope;
    int k;
    double alpha; // ÎÄ±¾ vs ¿Õ¼äÈ¨ÖØ

public:
    Query(const std::vector<std::string>& kw, const MBR& scope, int top_k = 10, double a = 0.5);

    // Getter
    const std::vector<std::string>& getKeywords() const { return keywords; }
    const MBR& getSpatialScope() const { return spatial_scope; }
    int getK() const { return k; }
    double getAlpha() const { return alpha; }

    std::string toString() const;
};

#endif