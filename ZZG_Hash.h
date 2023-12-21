//ZZG_Hash.h by Phelps Zhao
//Version 20231001
#ifndef ZZG_HASH_H_2310
#define ZZG_HASH_H_2310
/********* Instructions for use *****************

*zHash is a thread-safe hash table.You may use all public functions. The following example codes illustrates how to use it:
#include "ZZG_Hash.h"
uint64 Value;
ZZG::zHash <uint64_t, uint64_t>  MyHash;     // Define a key and a value whose type is a 64-bit integer
// Insert a key-value pair.
MyHash. Insert (1001, 3);
// Get the value for key 1001 from the hash table.
MyHash.Value(1001,&Value);
* Check source code comments for more specifications. The access efficiency of a hash table depends largely on the hash function.
* If you are not satisfied with the default hash function, you may use the member function SetHashFunction() to set  your own hash
* function. So far I've only defined default hash functions for the basic types (integers, floating numbers, and pointers),
* std::string,std::wstring and QString. Please define for other types yourself

********Technical specification *****************

1. On the whole,it is similar to a normal hash table. It consists of a linear table in which items are the buckets(their entries are ENTRYs). Each bucket is
associated with some nodes(DATA_NODE) that store specific data(Key,value and the corresponding hash).
The data nodes attached to each bucket are organized in two ways: a linked list with few data nodes or a B-Tree with a lot of data nodes

2. Each bucket entrance has a read/write lock, and each data node has a sequential version lock. The operation of inserting or removing
data nodes in each bucket is exclusive.That is, only one thread can operate in inserting or removing.Other threads must wait, cannot read data,
cannot modify data. It's necessary to get the read lock of  the bucket entry for read and modify operations.

3, Read is completely concurrent, a data node can be read by multiple threads at the same time. During reading, the bucket is read locked and cannot be inserted or deleted data.

4, the updating of the data node is exclusive, only one thread at the same time can update after obtaining the write lock of the version lock.
The bucket is read locked and cannot be insert and delete data.

5, the reading and updating of the data node can be carried out concurrently, and the reading thread determines whether the data is consistent by version comparison.
In general, the read and update operations are fully concurrent; Insert and delete are concurrent at the bucket level

/*****************consideration for improvement*************
* If there are too many hash collisions, the concurrency performance will be reduced due to the presence of bucket locks. There are two optimization ways:

* 1, Divid the bucket with collitions into several buckets, each bucket has an independent bucket lock

* 2,  Divided  the datas in this bucket into several parts according to some sorting rule, such as key value order, attach them to other empty buckets,
* and record the addresses and key value range......

* */
#include "ZZG_Bit.h"
#include "ZZG_Mem.h"
#include "ZZG_Sync.h"
#include <string>
#include <new>
using namespace std;
#define MAX_LINKEDLIST_SIZE	6	//The maximum length of a linked list attached to a hash table entry, beyond which a B-tree is used instead
#define MIN_BTREE_SIZE	5	//The minimum size of B-tree attached to the hash table entry, less than this size the linked list is used instead

namespace ZZG {

// Hash function definition. The quality of hash function greatly affects the performance of hash table.
// Hash of numeric numbers, including integers, floating-point numbers, and Pointers.The type size shouldn't be longer than that of size_t
// Be careful not to directly take number as hash unless they are consecutive integers. Considering that we only take the lower part of the hash
//value as the address of the hash bucket entry,the hash function only needs to pass the effect of the change in the high bits to the low bits,
//and does not need to consider the effect of the change in the low bits to the high bits. In this way, both the hash distribution and the calculation
//speed can be balanced.
template <typename TK,typename=std::enable_if_t<(std::is_arithmetic_v<TK>||std::is_pointer_v<TK>),TK>>
size_t zHashFun(const TK &Key,uint16_t MaskBits)
{
    if constexpr(sizeof(TK)==8)
    {
        uint64_t Tmp=*(uint64_t*)&Key;
		uint64_t h = Tmp;
		while (Tmp >>= MaskBits)
			h^= Tmp;
		return h;
    }
    else if constexpr(sizeof(TK)==4)
    {
        uint32_t Tmp=*(uint32_t*)&Key;
		uint32_t h = Tmp;
		while (Tmp >>= MaskBits)
			h ^= Tmp;
		return h;
    }
    else if constexpr(sizeof(TK)==2)
    {
        uint16_t Tmp=*(uint16_t*)&Key;
		uint16_t h = Tmp;
		while (Tmp >>= MaskBits)
			h ^= Tmp;
		return h;
    }
    else
    {
        uint8_t Tmp=*(uint8_t*)&Key;
        return Tmp;
    }
}

//hash function for C++ standard string
size_t zHashFun(const std::string &Key,uint16_t MaskBits)
{
	int8_t* pc = (int8_t*)Key.data();
	size_t h=0;
	while (*(pc++))
        h = *pc + h * 9;//*pc+(h<<3)+h，The number 9 can  be replaced with 3,17,33, etc
    h ^= (h >> MaskBits);	//Increase the imppact of high bits on low bits, making the hash distribution more even
	return h;
}

//hash function for C++ wide string
size_t zHashFun(const std::wstring &Key,uint16_t MaskBits)
{
	wchar_t * pc = (wchar_t*)Key.data();
	size_t h = 0;
	while (*(pc++))
		h = *pc + h * 9;//*pc+(h<<3)+h
    h ^= (h >> MaskBits);	//Increase the imppact of high bits on low bits, making the hash distribution more even
	return h;
}

//hash function for Qt string
#ifdef QSTRING_H
//test
size_t zHashFun(const QString &Key,uint16_t MaskBits)
{
    uint16_t * pc =(uint16_t*)Key.constData();
	size_t h = 0;
	while (*(pc++))
	h = *pc+ h * 9;//*pc+(h<<3)+h
    h ^= (h >> MaskBits);	//Increase the imppact of high bits on low bits, making the hash distribution more even
	return h;
}
#endif


// Data node template
template <class TK, class TV>
class DATA_NODE
{
public:

    size_t h;	// Hash value. Comparing hash values is generally faster than comparing key values.
                //When searching in a bucket list, compare hash values first, if equal, then compare key values
    TK key;	//Key
    TV value;	//Value corresponding to the key
    zSeqLock slock;	//Version lock. No lock is required for reading, it has high concurrent efficiency
    DATA_NODE *pNext;	//The pointer to the next data node. This member is used  when the datas in the bucket are organized in a linked list
	DATA_NODE()
	{};
	~DATA_NODE()
	{};
};

//Define some constants for B-Tree
static const int M = 3;                  //The minimum degree of the B-Tree
static const int KEY_MAX = 2 * M - 1;        //All nodes (including root) may contain at most (2*M – 1) keys.
static const int KEY_MIN = M - 1;          //Every node except the root must contain at least M-1 keys. The root may contain 1 key.
static const int CHILD_MAX = KEY_MAX + 1;  //Maximum children of a node.The number of a children of a node is equal to the number of keys in it plus 1.
static const int CHILD_MIN = KEY_MIN + 1;  //Minimum children of a node.

template<class TK, class TV>
class zBTreeNode
{
public:
    zBTreeNode * parent;	//Pointer to the parent node
    int KeyNum;              //Number of the keys of the node
    DATA_NODE<TK, TV> *Key[KEY_MAX];     //Keys(Pointers to the data nodes)
    zBTreeNode *pChild[CHILD_MAX]; //Pointers to children.If the first is 0, then all must be 0, and the node must be a leaf node

	zBTreeNode()
	{}
	~zBTreeNode()
	{}

    // Finds the first element in the Key array that is not less than (key,h) and return the index number
    // If none of the elements are greater than (key,h), then return pNode->KeyNum
    int searchKey(TK key, size_t h)
	{
		int i = 0;
		while (i < KeyNum)
		{
			if (h < Key[i]->h || (h == Key[i]->h && key < Key[i]->key))
				return i;
			++i;
		}
		return i;
	}

    //Inserts (key) at index position (pos)
	void insertKey(int pos, DATA_NODE<TK, TV> *key)
	{
		for (int i = KeyNum - 1; i >= pos; --i)
			Key[i + 1] = Key[i];
		Key[pos] = key;
	}

	void insertChild(int pos, zBTreeNode *pN)
	{
		for (int i = KeyNum; i >= pos; --i)
			pChild[i + 1] = pChild[i];
		pChild[pos] = pN;
	}

	void removeKey(int pos)
	{
		for (int i = pos + 1; i < KeyNum; ++i)
			Key[i - 1] = Key[i];
	}

	void removeChild(int pos)
	{
		for (int i = pos + 1; i <= KeyNum; ++i)
			pChild[i - 1] = pChild[i];
	}
};


//B tree template, specially for hash table
// The difference between zBtree and normal B-tree is that the key type is data node,
// and there are two keywords for comparison (hash value and key value).
template<class TK, class TV>
class zBTree
{
private:
    size_t Size;	//Total number of data nodes
    zBTreeNode <TK, TV> * m_pRoot;  //The pointer to the root of the B-tree
    zMemHeap<zBTreeNode<TK, TV>> * pNodeHeap;	//The memory heap for allocation of zBTreeNode
	

