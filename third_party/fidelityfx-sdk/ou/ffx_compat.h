// Force-included into every ou_ffx source (CMakeLists.txt). Supplies what the SDK 2.4.0 sources expect from MSVC and
// from AMD's build tooling, so the upstream files stay unmodified except for the patches listed in README.md.
#pragma once

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#if !defined(_WIN32)
#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

inline int wcscpy_s(wchar_t* destination, std::size_t size, const wchar_t* source)
{
    if (!destination || !size) return EINVAL;
    const std::size_t length = source ? std::wcslen(source) : 0;
    if (!source || length >= size) { destination[0] = L'\0'; return source ? ERANGE : EINVAL; }
    std::wmemcpy(destination, source, length + 1);
    return 0;
}
template <std::size_t Size> int wcscpy_s(wchar_t (&destination)[Size], const wchar_t* source) { return wcscpy_s(destination, Size, source); }

inline int strcpy_s(char* destination, std::size_t size, const char* source)
{
    if (!destination || !size) return EINVAL;
    const std::size_t length = source ? std::strlen(source) : 0;
    if (!source || length >= size) { destination[0] = '\0'; return source ? ERANGE : EINVAL; }
    std::memcpy(destination, source, length + 1);
    return 0;
}
template <std::size_t Size> int strcpy_s(char (&destination)[Size], const char* source) { return strcpy_s(destination, Size, source); }

#ifndef _TRUNCATE
#define _TRUNCATE (static_cast<std::size_t>(-1))
#endif
// Only the _TRUNCATE form is used (ffx_provider.h): copy what fits, always terminate.
inline int strncpy_s(char* destination, std::size_t size, const char* source, std::size_t count)
{
    if (!destination || !size || !source) return EINVAL;
    std::size_t length = std::strlen(source);
    if (count != _TRUNCATE && count < length) length = count;
    if (length >= size) length = size - 1;
    std::memcpy(destination, source, length);
    destination[length] = '\0';
    return 0;
}
// Converts at most count bytes; *converted includes the terminator, as on MSVC.
inline int wcstombs_s(std::size_t* converted, char* destination, std::size_t size, const wchar_t* source, std::size_t count)
{
    if (converted) *converted = 0;
    if (!destination || !size || !source) return EINVAL;
    destination[0] = '\0';
    const std::size_t needed = std::wcstombs(nullptr, source, 0);
    if (needed == static_cast<std::size_t>(-1)) return EILSEQ;
    const std::size_t limit = needed < count ? needed : count;
    if (limit >= size) return ERANGE;
    const std::size_t written = std::wcstombs(destination, source, limit);
    if (written == static_cast<std::size_t>(-1)) { destination[0] = '\0'; return EILSEQ; }
    destination[written] = '\0';
    if (converted) *converted = written + 1;
    return 0;
}

inline int sprintf_s(char* buffer, std::size_t size, const char* format, ...)
{
    if (!buffer || !size) return -1;
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    if (length < 0 || static_cast<std::size_t>(length) >= size) { buffer[0] = '\0'; return -1; }
    return length;
}
template <std::size_t Size> int sprintf_s(char (&buffer)[Size], const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(buffer, Size, format, arguments);
    va_end(arguments);
    return length < 0 || static_cast<std::size_t>(length) >= Size ? (buffer[0] = '\0', -1) : length;
}
#endif

// Entry of the generated <pass>_permutations.h tables (tools/ffx_shaders.py); the field names are the ones the SDK's
// POPULATE_SHADER_BLOB_FFX (api/internal/ffx_internal_types.h) reads. AMD's FidelityFX_SC generated these headers.
struct OuFfxPermutationInfo {
    const uint8_t* blobData;
    uint32_t blobSize;
    const char* entryName;
    uint32_t numConstantBuffers, numSRVTextures, numUAVTextures, numSRVBuffers, numUAVBuffers, numSamplers,
        numRTAccelerationStructures;
#define OU_FFX_BINDINGS(prefix) const char** prefix##Names; const uint32_t* prefix##Bindings; const uint32_t* prefix##Counts; const uint32_t* prefix##Spaces;
    OU_FFX_BINDINGS(constantBuffer)
    OU_FFX_BINDINGS(srvTexture)
    OU_FFX_BINDINGS(uavTexture)
    OU_FFX_BINDINGS(srvBuffer)
    OU_FFX_BINDINGS(uavBuffer)
    OU_FFX_BINDINGS(sampler)
    OU_FFX_BINDINGS(rtAccelerationStructure)
#undef OU_FFX_BINDINGS
};

// ffx_api.h marks the API __declspec(dllexport); a static library on GCC/Clang needs nothing.
#if !defined(_MSC_VER) && !defined(__declspec)
#define __declspec(x)
#endif
// ffx_message.cpp's non-Windows branch uses FFX_UNUSED without including the header that defines it.
#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif
#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#endif
// ffx_vk.cpp's non-Windows UTF-8 conversion (std::wstring_convert) and its unqualified floor/log2.
#include <codecvt>
#include <locale>
#include <string>
#include <math.h>
// 2x the SDK's 128 Ki words: room for the 4-byte wchar_t names in the private contexts (patch 5).
#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 256)

#if !defined(_WIN32)
inline int swprintf_s(wchar_t* buffer, std::size_t size, const wchar_t* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vswprintf(buffer, size, format, arguments);
    va_end(arguments);
    return length;
}
template <std::size_t Size> int swprintf_s(wchar_t (&buffer)[Size], const wchar_t* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vswprintf(buffer, Size, format, arguments);
    va_end(arguments);
    return length;
}
#endif
