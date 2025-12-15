#include "block.h"
#include <cstring> 

block::block()
    :leaf_id(-1), blockindex(-1), data()
{
}

block::block(int leaf_id, int blockindex, vector<char> data)
    :leaf_id(leaf_id), blockindex(blockindex), data(data)
{
}

int block::GetBlockindex() const
{
    return blockindex;
}

void block::SetBlockindex(int blockindex)
{
    this->blockindex = blockindex;
}

int block::GetLeafid() const
{
    return leaf_id;
}

void block::SetLeafid(int lead_id)
{
    this->leaf_id = lead_id;
}

vector<char> block::GetData() const
{
    return data;
}

void block::SetData(vector<char> data)
{
    this->data = data;
}