    // Searches the position of (key,h) in the B-tree. Similar to the search() function, the only
    //difference is that search does one more check to see if the node needs to be split. A
    //non-0 value is returned if the (key) already exists in the B-tree,and the pointer to the node
    //and the index number in the node are returned. The index number is stored in (index). If
    //0 is returned, the (key) doesn't exist in the B-tree. (hot) points to the last searched
    //leaf node. If you want to insert (key,h) then you should insert this leaf node in the first
    //position after (index)
    //@para[memError:out]: If the value is true, the function ends due to memory allocation failure.
    //If the value is false, the function ends normally
    zBTreeNode <TK, TV> * searchForInsert(const TK &key, size_t h, zBTreeNode <TK, TV> * &hot, int &index, bool &memError);


    // Splits the node. Returns true on success or false if memory allocation fails
    //@para[nChildIndex:in]: indicates the index position in the parent node
    // When number of a node keys reaches KEY_MAX, it needs to be split into two nodes
    // This function cannot be used to split the root node
	bool splitChild(zBTreeNode <TK, TV> *pParent, int nChildIndex, zBTreeNode <TK, TV> *pNode);


    //Solves the underflow problem. If the number of keys in a node is less than KEY_MIN, underflow occurs. Underflow is not allowed and needs to be corrected
	void solveUnderflow(zBTreeNode <TK, TV> *q);


    // Recursively gets all data node Pointers to the subtree where the pNode is the root node
    // Returns the starting address of the next unfilled buffer for the next recursive_getData call
    //@para[pBuf:out]:the buffer to save the found data node pointers
	DATA_NODE<TK, TV>** recursive_getData(zBTreeNode <TK, TV> *pNode, DATA_NODE<TK, TV>** pBuf);


    //clears the subtree pNode including deleting its data nodes
	void recursive_clear(zBTreeNode <TK, TV> *pNode);


public:
	zBTree(zMemHeap<zBTreeNode<TK, TV>> * pNodeHeap)
	{
		this->pNodeHeap = pNodeHeap;
		m_pRoot = NULL;  //创建一棵空的B树
		Size = 0;
	}

	~zBTree()
	{
        // zHash cleans the data node separately from the tree node and the tree itself
        // During capacity expansion, all tree nodes are deleted. You do not need to delete tree nodes
        //individually when deleting B-tree.
        // When the B-tree is converted to a linked list, the tree nodes need to be deleted separately,
        //and the Clear() function needs to be called separately to clear the B-tree node
        //Clear();
	}
	size_t Count()
	{
		return Size;
	}

    //Insert a new data node for (key,h)
    //@para[h:in]:key's hash
    //@para[pD:out]:points to a data node
    //@ret:returns 0 on success.and (pD) contains the pointer to the pointer to the data node which is empty
    //returns 1 if the key exists in the tree,and (pD) contains the pointer  to the existing data node pointer
    int Insert(const TK &key, size_t h, DATA_NODE<TK, TV> **&pD);


    //deletes a node (Key,h)
    //@ret:returns the pointer to the deleted data node.the function is not responsibe for freeing memory
    //returns 0 if (key,h) does not exist in the tree
    DATA_NODE<TK, TV> * Remove(const TK &key, size_t h);


    //gets the pointer to the data node of (key,h)
    //returns the pointer onsuccess,0 otherwise
    DATA_NODE<TK, TV>* FindData(const TK &key, size_t h)
	{
		int index;
		zBTreeNode <TK, TV> *pN = Search(key, h, index);
		if (pN)
			return pN->Key[index];
		return 0;
	}


    //Finds the node of (key,h) in the tree
    //Returns the pointer to the node on success,0 otherwise
    zBTreeNode <TK, TV>* Search(const TK &key, size_t h, int& index);


    //Traverses the B-tree, output all data node pointers of the tree to the buffer pointed by pBuf
	void FindAllData(DATA_NODE<TK, TV>** pBuf)
	{
		recursive_getData(m_pRoot, pBuf);
	}

    //Clears the B-tree,makes it empty
	void Clear();
};

template<class TK, class TV>
class zHash
{
    //Do not change the values of these codes, because some functions use numeric values directly
	enum RETURN_CODE{
        HASH_KEY_EXIST=1,	//the key exists
        ERR_MEMORY	=-1,	//memory error
        SUCCESS	=0	//succeeds
	};
    //Defines the type of hash function.
    //@para[Key:in]:Key
    //@para[MaskBits:in]:equal to the member MaskBits of zHash
    typedef size_t(*ZHASH_FUNCTION)(const TK &Key,uint16_t MaskBits);
    zMemHeap<DATA_NODE<TK, TV>> *pHeap;	//The heap for data node memory allocation. At least ThreshHold data nodes can be stored in it

    //memory allocation heap for B-tree node. Centralized storage reduces memory fragmentation and improves access efficiency.
    //At least KEY_MIN data nodes can be mounted at each tree node. Therefore, as long as (ThreshHold+ KEY_min-1)/KEY_MIN is reserved in advance for
    //tree nodes allocation,memory shortage of data node will not occur before the number of  data nodes reaches ThreshHold
	zMemHeap<zBTreeNode<TK, TV>> * pBTNodeHeap;

    // Indicates whether the capacity is being adjusted. If the capacity is being adjusted, the data cannot be accessed and you must
    // wait for the adjustment to be finished. When the data load reaches the specified amount, the hash table expands automatically.
	volatile bool FlagResize;
    bool Resizable;	//Rezizable flag.If true,the capacity will be adjusted according to current data number.If false,the capacity is fixed
    bool Countable;	//If true,zHash will record the number of items automatically,otherswise it doesn't. Countable must be set true if Resizable is true
    size_t MaxSize;	//Maximum number of buckets capacity. The maximum number of buckets to be automatically resized cannot exceed this value

    //Structure of the bucket entrance
	struct ENTRY {
        zBTree<TK, TV> *p;	//the pointer to B-tree of the head of the linked list.0 means no data(empty)
        zRWLock lock;	//read/write lock. You must get the read lock of the bucket before reading/updating data,write lock before inserting/deleting data
        size_t Size_Type;	//If the datas organized as a B-tree,it's 0. Otherwise a linked list,and the value indicates the number of items
	};

    ENTRY *pBucket;	//The pointer to the bucket table
    size_t Buckets;	//Total number of buckets.It's always an integer power of 2. e.g. 16,32,64,128,256....

    size_t PosMask;	//Buckets minus 1. Because Buckets are an integer power of 2, so all bits of PosMask are 1s.
                    // ANDing any number to PosMask is equivalent to being divided by Buckets. We can get the index
                    //position of the bucket entry in the bucket table by ANDing hash to PosMask
    uint16_t MaskBits;//The number of 1s of PosMask(binary)
    volatile std::atomic_size_t DataCount;	//Current total number of items in zHash
    double LoadFactor;	//Load factor。The table may be cluttered and have longer search times and collisions if the load factor is  too high.The default value is 0.75
    size_t Threshold;    // Data load threshold. The load factor is 0.75. Threshold=Buckets*0.75. When the total number of data reaches this threshold, the hash table should be expanded.
                        // Capacity is expanded only when the data node memory allocation heap pHeap or B-tree node memory allocation heap pBTNodeHeap fails to allocate.
                        // The actual Threshold is approximately 0 to 1/15 higher than Threshold. If Threshold=0.75, the actual maximum threshold will be 0.80

    zLock ResizeLock;	//Resizing lock.Only one thread is allowed to perform resizing operation at a time
    volatile std::atomic_uint32_t  Vistors; //Number of threads visiting(all operations including read,update,insert,delete)
    ZHASH_FUNCTION pHashFun;	//The pointer to the hash function

public:
    // Defines the type of a check function.Just for the future
    //If the return value is 0, the lock condition is met. >0 Does not meet the condition but continues to wait;
    //<0 Does not meet the condition, and you don't want to wait.
    //Extra is used to pass external data to the conditional function
	typedef int(*CHECK_FP)(TV *, size_t Extra);

    //Data structure used when locking an item of zHash.Just for the future
	struct LOCKPACK
	{
        TV *pV;	//pointer to the data node
        void *pE;	//Pointer go the bucket entrance
	};

    zHash();

	~zHash()
	{
        close();
    }


    //Inserts an item(Key,*pValue)
    //@ret:SUCCESS on success
    //HASH_KEY_EXIST if Key already exists,ERR_MEMORY if no space or resizing(expansion) fails
    int Insert(TK Key, const TV *pValue);

    //Inserts an item(Key,Value)
    //@ret:SUCCESS on success
    //HASH_KEY_EXIST if Key already exists,ERR_MEMORY if no space or resizing(expansion) fails
	int Insert(TK Key, TV Value)
	{
		return Insert(Key,&Value);
	}

    //Inserts/updates an item(Key,*pValue).
    //If Key does not exist,inserting is performed; Otherwise, performs updating
    //@ret:SUCCESS on success
    //ERR_MEMORY if no space or resizing(expansion) fails
    bool Upsert(TK Key, TV *pValue);

    //Inserts/updates an item(Key,Value).
    //If Key does not exist,inserting is performed; Otherwise, performs updating
    //@ret:SUCCESS on success
    //ERR_MEMORY if no space or resizing(expansion) fails
	bool Upsert(TK Key, TV Value)
	{
		return Upsert(Key, &Value);
	}


