// Minimal DLL wrapper around ooz's Kraken_Decompress so Python can call it via ctypes.
#include <cstddef>
typedef unsigned char byte;
int Kraken_Decompress(const byte *src, size_t src_len, byte *dst, size_t dst_len);
extern "C" __declspec(dllexport)
int OozDecompress(const unsigned char* src, long long src_len, unsigned char* dst, long long dst_len)
{
    return Kraken_Decompress(src, (size_t)src_len, dst, (size_t)dst_len);
}
