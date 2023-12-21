//ZZG_Mem.h by Phelps Zhao
//Version 20231001
#ifndef ZZG_MEM_H_2310
#define ZZG_MEM_H_2310
/* ******说明********
 * 主要定义了一个快速内存分配堆zMemHeap，主要用于固定长度的小块内存分配，避免大量小内存块的频繁分配和释放造成的内存碎片化，
 * 提升运行效率。可以单线程使用，也可以多线程使用。使用方式很简单:
1、定义个zMemHeap类型实例，eg:zMemHeap* pHeap=new zMemHeap(MaxNum);//MaxNum为可以分配的最多个数
2、分配和释放。单线程:pHeap->Alloc()和 pHeap->Free()。多线程:pHeap->LockAlloc()和pHeap->LockFree()
3、不需要的时候就直接delete pHeap;
详细函数使用说明看注释
**********************/

#include <new>
#include <stdint.h>
#include <stddef.h>
#include <cstring>
#include "ZZG_Sync.h"
namespace ZZG {

//zCAT分配树叶子节点的分配缓冲空间位数，每个叶子节点32位，缓冲空间为FREE_THRESH_HOLD-1位。
//缓冲空间设置是为了防止临界满状态时可能发生的频繁多层操作。
//缓冲空间的存在会导致空间利用率的下降。最差情况时，空间利用率只有（33-FREE_THRESH_HOLD）/32
#define FREE_THRESH_HOLD	3

#if (defined(_DEBUG)||defined(DEBUG))
class zMemStack;
extern zMemStack zMem_Stack;
//内存记录
struct zMemInfo {
	zMemInfo *pNext;	//指向下一个
	void *ptr;
	const char *File;
	unsigned int Line;
};

//内存记录栈
class zMemStack {
private:
	zMemInfo *pHead;		//指向链表第一个记录
	zMemInfo *pLast;	//指向链表最后一个记录	
	zLock	lk;
public:
	zMemStack() {
		pHead = NULL;
		pLast = NULL;
	};
	~zMemStack() {};
	int Insert(void *p, const char *File, unsigned int Line);
	int Delete(void *p);
	zMemInfo* GetNextLeak(zMemInfo*p);

};
#endif
//************************End***********************


//自定义分配管理树。类似B+树的分配树，和B+树不同点是从不删除节点。
#define MAX_LAYER	8
class z__AT;
class zAT {
private:
	//申请线程计数器，根据奇偶属性把线程分配到不同的分配树上。由于不用锁，多线程的时候计数并不一定准，
	//但是用来奇偶分类，减少线程冲突足够了
	volatile int Count;
	void *pBuf;	//指向存放数据的缓冲区
    z__AT *pAT1, *pAT2;
	size_t Size; //pAT1和pAT2实际可分配的数量都是Size;
	size_t NodeNum;	//pAT1和pAt2的节点数，两个节点数一样。总的节点数为2*NodeNum
	uint32_t MaxLayer;
	size_t Nodes[MAX_LAYER];	//pAT1和pAT2各层节点数
public:
static	const size_t RET_MEM_FULL = ~0x0;	//	内存分配完时的错误返回值
static	const size_t RET_SUCCESS	=0;	//函数成功返回值

	//初始化分配树
	//@para[Capacity:in]:最大分配数，此值若为0则初始化失败。最好是64的倍数，那么实际最大分配数就是Capacity
	//为了优化效率和保证速度，正常情况下分配管理算法会保留一部分空单元，实际可分配使用单元数大概为最大值的97%左右，
	//最差情况空间利用率为最大值的93.75%。使用时需要精确最大值的，使用者自己控制。
	//若内存不足，则抛出异常std::bad_alloc
	zAT(size_t Capacity) {
		pBuf = 0;
		NodeNum = 0;
		MaxLayer = 1;
		if (!Init(Capacity))
			throw std::bad_alloc();
	};
	~zAT() {
		close();
	};

	//#if defined(_DEBUG)||defined(DEBUG)
	//得到对应单元Unit的状态，被占用返回false,否则为true
	bool GetUnitStatus(size_t Unit);
	//#endif

	//@ret:返回值为分配的单元序数。若已满，无单元可分配则返回RET_MEM_FULL
	//多线程用
	size_t LockedAlloc();

	//释放指定内存单元
	//多线程用
	void LockedFree(size_t UnitPos);

	//@ret:返回值为分配的单元序数。若已满，无单元可分配则返回RET_MEM_FULL
	size_t Alloc();

	//释放指定内存单元
	//不锁定，单线程适用
	void Free(size_t UnitPos);

	//恢复初始状态，相当于执行Init()之后的状态。必须确保没有线程在使用本类时才可以调用本函数
	//比起重新实例化初始化一个可以节约内存分配之类的操作。少分配内存也意味着可以减少内存碎片化。
	void Reset();