    //Gets the value associated with Key.
    //Returns true on success.The value is stored in the buffer Ret pointing to
    //Returns false if zHash contains no item with Key
	bool Value(TK Key, TV *Ret);


    //@para[pRet:in/out]:If pRet set 1 before calling,The deleted data value is store in *pRet after return.Otherwise,the deleted data is discarded
    //Deletes the item assosiated with Key.
    //@ret:true if the item exists and deleted,false if zHash does not contain the item
	bool Del(TK Key, TV *pRet = 0);


    //Updates the item assosiated with Key
    //@para[pValue:in]:Pointer to the new value
    //@ret:true if the item exists and is updated,false if the item doesn't exist
	bool Update(TK Key, TV *pValue);


    //Updates the item assosiated with Key
    //@para[Value:in]: new value
    //@ret:true if the item exists and is updated,false if the item doesn't exist
    bool Update(TK Key, TV Value)
    {
        return Update(Key,&Value);
    }

    //Gets current total number of buckets of the hash table
	size_t GetBucketNum()
	{
		return Buckets;
	}


    //Sets the initial number of buckets. The number of buckets multiplied by the load factor (0.75 by default) is the amount of data that can be stored
    //@para[InitBuckets:in]: specifies the initial number of buckets to be set. If it is not a power of 2, then the function will round it up to the nearest power of 2
    //@ret: returns true on success. False is returned if memory allocation fails
    // The default initial bucket number is 256
    // This function must be executed before any data operation (insert, delete, modify, read) is performed
    bool SetInitBuckets( size_t InitBuckets);


    //Sets a new hash function to replace the default
    //@ret: returns true on success. false is returned if memory allocation fails
    //This function must be executed before any data operation (insert, delete, modify, read) is performed
    void SetHashFunction(ZHASH_FUNCTION pFun)
    {
        pHashFun=pFun;
    }


    //Sets the load factor.
    //The default value of the load factor is 0.75, which can be adjusted as required. Reducing this value can reduce the collision probability
    //and improve performance, but the memory utilization will decrease.
    //If you increase this value, performance deteriorates but memory utilization increases.
    //This function must be executed before any data operation (insert, delete, modify, read) is performed
    bool SetLoadFactor(double LoadFactor)
    {
        this->LoadFactor=LoadFactor;
        return SetInitBuckets(this->Buckets);
    }


    // Sets whether the hash table capacity automatically expands according to the number of items.
    //@para[bResize:in]:true indicates that the capacity automatically increases. false indicates that the capacity is fixed
    // This parameter has a slight impact on performance. slightly faster if the hash table has a fixed capacity.
    //This function must be executed before any data operation (insert, delete, modify, read) is performed
    void SetResizable(bool bResize)
    {
        this->Resizable=bResize;
        this->Countable=true;
    }


    //Sets the maximum of buckets number. If the number of buckets reaches MaxSize, the capacity of the hash table can not be increased even there is enough memory
    //The function is only for resizable hash table,has no effect on a hash table with fixed capacity
    //This function must be executed before any data operation (insert, delete, modify, read) is performed
    void SetMaxBuckets(size_t MaxSize)
    {
        this->MaxSize=MaxSize;
    }


    // Sets whether to count items automatically.
    //This function only works on the fixed hash table.No effect on resizable hashtable
    //@para[bCount:in]: If true, records the number of items in the table in real time. Otherwise, doesn't record.
    // This parameter has a slight impact on insert and delete performance. If not recording, it will be slightly faster.
    void SetCountable(bool bCount)
    {
        if(!this->Resizable)
            Countable=bCount;
    }


    //Checks the current items distribution on buckets.
    //@para[Buckets:out]: indicates the current bucket capacity including empty buckets and buckets with items
    //@para[FilledBuckets:out]: Specifies the number of buckets with iems. The larger the value, the better the distribution.
    //The ideal case is the same as the amount of items, that is, there are no collision
    //@para[Items:out]: indicates the current total number of items.
    //@para[Collisions:out]: indicates the number of buckets with collision(with multiple items). As little as possible, preferably none.
    //@para[MaxCollision:out]: indicates the number of items in the bucket with the most severe collision(with most items). If there is
    //no collision, the return value is 0. Generally, as long as the value is not too large, it should not have a great impact on the overall performance.
    // For example, the absolute value does not exceed 100, and the relative value (as a percentage of the total number of items) does not exceed 1%.
    //If the value is too large, it has a significant impact on insert and delete performance, but not much on update and read performance
    // When the function is executed, the hash table expansion function is suspended, but the data operation is allowed.
    //It's re
    //To get an acurate result,it is recommended to pause the insert and delete operations while running this function
    void CheckHash(size_t &Buckets,size_t &FilledBuckets,size_t &Items,size_t &Collisions, size_t &MaxCollision);


    //Evaluates the hash distribution of a given hash function for a specific keys sequence and bucket table size.
    //@para[pFun:in]: Specifies the hash function. Note that the key type must be TK
    //@para[Key[]:in]: an array of key values
    //@para[Keynum:in]: indicates the amount of key contained in Key[].
    //@para[Buckets:in]: specifies the number of buckets. Preferably a power of two. For the convenience of calculation, zHash table uses
    //the NTH power of 2 as the buckets amount, and the lower n bits of the hash value are intercepted as the bucket address.
    //If it is not a power of 2, the result of the calculation is of little significance for zHash
    //@para[FilledBuckets:out]: Specifies the number of buckets with items. The larger the value, the better. Ideally, it is the same as
    //the total number of keys, that is, there are no collision
    //@para[Collisions:out]: indicates the number of buckets with collision(with multiple items). As little as possible, preferably none.
    //@para[MaxCollision:out]: indicates the number of items in the bucket with the most severe collision(with most items).
    //@ret: Usually succeeds and returns true. However, if the memory is insufficient, false is returned, indicating that the execution failed
    static bool TestHash(ZHASH_FUNCTION pFun,TK Key[],size_t KeyNum,size_t Buckets,size_t &FilledBuckets,size_t &Collitions,size_t &MaxCollition);

private:

    //Closes the hash table,free all resources
    //Do not visit after closing
    void close();


    //Waits for all threads to pause
    //This function is only used when resizing the capacity
	void waitVisitorsPause(void)
	{
        volatile int count = 3;
		do {
			if (!count)
			{
                std::this_thread::yield();
				count = 3;
			}
            else if (!Vistors)//stops waiting if there's no thread running
                break;
			--count;
			for (int i = 0; i < 31; ++i)
                zNop8();
        } while (true);
	}


    //Initializes the bucket entrances,excutes constructors for each entrances and assigns necessary initial values
	void initBucketList()
	{
		for (size_t i = 0; i < Buckets; ++i)
		{
			new (pBucket + i) ENTRY;
			pBucket[i].p = 0;
		}
	}


    //Synchronizes resizing with data operations
    //All functions which
	void begin()
    {
		if (Resizable)
		{
            //Increases the number of threads visiting.
            //"acquire order" guarantees that subsequent(C++ codes order) reads and writes will not be executed until this instruction has been executed
            std::atomic_fetch_add_explicit(&Vistors,1,std::memory_order_acquire);
            if(FlagResize)//If being resizing,wait untill it is finished
            {
                std::atomic_fetch_sub_explicit(&Vistors,1,std::memory_order_acquire);
                zWaitUntil(FlagResize,false);
                //"acq_rel order" guarntees strict excecution order
                std::atomic_fetch_add_explicit(&Vistors,1,std::memory_order_acq_rel);
            }
		}
	}


    //This function is only used after a successful inserting operation
    //Synchronizes resizing with data operations
	void endAdd()
	{
		if (Resizable)
		{
            std::atomic_fetch_add_explicit(&DataCount,1,std::memory_order_relaxed);
            //"release order" guarantees all previous(word order) reads/writes are excecuted before the instruction
            std::atomic_fetch_sub_explicit(&Vistors,1,std::memory_order_release);            
		}
		else if (Countable)
            std::atomic_fetch_add_explicit(&DataCount,1,std::memory_order_relaxed);
	}


    //This function is only used after a successful deleting operation
     //Synchronizes resizing with data operations
	void endDel()
	{
		if (Resizable)
		{
            std::atomic_fetch_sub_explicit(&DataCount,1,std::memory_order_relaxed);
            std::atomic_fetch_sub_explicit(&Vistors,1,std::memory_order_release);            
		}
		else if (Countable)
            std::atomic_fetch_sub_explicit(&DataCount,1,std::memory_order_relaxed);
	}


    //This function is used after reads,updates or failure of add/delete operation.That is to say,after the operation by which the number of items has no change
	void end()
	{
		if (Resizable)
		{
            std::atomic_fetch_sub_explicit(&Vistors,1,std::memory_order_release);
		}
	}


    // Expansion function. When the number of items reaches Threshold, the hash table will be expanded
    // The hash table must be locked before executing this function. No thread can access the hash table during expansion
    // Returned value: true indicates success. false indicates failure, and success is guaranteed unless memory is insufficient
	bool upSize();


