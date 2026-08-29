using PortJeuno.Core.Ffxi;

namespace PortJeuno.Core.Tests.Ffxi;

/// <summary>
/// Reference values below were NOT computed by this C# port - they came
/// from compiling LandSandBoat/server's actual src/common/blowfish.cpp
/// standalone (MSVC, this environment's toolchain: cl.exe via vcvarsall)
/// and running it with the same key/inputs, so this test is checking the
/// port against real, independently-produced output, not just internal
/// consistency.
/// </summary>
public class FfxiBlowfishTests
{
    private static byte[] SequentialKey()
    {
        var key = new byte[16];
        for (int i = 0; i < 16; i++)
        {
            key[i] = (byte)i;
        }
        return key;
    }

    [Fact]
    public void Encipher_ZeroBlock_MatchesRealServerReference()
    {
        var bf = new FfxiBlowfish(SequentialKey());
        uint xl = 0, xr = 0;

        bf.Encipher(ref xl, ref xr);

        Assert.Equal(0x20972ED7u, xl);
        Assert.Equal(0x573B335Cu, xr);
    }

    [Fact]
    public void Encipher_NonZeroBlock_MatchesRealServerReference()
    {
        var bf = new FfxiBlowfish(SequentialKey());
        uint xl = 0x01234567, xr = 0x89ABCDEF;

        bf.Encipher(ref xl, ref xr);

        Assert.Equal(0x9E83B3ECu, xl);
        Assert.Equal(0xE564651Cu, xr);
    }

    [Fact]
    public void Decipher_UndoesEncipher_RoundTrips()
    {
        var bf = new FfxiBlowfish(SequentialKey());
        uint xl = 0x01234567, xr = 0x89ABCDEF;

        bf.Encipher(ref xl, ref xr);
        bf.Decipher(ref xl, ref xr);

        Assert.Equal(0x01234567u, xl);
        Assert.Equal(0x89ABCDEFu, xr);
    }

    [Fact]
    public void EncipherBlocks_FiveBlocks_MatchesRealServerReference()
    {
        var bf = new FfxiBlowfish(SequentialKey());
        var data = new uint[10];
        for (int i = 0; i < 10; i++)
        {
            data[i] = (uint)(0x10101010 * (i + 1));
        }

        bf.EncipherBlocks(data);

        uint[] expected =
        [
            0xD16A3FBC, 0xCA0C7E59, 0x9182A0F8, 0x5A57F5A0, 0x7273C83C,
            0x5D93DEE2, 0x52792382, 0x42A087C0, 0x6F4074E1, 0xCD6386C1,
        ];
        Assert.Equal(expected, data);
    }

    [Fact]
    public void DecipherBlocks_UndoesEncipherBlocks_RoundTrips()
    {
        var bf = new FfxiBlowfish(SequentialKey());
        var original = new uint[10];
        for (int i = 0; i < 10; i++)
        {
            original[i] = (uint)(0x10101010 * (i + 1));
        }
        var data = (uint[])original.Clone();

        bf.EncipherBlocks(data);
        bf.DecipherBlocks(data);

        Assert.Equal(original, data);
    }

    [Fact]
    public void FromSessionKey_ProducesUsableCipher()
    {
        // No independent reference for this specific path (MD5-derivation
        // is separate from the raw-key encipher tests above) - this just
        // confirms it doesn't throw and produces a working, self-consistent
        // round-trip, matching MapSession::initBlowfish()'s hash-truncation
        // behavior.
        var bf = FfxiBlowfish.FromSessionKey([1, 2, 3, 4, 5]);
        uint xl = 0x11111111, xr = 0x22222222;

        bf.Encipher(ref xl, ref xr);
        bf.Decipher(ref xl, ref xr);

        Assert.Equal(0x11111111u, xl);
        Assert.Equal(0x22222222u, xr);
    }
}
