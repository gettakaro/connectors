#pragma once
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unistd.h>
typedef void* HMODULE; typedef void* HINSTANCE; typedef unsigned long DWORD;
struct SRWLOCK { std::mutex* m; };
#define SRWLOCK_INIT {new std::mutex}
inline void AcquireSRWLockExclusive(SRWLOCK* l){l->m->lock();}
inline void ReleaseSRWLockExclusive(SRWLOCK* l){l->m->unlock();}
inline unsigned long long GetTickCount64(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct SYSTEMTIME{int wYear,wMonth,wDay,wHour,wMinute,wSecond,wMilliseconds;};
inline void GetSystemTime(SYSTEMTIME*t){*t={2026,9,13,0,0,0,0};}
inline void GetLocalTime(SYSTEMTIME*t){GetSystemTime(t);}
inline DWORD GetCurrentThreadId(){return 1;}
#define MAX_PATH 260
inline DWORD GetModuleFileNameA(HMODULE,char*p,DWORD){strcpy(p,"srv\\dbghelp.dll");return 1;}
