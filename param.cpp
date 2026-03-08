#include "param.h"
#include <cmath>
#include<cstring>
#include<string>

int totalnumRealblock = 2000000;
int OramL = static_cast<int>(ceil(log2(totalnumRealblock)));
int cacheLevel=2;
int numLeaves = 1 << OramL;
int capacity=(1 << (OramL + 1)) - 1;
int blocksize = 4096;
int realBlockEachbkt = 4;
int dummyBlockEachbkt = 6;
int EvictRound = 10;
block dummyBlock(-1, -1, {});
int maxblockEachbkt = realBlockEachbkt + dummyBlockEachbkt;

std::string dataname = "data/dataset_4096.txt";
std::string queryname = "data/query_dataset_1024_3keywords.txt";

int k=1;
int nodes_load=2;  ///<  可根据k的大小调整

int roundtrip=0;

int bandwidth=0;

