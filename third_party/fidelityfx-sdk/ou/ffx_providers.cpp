// Provider registry of the statically linked ffx-api (AMD builds one per effect DLL; that source is not released).
// Order = preference: FSR 3.1.6 upscaler, FSR 3.1.6 frame generation.
#include "../api/internal/ffx_provider.h"
#include "../upscalers/fsr3/include/ffx_provider_fsr3upscale.h"
#include "../framegeneration/fsr3/include/ffx_provider_fsr3framegeneration.h"

static ffxProvider* const kProviders[] = {&ffxProvider_FSR3Upscale::GetInstance(), &ffxProvider_Fsr3FrameGeneration::GetInstance()};

ffxProvider* GetProvider(ffxStructType_t descType, uint64_t overrideId, void* device, std::optional<ffxProviderExternal>& slot)
{
    return GetProvider(descType, overrideId, device, slot, kProviders);
}

uint64_t GetProviderVersions(ffxStructType_t descType, void* device, uint64_t capacity, uint64_t* versionIds, const char** versionNames,
                             std::optional<ffxProviderExternal>& slot)
{
    return GetProviderVersions(descType, device, capacity, versionIds, versionNames, slot, kProviders);
}
