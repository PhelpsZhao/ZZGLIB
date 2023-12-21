//ZZG_Mem.cpp by Phelps Zhao
//Version 20231001
#include "ZZG_Bit.h"
#include "ZZG_Sync.h"
#include <malloc.h>
#include "ZZG_Mem.h"
#include <stdint.h>
namespace ZZG {

#if (defined(_DEBUG)||defined(DEBUG))
zMemStack zMem_Stack;
//在堆栈链末尾插入一个新记录，记录内存分配指针ptr
//@paras[in]:p为指向新分配的内存块的指针，File为文件名,Line为所在文件中的行数位置
//@ret成功则返回0，否则返回非0
int zMemStack::Insert(void *ptr, const char *File, unsigned int Line)
{
	zMemInfo *p;
	p = (zMemInfo *)malloc(sizeof(zMemInfo));
	if (!p)return -1;	//内存分配失败，返回
	lk.Lock();

	p->ptr = ptr;
	p->File = File;
	p->Line = Line;
	p->pNext = NULL;
	if (!pHead)
	{
		pLast = p;
		pHead = pLast;
	}
	else
	{
		pLast->pNext = p;
		pLast = p;
	}
	lk.Unlock();
	return 0;
}

//删除内存堆栈链表中内存指针ptr的记录
//@paras[in]：ptr内存分配函数返回的指针，指向一个分配的内存块
//@ret成功则返回0，否则返回非0
int zMemStack::Delete(void *ptr)
{
	lk.Lock();
	zMemInfo *pre = NULL, *p = pHead;
	while (p)
	{
		if (p->ptr == ptr)
		{
			//如果是第一个元素
			if (!pre)pHead = p->pNext;
			else pre->pNext = p->pNext;
			if (p == pLast)
			{
				pLast = pre;
				//最后一条记录不是第一条记录的话
				if (pLast)pLast->pNext = NULL;
			}
			free(p);
			lk.Unlock();
			return 0;
		}
		pre = p;
		p = p->pNext;
	}
	lk.Unlock();
	return -1;
}

//得到内存记录栈上的下一条信息.
//@paras[in]:p指向内存记录栈上的当前记录。如果是NULL，则返回第一条记录
//@ret:返回值为指向内存记录栈上下一条记录的指针。如果已到达末尾，则返回值为NULL
zMemInfo* zMemStack::GetNextLeak(zMemInfo*p)
{
	if (!p)return pHead;
	return p->pNext;
}
#endif
//z__AT 只供zAT使用
class z__AT {
private:
	uint32_t *pT[MAX_LAYER];	//指向分配表B+树。pT[i]为第i层首地址。每层都是连续的内存，每个元素都代表B+树的一个节点
	struct LOCKFLAG {
		volatile std::atomic_flag LockFlag;	//写锁定标志
		volatile uint8_t SearchCount;	//记录搜索时读取该节点的次数，也就是线程数。主要是用来线程碰撞检测，避免同一路线上线程过于拥挤
	} *pLockFlag[MAX_LAYER]; //指向节点锁表树。数组元素为每层锁的首地址，PT中每个元素一一对应一把锁。读写锁
	uint32_t MaxLayer;	//最大层数

	void Read(volatile LOCKFLAG *pFlag)
	{
		++pFlag->SearchCount;//主要用于避开搜索的线程间竞争，所以不用锁定的精准操作
	}

	void UnRead(volatile LOCKFLAG *pFlag)
	{
		--pFlag->SearchCount;
	}
	inline void Lock(volatile LOCKFLAG *pFlag);
	inline void Unlock(volatile LOCKFLAG *pFlag)
	{
		//The release mode guarantees that no previous reads and writes can be reodered after LockFlag.clear()
		pFlag->LockFlag.clear(std::memory_order_release);
	}

public:
	z__AT() {};
	~z__AT() {};


	//@ret:返回值为分配的单元序数。若已满，无单元可分配则返回zA_ERROR_FULL
	//多线程用
	size_t LockedAlloc();

	//@paras[in]:UnitPos单元位置
	//多线程用
	void LockedFree(size_t UnitPos);

