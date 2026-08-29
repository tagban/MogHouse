using PortJeuno.Core.Ffxi;

namespace PortJeuno.Core.Tests.Ffxi;

/// <summary>
/// The expected datagram below was NOT produced by this C# code. It came from
/// compiling LandSandBoat/server's real `GP_CLI_LOGIN` struct and its real
/// md52.cpp with this environment's MSVC toolchain, building the same packet
/// through the C struct, and running the server's own four acceptance checks
/// (`recv_parse`) against it - all of which passed. So this is checking the
/// port against a byte-exact frame the real server would accept, not against
/// this port's own idea of the layout.
/// </summary>
public class FfxiZoneLoginPacketTests
{
    // Kept as one unbroken literal on purpose - splitting it across
    // concatenated lines is exactly how a transcription error slips in.
    private const string ReferenceDatagramHex = "010000000000000066666666000000000000000000000000000000000A2E010023000000000000000100000000000000000000000000000000000000000051756573747269610000000000000074616762616E00000000000000000000000000000000000000000000000000630000000200000002000000316CC9AB37FF756F7AC7F56A7EC56A86";

    private static byte[] BuildReferenceDatagram() =>
        FfxiZoneLoginPacket.BuildDatagram(
            uniqueNo: 1,
            characterName: "Questria",
            accountName: "tagban",
            ticket: default,
            clientVersion: 99,
            clientLanguage: 2,
            platform: 2,
            ownCounter: 1,
            sync: 1,
            timestamp: 0x66666666);

    [Fact]
    public void BuildDatagram_MatchesRealServerAcceptedReferenceBytes()
    {
        byte[] datagram = BuildReferenceDatagram();

        Assert.Equal(ReferenceDatagramHex.Replace(" ", ""), Convert.ToHexString(datagram));
    }

    [Fact]
    public void BuildDatagram_HasExpectedTotalSize()
    {
        byte[] datagram = BuildReferenceDatagram();

        // 28-byte header + 92-byte payload + 16-byte MD5.
        Assert.Equal(136, datagram.Length);
    }

    [Fact]
    public void BuildDatagram_TrailingHashMatchesPayload()
    {
        byte[] datagram = BuildReferenceDatagram();

        ReadOnlySpan<byte> payload = datagram.AsSpan(FfxiZonePacket.HeaderSize, FfxiZoneLoginPacket.PacketSize);
        ReadOnlySpan<byte> trailer = datagram.AsSpan(datagram.Length - FfxiZonePacket.ChecksumSize);

        Assert.Equal(System.Security.Cryptography.MD5.HashData(payload.ToArray()), trailer.ToArray());
    }

    /// <summary>
    /// The exact check `recv_parse` runs before it will accept an 0x00A -
    /// see the LoginPacketCheck loop over `offsetof(GP_CLI_LOGIN, unknown01)`.
    /// </summary>
    [Fact]
    public void BuildDatagram_LoginPacketCheckSatisfiesServerValidation()
    {
        byte[] datagram = BuildReferenceDatagram();
        ReadOnlySpan<byte> payload = datagram.AsSpan(FfxiZonePacket.HeaderSize, FfxiZoneLoginPacket.PacketSize);

        Assert.Equal(0x23, payload[4]);
        Assert.Equal(payload[4], FfxiZoneLoginPacket.ComputeLoginPacketCheck(payload));
    }

    [Fact]
    public void BuildDatagram_PacketIdAndSizeMatchWhatServerParses()
    {
        byte[] datagram = BuildReferenceDatagram();
        ushort word = BitConverter.ToUInt16(datagram, FfxiZonePacket.HeaderSize);

        // The real parser reads the id as `word & 0x1FF` and recovers the
        // size as `(byte1 & 0xFE) * 2`.
        Assert.Equal(0x00A, word & 0x1FF);
        Assert.Equal(92, (datagram[FfxiZonePacket.HeaderSize + 1] & 0xFE) * 2);
    }

    [Fact]
    public void BuildDatagram_RejectsSyncGreaterThanOwnCounter()
    {
        // The server's parse loop silently skips any sub-packet whose sync
        // exceeds the outer header counter - a packet built this way would
        // be accepted by the transport and then ignored, which is far worse
        // to debug than an exception here.
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            FfxiZoneLoginPacket.BuildDatagram(1, "Questria", "tagban", default, 99, 2, platform: 0, ownCounter: 1, sync: 2));
    }

    [Fact]
    public void BuildDatagram_RejectsZeroSync()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            FfxiZoneLoginPacket.BuildDatagram(1, "Questria", "tagban", default, 99, 2, platform: 0, ownCounter: 1, sync: 0));
    }
}

public class FfxiZonePacketTests
{
    [Theory]
    [InlineData(0x00A, 92, 0x2E0A)]
    [InlineData(0x000, 4, 0x0200)]
    [InlineData(0x1FF, 4, 0x03FF)]
    public void PackIdAndSize_MatchesRealCompilerBitfieldPacking(ushort id, int size, ushort expected)
    {
        // 0x2E0A is what MSVC actually laid `id=0x00A, size=23` out as -
        // read back from a compiled copy of the real struct, because
        // bitfield bit-ordering is implementation-defined and guessing it
        // wrong produces a packet the server reads as a different opcode.
        Assert.Equal(expected, FfxiZonePacket.PackIdAndSize(id, size));
    }

    [Fact]
    public void UnpackIdAndSize_RoundTrips()
    {
        Assert.Equal((0x00A, 92), FfxiZonePacket.UnpackIdAndSize(0x2E0A));
    }

    [Fact]
    public void PackIdAndSize_RejectsNonMultipleOfFour()
    {
        Assert.Throws<ArgumentException>(() => FfxiZonePacket.PackIdAndSize(0x00A, 90));
    }

    [Fact]
    public void PackIdAndSize_RejectsSizeBeyondSevenBits()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => FfxiZonePacket.PackIdAndSize(0x00A, 512));
    }
}
