#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <vector>
#include <string>
#include "RingoramStorage.h"
#include "IRTree.h"
#include "param.h"

int main(int argc, char* argv[]) {
    std::string query_filename = queryname;
    bool show_details = true;
    std::string data_file = dataname;
    std::string server_ip = "127.0.0.1";
    int server_port = 12345;
    if (argc > 1) server_ip = argv[1];
    if (argc > 2) server_port = std::stoi(argv[2]);

  
    std::cout << "=== IR-Tree Query Test ===" << std::endl;
    
    try {
        // 1. 初始化RingOramStorage
        std::cout << "Initializing RingOramStorage..." << std::endl;
        auto storage = std::make_shared<RingOramStorage>(
            totalnumRealblock, 
            blocksize, 
            server_ip, 
            server_port
        );
        
        // 2. 初始化IR-tree
        std::cout << "Initializing IR-tree..." << std::endl;
        IRTree tree(storage, 2, 2, 12);
        
        // 3. 批量插入数据
        tree.optimizedBulkInsertFromFile(data_file);
        
        // 4. 执行查询
        std::ifstream query_file(query_filename);
        if (!query_file.is_open()) {
            std::cerr << "Error: Cannot open query file " << std::endl;
            return 1;
        }

        std::vector<std::chrono::nanoseconds> query_times;
        std::vector<int> roundtrip_records;           // 记录每次查询的roundtrip
        std::vector<int64_t> bandwidth_records;       // 记录每次查询的带宽（字节）
        std::string line;
        int query_count = 0;

        while (std::getline(query_file, line)) {
            if (line.empty()) continue;

            // 新解析逻辑：支持多个关键词
            std::istringstream iss(line);
            std::vector<std::string> parts;
            std::string token;
            
            // 读取所有tokens
            while (iss >> token) {
                parts.push_back(token);
            }
            
            // 至少需要3个部分：至少1个关键词 + 2个坐标
            if (parts.size() < 3) {
                std::cerr << "Invalid query format: " << line << std::endl;
                continue;
            }
            
            // 最后两个是坐标
            double x, y;
            try {
                x = std::stod(parts[parts.size() - 2]);
                y = std::stod(parts[parts.size() - 1]);
            } catch (...) {
                std::cerr << "Invalid coordinates in query: " << line << std::endl;
                continue;
            }
            
            // 前面所有部分都是关键词
            std::string text;
            for (size_t i = 0; i < parts.size() - 2; i++) {
                if (i > 0) text += " ";
                text += parts[i];
            }
            
            query_count++;
            
            // 创建搜索范围
            double epsilon = 150;
            MBR search_scope({ x - epsilon, y - epsilon }, 
                             { x + epsilon, y + epsilon });

            auto query_time = tree.getRunTime(text, search_scope, k, show_details);
            query_times.push_back(query_time);

            // 记录这个查询的roundtrip和带宽
            roundtrip_records.push_back(roundtrip);  
            bandwidth_records.push_back(bandwidth);  
                  
            // if (show_details) {
            //     std::cout << "Query completed in " 
            //               << std::chrono::duration<double>(query_time).count() 
            //               << " seconds" << std::endl;
                
            //     // 显示带宽信息
            //     double bandwidth_kb = static_cast<double>(bandwidth) / 1024.0;
            //     double bandwidth_mb = bandwidth_kb / 1024.0;
            //     std::cout << "Roundtrip: " << roundtrip << std::endl;
            //     std::cout << "Bandwidth: " << bandwidth << " bytes (" 
            //               << std::fixed << std::setprecision(2) << bandwidth_kb << " KB, "
            //               << bandwidth_mb << " MB)" << std::endl;
            // }
        }

        query_file.close();

        // 5. 性能统计
        if (!query_times.empty()) {
            std::chrono::nanoseconds total_time = std::chrono::nanoseconds::zero();
        
            int total_roundtrip = 0;
            int64_t total_bandwidth = 0;  

            for (size_t i = 0; i < query_times.size(); i++) {
                total_time += query_times[i];
                total_roundtrip += roundtrip_records[i];
                total_bandwidth += bandwidth_records[i];
            }

            double total_seconds = double(total_time.count()) * std::chrono::nanoseconds::period::num /
                std::chrono::nanoseconds::period::den;
            double avg_seconds = total_seconds / query_times.size();
            double avg_roundtrip = static_cast<double>(total_roundtrip) / query_times.size();
            
            
            double avg_bandwidth_bytes = static_cast<double>(total_bandwidth) / query_times.size();
            double avg_bandwidth_kb = avg_bandwidth_bytes / 1024.0;
            
            // 格式化输出
            std::cout << "\n" << std::string(50, '=') << std::endl;
            std::cout << "PERFORMANCE SUMMARY" << std::endl;
            std::cout << std::string(50, '=') << std::endl;
    
            std::cout << "Average time: " << std::fixed << std::setprecision(3) 
                      << avg_seconds << " seconds" << std::endl;
            std::cout << "Average roundtrip: " << std::fixed << std::setprecision(1)
                      << avg_roundtrip << std::endl;
            
            // 输出多种单位的带宽
            std::cout << "Average bandwidth: " << std::fixed << std::setprecision(2)
                      << avg_bandwidth_bytes << " bytes" << std::endl;
            std::cout << "Average bandwidth: " << std::fixed << std::setprecision(2)
                      << avg_bandwidth_kb << " KB" << std::endl;
           
            // 总带宽统计
           
            // QPS（每秒查询数）
            double qps = query_times.size() / total_seconds;
            std::cout << "Queries per second (QPS): " << std::fixed << std::setprecision(2) 
                      << qps << std::endl;

            // 重置输出格式
            std::cout.unsetf(std::ios_base::floatfield);
        } else {
            std::cout << "No valid queries found in the file." << std::endl;
        }
        
        std::cout << "\nTest completed successfully!" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}