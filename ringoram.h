#pragma once
#include"block.h"
#include"bucket.h"
#include"CryptoUtil.h"
#include"param.h"
#include<vector>
#include<cmath>
#include <memory>
#include<iostream>

using namespace std;
class ringoram
{
public:
	static int round;
	static int G;
	int* positionmap;
	vector<block> stash;
	int c;
	// === 加密支持 ===
	std::shared_ptr<CryptoUtils> crypto;      // 加密工具类
	std::vector<uint8_t> encryption_key;      // 加密密钥


	int N;
	int L;
	int num_bucket;
	int num_leaves;

	int cache_levels;  // 缓存的树层级数

     // 网络通信
    std::string server_ip_;
    int server_port_;

	enum Operation { READ, WRITE };
	 ringoram(int n, const std::string& server_ip, int server_port, int cache_levels = cacheLevel);
    ~ringoram();

    // 网络初始化
    void initNetwork();

	bool isPositionCached(int position) const {
		return position < (1 << cache_levels) - 1;
	}
	
	int get_random();//获取随机数
	int Path_bucket(int leaf, int level);//计算指定叶子节点和层数的路径。
	int GetlevelFromPos(int pos);//根据position得到层
	block FindBlock(bucket bkt, int offset);//在桶中找到对应Block
	int GetBlockOffset(bucket bkt, int blockindex);//得到Block的offset
	void ReadBucket(int pos);
    bucket Read_bucket(int pos);
	void WriteBucket(int position);

	block ReadPath(int leafid, int blockindex);
	std::vector<bucket> ReadPathFull(int leaf_id);
	bool WritePathFull(int leaf_id, const std::vector<bucket>& buckets_to_write);
	void EvictPath();
	std::vector<int> CheckNeedEarlyShuffle(int leaf_id);
	void EarlyReshuffle(int l);

	// === 数据加密与解密 ===
	std::vector<char> encrypt_data(const std::vector<char>& data);
	std::vector<char> decrypt_data(const std::vector<char>& encrypted_data);

	vector<char> access(int blockindex, Operation op, vector<char> data);

};