ZZGLIB
An implementation of thread-safe lock-free hash table in C++. When I needed a thread-safe high-performance cross-platform hash table written in C++, but it's disappointing that I didn't find a suitable C++ implementation for such a basic and heavily used data structure. After studying some related documents,I found that C++ has been very well developed in memory model and synchronization.It's enough to write an implementation in platform-independent codes in standard C++,lock-free codes even! ZZGLIB does not rely on any special library.It's easy to integrate it into your project,just copy! The only requirement is for the C++ compiler version.To compile ZZGLIB,your compiler must support C++ 17 at least(I compiled it using gcc 9.5 and vs 2022,no problem).As you know,it's difficult to find all bugs in short time,especially for multithreading codes.Please tell me the problems found in using it, give me a chance to improve it. The email address for this project is: zzglib@hotmail.com.

Usage:

copy the following 7 fils to the directory of your project:

ZZG_Config.h //defines plateform-specific things.Actully there's almost nothing for the moment

ZZG_Bit.h //Some fast bits operation functions are defined

ZZG_Hash.h //Defines zHash, which is a thread-safe hash table

ZZG_Mem.h

ZZG_Mem.cpp //Defines a fast memory heap for size-fixed memory allocation

ZZG_Sync.h

ZZG_Sync.cpp //Defines some locks,including spin lock,version lock,read/write lock

main.cpp //Demo codes with Qt
