using System;
using System.Buffers.Binary;
using System.Numerics;

// SHA-256 (FIPS 180-4) for the byte-exact vertex-counter allowlist. Android's BoringSSL is not
// OpenSSL: System.Security.Cryptography.SHA256 roots the NativeAOT OpenSSL loader on linux-bionic.
// Keep all hashing here managed; callers supply storage and no native crypto provider is loaded.
static class ManagedSha256
{
    static ReadOnlySpan<uint> Constants =>
    [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ];

    internal static void HashData(ReadOnlySpan<byte> data, Span<byte> digest)
    {
        if (digest.Length < 32) throw new ArgumentException("SHA-256 needs a 32-byte destination", nameof(digest));
        Span<uint> state = stackalloc uint[] { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
        Span<uint> words = stackalloc uint[64];
        int offset = 0;
        while (data.Length - offset >= 64) { Compress(data.Slice(offset, 64), state, words); offset += 64; }
        Span<byte> tail = stackalloc byte[128]; tail.Clear();
        data[offset..].CopyTo(tail); tail[data.Length - offset] = 0x80;
        int padded = data.Length - offset < 56 ? 64 : 128;
        BinaryPrimitives.WriteUInt64BigEndian(tail.Slice(padded - 8, 8), (ulong)data.Length * 8);
        Compress(tail[..64], state, words);
        if (padded == 128) Compress(tail[64..], state, words);
        for (int i = 0; i < 8; i++) BinaryPrimitives.WriteUInt32BigEndian(digest.Slice(i * 4, 4), state[i]);
    }

    static void Compress(ReadOnlySpan<byte> block, Span<uint> state, Span<uint> words)
    {
        unchecked
        {
            for (int i = 0; i < 16; i++) words[i] = BinaryPrimitives.ReadUInt32BigEndian(block.Slice(i * 4, 4));
            for (int i = 16; i < 64; i++)
            {
                uint x = words[i - 15], y = words[i - 2];
                uint s0 = BitOperations.RotateRight(x, 7) ^ BitOperations.RotateRight(x, 18) ^ (x >> 3);
                uint s1 = BitOperations.RotateRight(y, 17) ^ BitOperations.RotateRight(y, 19) ^ (y >> 10);
                words[i] = words[i - 16] + s0 + words[i - 7] + s1;
            }
            uint a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
            ReadOnlySpan<uint> k = Constants;
            for (int i = 0; i < 64; i++)
            {
                uint s1 = BitOperations.RotateRight(e, 6) ^ BitOperations.RotateRight(e, 11) ^ BitOperations.RotateRight(e, 25);
                uint t1 = h + s1 + ((e & f) ^ (~e & g)) + k[i] + words[i];
                uint s0 = BitOperations.RotateRight(a, 2) ^ BitOperations.RotateRight(a, 13) ^ BitOperations.RotateRight(a, 22);
                uint t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
                h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
            }
            state[0] += a; state[1] += b; state[2] += c; state[3] += d;
            state[4] += e; state[5] += f; state[6] += g; state[7] += h;
        }
    }
}
