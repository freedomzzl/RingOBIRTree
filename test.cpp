#include "IRTree.h"
#include "RingOramStorage.h"
#include"NodeSerializer.h"
#include "query.h"
#include <iostream>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip> 

void testWithQueryFile(const std::string& query_filename, bool show_details = true) {
    std::cout << "=== TESTING WITH QUERY FILE: " << query_filename << " ===" << std::endl;

    // 初始化IR-tree
    auto storage = std::make_shared<RingOramStorage>(totalnumRealblock, blocksize);
    IRTree tree(storage, 2, 2, 5);
    tree.optimizedBulkInsertFromFile("large_data.txt");

    std::ifstream query_file(query_filename);
    if (!query_file.is_open()) {
        std::cerr << "Error: Cannot open query file " << query_filename << std::endl;
        return;
    }

    std::vector<std::chrono::nanoseconds> query_times;
    std::vector<double> query_bandwidths;  // 存储每个查询的带宽(KB)
    std::vector<size_t> query_blocks;      // 存储每个查询的块数
    std::string line;
    int query_count = 0;

    while (std::getline(query_file, line)) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string text;
        double x, y;

        // 解析格式: 文本 经度 纬度
        if (iss >> text >> x >> y) {
            query_count++;

            // 创建搜索范围
            double epsilon = 0.01;
            MBR search_scope({ x - epsilon, y - epsilon }, { x + epsilon, y + epsilon });

            //// 重置ORAM统计用于这个查询
            //tree.resetOramStats();
            //auto initial_stats = tree.getOramStats();

            auto query_time = tree.getRunTime(text, search_scope, 10, show_details);
            query_times.push_back(query_time);

            // 计算这个查询的带宽和块数

            query_bandwidths.push_back(tree.search_blocks * blocksize / 1024);
            query_blocks.push_back(tree.search_blocks);

        }
    }

    query_file.close();

    // 性能统计
    if (!query_times.empty()) {
        std::chrono::nanoseconds total_time = std::chrono::nanoseconds::zero();
        double total_bandwidth_kb = 0.0;
        size_t total_blocks = 0;

        for (size_t i = 0; i < query_times.size(); i++) {
            total_time += query_times[i];
            total_bandwidth_kb += query_bandwidths[i];
            total_blocks += query_blocks[i];
        }

        double total_seconds = double(total_time.count()) * std::chrono::nanoseconds::period::num /
            std::chrono::nanoseconds::period::den;
        double avg_seconds = total_seconds / query_times.size();
        double avg_bandwidth_kb = total_bandwidth_kb / query_times.size();
        double avg_blocks = static_cast<double>(total_blocks) / query_times.size();

        // 格式化输出，避免科学计数法
        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "PERFORMANCE SUMMARY" << std::endl;
        std::cout << std::string(50, '=') << std::endl;
        std::cout << "Total queries: " << query_times.size() << std::endl;
        std::cout << "Total time: " << std::fixed << std::setprecision(3) << total_seconds << " seconds" << std::endl;
        std::cout << "Average time: " << std::fixed << std::setprecision(3) << avg_seconds << " seconds" << std::endl;

        // 格式化带宽输出
        std::cout << "Total bandwidth: " << std::fixed << std::setprecision(0) << total_bandwidth_kb << " KB";
        if (total_bandwidth_kb >= 1024) {
            std::cout << " (" << std::fixed << std::setprecision(2) << (total_bandwidth_kb / 1024) << " MB)";
        }
        std::cout << std::endl;

        std::cout << "Average bandwidth: " << std::fixed << std::setprecision(2) << avg_bandwidth_kb << " KB per query" << std::endl;

        // 格式化块数输出
        std::cout << "Total blocks: " << total_blocks << " blocks" << std::endl;
        std::cout << "Average blocks: " << std::fixed << std::setprecision(1) << avg_blocks << " blocks per query" << std::endl;

        // 重置输出格式
        std::cout.unsetf(std::ios_base::floatfield);
    }
}

int main() {
    try {

        testWithQueryFile("query.txt", true);
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
