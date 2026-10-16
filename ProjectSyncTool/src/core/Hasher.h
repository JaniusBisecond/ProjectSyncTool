#pragma once

#include <string>

namespace Hasher
{
// 计算文件 SHA256，输出小写十六进制字符串。
// 失败（文件被占用、不存在、无权限）返回 false，不抛异常。
bool Sha256File(const std::wstring& path, std::wstring& outHex);
}
