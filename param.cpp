#include "param.h"
#include <cmath>

int totalnumRealblock = 20000;
int OramL = static_cast<int>(ceil(log2(totalnumRealblock)));
int numLeaves = 1 << OramL;
int blocksize = 4096;
int realBlockEachbkt = 5;
int dummyBlockEachbkt = 6;
int EvictRound = 5;
block dummyBlock(-1, -1, {
    'd', 'u', 'm', 'm', 'y', 'b', 'l', 'o',
    'c', 'k', 'd', 'a', 't', 'a', '0', '0'
    });
int maxblockEachbkt = realBlockEachbkt + dummyBlockEachbkt;

int cacheLevel = 3;