// The SDK's driver-side provider bridge (ffxProviderExternal) is not part of the 2.4.0 source release: it only exists
// inside AMD's signed Windows DLLs. Linux drivers offer no FidelityFX providers, so this slot is never valid and
// ffx_provider.h always picks the providers compiled into ou_ffx (ffx_providers.cpp). Included by ffx_provider.h
// (local patch, README.md).
#pragma once

class ffxProviderExternal final : public ffxProvider
{
public:
    ffxProviderExternal(void*, uint64_t) : ffxProvider(0, 0, "none") {}
    bool Valid() const { return false; }
    bool CanProvide(uint64_t) const override { return false; }
    ffxReturnCode_t CreateContext(ffxContext*, ffxCreateContextDescHeader*, Allocator&) override { return FFX_API_RETURN_NO_PROVIDER; }
    ffxReturnCode_t DestroyContext(ffxContext*, Allocator&) override { return FFX_API_RETURN_NO_PROVIDER; }
    ffxReturnCode_t Configure(ffxContext*, const ffxConfigureDescHeader*) const override { return FFX_API_RETURN_NO_PROVIDER; }
    ffxReturnCode_t Query(ffxContext*, ffxQueryDescHeader*) const override { return FFX_API_RETURN_NO_PROVIDER; }
    ffxReturnCode_t Dispatch(ffxContext*, const ffxDispatchDescHeader*) const override { return FFX_API_RETURN_NO_PROVIDER; }
};
