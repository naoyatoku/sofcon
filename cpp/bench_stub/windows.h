// Stub windows.h for benchmark builds (non-Windows symbols only)
#pragma once
typedef void* HMODULE;
typedef unsigned long DWORD;
typedef void* LPVOID;
typedef int BOOL;
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
// __declspec(dllexport) を無効化
#ifndef __declspec
#define __declspec(x)
#endif