	//@ret:返回值为分配的单元序数。若已满，无单元可分配则返回zAT_ERROR_FULL
	size_t Alloc();

	//@paras[in]:UnitPos单元位置
	//不锁定，单线程适用
	void Free(size_t UnitPos);

	//预先设置一些单元为占用状态。当分配树初始化后，所有单元都为空闲状态，用这个函数可以预设一些单元为已被分配的状态
	//此函数必须在Init()后，LockedAlloc()/Alloc()之前使用。
	void PreSet(size_t UnitPos);

	friend class zAT;
};


//@ret:返回值为分配的单元序数。若已满，无单元可分配则返回zCAT::ERROR_FULL
size_t z__AT::LockedAlloc()
{
    unsigned long index;	//位索引
	size_t ir, k = 0;	//k表示节点在某层的位置
	int BSRet;	//位搜索返回值

    for (unsigned int i = 0;;)	//i表示层号
	{
		//如果到达最底层（叶子节点，每一个位标识一个单元是否为空状态）
		if (i == this->MaxLayer - 1)
		{
			//如果有空位，那么锁定节点，准备修改节点
            if (zBSF(&index, *(this->pT[i] + k)))
			{
				Lock(pLockFlag[i] + k);
				//再次扫描有否空闲位如果没有，继续下一个循环。如果有那么函数返回
				if (zBSF(&index, *(this->pT[i] + k)))
				{
					//成功找到空位
					zBitReset(this->pT[i] + k, index);
					Unlock(pLockFlag[i] + k);
					return (k << 5) + index;
				}
				Unlock(pLockFlag[i] + k);
			}
			//如果没有空闲位，那么返回上一层，并把上一层对应位置0，表示满
			//回到上一层，并设置相应位为0（表示下一层对应节点为满状态）
			if (!i)
				return zAT::RET_MEM_FULL;	//如果叶子节点就是根节点，那么直接返回
			--i;
			uint64_t tmp = k;
			uint16_t BitPos = k & 0x1f;
			k >>= 5;
			//多线程下有可能别的线程已经设置了0。所以先检查是否为0，不为0那么尝试锁定修改
			if (zBitTest(this->pT[i] + k, BitPos))
			{
				Lock(pLockFlag[i] + k);
				//锁定上层后，再次确认下层是否为满的状态,为满则置0
				if (!zBSF(&index, *(this->pT[i + 1] + tmp)))
					zBitReset(this->pT[i] + k, BitPos);
				Unlock(pLockFlag[i] + k);
			}
			continue;
		}
		//对非叶子节点记录搜索访问次数，并采用奇偶分开，减少线程间碰撞概率。如果读锁定次数奇数，那么采用正向搜索，否则用倒向搜索
		Read(pLockFlag[i] + k);
		
        if ((pLockFlag[i] + k)->SearchCount & 0x1)
			BSRet = zBSF(&index, *(this->pT[i] + k));
		else
			BSRet = zBSR(&index, *(this->pT[i] + k));

		UnRead(pLockFlag[i] + k);
		//如果没有空位(全0)
		if (!BSRet)
		{
			//如果是到根节点
			if (!i)
			{
				ir = zAT::RET_MEM_FULL;
				goto FULL_EXIT;
			}

			//回到上一层，并设置相应位为0（表示下一层对应节点为满状态）。但是有可能刚设置0，另外有线程就设置为了1，
			//于是下一个循环又回到同一个节点，但是这时该节点可能又刚好被分配完了，如此循环往复，这个线程就陷入了死循环。
			//不过实际上几乎不会出现这种情况。首先能出现这种情况的，分配树最少是3层，多个线程长时间分配释放在同一个节点上
			//的可能性就不大。再考虑实际情况下，一般分配释放的频率比分配树上一个循环搜索的频率要低得多，最少也要慢几十倍。
			//比如当作为管理网络服务器发送数据缓冲区的分配树的时候，假设网卡是万兆网卡，即使在局域网内，网卡能全速运行,MTU
			//大小为1.5K,那么最多发送频率也就是六十万左右,也就是分配释放的频率最高六十万。另一方面，即使在普通电脑上，分配
			//树搜索一个循环也在千万次以上，拥有如此高速网卡的机器，更是应该轻松达到几千万次。单单考虑时间上的可能性，一个
			//循环之间发生分配释放的概率也只有百分之一可能性，要发生一次无效的上下层来回反复的概率最多只有亿分之一，连续发生
			//两次的概率只有亿亿分之一，要连续发生很多次，直至陷入死循环的概率可以认为无限小，忽略不计。
			--i;
			uint64_t tmp = k;
			uint16_t BitPos = k & 0x1f;
			k >>= 5;
			//多线程下有可能别的线程已经设置了0。所以先检查是否为0，不为0那么尝试锁定修改
			if (zBitTest(this->pT[i] + k, BitPos))
			{
				Lock(pLockFlag[i] + k);
				//锁定上层后，再次确认下层是否为满的状态,为满则置0
				if (!zBSF(&index, *(this->pT[i + 1] + tmp)))
					zBitReset(this->pT[i] + k, BitPos);
				Unlock(pLockFlag[i] + k);
			}
		}
		//有空位
		else
		{
			//如果是中间层，则进到下一层对应节点
			++i;
			k = (k << 5) + index;
		}
	}
FULL_EXIT:return ir;
}

void z__AT::LockedFree(size_t UnitPos)
{
	uint16_t BitPos = UnitPos & 0x1f;
	UnitPos >>= 5;
	uint32_t *pUnit = this->pT[this->MaxLayer - 1] + UnitPos;
	Lock(pLockFlag[MaxLayer - 1] + UnitPos);
	zBitSet(pUnit, BitPos);
	//空位刚好为FREE_THRESH_HOLD个时，表示之前该节点在父节点中的标志位可能为满的状态，需要逐层回溯检查,
	//设置本节点在上层中的标志位为空(1)。否则直接返回
	//这个是为了防止临界满状态时可能发生的频繁多层锁定而设计的缓冲空间。
	//此设定会导致空间利用率下降，最差情况时，空间利用率只有(33-FREE_THRESH_HOLD)/32
	if (zBitCount(*pUnit) != FREE_THRESH_HOLD)
	{
		Unlock(pLockFlag[MaxLayer - 1] + UnitPos);
		return;
	}
	Unlock(pLockFlag[MaxLayer - 1] + UnitPos);
	//空位大于FREE_THRESH_HOLD个，倒数第二层对应位可能是1（表示有空位，本节点近来没有分配完过），也可能是0（表示满，本节点
	//近来有分配完毕的情况。所以即使空位大于FREE_THRESH_HOLD个，也需要先查看倒数第二层对应位是否为0，若为0则置1
	uint32_t *pLower = pUnit;	//指向下层对应节点的指针
	for (int i = this->MaxLayer - 2; i >= 0; --i)
	{
		BitPos = UnitPos & 0x1f;
		UnitPos >>= 5;
		pUnit = this->pT[i] + UnitPos;
		//如果是1,说明有另一线程已经在释放或者其他子树有空节点，那么跳出循环，终止执行。
		if (zBitTest(pUnit, BitPos))break;
		//否则锁定单元准备置空(1)
		Lock(pLockFlag[i] + UnitPos);
		//锁定后再次确认是否可以置空。因为有可能在锁定时其他线程已经置空了;或者又被分配完毕，不能置空
		if (zBitCount(*pLower) == FREE_THRESH_HOLD)
			zBitSet(pUnit, BitPos);
		Unlock(pLockFlag[i] + UnitPos);
	}
	return;
}

//#define zNop8 __nop();__nop();__nop();__nop();__nop();__nop();__nop();__nop()
#define WRITELOCKMASK 0x1000000ui32
void z__AT::Lock(volatile LOCKFLAG * pFlag)
{
	int Count = 3;
	do {
		//returns after acquiring the lock
		if (!pFlag->LockFlag.test_and_set(std::memory_order_acquire))
			return;
		
		if (!Count)
		{
			//After three attempts, gives up the left of the time slice for the current thread 
			// and waits for the next time slice
			std::this_thread::yield();
			Count = 3;
		}		
		//waits for about 370 instruction cycles before trying again
        for (int k = 0; k < 37; ++k) { zNop8(); }
	} while (true);
}

//@ret:返回值为分配的单元序数。若已满，无单元可分配则返回zCAT::ERROR_FULL
size_t z__AT::Alloc()
{
	unsigned long index;	//位索引
	size_t ir, k = 0;	//k表示节点在某层的位置
    for (unsigned int i = 0;;)	//i表示层号
	{

		if (!zBSF(&index, *(this->pT[i] + k)))
		{
			//如果是到根节点
			if (!i)
			{
				ir = zAT::RET_MEM_FULL;
				goto FULL_EXIT;
			}
			//回到上一层，并设置相应位为0（表示下一层对应节点为满状态）
            --i;
			uint16_t BitPos = k & 0x1f;
			k >>= 5;
			zBitReset(this->pT[i] + k, BitPos);
		}
		else
		{
			//如果到达最底层（叶子节点，每一个位标识一个单元是否为空状态）
			if (i == this->MaxLayer - 1)
			{
				//成功找到空位，相应位置0，返回
				zBitReset(this->pT[i] + k, (uint16_t)index);
				return (k << 5) + index;
			}
			//如果是中间层，则进到下一层对应节点
			++i;
			k = (k << 5) + index;
		}

	}
FULL_EXIT:return ir;
}

//@paras[in]:UnitPos单元位置
//不锁定，单线程适用
void z__AT::Free(size_t UnitPos)
{
	uint16_t BitPos = UnitPos & 0x1f;
	UnitPos >>= 5;
	uint32_t *pUnit = this->pT[this->MaxLayer - 1] + UnitPos;
	zBitSet(pUnit, BitPos);
	//空位刚好为FREE_THRESH_HOLD个时，表示之前该节点在父节点中的标志位可能为满的状态，需要逐层回溯检查
	//设置节点在父节点中的标志位为空(1)
	//这个FREE_THRESH_HOLD的设置是为了防止临界满状态时可能发生的频繁多层操作而设计的缓冲空间。
	//此设定会导致空间利用率下降，最差情况时，空间利用率只有（33-FREE_THRESH_HOLD）/32
	if (zBitCount(*pUnit) == FREE_THRESH_HOLD)
	{
		for (int i = this->MaxLayer - 2; i >= 0; --i)
		{
			BitPos = UnitPos & 0x1f;
			UnitPos >>= 5;
			pUnit = this->pT[i] + UnitPos;
			//如果是1,说明有其他子树已经回溯过或者其它子树有空节点，那么跳出循环，终止执行。
			if (zBitTest(pUnit, BitPos))break;
			zBitSet(pUnit, BitPos);
		}
	}
	return;
}

void z__AT::PreSet(size_t UnitPos)
{
	uint16_t BitPos = UnitPos & 0x1f;
	UnitPos >>= 5;
	uint32_t *pUnit = this->pT[this->MaxLayer - 1] + UnitPos;
	zBitReset(pUnit, BitPos);
	if (*pUnit)	//如果没有分配完，那么直接返回
		return;
	//最低层单元已经分配完毕，那么逐层回溯检查设置父节点对应位为0；
	for (int i = this->MaxLayer - 2; i >= 0; --i)
	{
		BitPos = UnitPos & 0x1f;
		UnitPos >>= 5;
		pUnit = this->pT[i] + UnitPos;
		zBitReset(pUnit, BitPos);
		//如果节点位不是全0,那么跳出循环，终止回溯。
		if (*pUnit)break;		
	}
	return;
}

bool zAT::Init(size_t MaxNum)
{
	Count = 0;
	
	if (MaxNum <= 0)return 0;
	Size = MaxNum - (MaxNum >> 1);
    Nodes[0] = (Size + 31) >> 5;
	Size = Nodes[0] << 5;
    for (int i = 1; Nodes[i - 1] > 0x1; ++i)
	{
		++MaxLayer;
		Nodes[i] = (Nodes[i - 1] + 31) >> 5;
	};

	//计算每棵分配树的节点总数。两棵分配数的节点数是一样的
	for (uint32_t i = 0; i < MaxLayer; ++i)NodeNum += Nodes[i];

	size_t MemCount = NodeNum * sizeof(uint32_t) * 2 + 2 * NodeNum * sizeof(z__AT::LOCKFLAG);
	//分配地址后内存地址cache line对齐
//	size_t CLS = zCacheLineSize();

    pBuf = malloc(MemCount);
    if (!pBuf)return false;

	pAT1 = new z__AT;
	pAT2 = new z__AT;

	pAT1->MaxLayer = MaxLayer;
	pAT2->MaxLayer = MaxLayer;
	//指定第一棵分配树内存
	pAT1->pT[0] = (uint32_t*)pBuf;
	//指定第二棵分配树内存
	pAT2->pT[0] = (uint32_t*)pBuf + NodeNum;
	//指定第一个锁表内存。紧接着第二棵分配树
	pAT1->pLockFlag[0] = (z__AT::LOCKFLAG*)(pAT2->pT[0] + NodeNum);
	//指定第二个锁表内存。紧接着第一个锁表内存
	pAT2->pLockFlag[0] = pAT1->pLockFlag[0] + NodeNum;
	//初始化锁表。清空为0，表示没有任何锁定
    memset((void*)pAT1->pLockFlag[0],0,2 * NodeNum * sizeof(z__AT::LOCKFLAG));
	//计算第一棵分配树每层开始地址
	for (uint32_t k = 1; k < MaxLayer; ++k)
	{
		pAT1->pT[k] = pAT1->pT[k - 1] + Nodes[MaxLayer - k];
		pAT1->pLockFlag[k] = pAT1->pLockFlag[k - 1] + Nodes[MaxLayer - k];
	}
	//计算第二棵分配树每层开始地址
	for (uint32_t k = 1; k < MaxLayer; ++k)
	{
		pAT2->pT[k] = pAT2->pT[k - 1] + Nodes[MaxLayer - k];
		pAT2->pLockFlag[k] = pAT2->pLockFlag[k - 1] + Nodes[MaxLayer - k];
	}

	//设置所有位为1，表示空闲,两棵树占的内存字节数刚好是8字节的倍数
	uint64_t *pt64 = (uint64_t*)pBuf;
    for (unsigned int i = 0; i < NodeNum; ++i, ++pt64)
        *pt64 = ~0x0;

	//如果层数大于等于2层，则除最底层外，从倒数第二层开始精确设置各层最后一个节点各位值
    for (int i = MaxLayer - 2; i >= 0; --i)//MaxLayer - 2为倒数第二层
	{
        uint32_t tmp = Nodes[MaxLayer - i - 2] & 0x1f;//
		if (tmp)
		{
			uint32_t *pt1 = pAT1->pT[i] + Nodes[MaxLayer - 1 - i] - 1;
			uint32_t *pt2 = pAT2->pT[i] + Nodes[MaxLayer - 1 - i] - 1;
			*pt1 = 1, *pt2 = 1;
			for (uint32_t k = 1; k < tmp; ++k)
			{
				*pt1 <<= 1;
                *pt1 |= 0x1;
				*pt2 <<= 1;
                *pt2 |= 0x1;
			}
		}
	}
	size_t Len = Size << 1;	//总的单元数，总是64的倍数
	//如果总的单元数多余用于要求数，那么把尾部多余的单元数预先设置为占用状态，防止被分配
	for (size_t i = Len - MaxNum; i > 0; --i)
		PreSet(Len - i);
	return true;
}

void zAT::Reset()
{
	//初始化锁表。清空为0，表示没有任何锁定
    memset((void*)pAT1->pLockFlag[0], 0,2 * NodeNum * sizeof(z__AT::LOCKFLAG));

	//设置所有位为1，表示空闲,两棵树占的内存字节数刚好是8字节的倍数
	uint64_t *pt64 = (uint64_t*)pBuf;
    for (unsigned int i = 0; i < NodeNum; ++i, ++pt64)
        *pt64 = ~0x0;

	//如果层数大于等于2层，则除最底层外，从倒数第二层开始精确设置各层最后一个节点各位值
	for (int i = MaxLayer - 2; i >= 0; --i)//MaxLayer - 2为倒数第二层
	{
        uint32_t tmp = Nodes[MaxLayer - i - 2] & 0x1f;//
		if (tmp)
		{
			uint32_t *pt1 = pAT1->pT[i] + Nodes[MaxLayer - 1 - i] - 1;
			uint32_t *pt2 = pAT2->pT[i] + Nodes[MaxLayer - 1 - i] - 1;
			*pt1 = 1, *pt2 = 1;
			for (uint32_t k = 1; k < tmp; ++k)
			{
				*pt1 <<= 1;
                *pt1 |= 0x1;
				*pt2 <<= 1;
                *pt2 |= 0x1;
			}
		}
	}
}

void  zAT::close()
{
	if (pBuf)
	{
        free(pBuf);
		delete pAT1;
		delete pAT2;
		pBuf = 0;
	}
}
//@ret:返回值为分配的单元序数。若已满，无单元可分配则返回zAT_ERROR_FULL
//多线程用
size_t zAT::LockedAlloc()
{
	size_t AtNum;
	++Count;
	//如果是奇数,则先从pAT1分配
    if (Count & 0x1)
	{
		AtNum = pAT1->LockedAlloc();
		if (AtNum != RET_MEM_FULL)
			return AtNum;
		else
		{
			AtNum = pAT2->LockedAlloc();
			if (AtNum != RET_MEM_FULL)
				return Size + AtNum;
			else
				return RET_MEM_FULL;
		}
	}
	//如果是偶数，则先从pAT2分配
	else
	{
		AtNum = pAT2->LockedAlloc();
		if (AtNum != RET_MEM_FULL)
			return Size + AtNum;
		else
		{
			AtNum = pAT1->LockedAlloc();
			return AtNum;
		}
	}
}

//多线程用
void zAT::LockedFree(size_t UnitPos)
{
	//如果Unit==Size1，那么应该在pAT2!
	if (UnitPos < Size)
		pAT1->LockedFree(UnitPos);
	else
		pAT2->LockedFree(UnitPos - Size);
}


//@ret:返回值为分配的单元序数。若已满，无单元可分配则返回zAT_ERROR_FULL
//单线程用
size_t zAT::Alloc()
{
	size_t AtNum;
	++Count;
	//如果是奇数,则先从pAT1分配
    if (Count & 0x1)
	{
		AtNum = pAT1->Alloc();
		if (AtNum != RET_MEM_FULL)
			return AtNum;
		else
		{
			AtNum = pAT2->Alloc();
			if (AtNum != RET_MEM_FULL)
				return Size + AtNum;
			else
				return RET_MEM_FULL;
		}
	}
	//如果是偶数，则先从pAT2分配
	else
	{
		AtNum = pAT2->Alloc();
		if (AtNum != RET_MEM_FULL)
			return Size + AtNum;
		else
		{
			AtNum = pAT1->Alloc();
			return AtNum;
		}
	}
}

//单线程用
void zAT::Free(size_t UnitPos)
{
	//如果Unit==Size，那么应该在pAT2!
	if (UnitPos < Size)
		pAT1->Free(UnitPos);
	else
		pAT2->Free(UnitPos - Size);
}

void zAT::PreSet(size_t UnitPos)
{
	//如果Unit==Size，那么应该在pAT2!
	if (UnitPos < Size)
		pAT1->PreSet(UnitPos);
	else
		pAT2->PreSet(UnitPos - Size);
}
//得到对应单元Unit的状态，被占用返回false,否则为true
bool zAT::GetUnitStatus(size_t Unit)
{
	//低5位表示位的位置
	uint16_t BitPos = Unit & 0x1f;
	uint32_t *p;
	if (Unit < Size)
		p = pAT1->pT[pAT1->MaxLayer - 1] + (Unit >> 5);
	else
		p = pAT2->pT[pAT2->MaxLayer - 1] + ((Unit - Size) >> 5);
	return zBitTest(p, BitPos);
}

}//NAME SPACE ZZG
