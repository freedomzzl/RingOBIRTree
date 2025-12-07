#include "ringoram.h"
#include"CryptoUtil.h"
#include"param.h"
#include<random>
#include<cmath>
#include<iostream>


using namespace std;
int ringoram::round = 0;
int ringoram::G = 0;

ringoram::ringoram(int n, ServerStorage* storage, int cache_levels)
	: N(n), L(static_cast<int>(ceil(log2(N)))), num_bucket((1 << (L + 1)) - 1), num_leaves(1 << L), storage(storage), cache_levels(cache_levels)
{
	c = 0;
	positionmap = new int[N];
	for (int i = 0; i < N; i++)
	{
		positionmap[i] = get_random();
	}

	storage->setCapacity(num_bucket);

	encryption_key = CryptoUtils::generateRandomKey(16);
	crypto = make_shared<CryptoUtils>(encryption_key);

	//初始化存储系统中的每个桶
	for (int i = 0; i < num_bucket; i++) {

		bucket init_bkt = bucket(realBlockEachbkt, dummyBlockEachbkt);

		storage->SetBucket(i, init_bkt);
	}

	cout << "[ORAM] Tree top cache enabled for " << cache_levels << " levels" << endl;
}

int ringoram::get_random()
{
	static std::random_device rd;      // 真随机数种子
	static std::mt19937 gen(rd());     // 高质量随机数引擎
	std::uniform_int_distribution<int> dist(0, num_leaves - 1);
	return dist(gen);
}

int ringoram::Path_bucket(int leaf, int level)
{

	int result = (1 << level) - 1 + (leaf >> (this->L - level));

	// 添加边界检查
	if (result < 0 || result >= num_bucket) {
		std::cerr << "ERROR: Path_bucket calculated invalid position: " << result
			<< " (leaf=" << leaf << ", level=" << level
			<< ", num_bucket=" << num_bucket << ")" << std::endl;
		// 返回一个安全的默认值
		return 0;
	}

	return result;

}

int ringoram::GetlevelFromPos(int pos)
{
	return (int)floor(log2(pos + 1));
}

block ringoram::FindBlock(bucket bkt, int offset)
{
	return bkt.blocks[offset];
}

int ringoram::GetBlockOffset(bucket bkt, int blockindex)
{

	int i;
	for (i = 0; i < (realBlockEachbkt + dummyBlockEachbkt); i++)
	{
		if (bkt.ptrs[i] == blockindex && bkt.valids[i] == 1) return i;
	}

	return bkt.GetDummyblockOffset();
}


void ringoram::ReadBucket(int pos)
{
	bucket& bkt = storage->GetBucket(pos);

	for (int j = 0; j < maxblockEachbkt; j++) {
		// 更严格的检查：只读取真实且有效的块
		if (bkt.ptrs[j] != -1 && bkt.valids[j] && !bkt.blocks[j].IsDummy()) {
			// 读取时解密
			block encrypted_block = bkt.blocks[j];
			vector<char> decrypted_data = decrypt_data(encrypted_block.GetData());
			block decrypted_block(encrypted_block.GetLeafid(), encrypted_block.GetBlockindex(), decrypted_data);
			stash.push_back(decrypted_block);
		}
	}
}

void ringoram::WriteBucket(int position)
{
	int level = GetlevelFromPos(position);
	vector<block> blocksTobucket;

	// 从stash中选择可以放在这个bucket的块
	for (auto it = stash.begin(); it != stash.end() && blocksTobucket.size() < realBlockEachbkt; ) {
		int target_leaf = it->GetLeafid();
		int target_bucket_pos = Path_bucket(target_leaf, level);
		if (target_bucket_pos == position) {
			// 对要写回当前bucket的块进行加密
			if (!it->IsDummy()) {
				vector<char> plain_data = it->GetData();  // 当前是明文
				vector<char> encrypted_data = encrypt_data(plain_data);

				// 创建加密后的block
				block encrypted_block(it->GetLeafid(), it->GetBlockindex(), encrypted_data);
				blocksTobucket.push_back(encrypted_block);
			}
			it = stash.erase(it);
		}
		else {
			++it;
		}
	}

	// 填充dummy块
	while (blocksTobucket.size() < realBlockEachbkt + dummyBlockEachbkt) {
		blocksTobucket.push_back(dummyBlock);
	}

	// 随机排列
	std::random_device rd;
	std::mt19937 g(rd());
	std::shuffle(blocksTobucket.begin(), blocksTobucket.end(), g);

	// 创建新的bucket
	bucket bktTowrite(realBlockEachbkt, dummyBlockEachbkt);
	bktTowrite.blocks = blocksTobucket;

	for (int i = 0; i < maxblockEachbkt; i++) {
		bktTowrite.ptrs[i] = bktTowrite.blocks[i].GetBlockindex();
		bktTowrite.valids[i] = 1;
	}
	bktTowrite.count = 0;

	storage->SetBucket(position, bktTowrite);
}

