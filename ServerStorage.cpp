#include"ServerStorage.h"
#include"param.h"
#include <iostream>
#include <string>
#include <sstream>
using namespace std;



ServerStorage::ServerStorage() : capacity(0)
{
    buckets = std::vector<bucket>();
}

void ServerStorage::setCapacity(int totalNumOfBuckets)
{

    this->capacity = totalNumOfBuckets;
    this->buckets.assign(totalNumOfBuckets, bucket(realBlockEachbkt, dummyBlockEachbkt));
}




bucket& ServerStorage::GetBucket(int position)
{
    if (position >= this->capacity || position < 0) {
        throw runtime_error("You are trying to access Bucket " + to_string(position) + ", but this Server contains only " + to_string(this->capacity) + " buckets.");
    }

    return this->buckets.at(position);
}

void ServerStorage::SetBucket(int position, bucket& bucketTowrite)
{
    if (position >= this->capacity || position < 0) {
        throw runtime_error("You are trying to access Bucket " + to_string(position) + ", but this Server contains only " + to_string(this->capacity) + " buckets.");
    }

    this->buckets.at(position) = bucketTowrite;
}