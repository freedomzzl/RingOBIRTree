#pragma once
#include"block.h"

class bucket
{
public:
	int Z;   //realblock_num 
	int S;   //dummyblock_num

	//桶中存储的Block
	vector<block> blocks;

	//桶被访问次数
	int count;

	//记录offset
	vector<int> ptrs;

	//记录有效位
	vector<int> valids;

	bucket();
	bucket(int Z, int S);

	//随机获取dummyblock的offset
	int GetDummyblockOffset();
};