block ringoram::ReadPath(int leafid, int blockindex)
{
	block interestblock = dummyBlock;
	size_t blocks_this_read = 0;
	for (int i = 0; i <= L; i++)
	{
		int position = Path_bucket(leafid, i);
		bucket& bkt = storage->GetBucket(position);
		int offset = GetBlockOffset(bkt, blockindex);
		if (!isPositionCached(position))
		{
			blocks_this_read += 1;
		}
		block blk = FindBlock(bkt, offset);

		bkt.valids[offset] = 0;

		bkt.count += 1;
		// 如果是目标块，记录下来，但仍继续走完路径
		if (blk.GetBlockindex() == blockindex) {

			interestblock = blk;


		}
		// 标记目标块为无效
		if (offset >= 0 && offset < maxblockEachbkt) {
			bkt.valids[offset] = 0;
			bkt.count += 1;
		}
	}

	return interestblock;
}

//block ringoram::ReadPath(int leafid, int blockindex)
//{
//	block interestblock = dummyBlock;
//	size_t blocks_this_read = 0;
//	for (int i = 0; i <= L; i++) {
//		int position = Path_bucket(leafid, i);
//		bucket& bkt = storage->GetBucket(position);
//		int offset = GetBlockOffset(bkt, blockindex);
		//if (!isPositionCached(position))
		//{
		//	blocks_this_read += 1;
		//}
//		
//
//		if (offset >= 0 && offset < maxblockEachbkt) {
//			block blk = bkt.blocks[offset];
//
//			// 标记为无效
//			bkt.valids[offset] = 0;
//			bkt.count += 1;
//
//			// 如果是目标块，记录下来（不在这里解密）
//			if (blk.GetBlockindex() == blockindex) {
//				interestblock = blk;
//
//				// 将块添加到stash（保持加密状态）
//				if (bkt.ptrs[offset] != -1 && bkt.valids[offset] == 0) {
//					stash.push_back(blk);
//				}
//			}
//
//			
//		}
//	}
//
//	comm_stats.total_blocks_read += blocks_this_read;
//	return interestblock;
//}

void ringoram::EvictPath()
{
	int l = G % (1 << L);
	G += 1;

	int i;
	for (i = 0; i <= L; i++)
	{
		ReadBucket(Path_bucket(l, i));
	}

	for (i = L; i >= 0; i--)
	{
		WriteBucket(Path_bucket(l, i));
		storage->GetBucket(Path_bucket(l, i)).count = 0;
	}
}

void ringoram::EarlyReshuffle(int l)
{
	for (int i = 0; i <= L; i++)
	{
		if (storage->GetBucket(Path_bucket(l, i)).count >= dummyBlockEachbkt)
		{
			ReadBucket(Path_bucket(l, i));
			WriteBucket(Path_bucket(l, i));

			storage->GetBucket(Path_bucket(l, i)).count = 0;
		}
	}
}

std::vector<char> ringoram::encrypt_data(const std::vector<char>& data)
{
	if (!crypto || data.empty()) return data;

	// 像ringoram一样直接转换和加密
	vector<uint8_t> data_u8(data.begin(), data.end());
	auto encrypted_u8 = crypto->encrypt(data_u8);
	return vector<char>(encrypted_u8.begin(), encrypted_u8.end());
}

std::vector<char> ringoram::decrypt_data(const std::vector<char>& encrypted_data)
{
	if (!crypto || encrypted_data.empty()) return encrypted_data;

	// 像ringoram一样检查数据大小
	if (encrypted_data.size() % 16 != 0) {
		cerr << "[DECRYPT] ERROR: Size " << encrypted_data.size() << " not multiple of 16" << endl;
		return encrypted_data;
	}

	try {
		vector<uint8_t> encrypted_u8(encrypted_data.begin(), encrypted_data.end());
		auto decrypted_u8 = crypto->decrypt(encrypted_u8);
		return vector<char>(decrypted_u8.begin(), decrypted_u8.end());
	}
	catch (const exception& e) {
		cerr << "[DECRYPT] ERROR: " << e.what() << endl;
		return encrypted_data;
	}
}



vector<char> ringoram::access(int blockindex, Operation op, vector<char> data)
{
	if (blockindex < 0 || blockindex >= N) {

		return {};
	}

	int oldLeaf = positionmap[blockindex];
	positionmap[blockindex] = get_random();

	// 1. 读取路径获取目标块（加密状态）
	block interestblock = ReadPath(oldLeaf, blockindex);
	vector<char> blockdata;

	// 2. 处理读取到的块
	if (interestblock.GetBlockindex() == blockindex) {
		// 从路径读取到的目标块，需要解密
		if (!interestblock.IsDummy()) {
			blockdata = decrypt_data(interestblock.GetData());
		}
		else {
			blockdata = interestblock.GetData();
		}
	}
	else {
		// 3. 如果不在路径中，检查stash
		for (auto it = stash.begin(); it != stash.end(); ++it) {
			if (it->GetBlockindex() == blockindex) {
				blockdata = it->GetData();   // stash中已经是明文
				stash.erase(it);
				break;
			}
		}
	}

	// 4. 如果是WRITE操作，更新数据
	if (op == WRITE) {
		blockdata = data;
	}

	// 明文放入stash
	stash.emplace_back(positionmap[blockindex], blockindex, blockdata);

	// 5. 路径管理和驱逐
	round = (round + 1) % EvictRound;
	if (round == 0) EvictPath();

	EarlyReshuffle(oldLeaf);

	return blockdata;
}