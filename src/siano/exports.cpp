#include "bondriver/DriverCore.h"

using namespace bondriver;

BONDRIVER_API IBonDriver2 *CreateBonDriver()
{
	return createDriverObject(BackendKind::Siano);
}

BONDRIVER_API const STRUCT_IBONDRIVER *CreateBonStruct()
{
	return createDriverStruct(BackendKind::Siano);
}
