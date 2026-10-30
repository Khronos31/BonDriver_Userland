#include "bondriver/DriverCore.h"

using namespace bondriver;

BONDRIVER_API IBonDriver2 *CreateBonDriver() try
{
	return createDriverObject();
}
catch (...) { return nullptr; }

BONDRIVER_API const STRUCT_IBONDRIVER *CreateBonStruct() try
{
	return createDriverStruct();
}
catch (...) { return nullptr; }
