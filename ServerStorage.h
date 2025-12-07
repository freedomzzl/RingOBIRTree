#pragma once
#include <cmath>
#include"bucket.h"
#include"block.h"
#include<vector>



class ServerStorage
{
public:
    std::vector<bucket> buckets;  // 存储所有的bucket

    

    ServerStorage();
    void setCapacity(int totalNumOfBuckets);  // 设置存储系统的总容量（桶的数量）

    

    bucket& GetBucket(int position);
    void SetBucket(int position, bucket& bucketTowrite);

    int GetCapacity() const { return capacity; }

private:
    int capacity;  // 总的bucket数量
};