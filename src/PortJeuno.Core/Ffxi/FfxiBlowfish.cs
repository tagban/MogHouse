using System.Buffers.Binary;

namespace PortJeuno.Core.Ffxi;

/// <summary>
/// LandSandBoat's Blowfish variant, used to encrypt the zone/map connection
/// (port 54230 on `xi_map`, UDP - a completely different transport and
/// cipher than the TLS+JSON auth or plain-TCP data/view sockets). Ported
/// directly from LandSandBoat/server's src/common/blowfish.cpp (branch
/// `base`, 2026-08-28) - this is NOT textbook Blowfish: the F-function
/// (<see cref="F"/>) masks two of its four S-box lookups down to their low
/// bit and XORs with 0x20 before adding, unlike standard Blowfish which uses
/// the raw 32-bit S-box values directly. Confirmed by reading the real
/// source, not assumed - a generic/off-the-shelf Blowfish implementation
/// would silently produce wrong ciphertext against a real server.
///
/// Verified against a real reference: compiled LandSandBoat's own
/// blowfish.cpp standalone (MSVC, this environment's toolchain) and ran it
/// with a known key/plaintext to get real ciphertext to test this port
/// against - see FfxiBlowfishTests. Key schedule, single-block
/// encipher/decipher, and the block-loop variants are all covered.
/// </summary>
public sealed class FfxiBlowfish
{
    private readonly uint[] _p = new uint[18];
    private readonly uint[] _s = new uint[4 * 256];

    /// <summary>
    /// Matches MapSession::initBlowfish() (map_session.cpp): MD5-hash the
    /// 5-word session key to get a 16-byte Blowfish key. If the hash contains
    /// a zero byte, everything from that byte to the end of the 16-byte hash
    /// is zeroed out (`memset(hash + i, 0, 16 - i)`) - the key stays a full
    /// 16 bytes, it's not shortened. This matters: <see cref="Init"/> XORs
    /// the key into P cyclically using its length as the modulus, so
    /// treating this as "shrink the key to i bytes" instead of "zero-pad to
    /// 16" changes the cycling pattern whenever 16 isn't a multiple of i,
    /// producing a different (wrong) key schedule. Confirmed against a real
    /// compiled reference: session key {132,0,0,0,0} hashes to a zero at
    /// byte 9, and the zero-padded (not truncated) key is what
    /// FfxiBlowfishTests.FromSessionKey_ZeroByteInHash_MatchesRealServerReference
    /// checks against.
    /// </summary>
    public static FfxiBlowfish FromSessionKey(ReadOnlySpan<uint> sessionKey)
    {
        if (sessionKey.Length != 5)
        {
            throw new ArgumentException("Session key must be 5 uint32 words.", nameof(sessionKey));
        }

        Span<byte> keyBytes = stackalloc byte[20];
        for (int i = 0; i < 5; i++)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(keyBytes.Slice(i * 4, 4), sessionKey[i]);
        }

        Span<byte> hash = stackalloc byte[16];
        System.Security.Cryptography.MD5.HashData(keyBytes, hash);

        int zeroAt = hash.IndexOf((byte)0);
        if (zeroAt >= 0)
        {
            hash[zeroAt..].Clear();
        }

