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
    int GetBlockindex() const;
    void SetBlockindex(int blockindex);
    int GetLeafid() const;
    void SetLeafid(int lead_id);
    vector<char> GetData() const;
    void SetData(vector<char> data);
    bool IsDummy() const {
        return blockindex == -1; 
    }

};