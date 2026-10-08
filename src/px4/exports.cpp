#include "bondriver/DriverCore.h"

using namespace bondriver;

BONDRIVER_API IBonDriver2 *CreateBonDriver() try
{
	return createDriverObject(BackendKind::Px4);
}
catch (...) { return nullptr; }

BONDRIVER_API const STRUCT_IBONDRIVER *CreateBonStruct() try
{
	return createDriverStruct(BackendKind::Px4);
}
catch (...) { return nullptr; }
