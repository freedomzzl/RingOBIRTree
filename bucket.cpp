#include "bucket.h"
#include"param.h"
#include<random>
#include<cmath>

bucket::bucket() :Z(realBlockEachbkt), S(dummyBlockEachbkt), blocks(Z + S, dummyBlock), count(0), ptrs(Z + S, -1), valids(Z + S, 1)
{
}

bucket::bucket(int Z, int S)
	:Z(Z), S(S), blocks(Z + S, dummyBlock), count(0), ptrs(Z + S, -1), valids(Z + S, 1)
{

}

int bucket::GetDummyblockOffset()
{
	vector<int> dummyoffset;
	for (int i = 0; i < (Z + S); i++)
	{
		if (ptrs[i] == -1 && valids[i] == 1)
			dummyoffset.push_back(i);
	}
	if (dummyoffset.empty())
	{
		printf("no valid dummyblock");
		return -1;
	}

	// 随机数引擎（用时间做种子）
	static std::mt19937 rng(static_cast<unsigned>(time(nullptr)));
	std::uniform_int_distribution<int> dist(0, dummyoffset.size() - 1);

	return dummyoffset[dist(rng)];
}