	//预先设置一些单元为占用状态。当分配树初始化后，所有单元都为空闲状态，用这个函数可以预设一些单元为已被分配的状态
	//此函数必须在Init()后，LockedAlloc()/Alloc()之前使用。
	void PreSet(size_t UnitPos);
private:
	//初始化分配树
	//MaxNum最大分配数，此值若为0则初始化失败。最好是64的倍数，那么实际最大分配数就是MaxMum
	//为了优化效率和保证速度，正常情况下分配管理算法会保留一部分空单元，实际可分配使用单元数大概为最大值的97%左右，
	//最差情况空间利用率为最大值的93.75%。使用时需要精确最大值的，使用者自己控制。
	//@ret:成功则返回true，失败则返回false
	bool Init(size_t MaxNum);
	//释放资源,只在析构时调用。
	void close();

};


//分配长度固定的快速内存堆管理。每次申请长度为class T的长度。
//多线程适用。避免内存碎片，数据存储集中在同一块内存，有利于CPU缓存，加速程序运行速度
template<class T>
class zMemHeap
{
    zAT *pAT;
	T *pT;	//指向类T的存储区
	size_t Count;//实际缓冲区的长度，单位为T的长度
static	const size_t RET_MEM_FULL = ~0x0;

public:
	
	//@para[MaxMum:in]:可能分配的最大数量
	//初始化堆内存,内存不足则失败,同时会有std::bad_alloc抛出。内存分配的起始地址是按页对齐的
	zMemHeap(size_t MaxMum)
	{
		//加1保证在最差空间利用率的情况下也能有所需分配空间
		Count = MaxMum * 32 / (33 - FREE_THRESH_HOLD) + 1;
		pAT = new zAT(Count);
		size_t SizeCount = Count * sizeof(T);
		pT = (T*)malloc(SizeCount);
		if (!pT)
			throw std::bad_alloc();
    };
	~zMemHeap()
	{
		close();
	};
	
	//获取内部缓冲的首地址，返回缓冲区长度，单位为T长度
	//有些时候需要初始化缓冲区，可以使用本函数得到缓冲区信息
	size_t GetBuf(T *pBuf)
	{
		pBuf = pT;
		return Count;
	}
	//分配内存。和Free()配合使用
	// 成功返回指向分配的内存区的指针；失败返回0
	//无锁，适合所单线程使用
	T *Alloc()
	{
		size_t ret=pAT->Alloc();
		if (ret != RET_MEM_FULL)
			return pT + ret;
		return 0;
	}
	//释放内存。和Alloc()配合使用
	//无所，单线程适用。
	void Free(T*p)
	{
		pAT->Free(((size_t)p - (size_t)pT) / sizeof(T));
	}

	//多线程分配内存。和LockFree()配合使用
	// 成功返回指向分配的内存区的指针；失败返回0
	//函数内部使用了锁机制，所以单线程最好不用
	T *LockAlloc()
	{
		size_t ret = pAT->LockedAlloc();
		if (ret != RET_MEM_FULL)
			return pT + ret;
		return 0;
	}
	//多线程释放内存。和LockAlloc()配合使用
	//函数内部使用了锁机制，所以单线程最好不用
	void LockFree(T*p)
	{
		pAT->LockedFree(((size_t)p - (size_t)pT) / sizeof(T));
	}

	//恢复初始状态，相当于第一次构造函数执行之后的状态。必须确保没有线程在使用本类时才可以调用本函数
	//比起重新实例化初始化一个可以节约内存分配之类的操作。少分配内存也意味着可以减少内存碎片化。
	void Reset()
	{
		pAT->Reset();
	}
private:
	//关闭释放资源
	void close()
	{
		if (pT)
        {
            free(pT);
		}
		delete pAT;
	}
};
//*****************调试检测内存泄漏用*********************
/*
#if defined(_DEBUG)||defined(DEBUG)
		//重载全局new运算符
	inline void * operator new(size_t size,const TCHAR*File,unsigned int Line)
	{
		void *p=malloc(size);
		if(p)zMem_Stack.Insert(p,File,Line);
		return p;
	}

	//重载全局new[]运算符
	inline void * operator new[](size_t size,const TCHAR *File,unsigned int Line)
	{
		return operator new(size,File,Line);
	}

	//重载全局delete运算符
	inline void operator delete(void *p)
	{
		free(p);
		zMem_Stack.Delete(p);
	}

	//重载全局delete[]运算符
	inline void operator delete[](void *p)
	{
		operator delete(p);
	}
#define new new(_T(__FILE__),__LINE__)
#endif*/

}//NAME SPACE ZZG
#endif//!ZZG_MEM_H_2310
