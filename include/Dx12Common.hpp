#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN


#ifndef DX_12_COMMON_HPP
#define DX_12_COMMON_HPP

#include <Windows.h>

#include <wrl.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

#include <stdexcept>
#include <string>
#include <comdef.h>

inline std::string WideToSystemString(const std::wstring& value) {
	if (value.empty()) {
		return {};
	}

	const int requiredSize = WideCharToMultiByte(
		CP_ACP,
		0,
		value.c_str(),
		-1,
		nullptr,
		0,
		nullptr,
		nullptr);
	if (requiredSize <= 1) {
		return {};
	}

	std::string result(static_cast<size_t>(requiredSize - 1), '\0');
	WideCharToMultiByte(
		CP_ACP,
		0,
		value.c_str(),
		-1,
		result.data(),
		requiredSize,
		nullptr,
		nullptr);
	return result;
}

inline void ThrowIfFailed(HRESULT hr) {
	if (FAILED(hr)) {
		_com_error err(hr);
		std::wstring msg = err.ErrorMessage();
		throw std::runtime_error(WideToSystemString(msg));
	}
}

static void DxTrace(const wchar_t* s)
{
#if defined(_DEBUG)
	OutputDebugStringW(s);
	OutputDebugStringW(L"\n");
#endif
}

inline ComPtr<ID3DBlob> CompileShader(
    const std::wstring& filename,
    const D3D_SHADER_MACRO* defines,
    const std::string& entrypoint,
    const std::string& target)
{
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> byteCode;
    ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompileFromFile(
        filename.c_str(),
        defines,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entrypoint.c_str(),
        target.c_str(),
        compileFlags,
        0,
        &byteCode,
        &errors
    );

    if (errors)
    {
        const char* msg = (const char*)errors->GetBufferPointer();
        OutputDebugStringA(msg);
        if (FAILED(hr))
        {
            MessageBoxA(nullptr, msg, "HLSL Compile Error", MB_OK | MB_ICONERROR);
        }
    }

    if (FAILED(hr))
    {
        ThrowIfFailed(hr);
    }


    return byteCode;
}

inline UINT CalcConstantBufferByteSize(UINT byteSize)
{
    // CBV size must be aligned to 256 bytes.
    return (byteSize + 255) & ~255;
}

#endif // !DX_12_COMMON_HPP