    // Temporarily suspends all threads visiting and try to expand capacity. Return false on failure, true on success;
    // This function coordinate threads to suspend for expanding the capacity. Calls the upSize () function to expand the capacity and restores the operation before the expansion
    // If Resizable is true, then this function is called when allocation of data node fails.
    // As mentioned above, all threads visiting must be suspended during capacity expansion.
    bool pauseAndUpsize();


    // Round the input number up to an integer power of 2（2 to the power of n,n is an integer
    // If the input number is a power of 2, then the return value is the input value
	size_t roundUp(size_t X)
	{
		size_t tmp = X;
		size_t ret = 1;
		
		while (X >>= 1)
		{
			ret <<= 1;
		}
        return ret < tmp ? ret << 1 : ret;//Chech if the input value is exactly a power of 2
	}


    // Converts the linked list to a B-Tree
    // If memory allocation fails, a failure is returned. Failure does not change the original data
    bool listToBTree(ENTRY*pT);

     // Converts B-Tree to linked list
	void treeToList(ENTRY*pT);


    //Copies item and inserts it into the hash table
    //Only used when resizing
    //@ret:true on success,false on failure
	bool inserCopyData(DATA_NODE<TK, TV>*pSrc);


    //Insert a key value into the specified bucket and  allocates a data node.
    //To use this function, it must be guaranteed that the target bucket is not accessed by other threads
    //that is, you must call pEntry->lock.WLock() before calling this function,and call pEntry->lock.WUnlock() after it returns.
    //One more important thing,the value of pEntry may be changed in the function if the hash table resized in it.In such case,the old
    //pEntry->lock.WUnlock() is called automatically before the pEntry is set a new pointer,and the new pEntry->lock.WLock() is called after in the function
    // Return value: SUCCESS indicates success, HASH_KEY_EXIST indicates that the key exists, and ERR_MEMORY indicates there's no memory
    //(or the amount of the items reaches the maximum).
    //@para[pRet:out]: returns the allocated data node pointer on success. If the key already exists, returns the existing data node pointer
    //@para[pEntry:inout]: indicates the pointer to the bucket entry.
    int  insertKey(ENTRY* &pEntry, size_t h, TK &key, DATA_NODE<TK, TV>* &pRet);


