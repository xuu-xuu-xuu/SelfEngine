#include "Hash.h"
#include <Windows.h>
#include <bcrypt.h>
#include <limits>
namespace se
{
std::string Sha256(std::span<const std::uint8_t> bytes)
{
    if(bytes.size()>(std::numeric_limits<ULONG>::max)())return {};
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    unsigned char digest[32]{};
    const auto result=BCryptHash(algorithm,nullptr,0,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),digest,sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm,0);if(result<0)return {};
    constexpr char digits[]="0123456789abcdef";std::string text;text.reserve(64);
    for(const auto byte:digest){text+=digits[byte>>4];text+=digits[byte&15];}return text;
}
}
