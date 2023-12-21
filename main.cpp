
#include <QCoreApplication>
//using namespace std;
#include <ZZG_Hash.h>
#include <QString>
#include <iostream>
#include <QRandomGenerator64>
#include <string>
#include <QTime>

//Multiplies by 3/4,just considering the load factor of 0.75
#define LOOPS   1024*1024*3/4
#define SHIFT   5
int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    ZZG::zHash <QString, size_t> MyHash;
    QHash <QString, size_t> qHash;
    size_t Buckets, FilledBuckets, Elements, Collitions, MaxCollition;
    QString str;
  //  zHash.SetCountable(false);
  //  MyHash.SetInitBuckets(1024 * 1024); //Sets initial amount of buckets. The default is 256
    QTime T0,T1,T2,T3,T4;   //Check time points to evaluate performance
    uint64_t Value;
    size_t i=0;
    uint64_t *pRandNum=new uint64_t[LOOPS*3];
    //-------Creates random strings------------------
    T0 = QTime::currentTime();
    //Create random serials
    QRandomGenerator64::system()->fillRange(pRandNum,LOOPS*3);
    QRandomGenerator RandGen;
   
    int8_t* pc = (int8_t*)pRandNum;
    int8_t* pEnd = pc + LOOPS * 24;
    //Modifies control characters
    for (; pc < pEnd; ++pc)
    {
        if ((*pc<0x21)||(*pc>0x7e))
            *pc = RandGen.bounded(0x21,0x7f);
    }
    //Creates strings one by one
    QString *Key=new QString[LOOPS];
    for (i; i < LOOPS; ++i)
        Key[i]=QString::fromUtf8((char*)(pRandNum + 3 * i),16);
    //________________________________________________________
    T1 = QTime::currentTime();    
    for (i = 0; i < LOOPS; ++i)
        MyHash.Insert(Key[i], i);
    for (i = 0; i < LOOPS; ++i)
        MyHash.Value(Key[i], &Value);

    T2 = QTime::currentTime();
    for (i = 0; i < LOOPS; ++i)
        qHash.insert(Key[i], i);
    for (i = 0; i < LOOPS; ++i)
        Value = qHash.value(Key[i]);
    T3 = QTime::currentTime();
   
    str = QString(u8"zHash time:%1;QHash time:%2;Gen time:%3\n").arg(T1.msecsTo(T2)).arg(T2.msecsTo(T3)).arg(T0.msecsTo(T1));
    std::cout << str.toUtf8().constData();

    MyHash.CheckHash(Buckets, FilledBuckets, Elements, Collitions, MaxCollition);
    str = QString(u8"Buckets=%1,FilledBuckets=%2,Elements=%3,Collitions=%4,MaxCollition=%5\n")
        .arg(Buckets).arg(FilledBuckets).arg(Elements).arg(Collitions).arg(MaxCollition);
    std::cout << str.toUtf8().constData();

  //  return a.exec();
}