    //Searches the data node associated with (key).
    //Returns the pointer to the data node associated with (key) if the bucket contains the item,or returns 0 if the bucket contains no item with the key
	DATA_NODE<TK, TV>*searchAndRLock(TK &key, ENTRY *&pEntry);

};
//*************************************************************/
/*********Function definitions*************/
/**************************************************************/
template<class TK, class TV>
zBTreeNode <TK, TV> *  zBTree<TK, TV>::Search(const TK &key, size_t h, int &index)
{
	zBTreeNode <TK, TV> * p = m_pRoot;
	zBTreeNode <TK, TV> * parent = NULL;

    //starts searching from the root.
	while (p)
	{
        //returns the first element that is not greater than (key,h)
		index = p->searchKey(key, h) - 1;

        //If index==-1, it means that (key,h) is smaller than the first element, and the target may be in the left subtree of the first element,
        //directly descending one level to continue searching
		if (index >= 0)
		{
            //finds the object
			if (h == p->Key[index]->h && key == p->Key[index]->key)
				return p;
		}
        //otherwise,descends to the lower level
		parent = p;
		p = p->pChild[index + 1];
	}
	return NULL;
}

template<class TK, class TV>
zBTreeNode <TK, TV> *  zBTree<TK, TV>::searchForInsert(const TK &key, size_t h, zBTreeNode <TK, TV> * &hot, int &index, bool &memError)
{
	zBTreeNode <TK, TV> * p = m_pRoot;
	hot = NULL;
	memError = false;
	while (p)
	{
        //returns the first element that is not greater than (key,h)
		index = p->searchKey(key, h) - 1;
        //If index==-1, it means that (key,h) is smaller than the first element, and the target may be in the left subtree of the first element,
        //directly descending one level to continue searching
		if (index >= 0)
		{
			//已经找到
			if (h == p->Key[index]->h && key == p->Key[index]->key)
				return p;
		}
        //or, descends one level
		hot = p;
		p = p->pChild[index + 1];

        //Splits the node if it's element amount reaches KEY_MAX
        //Here we only check inner nodes or leaf nodes
        //The root node checked before,so the number of elements of the root must be less than KEY_MAX
		if (p&&p->KeyNum >= KEY_MAX)
		{
			if (!splitChild(hot, index + 1, p))
			{
				memError = true;
				return 0;
			}

            //Checks the newly added key in the parent node, decides the new searching path according to its value
            //If(key,h) is greater than the newly added key,the node to search for the next step is the node pointed to by the newly added pointer in the parent node
			if (h > hot->Key[index + 1]->h || (h == hot->Key[index + 1]->h&&key > hot->Key[index + 1]->key))
				p = hot->pChild[index + 2];
			else
                //ends searching if the newly added key in the parent node is equal to (key,h)
				if (h == hot->Key[index + 1]->h && key == hot->Key[index + 1]->key)
				{
					++index;
					return hot;
				}
		}
	}
	return NULL;
}
template<class TK, class TV>
int zBTree<TK, TV>::Insert(const TK &key, size_t h, DATA_NODE<TK, TV> **&pD)
{
    //Checks if the root node is full.Splits it if full
    //The check must be done before search(),otherwise the splitting of the child node of the root will make the number of the children of the root overflow
	if (m_pRoot&&m_pRoot->KeyNum == KEY_MAX)
	{
        zBTreeNode <TK, TV> *pNode = pNodeHeap->LockAlloc();//New root node
		if (!pNode)
			return -1;
        new(pNode) zBTreeNode <TK, TV>;

		zBTreeNode <TK, TV> *pRight = pNodeHeap->LockAlloc();
		if (!pRight)
		{
			pNodeHeap->LockFree(pNode);
			return -1;
		}
        new(pRight) zBTreeNode <TK, TV>;  //Creates a new node for splitting,it will become the right sibling node of the original node

		pNode->pChild[0] = m_pRoot;
		pNode->pChild[1] = pRight;
        pNode->Key[0] = m_pRoot->Key[KEY_MIN];	//Raises the middle key to the new root node as the first key
		pNode->KeyNum = 1;
		pNode->parent = 0;

        //moves the second left half of elenments to the new right sibling node,and modifies the pointer to the parent node
		int i;
		for (i = 0; i < KEY_MIN; ++i)
		{
			pRight->Key[i] = m_pRoot->Key[i + KEY_MIN + 1];
			pRight->pChild[i] = m_pRoot->pChild[i + KEY_MIN + 1];
		}
        pRight->pChild[KEY_MIN] = m_pRoot->pChild[CHILD_MAX - 1];	//copies the pointer to the last child node

        //If the root node is a leaf node, then the split right node is also a leaf node, and the child node pointers need to be set 0
		if (!m_pRoot->pChild[0])
			for (int i = KEY_MIN + 1; i <= KEY_MAX; ++i)
				pRight->pChild[i] = 0;
		else
		{
            //If not leaf node,it is neccessary to set the pointer to new parent node for  the child nodes
			for (int i = 0; i <= KEY_MIN; ++i)
				pRight->pChild[i]->parent = pRight;
		}
		pRight->KeyNum = KEY_MIN;
		pRight->parent = pNode;

		m_pRoot->KeyNum = KEY_MIN;
		m_pRoot->parent = pNode;

        m_pRoot = pNode;  //Upates root node
	}
	bool MemError;
	int index;
	zBTreeNode <TK, TV> * hot;
	//不允许重复元素
	zBTreeNode <TK, TV> *pR = searchForInsert(key, h, hot, index, MemError);
	if (pR)
	{
		pD = &(pR->Key[index]);
		return 1;
	}
	if (MemError)
		return -1;


    //After executing search(),(hot) points to the parent node of the target node.
    //If search() fails,(hot) points to a leaf node(external node's parent)
	zBTreeNode<TK, TV> *p = hot;
	if (!p)
    {	//If the tree is empty
		m_pRoot = pNodeHeap->LockAlloc();
		if (!m_pRoot)
			return -1;
		new(m_pRoot) zBTreeNode <TK, TV>;

		m_pRoot->parent = 0;
		//	m_pRoot->Key[0] = pD;
        pD = &(m_pRoot->Key[0]);	//returns the index position to insert
                                    //The root node is also a leaf node, and all pointers to children are set to 0
		for (int i = 0; i <= KEY_MAX; ++i)
			m_pRoot->pChild[i] = 0;
		m_pRoot->KeyNum = 1;
		++Size;
		return 0;
	}

    //If the tree not empty,inserts the data node at index+1 position of the leaf node
    //p->insertKey(index + 1, pD);
	for (int i = p->KeyNum - 1; i > index; --i)
		p->Key[i + 1] = p->Key[i];
    pD = &(p->Key[index + 1]);	//returns the index position to insert
    ++p->KeyNum;
	++Size;
	return 0;
}

//分裂子节点
template<class TK, class TV>
bool zBTree<TK, TV>::splitChild(zBTreeNode <TK, TV> *pParent, int nChildIndex, zBTreeNode <TK, TV> *pNode)
{
    //New node for the right sibling node after splitting.
    //The original node wil become the left sibling node after splitting
	zBTreeNode <TK, TV> *pRightNode = pNodeHeap->LockAlloc();
	if (!pRightNode)
		return false;
	new(pRightNode) zBTreeNode <TK, TV>;

	pRightNode->KeyNum = KEY_MIN;
	pRightNode->parent = pParent;
	int i;
    for (i = 0; i < KEY_MIN; ++i)//Copies the second half elements to the new right sibling node
	{
		pRightNode->Key[i] = pNode->Key[i + CHILD_MIN];
	}

    //If not leaf node,copies children node and updates their pointes to the parent
    if (pNode->pChild[0])
	{
		for (i = 0; i < CHILD_MIN; ++i)
		{
			pRightNode->pChild[i] = pNode->pChild[i + CHILD_MIN];
			pRightNode->pChild[i]->parent = pRightNode;
		}
	}

    //If a leaf node,the splitted nodes are still leaf nodes,and all child poiters should be set to 0
	else
		for (int i = 0; i < CHILD_MAX; ++i)
			pRightNode->pChild[i] = 0;
    pNode->KeyNum = KEY_MIN;  //Updates the amount of the keys of the left splitted child node(original node)

    pParent->insertKey(nChildIndex, pNode->Key[KEY_MIN]);//Raises the middle node to the parent node
	pParent->insertChild(nChildIndex + 1, pRightNode);
	++pParent->KeyNum;  //更新父节点的关键字个数

	return  true;
}

template<class TK, class TV>
DATA_NODE<TK, TV> * zBTree<TK, TV>::Remove(const TK &key, size_t h)
{

    //Finds the position of (key,h)
	int index;
	zBTreeNode <TK, TV> *p = Search(key, h, index);
	if (!p) return 0;
    DATA_NODE<TK, TV> *	ret = p->Key[index];//Keeps the poiter to the deleted data node

    //If the node which contains (key,h) is not a leaf, replace it with its inorder successor and then simply delete the successor at the leaf node
	if (p->pChild[0])
	{
		zBTreeNode <TK, TV> *q = p->pChild[index + 1];
		while (q->pChild[0])
		{
			q = q->pChild[0];
		}
		p->Key[index] = q->Key[0];
		//	p->key.put(n, q->key[0]);
		index = 0;
		p = q;
	}

    //Now the node to be deleted is located at leaf node. The child node Pointers of a leaf node are all null values and no need to be considered
	int i = index;
	for (; i < p->KeyNum - 1; ++i)
		p->Key[i] = p->Key[i + 1];
	--p->KeyNum;

    //After deleting,there may be underflow issue that need to be solved.
	if (p != m_pRoot)
	{
		solveUnderflow(p);
	}
	--Size;
	return ret;
}

template<class TK, class TV>
void zBTree<TK, TV>::solveUnderflow(zBTreeNode <TK, TV> *q)
{
    //If no underflow
	if (q->KeyNum >= KEY_MIN) return;

	if (q == m_pRoot)
	{
        //If the number of the (Key) is 0,raises the only child tree as the root
        //The height of the tree decrease by 1
		if (!q->KeyNum)
		{			
			m_pRoot = q->pChild[0];
			pNodeHeap->LockFree(q);
			m_pRoot->parent = 0;
		}
		return;
	}

	zBTreeNode <TK, TV> *p = q->parent;
	int n;
	for (n = 0; n <= p->KeyNum; ++n)
	{
		if (p->pChild[n] == q)
			break;
	}

	zBTreeNode <TK, TV> *lc;
	zBTreeNode <TK, TV> *rc;

    //The left sibling node of q can lend keys
	if (n > 0 && p->pChild[n - 1]->KeyNum > KEY_MIN)
	{
        //Move the corresponding key from the parent node to the 1st positon of the node
		lc = p->pChild[n - 1];
		q->insertKey(0, p->Key[n - 1]);

        //if not a leaf node,moves the last child node of the left sibling node to the node as the first child node,and updates the parent node of the child node
		if (q->pChild[0])
		{
			q->insertChild(0, lc->pChild[lc->KeyNum]);
			q->pChild[0]->parent = q;
		}
		++q->KeyNum;

        //Moves the last key of the left sibling node to the parent node as a substitute for the moved key
		p->Key[n - 1] = lc->Key[lc->KeyNum - 1];

		--lc->KeyNum;
		return;
	}
    //The right sibling node of q can lent keys
	if (n < p->KeyNum && p->pChild[n + 1]->KeyNum >KEY_MIN)
	{
		rc = p->pChild[n + 1];

        //Moves the corresponding key from the parent node to the rightmost positon of the node
		q->Key[q->KeyNum] = p->Key[n];
		++q->KeyNum;

        //if not a leaf node,moves the firs child node of the right sibling node to the node as the last child node,and updates the parent node of the child node
		if (rc->pChild[0])
		{
			q->pChild[q->KeyNum] = rc->pChild[0];
			rc->pChild[0]->parent = q;
		}

        //Moves the first key of the right sibling node to the paren node as a substitute for the moved key
		p->Key[n] = rc->Key[0];
		//有兄弟节点中移除第一个关键字和孩子节点
		rc->removeKey(0);
		rc->removeChild(0);
		--rc->KeyNum;
		return;
	}


    //If both the immediate sibling nodes already have a minimum number of keys, then merge the node with either the left sibling node
    //or the right sibling node. This merging is done through the parent node.

    //If left sibling node exists,merges the node with it
    if (n > 0)
	{
		lc = p->pChild[n - 1];
		//把父节点对应关键字下移并入左节点
		lc->Key[KEY_MIN] = p->Key[n - 1];
		++lc->KeyNum;
		//把本节点的关键字移入左节点
		int i;
		for (i = 0; i < q->KeyNum; ++i)
			lc->Key[lc->KeyNum + i] = q->Key[i];
		//如果是非叶子节点，把孩子节点指针也移入，同时修改子节点的父节点为左节点
		if (q->pChild[0])
			for (i = 0; i <= q->KeyNum; ++i)
			{
				q->pChild[i]->parent = lc;
				lc->pChild[lc->KeyNum + i] = q->pChild[i];
			}
		lc->KeyNum += q->KeyNum;
		//父节点中移除对应关键字
		p->removeKey(n - 1);
		p->removeChild(n);
		--p->KeyNum;
		pNodeHeap->LockFree(q);
	}
    //otherwise,merges the node with the right sibling node
    else
	{
		rc = p->pChild[n + 1];
		//把父节点对应关键字下移并入本节点
		q->Key[KEY_MIN - 1] = p->Key[n];
		++q->KeyNum;
		//把右节点的关键字移入本节点
		int i;
		for (i = 0; i < rc->KeyNum; ++i)
			q->Key[q->KeyNum + i] = rc->Key[i];
		//如果是非叶子节点，把孩子节点指针也移入，同时修改子节点的父节点为左节点
		if (rc->pChild[0])
			for (i = 0; i <= rc->KeyNum; ++i)
			{
				rc->pChild[i]->parent = q;
				q->pChild[q->KeyNum + i] = rc->pChild[i];
			}
		q->KeyNum += rc->KeyNum;
		//父节点中移除对应关键字
		p->removeKey(n);
		p->removeChild(n + 1);
		--p->KeyNum;
		pNodeHeap->LockFree(rc);
	}

    //The number of children of the parent node decreases after merging
    //So we should check and solve the underflow problem of the parent node
	solveUnderflow(p);
}

template<class TK, class TV>
DATA_NODE<TK, TV>** zBTree<TK, TV>::recursive_getData(zBTreeNode <TK, TV> *pNode, DATA_NODE<TK, TV>** pBuf)
{
	DATA_NODE<TK, TV>** pRet = pBuf;
	if (pNode->pChild[0])//如果不是叶子节点,先把各个子树的数据取完
	{
		for (int i = 0; i <= pNode->KeyNum; ++i)
		{
			pRet = recursive_getData(pNode->pChild[i], pRet);
		}
	}
	//取节点自身的数据
	for (int i = 0; i < pNode->KeyNum; ++i, ++pRet)
		*pRet = pNode->Key[i];
	return pRet;
}

template<class TK, class TV>
void zBTree<TK, TV>::Clear()  //清空B树
{
	if (!m_pRoot)
		return;
	recursive_clear(m_pRoot);
	m_pRoot = NULL;
}

template<class TK, class TV>
void zBTree<TK, TV>::recursive_clear(zBTreeNode <TK, TV> *pNode)
{
	//如果不是叶子节点，那么递归删除子树后再删除自身。叶子节点直接删除自己
	if (pNode->pChild[0])
	{
		for (int i = 0; i <= pNode->KeyNum; ++i)
			recursive_clear(pNode->pChild[i]);
	}
	pNodeHeap->LockFree(pNode);
}

template<class TK, class TV>
zHash<TK, TV>::zHash()
{
    atomic_init(&Vistors,0);
    pHashFun = zHashFun;
    Buckets = 256;//2**8,initial default number of buckets
    this->LoadFactor=0.75;
    this->Threshold = (size_t)((double)Buckets*LoadFactor);

    pHeap = new zMemHeap<DATA_NODE<TK, TV>>(Threshold);
	try {
        pBTNodeHeap = new zMemHeap<zBTreeNode<TK, TV>>((Threshold + KEY_MIN - 1) / KEY_MIN);
	}
    catch(std::bad_alloc)
    {
        delete pHeap;
        throw std::bad_alloc();
    }
    size_t SizeCount = Buckets * sizeof(ENTRY);
    pBucket =(ENTRY*) malloc(SizeCount);
    if (!pBucket)
    {
		delete pHeap;
		delete pBTNodeHeap;
        throw std::bad_alloc();
    }
    PosMask = Buckets - 1;
    MaskBits = zBitCount(PosMask);
    DataCount = 0;
    FlagResize = false;
    Resizable = true;
    Countable = true;

    //By default, MaxSize is the largest number that can be represented by size_t value
    MaxSize=0xff;
    for(unsigned int i=1;i<sizeof(size_t);++i)
        MaxSize=(MaxSize<<8)|0xff;
    initBucketList();
}

template<class TK, class TV>
bool zHash<TK, TV>::upSize()
{
	zMemHeap<DATA_NODE<TK, TV>>* pHeapOld = pHeap;
	ENTRY* pBucketOld = pBucket;
	size_t BucketsOld = Buckets;
	size_t PosMaskOld = PosMask;
	int MaskBitsOld = MaskBits;
	size_t DataCountOld = DataCount;
	zMemHeap<zBTreeNode<TK, TV>>* pBTNodeHeapOld = pBTNodeHeap;

	pBucket = 0;
	pHeap = 0;
	pBTNodeHeap = 0;
	Buckets = BucketsOld << 1;
    Threshold = (size_t)((double)Buckets * LoadFactor);
	try {
        pHeap = new zMemHeap<DATA_NODE<TK, TV>>(Threshold);
        pBTNodeHeap = new zMemHeap<zBTreeNode<TK, TV>>((Threshold + 1) >> 1);
		
		size_t SizeCount = Buckets * sizeof(ENTRY);
		pBucket =(ENTRY*) malloc(SizeCount);
		if (!pBucket)
			throw std::bad_alloc();
		initBucketList();
		PosMask = Buckets - 1;
		MaskBits = zBitCount(PosMask);

        //Scans and copies items from the old hash table
		for (size_t i = 0; i < BucketsOld; ++i)
		{
            //if not empty
			if (pBucketOld[i].p)
			{
                //if a linked list
				if (pBucketOld[i].Size_Type > 0)
				{
					DATA_NODE<TK, TV>* pNext = (DATA_NODE<TK, TV>*)pBucketOld[i].p;
					do
					{
						if (!inserCopyData(pNext))
							throw std::bad_alloc();
                    } while ((pNext = pNext->pNext));

				}
                //if a B-tree
                else
				{
					DATA_NODE<TK, TV>** pBuf;
					size_t Count = (pBucketOld[i].p)->Count();
					pBuf = new(nothrow) DATA_NODE<TK, TV>* [Count];
					if (!pBuf)
						throw std::bad_alloc();
					pBucketOld[i].p->FindAllData(pBuf);
                    for (unsigned k = 0; k < Count; ++k)
						if (!inserCopyData(pBuf[k]))
							throw std::bad_alloc();
				}
			}
		}

        // Deletes the B-tree on the bucket entrances only after the capacity expansion is successfully complete
		for (size_t i = 0; i < BucketsOld; ++i)
		{
			if (pBucketOld[i].p && pBucketOld[i].Size_Type <= 0)
				delete pBucketOld[i].p;
		}
        //Free resources
		delete pHeapOld;
		delete pBTNodeHeapOld;
		free(pBucketOld);
		return true;
	}
	catch (std::bad_alloc)
	{
        //Restores the original state and return the error code

        //If the new hash table has been established,cleans the occupied resources by it
        //Cleans the B-trees first,because B-trees uses he memory heep pointed by pBTNodeHeap
        if (pBucket)
		{
            //Free memory of B-Trees
            for (unsigned int i = 0; i < Buckets; ++i)
				if (pBucket[i].p && !pBucket[i].Size_Type)
					delete pBucket[i].p;
			free(pBucket);
		}
		if (pHeap)
			delete pHeap;
		if (pBTNodeHeap)
			delete pBTNodeHeap;
		pHeap = pHeapOld;
		pBTNodeHeap = pBTNodeHeapOld;
		pBucket = pBucketOld;
		Buckets = BucketsOld;
		PosMask = PosMaskOld;
		MaskBits = MaskBitsOld;
		DataCount = DataCountOld;
		return false;
	}
}

template<class TK, class TV>
bool zHash<TK, TV>::pauseAndUpsize()
{
    //If Resizable= true, checks and tries to expand the capacity
	if (Resizable)
	{
        if (Buckets > MaxSize)
			return false;
        //Exits current visiting,then tries to lock.Otherwise,the threads trying to lock will deadlock because of waiting for each other to exit
        //"acquire order" guarantees that atomic_fetch_sub_explicit() is executed before ResizeLock.Lock();
        std::atomic_fetch_sub_explicit(&Vistors, 1, std::memory_order_acquire);
		ResizeLock.Lock();

        //Because of multithreading,checks again after locking to confirm if expansion is needed
        //If Threshold is large enough,it means that probably another thread has expand the capacity,no need to expand again
        if (DataCount >=Threshold)
		{
            //Sets FlagResize to true. Any thread will suspend its visiting when FlagResize is true
			FlagResize = true;
            //Release memory fence guarantees that "FlagResize = true" is executed before the first writes of the subsequent codes(C++ order)
            //That is,it's guaranteed that FlagResize is set to true before the expansion is really started
            std::atomic_thread_fence(std::memory_order_release);
            //waits for all threads visiting to pause
			waitVisitorsPause();

            //If expansion fails
            if (!upSize())
			{
                FlagResize = false;
				ResizeLock.Unlock();
                begin();	//Restart visiting
				return false;
            }
            //Release memory fence guarantees that "FlagResize = false" is executed after the prior(C++ codes order) reads/writes
            std::atomic_thread_fence(std::memory_order_release);
			FlagResize = false;
		}
		ResizeLock.Unlock();
        begin();	//Restart visiting
		return true;
	}
    else    //If the hash table is  not resizeable
		return false;

}

template<class TK, class TV>
bool zHash<TK, TV>::listToBTree(ENTRY * pT)
{
    //Keeps the old head of the linked list for restoring on failure
    DATA_NODE<TK, TV>* pOld = (DATA_NODE<TK, TV>*) pT->p;
    DATA_NODE<TK, TV>*pNext = pOld;
    pT->p = new (nothrow) zBTree<TK, TV>(pBTNodeHeap);
    if(!pT->p)
    {
        pT->p=(zBTree<TK, TV>*)pOld;
        return false;
    }
	DATA_NODE<TK, TV>**tmp;
	do {
        //It's guaranteed that There are enough B-Tree node for allocation
        pT->p->Insert(pNext->key, pNext->h, tmp);
        *tmp = pNext;	//Inserts the pointer to data node
    } while ((pNext = pNext->pNext));
	pT->Size_Type = 0;
    return true;
}

template<class TK, class TV>
void zHash<TK, TV>::treeToList(ENTRY * pT)
{
	size_t count = pT->p->Count();
	DATA_NODE<TK, TV>* *pBuf = new DATA_NODE<TK, TV>*[count];
    //Gets pointers to all data nodes
    pT->p->FindAllData(pBuf);
    //Frees the resources of the B-Tress
    pT->p->Clear();
    delete pT->p;
    //Attaches data nodes to the bucket
	pT->p = (zBTree<TK, TV> *)pBuf[0];
	pT->Size_Type = count;
	size_t i = 1;
	for (; i < count; ++i)
		pBuf[i - 1]->pNext = pBuf[i];
	pBuf[i - 1]->pNext = 0;
    delete[] pBuf;

}

template<class TK, class TV>
bool zHash<TK, TV>::inserCopyData(DATA_NODE<TK, TV>* pSrc)
{
    DATA_NODE<TK, TV>*pData = pHeap->LockAlloc();	//allocates data nodes
    new(pData) DATA_NODE<TK, TV>;	//initializes data node

    //The hash is related to the size of the bucket table,so it should be recalculated after expantion
    pData->h = pHashFun(pSrc->key, MaskBits);
	pData->key = pSrc->key;
	pData->value = pSrc->value;

    size_t pos = pData->h&(size_t)PosMask;	//gets the index position of the bucket
    if (!pBucket[pos].p)	//if the bucket is empty,attaches the data node
	{
		pBucket[pos].p = (zBTree<TK, TV> *)pData;
        pBucket[pos].Size_Type = 1;	//"1" means a linked list in the bucket
        pData->pNext = 0;	//"0" identifies the end of the linked list
	}
    else if (pBucket[pos].Size_Type > 0)	//If it the bucket contains a linked list
	{
        //Directly inserts the data node into the linked list if the size of the linked list is less than MAX_LINKEDLIST_SIZE
        if (pBucket[pos].Size_Type < MAX_LINKEDLIST_SIZE)
		{
			pData->pNext = (DATA_NODE<TK, TV>*)pBucket[pos].p;
			pBucket[pos].p = (zBTree<TK, TV> *)pData;
			++pBucket[pos].Size_Type;
		}
        else    //If the size of the linked list not less than MAX_LINKEDLIST_SIZE,converts it into a B-Tree
		{
            //retursn false on failure
            if(!listToBTree(pBucket + pos))
                return false;
			DATA_NODE<TK, TV>**tmp;
            //Inserts the data node
			if (pBucket[pos].p->Insert(pData->key, pData->h, tmp))
				return false;
			*tmp = pData;
		}
	}
    else    //If the bucket contains a B-Tree
	{
		DATA_NODE<TK, TV>**tmp;
		if (pBucket[pos].p->Insert(pData->key, pData->h, tmp))
			return false;
		*tmp = pData;
	}
	return true;
}

template<class TK, class TV>
int zHash<TK, TV>::insertKey(ENTRY* &pEntry, size_t h, TK &Key, DATA_NODE<TK, TV>* &pRet)
{
INSERT_BEGIN:
    if (!pEntry->p)	//If the bucket is empty
	{
        pRet = pHeap->LockAlloc();	//allocates a data node

        //If the allocation fails,tries to expand the capacity.
        //tries inserting again after successful expansion
        if (!pRet)
		{
            if (pauseAndUpsize())
            {
                //The hash is related to the size of the bucket table,so it should be recalculated after expantion
                h = pHashFun(Key, MaskBits);
                //Be careful,do not forget to unlock the old bucket before changing into the new bucket
                pEntry->lock.WUnlock();
                pEntry = pBucket + (h&(size_t)PosMask);
                pEntry->lock.WLock();	//Locks the new bucket
				goto INSERT_BEGIN;
			}
			else
				return -1;
		}
        new(pRet) DATA_NODE<TK, TV>;	//Initializes the data node
		pRet->h = h;
		pRet->key = Key;

		pEntry->p = (zBTree<TK, TV> *) pRet;
        pEntry->Size_Type = 1;	// Linked list
        pRet->pNext = 0;	//identifies end of the list
	}
    else if (pEntry->Size_Type > 0)	//If the bucket contains a linked list
	{
        //Checks if the bucket contains the key.If yes,returns
		for (DATA_NODE<TK, TV>*pNext = (DATA_NODE<TK, TV>*)pEntry->p; pNext; pNext = pNext->pNext)
		{
			if (h == pNext->h&&Key == pNext->key)
			{
				pRet = pNext;
				return 1;
			}
		}
        pRet = pHeap->LockAlloc();
        if (!pRet)
		{
            if (pauseAndUpsize())
            {
                //The hash is related to the size of the bucket table,so it should be recalculated after expantion
                h = pHashFun(Key, MaskBits);
                //Be careful,do not forget to unlock the old bucket before changing into the new bucket
                pEntry->lock.WUnlock();
                pEntry = pBucket + (h&(size_t)PosMask);
                pEntry->lock.WLock();	//Locks the new bucket
				goto INSERT_BEGIN;
			}
			else
				return -1;
		}
        new(pRet) DATA_NODE<TK, TV>;	//Initializes the node
		pRet->h = h;
		pRet->key = Key;
		pRet->pNext = (DATA_NODE<TK, TV>*)pEntry->p;
		pEntry->p = (zBTree<TK, TV> *)pRet;
        ++pEntry->Size_Type;	//Increases the size of the bucket

        //If the size greater than MAX_LINKEDLIST_SIZE,converts it into a B-tree
        if (pEntry->Size_Type >= MAX_LINKEDLIST_SIZE)
            if(!listToBTree(pEntry))
                return ERR_MEMORY;
	}
    else    //If the bucket contains a B-tree
	{

        // Pre-allocation of data nodes
        // Be careful here. The execution order of LockAlloc() and pEntry->p->Insert(Key, h, tmp) is important.
        //when you call pHeap->LockAlloc() after success calling of pEntry->p->Insert(), if there is insufficient memory,and the cpacity is expanded,
        //an error will occur. Because the size of inner B-tree increases after a successful calling pEntry->p->Insert(), but the corresponding data
        //node has not been allocated by pHeap->LockAlloc() and filled data. That is,in such case, the BTree pointed by p contains a "incomplete" item which
        //doesn't have data
        // There will be an error
        pRet = pHeap->LockAlloc();
        //If the allocation fails,tries to expand the capacity.
        //tries inserting again after successful expansion
        if (!pRet)
        {
            int index;
            zBTreeNode<TK,TV> *pBTNode=pEntry->p->Search(Key,h,index);
            if(pBTNode) //If (key,h) already exists
            {
                pRet=pBTNode->Key[index];
                return 1;
            }
			if (pauseAndUpsize())
            {
                //The hash is related to the size of the bucket table,so it should be recalculated after successful expantion
                h = pHashFun(Key, MaskBits);
                //Be careful,do not forget to unlock the old bucket before changing into the new bucket
                pEntry->lock.WUnlock();
                pEntry = pBucket + (h & (size_t)PosMask);
                pEntry->lock.WLock();	//Locks the new bucket
				goto INSERT_BEGIN;
			}
			else
				return -1;
		}
        DATA_NODE<TK, TV>**tmp;//To store the data node pointer returned from the B-tree

		int ret = pEntry->p->Insert(Key, h, tmp);
        if (!ret)//If inserting succeeds
		{
            new(pRet) DATA_NODE<TK, TV>;	//Initializes the data node
			pRet->h = h;
			pRet->key = Key;
			*tmp = pRet;
		}
        else if (ret == 1)	//If (key,h) already exists
		{
            pHeap->LockFree(pRet);//Frees the pre-allocated data node
			pRet = *tmp;
			return 1;
		}
        else    //If because of insufficient capacity
		{
            pHeap->LockFree(pRet);//Frees the pre-allocated data node
            if (pauseAndUpsize())
            {
                //The hash is related to the size of the bucket table,so it should be recalculated after expantion
                h = pHashFun(Key, MaskBits);
                //Be careful,do not forget to unlock the old bucket before changing into the new bucket
                pEntry->lock.WUnlock();
                pEntry = pBucket + (h&(size_t)PosMask);
                pEntry->lock.WLock();	//Locks the new bucket
				goto INSERT_BEGIN;
			}
			else
				return -1;
		}
	}
	return 0;
}

template<class TK, class TV>
DATA_NODE<TK, TV>* zHash<TK, TV>::searchAndRLock(TK &key, ENTRY *& pEntry)
{
    size_t h = pHashFun(key, MaskBits);
    pEntry = pBucket + (h&(size_t)PosMask);
    if (!pEntry->p)	//If empty,searching fails,returns
		return 0;
    pEntry->lock.RLock();	//read locks the entrance
	DATA_NODE<TK, TV>*pRet;
    if (!pEntry->p)	//Because of multithreading,checks again after locking
	{
		pEntry->lock.RUnlock();
		return 0;
	}
    if (pEntry->Size_Type > 0)	//If the bucket contains a linked list
	{
		for (pRet = (DATA_NODE<TK, TV>*)pEntry->p; pRet; pRet = pRet->pNext)
		{
			if (h == pRet->h&&key == pRet->key)
				return pRet;
		}
	}
    else    //If the bucket contains a B-tree
	{
		pRet = pEntry->p->FindData(key, h);
		if (pRet)
			return pRet;
	}
	pEntry->lock.RUnlock();
	return 0;
}

template<class TK, class TV>
void zHash<TK, TV>::close()
{
	if (pBucket)
	{
        //Scans to delete B-trees
		for (size_t i = 0; i < Buckets; ++i)
		{
			if (pBucket[i].p && pBucket[i].Size_Type <= 0)
				delete pBucket[i].p;
		}
		delete pHeap;
		pHeap = 0;
		delete pBTNodeHeap;
		pBTNodeHeap = 0;
        free(pBucket);
		pBucket = 0;
	}
}

// Summary: Calculates the hash value according to the key value and finds the corresponding bucket entrance.
//Tries to insert a key value after the bucket entry is write locked. After (Key) is successfully inserted, the data
//pointed to by pValue is written into. Then the bucket unlocks the bucket and returns
template<class TK, class TV>
int zHash<TK, TV>::Insert(TK Key, const TV *pValue)
{
	begin();
    size_t h = pHashFun(Key, MaskBits);
    ENTRY *pT = pBucket + (h&(size_t)PosMask);
    pT->lock.WLock();	//locks the bucket entry
	DATA_NODE<TK, TV>* pRet;
    int ret = insertKey(pT, h, Key, pRet);
    if (!ret)	//Fills the data node on successful inserting
	{
		pRet->value = *pValue;
		pT->lock.WUnlock();
		endAdd();
		return 0;
	}
	pT->lock.WUnlock();
	end();
	return ret;
}

//Summary:Finds the bucket entry based on the hash value of (Key) and write locks it. Tries to insert a key value,
//if it does not exist and the insertion fails, returns false; If the insertion is successful or the key already exists,
// Then fills/updates the data
template<class TK, class TV>
bool zHash<TK, TV>::Upsert(TK Key, TV *pValue)
{
	begin();
    size_t h = pHashFun(Key, PosMask, MaskBits);
    ENTRY *pT = pBucket + (h&(size_t)PosMask);
	DATA_NODE<TK, TV>* pRet;
    pT->lock.WLock();	//locks the bucket entry
    int ret = insertKey(pT, h, Key, pRet);
    if (ret == ERR_MEMORY)	//Full,no space to insert
	{
		pT->lock.WUnlock();
		end();
		return false;
	}

    //Updates the data node with new data  if the (key) is inserted or exists before
	pRet->value = *pValue;
	pT->lock.WUnlock();
	if (!ret)	//如果插入了一条记录
		endAdd();
	else
		end();
	return true;
}

//Summary:calls searchAndRLock() to find the data node,then returns the data
template<class TK, class TV>
bool zHash<TK, TV>::Value(TK Key, TV *pRet)
{
	ENTRY *pT;
	begin();
	DATA_NODE<TK, TV>*pD = searchAndRLock(Key, pT);
	if (!pD)	//如果没有
	{
		pT->lock.RUnlock();
		end();
		return false;
	}

    // When the entry of the bucket is read locked, the data node cannot be deleted, but may be modified.
    // To ensure the consistency of read data, use sequence lock to control reading and writing data
	int Ver;
	do {
		Ver = pD->slock.ReadBegin();
		*pRet = pD->value;
	} while (pD->slock.ReadRetry(Ver));
	pT->lock.RUnlock();
	end();
	return true;
}

//Summary:Finds the corresponding bucket based on the hash value calculated by the key value. If there is no data
//in the bucket, return; Otherwise, the bucket entry is write locked. After the locking, searches for and deletes
//data according to data structures attached to the bucket
template<class TK, class TV>
bool zHash<TK, TV>::Del(TK Key, TV *pRet)
{
    size_t h = pHashFun(Key, PosMask, MaskBits);
	begin();
    ENTRY *pEntry = pBucket + (h&(size_t)PosMask);
    if (!pEntry->p)	//(key) doesn't exist
	{
		end();
		return false;
	}
    pEntry->lock.WLock();	//locks the entry
    if (!pEntry->p)	//Checks the entry again after locking bcause of muti-threads
		goto EXIT_NONE;
    if (pEntry->Size_Type > 0)	//If linked list
	{
		DATA_NODE<TK, TV> *pD = (DATA_NODE<TK, TV>*)pEntry->p;
        DATA_NODE<TK, TV> * pPre = 0;//The previos node of pD
		while (pD)
		{
            //Deletes if same
			if (h == pD->h&&Key == pD->key)
				break;
			pPre = pD;
			pD = pD->pNext;
		}
        if (!pD)	//Already reaches the end of the linked list if pD=0
			goto EXIT_NONE;
        if (!pPre)	//If it's the first data node
			pEntry->p = (zBTree<TK, TV> *)pD->pNext;
        else    //If it's not the first data node
			pPre->pNext = pD->pNext;

        if (pRet)	//If the caller needs the the deleted data value
			*pRet = pD->value;
        pHeap->LockFree(pD);//Frees the data node
        if (!(--pEntry->Size_Type))	//If the amount decreases to zero,marks the bucket as empty
			pEntry->p = 0;
	}
    else    //If B-tree
	{
		DATA_NODE<TK, TV> *pD = pEntry->p->Remove(Key, h);
        if (!pD)//(Key,h) doesn't exist in the tree
			goto EXIT_NONE;
        if (pRet)	//If the caller needs the the deleted data value
			*pRet = pD->value;
        //Frees the data node
		pHeap->LockFree(pD);
        //如If the amount is less than MIN_BTREE_SIZE,convert B-tree into linked list
		if (pEntry->p->Count() < MIN_BTREE_SIZE)
            treeToList(pEntry);
	}
	pEntry->lock.WUnlock();
	endDel();
	return true;

EXIT_NONE:	//Doesn't find the key,unlocks and returns
	pEntry->lock.WUnlock();
	end();
	return false;
}


template<class TK, class TV>
void zHash<TK, TV>::CheckHash(size_t &Buckets,size_t &FilledBuckets,size_t &Elements,size_t &Collisions, size_t &MaxCollision)
{
    Buckets=this->Buckets;
	if (Resizable)
    {
        //If the hash table is resizable,pauses resizing function
		ResizeLock.Lock();
        FlagResize = true;
        //全内存屏障，保证下面等待过程的执行发生在FlagResize设置之后
        //和begin()内对应形成互锁
        //Release memory fence guarantees that "FlagResize = true" is executed before the first writes of the subsequent codes(C++ order)
        //That is,it's guaranteed that FlagResize is set to true before the thread really begins to wait for other threads to suspend
        std::atomic_thread_fence(std::memory_order_release);
		waitVisitorsPause();
	}

    //Checks and collects the situation of distribution and collisions
    FilledBuckets=0;
    Elements=0;
    Collisions = 0;
    MaxCollision=0;

    for (unsigned int i = 0; i < Buckets; ++i)
		if (pBucket[i].p)
		{
            ++FilledBuckets;
            if(pBucket[i].Size_Type==1)
            {
                ++Elements;
            }
            else if (pBucket[i].Size_Type >= 2) //Linked list
            {
                Elements+=pBucket[i].Size_Type;
                ++Collisions;
                if(MaxCollision<pBucket[i].Size_Type)
                    MaxCollision=pBucket[i].Size_Type;
			}
            else if (!pBucket[i].Size_Type) //B-tree
			{
                size_t temp=pBucket[i].p->Count();
                Elements+=temp;
                ++Collisions;
                if(MaxCollision<temp)
                    MaxCollision=temp;
			}
		}
	if (Resizable)
	{
        //Release memory fence guarantees that "FlagResize = false" is executed after all previous codes (C++ order)
        std::atomic_thread_fence(std::memory_order_release);
		FlagResize = false;
		ResizeLock.Unlock();
	}
}

template<class TK, class TV>
bool zHash<TK, TV>::TestHash(ZHASH_FUNCTION pFun,TK Key[],size_t KeyNum,size_t Buckets,size_t &FilledBuckets,size_t &Collitions,size_t &MaxCollition)
{
    FilledBuckets=0;
    Collitions=0;
    MaxCollition=0;
    size_t *pB=(size_t*)malloc(KeyNum*sizeof(size_t));
    if(!pB)
        return false;
    memset(pB,0,KeyNum*sizeof(size_t));
    for(size_t i=0;i<KeyNum;++i)
        ++pB[pFun(Key[i])%Buckets];
    for(size_t i=0;i<Buckets;++i)
    {
        if(pB[i])
        {
            ++FilledBuckets;
            if(pB[i]>=2)
            {
                ++Collitions;
                if(MaxCollition<pB[i])
                    MaxCollition=pB[i];
            }
        }
    }
    delete pB;
    return true;
}

//代码思路:根据键值计算所得哈希值找到对应桶，同时读锁定。若再桶中找到对应记录，那么写锁定记录，更新数据，更新完成解锁返回
//Summary:According to the calculated hash value of (Key), finds the corresponding bucket and read locked it
//at the same time. If the item associated with (Key) is found in the bucket, locks the data node and updates the data
template<class TK, class TV>
bool zHash<TK, TV>::Update(TK Key, TV *pValue)
{
	ENTRY *pT;
	begin();
	DATA_NODE<TK, TV>*pD = searchAndRLock(Key, pT);
    if (!pD)	//Key is not found
	{
		pT->lock.RUnlock();
		end();
		return false;
	}

    //Locks the data node and updates the data
	pD->slock.WLock();
	pD->value = *pValue;
    pD->slock.WUnlock();

    // The read lock of the bucket entry can be unlocked only after the updating is complete; otherwise, it may be deleted by other threads
    // The deleted data node may have incorrect data if it is immediately reallocated
    pT->lock.RUnlock();
	end();
	return true;
}

template<class TK, class TV>
bool zHash<TK, TV>::SetInitBuckets( size_t InitBuckets)
{
    zMemHeap<DATA_NODE<TK, TV>> *pNewHeap=0;
    zMemHeap<zBTreeNode<TK, TV>> * pNewBTNodeHeap=0;
    ENTRY *pNewBucket=0;

    size_t NewBuckets=roundUp(InitBuckets);
    size_t NewThreshHold=(size_t)((double)NewBuckets*LoadFactor);

	try {
		pNewHeap = new zMemHeap<DATA_NODE<TK, TV>>(NewThreshHold);
		
		pNewBTNodeHeap = new zMemHeap<zBTreeNode<TK, TV>>((NewThreshHold + KEY_MIN - 1) / KEY_MIN);
		
		size_t NewSizeCount = NewBuckets * sizeof(ENTRY);
		pNewBucket = (ENTRY*)malloc(NewSizeCount);
		if (!pNewBucket)
			throw std::bad_alloc();
	}
	catch (std::bad_alloc)
	{
		if (pNewBTNodeHeap)
			delete pNewBTNodeHeap;
		if (pNewBucket)
			delete pNewBucket;
		if (pNewBucket)
			delete pNewBucket;
		return false;
    }

    //Frees the old resources and setups new resources
	delete pHeap;
	delete pBTNodeHeap;
    free(pBucket);
    pHeap=pNewHeap;
    pBTNodeHeap=pNewBTNodeHeap;
    pBucket=pNewBucket;

    Buckets=NewBuckets;
    Threshold=NewThreshHold;
    PosMask = Buckets - 1;
	MaskBits = zBitCount(PosMask);
    return true;
}
}//NAME SPACE ZZG
#endif // !ZZG_HASH_H_2310
