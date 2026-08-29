using System.Buffers.Binary;
using PortJeuno.Core.Ffxi;

namespace PortJeuno.Core.Tests.Ffxi;

/// <summary>
/// Covers the outbound encrypted path (compress -> size -> MD5 -> Blowfish)
/// against the inbound one. This is a self-consistency check by nature, so it
/// isn't the primary evidence the framing is right - that came from the live
/// server accepting these packets and keeping the character active. What these
/// tests are for is catching a regression in either direction later.
/// </summary>
public class FfxiZoneClientTests
{
    private static readonly uint[] SessionKey = [0, 0, 0xDC000000, 0xD93C6D06, 0x363F81AF];

    private static FfxiHuffman RequireCodec()
    {
        FfxiHuffmanTables? tables = FfxiHuffmanTables.TryLoadDefault();
        Skip.If(tables is null,
            "FFXI compression tables not installed - set PORTJEUNO_FFXI_RES. See FfxiHuffmanTables.");
        return new FfxiHuffman(tables!);
    }

    [SkippableFact]
    public void BuildEncryptedDatagram_RoundTripsThroughDecode()
    {
        FfxiHuffman codec = RequireCodec();
        var blowfish = FfxiBlowfish.FromSessionKey(SessionKey);
        byte[] position = FfxiPositionPacket.Build(sync: 3, x: -274.49f, vertical: -12.02f, depth: -90f, direction: -123);

        byte[] datagram = FfxiZoneClient.BuildEncryptedDatagram(
            position, ownCounter: 3, peerCounter: 1, timestamp: 0x66666666, codec, blowfish);

        FfxiZoneReply reply = FfxiZoneClient.Decode(datagram, FfxiBlowfish.FromSessionKey(SessionKey), codec);

        Assert.True(reply.ChecksumValid);
        Assert.Equal(3, reply.ServerCounter);            // our own counter, read back from the header
        Assert.Equal(1, reply.AcknowledgedClientCounter);
        Assert.NotNull(reply.Plaintext);
        Assert.Equal(position, reply.Plaintext!);
    }

    [SkippableFact]
    public void BuildEncryptedDatagram_CarriesHeaderCountersAndTimestamp()
    {
        FfxiHuffman codec = RequireCodec();
        var blowfish = FfxiBlowfish.FromSessionKey(SessionKey);

        byte[] datagram = FfxiZoneClient.BuildEncryptedDatagram(
            FfxiPositionPacket.Build(1, 0, 0, 0), ownCounter: 9, peerCounter: 4, timestamp: 0xDEADBEEF, codec, blowfish);

        Assert.Equal(9, BinaryPrimitives.ReadUInt16LittleEndian(datagram.AsSpan(0, 2)));
        Assert.Equal(4, BinaryPrimitives.ReadUInt16LittleEndian(datagram.AsSpan(2, 2)));
        Assert.Equal(0xDEADBEEF, BinaryPrimitives.ReadUInt32LittleEndian(datagram.AsSpan(8, 4)));
    }

    /// <summary>
    /// The body is encrypted from byte 28 on, so an untouched header plus a
    /// body that differs from the plaintext is what "actually encrypted"
    /// looks like. Guards against a refactor that quietly skips the cipher.
    /// </summary>
    [SkippableFact]
    public void BuildEncryptedDatagram_BodyIsActuallyEnciphered()
    {
        FfxiHuffman codec = RequireCodec();
        var blowfish = FfxiBlowfish.FromSessionKey(SessionKey);
        byte[] position = FfxiPositionPacket.Build(sync: 1, x: 1f, vertical: 2f, depth: 3f);

        byte[] datagram = FfxiZoneClient.BuildEncryptedDatagram(
            position, ownCounter: 1, peerCounter: 0, timestamp: 0, codec, blowfish);

        var compressed = new byte[512];
        int bits = codec.Compress(position, compressed);
        int compressedBytes = FfxiHuffman.CompressedByteLength(bits);

        Assert.NotEqual(
            Convert.ToHexString(compressed.AsSpan(0, compressedBytes)),
            Convert.ToHexString(datagram.AsSpan(FfxiZonePacket.HeaderSize, compressedBytes)));
    }

    [SkippableFact]
    public void Decode_TamperedBody_FailsChecksum()
    {
        FfxiHuffman codec = RequireCodec();
        var blowfish = FfxiBlowfish.FromSessionKey(SessionKey);

        byte[] datagram = FfxiZoneClient.BuildEncryptedDatagram(
            FfxiPositionPacket.Build(1, 0, 0, 0), ownCounter: 1, peerCounter: 0, timestamp: 0, codec, blowfish);
        datagram[FfxiZonePacket.HeaderSize + 2] ^= 0xFF;

        FfxiZoneReply reply = FfxiZoneClient.Decode(datagram, FfxiBlowfish.FromSessionKey(SessionKey), codec);

        Assert.False(reply.ChecksumValid);
        Assert.Null(reply.Plaintext);
    }

    [SkippableFact]
    public void Decode_WrongSessionKey_FailsChecksum()
    {
        FfxiHuffman codec = RequireCodec();

        byte[] datagram = FfxiZoneClient.BuildEncryptedDatagram(
            FfxiPositionPacket.Build(1, 0, 0, 0), ownCounter: 1, peerCounter: 0, timestamp: 0,
            codec, FfxiBlowfish.FromSessionKey(SessionKey));

        FfxiZoneReply reply = FfxiZoneClient.Decode(
            datagram, FfxiBlowfish.FromSessionKey([1, 2, 3, 4, 5]), codec);

        Assert.False(reply.ChecksumValid);
    }
}
