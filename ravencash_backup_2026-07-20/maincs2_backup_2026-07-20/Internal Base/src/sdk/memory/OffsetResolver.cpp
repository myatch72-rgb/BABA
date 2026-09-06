#include "OffsetResolver.h"
#include "Globals.h"

OffsetResolver::Result OffsetResolver::Resolve()
{
    auto inner = Memory::Globals::Initialize();
    return Result{ inner.total, inner.resolved, inner.failed };
}