        return new FfxiBlowfish(hash);
    }

    public FfxiBlowfish(ReadOnlySpan<byte> key)
    {
        Init(key);
    }

    /// <summary>
    /// The modified F-function - see the class remarks. Byte 0 (bits 0-7)
    /// and byte 2 (bits 16-23) use their full S-box word; byte 1 (bits
    /// 8-15) and byte 3 (bits 24-31) are masked to their low bit and XORed
    /// with 0x20 before being added in.
    /// </summary>
    private uint F(uint working)
    {
        uint s1 = (_s[256 + (int)((working >> 8) & 0xff)] & 1) ^ 32;
        uint s3 = (_s[768 + (int)(working >> 24)] & 1) ^ 32;
        uint s2 = _s[512 + (int)((working >> 16) & 0xff)];
        uint s0 = _s[working & 0xff];
        return s1 + s3 + s2 + s0;
    }

    public void Encipher(ref uint xl, ref uint xr)
    {
        uint left = xl;
        uint right = xr;

        for (int i = 0; i < 16; i++)
        {
            left ^= _p[i];
            right = F(left) ^ right;
            (left, right) = (right, left);
        }

        (left, right) = (right, left);

        right ^= _p[16];
        left ^= _p[17];

        xl = left;
        xr = right;
    }

    public void Decipher(ref uint xl, ref uint xr)
    {
        uint left = xl;
        uint right = xr;

        for (int i = 17; i > 1; i--)
        {
            left ^= _p[i];
            right = F(left) ^ right;
            (left, right) = (right, left);
        }

        (left, right) = (right, left);

        right ^= _p[1];
        left ^= _p[0];

        xl = left;
        xr = right;
    }

    /// <summary>ECB - each consecutive 64-bit (two uint32) block is enciphered independently, matching blowfish_encipher_blocks.</summary>
    public void EncipherBlocks(Span<uint> data)
    {
        for (int i = 0; i + 1 < data.Length; i += 2)
        {
            uint l = data[i];
            uint r = data[i + 1];
            Encipher(ref l, ref r);
            data[i] = l;
            data[i + 1] = r;
        }
    }

    public void DecipherBlocks(Span<uint> data)
    {
        for (int i = 0; i + 1 < data.Length; i += 2)
        {
            uint l = data[i];
            uint r = data[i + 1];
            Decipher(ref l, ref r);
            data[i] = l;
            data[i + 1] = r;
        }
    }

    private void Init(ReadOnlySpan<byte> key)
    {
        if (key.Length == 0)
        {
            throw new ArgumentException("Key must not be empty.", nameof(key));
        }

        byte[] subkey = FfxiBlowfishSubkey.Bytes;
        for (int i = 0; i < 18; i++)
        {
            _p[i] = BinaryPrimitives.ReadUInt32LittleEndian(subkey.AsSpan(i * 4, 4));
        }
        for (int i = 0; i < 1024; i++)
        {
            _s[i] = BinaryPrimitives.ReadUInt32LittleEndian(subkey.AsSpan(72 + i * 4, 4));
        }

        // XOR the key cyclically into P, four big-endian bytes at a time per
        // word - matches blowfish_init's `data = (data << 8) | key[j]` loop
        // exactly (this is big-endian despite everything else in this
        // protocol being little-endian - confirmed directly from source,
        // not assumed).
        //
        // The real signature is `blowfish_init(const int8 key[], ...)` -
        // key[j] is a SIGNED char. `data << 8 | key[j]` sign-extends any
        // byte >= 0x80 to a negative int before it's converted to uint32_t
        // for the OR, which clobbers the upper 3 bytes of `data` with 1s
        // instead of leaving them alone. This looks like a bug in the real
        // server, but it's what the real server does, so it must be
        // replicated exactly - confirmed against a real compiled reference
        // using a key with bytes >= 0x80 (see
        // FfxiBlowfishTests.FromSessionKey_ZeroByteInHash_MatchesRealServerReference,
        // which silently produced wrong output without this until the real
        // reference value caught it).
        int j = 0;
        for (int i = 0; i < 18; i++)
        {
            uint data = 0;
            for (int k = 0; k < 4; k++)
            {
                uint signExtended = unchecked((uint)(int)(sbyte)key[j]);
                data = (data << 8) | signExtended;
                j++;
                if (j >= key.Length)
                {
                    j = 0;
                }
            }
            _p[i] ^= data;
        }

        uint left = 0;
        uint right = 0;
        for (int i = 0; i < 18; i += 2)
        {
            Encipher(ref left, ref right);
            _p[i] = left;
            _p[i + 1] = right;
        }

        for (int i = 0; i < 4; i++)
        {
            for (int k = 0; k < 256; k += 2)
            {
                Encipher(ref left, ref right);
                _s[i * 256 + k] = left;
                _s[i * 256 + k + 1] = right;
            }
        }
    }
}
