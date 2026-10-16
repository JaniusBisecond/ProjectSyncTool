#include "core/Hasher.h"

#include <Windows.h>
#include <bcrypt.h>

#include <vector>

namespace
{
// 读取缓冲大小：1 MB，兼顾大文件吞吐与内存占用
constexpr size_t kChunkSize = 1u << 20;

std::wstring ToHex(const unsigned char* data, size_t size)
{
	static const wchar_t* kDigits = L"0123456789abcdef";

	std::wstring out;
	out.resize(size * 2);
	for (size_t i = 0; i < size; ++i)
	{
		out[i * 2] = kDigits[data[i] >> 4];
		out[i * 2 + 1] = kDigits[data[i] & 0x0F];
	}
	return out;
}
}	// namespace

namespace Hasher
{
bool Sha256File(const std::wstring& path, std::wstring& outHex)
{
	outHex.clear();

	// 共享读写：即使目标被 P4 / 编辑器以只读方式打开，也能算出哈希
	HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
	                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                            nullptr, OPEN_EXISTING,
	                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	std::vector<unsigned char> hashObject;
	std::vector<unsigned char> buffer(kChunkSize);
	bool ok = false;

	do
	{
		if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
		{
			break;
		}

		DWORD objectSize = 0;
		DWORD written = 0;
		if (::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
		                        reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &written, 0) < 0)
		{
			break;
		}

		hashObject.resize(objectSize);
		if (::BCryptCreateHash(algorithm, &hash, hashObject.data(), objectSize, nullptr, 0, 0) < 0)
		{
			break;
		}

		bool readOk = true;
		for (;;)
		{
			DWORD read = 0;
			if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
			{
				readOk = false;
				break;
			}
			if (read == 0)
			{
				break;
			}
			if (::BCryptHashData(hash, buffer.data(), read, 0) < 0)
			{
				readOk = false;
				break;
			}
		}
		if (!readOk)
		{
			break;
		}

		unsigned char digest[32] = {};
		if (::BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0)
		{
			break;
		}

		outHex = ToHex(digest, sizeof(digest));
		ok = true;
	} while (false);

	if (hash != nullptr)
	{
		::BCryptDestroyHash(hash);
	}
	if (algorithm != nullptr)
	{
		::BCryptCloseAlgorithmProvider(algorithm, 0);
	}
	::CloseHandle(file);

	return ok;
}
}	// namespace Hasher
