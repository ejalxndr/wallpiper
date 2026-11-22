/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Ethan Alexander
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef WP_WIN32_TYPES_H
#define WP_WIN32_TYPES_H

#include <stdarg.h>
#include <stddef.h>

typedef unsigned char BYTE;
typedef unsigned char UCHAR;
typedef signed char CHAR8;
typedef unsigned short WORD;
typedef unsigned short USHORT;
typedef short SHORT;
typedef unsigned int DWORD;
typedef unsigned int UINT;
typedef unsigned int ULONG;
typedef int INT;
typedef int LONG;
typedef int BOOL;
typedef int WINBOOL;
typedef unsigned long long DWORD64;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef unsigned long long QWORD;

typedef unsigned char UINT8;
typedef signed char INT8;
typedef unsigned short UINT16;
typedef short INT16;
typedef unsigned int UINT32;
typedef int INT32;
typedef unsigned long long UINT64;
typedef long long INT64;

typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned int uint32_t;
typedef signed char int8_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned long long uintptr_t;

typedef char CHAR;
typedef unsigned short WCHAR;
typedef CHAR *LPSTR;
typedef const CHAR *LPCSTR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;

typedef void VOID;
typedef void *PVOID;
typedef void *LPVOID;
typedef const void *LPCVOID;

typedef unsigned long long SIZE_T;
typedef long long SSIZE_T;
typedef unsigned long long UINT_PTR;
typedef long long INT_PTR;
typedef unsigned long long ULONG_PTR;
typedef long long LONG_PTR;
typedef unsigned long long DWORD_PTR;

typedef void *HANDLE;
typedef HANDLE *PHANDLE;
typedef HANDLE HMODULE;
typedef HANDLE HINSTANCE;
typedef HANDLE HLOCAL;
typedef HANDLE HGLOBAL;
struct HWND__ {
  int unused;
};
typedef struct HWND__ *HWND;
struct HMENU__ {
  int unused;
};
typedef struct HMENU__ *HMENU;
struct HICON__ {
  int unused;
};
typedef struct HICON__ *HICON;
typedef HICON HCURSOR;
struct HBITMAP__ {
  int unused;
};
typedef struct HBITMAP__ *HBITMAP;
struct HBRUSH__ {
  int unused;
};
typedef struct HBRUSH__ *HBRUSH;
typedef HANDLE HMONITOR;
struct HDC__ {
  int unused;
};
typedef struct HDC__ *HDC;
struct HGDIOBJ__ {
  int unused;
};
typedef struct HGDIOBJ__ *HGDIOBJ;

typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef WORD ATOM;
typedef LONG HRESULT;
typedef DWORD COLORREF;

typedef int (*FARPROC)(void);
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef INT_PTR (*DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef INT_PTR (*WNDENUMPROC)(HWND, LPARAM);
typedef unsigned char BOOLEAN;
typedef void (*WAITORTIMERCALLBACK)(PVOID, BOOLEAN);

#define WINAPI
#define CALLBACK
#define APIENTRY
#define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))
#define WP_DLLEXPORT __attribute__((dllexport))

#define TRUE 1
#define FALSE 0
#ifndef NULL
#define NULL ((void *)0)
#endif
#define MAX_PATH 260
#define IS_INTRESOURCE(v) (((ULONG_PTR)(v) >> 16) == 0)

typedef struct _SECURITY_ATTRIBUTES *LPSECURITY_ATTRIBUTES;
typedef struct _OVERLAPPED *LPOVERLAPPED;
typedef struct _STARTUPINFOA *LPSTARTUPINFOA;
typedef struct _STARTUPINFOW *LPSTARTUPINFOW;
typedef DWORD *LPDWORD;
typedef DWORD *PDWORD;
typedef DWORD *PDWORD_PTR;
typedef LONG *PLONG;
typedef BOOL *LPBOOL;

typedef struct _GUID {
  DWORD Data1;
  WORD Data2;
  WORD Data3;
  BYTE Data4[8];
} GUID;

typedef struct _FILETIME {
  DWORD dwLowDateTime;
  DWORD dwHighDateTime;
} FILETIME, *LPFILETIME;

typedef struct _ULARGE_INTEGER {
  union {
    struct {
      DWORD LowPart;
      DWORD HighPart;
    };
    ULONGLONG QuadPart;
  };
} ULARGE_INTEGER;

#endif
