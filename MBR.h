#ifndef MBR_H
#define MBR_H

#include <vector>
#include<string>
#include <cmath>

class MBR {
private:
    std::vector<double> min_coords;
    std::vector<double> max_coords;

public:
    MBR(const std::vector<double>& min, const std::vector<double>& max);


    double area() const;
    void expand(const MBR& other);
    bool contains(const MBR& other) const;
    bool overlaps(const MBR& other) const;
    double minDistance(const std::vector<double>& point, int p_norm = 2) const;

    // Getter/Setter
    const std::vector<double>& getMin() const { return min_coords; }
    const std::vector<double>& getMax() const { return max_coords; }

    std::string toString() const;

    std::vector<double> getCenter() const {
        std::vector<double> center;
        for (size_t i = 0; i < min_coords.size(); i++) {
            center.push_back((min_coords[i] + max_coords[i]) / 2.0);
        }
        return center;
    }

    double getDiagonalLength() const {
        double sum = 0.0;
        for (size_t i = 0; i < min_coords.size(); i++) {
            sum += std::pow(max_coords[i] - min_coords[i], 2);
        }
        return std::sqrt(sum);
    }
};

#endif