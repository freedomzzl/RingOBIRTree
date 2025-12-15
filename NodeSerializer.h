#ifndef NODE_SERIALIZER_H
#define NODE_SERIALIZER_H

#include "Node.h"
#include "Document.h"
#include <vector>
#include <cstdint>
#include <memory>

class NodeSerializer {
public:
    // 序列化节点到字节流
    static std::vector<uint8_t> serialize(const Node& node);

    // 从字节流反序列化节点
    static std::shared_ptr<Node> deserialize(const std::vector<uint8_t>& data);

    // 序列化文档到字节流
    static std::vector<uint8_t> serializeDocument(const Document& doc);

    // 从字节流反序列化文档
    static std::shared_ptr<Document> deserializeDocument(const std::vector<uint8_t>& data);

private:
    // 辅助方法
    static void writeInt(std::vector<uint8_t>& data, int value);
    static int readInt(const std::vector<uint8_t>& data, size_t& offset);
    static void writeDouble(std::vector<uint8_t>& data, double value);
    static double readDouble(const std::vector<uint8_t>& data, size_t& offset);
    static void writeString(std::vector<uint8_t>& data, const std::string& str);
    static std::string readString(const std::vector<uint8_t>& data, size_t& offset);
};

#endif