#include "param.h"
#include <cmath>

int totalnumRealblock = 20000;
int OramL = static_cast<int>(ceil(log2(totalnumRealblock)));
int numLeaves = 1 << OramL;
int capacity=(1 << (OramL + 1)) - 1;
int blocksize = 4096;
int realBlockEachbkt = 5;
int dummyBlockEachbkt = 6;
int EvictRound = 10;
block dummyBlock(-1, -1, {});
int maxblockEachbkt = realBlockEachbkt + dummyBlockEachbkt;

int cacheLevel = 3;

const size_t FIXED_BUCKET_SIZE = 65536;