// Compiles a consumer against the *real* EDCB BonCtrl headers (when a checkout
// is configured) and asserts the ABI contract our libraries implement:
// 32-bit scalars, 16-bit BON16CHAR and the exact STRUCT_IBONDRIVER2 layout.
//
// Win32 scalar types are supplied here because the EDCB headers are normally
// consumed on Windows; the layout they define is what MSVC consumers see.
#include <cstddef>
#include <cstdint>

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef int BOOL;
typedef uint16_t WCHAR;
#ifndef WCHAR_MAX
#  define WCHAR_MAX 0xFFFF
#endif

#include "IBonDriver.h"
#include "IBonDriver2.h"

static_assert(sizeof(BYTE) == 1, "BYTE");
static_assert(sizeof(WORD) == 2, "WORD");
static_assert(sizeof(DWORD) == 4, "DWORD");
static_assert(sizeof(BOOL) == 4, "BOOL");
static_assert(sizeof(IBonDriver2::BON16CHAR) == 2, "BON16CHAR");
static_assert(sizeof(STRUCT_IBONDRIVER) == 12 * sizeof(void *), "STRUCT_IBONDRIVER size");
static_assert(sizeof(STRUCT_IBONDRIVER2) == 19 * sizeof(void *), "STRUCT_IBONDRIVER2 size");
static_assert(offsetof(STRUCT_IBONDRIVER2, pF10) == 12 * sizeof(void *), "pF10 slot");
static_assert(offsetof(STRUCT_IBONDRIVER2, pF16) == 18 * sizeof(void *), "pF16 slot");

int main()
{
	return 0;
}
