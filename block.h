// block.h
#pragma once
#include<vector>
#include<algorithm>

using namespace std;

class block
{
private:
    int leaf_id;
    int blockindex;
    vector<char> data;


public:
    block();
    block(int leaf_id, int blockindex, vector<char> data);
    int GetBlockindex();
    void SetBlockindex(int blockindex);
    int GetLeafid();
    void SetLeafid(int lead_id);
    vector<char> GetData();
    void SetData(vector<char> data);
    bool IsDummy() const {
        return blockindex == -1; // 根据你的设计，dummy块的index为-1
    }

};