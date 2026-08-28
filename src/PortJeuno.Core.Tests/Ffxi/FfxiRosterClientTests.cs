using System.Buffers.Binary;
using System.Text;
using PortJeuno.Core.Ffxi;

namespace PortJeuno.Core.Tests.Ffxi;

public class FfxiRosterClientTests
{
    private const int HeaderSize = 28;
    private const int RecordSize = 140;

    // Mirrors login_packets.h's lpkt_chr_info_sub2/TC_OPERATION_MAKE layout
    // (see FfxiRosterClient's offset constants) so tests build a packet the
    // same way a real server would, independent of the class under test.
    private static byte[] BuildRecord(uint contentId, ushort charIdMain, string name, string worldName, ushort race, byte mainJob, byte mainJobLevel, byte subJob, byte zone, bool canRename, bool raceChange)
    {
        var record = new byte[RecordSize];
        BinaryPrimitives.WriteUInt32LittleEndian(record.AsSpan(0, 4), contentId);
        BinaryPrimitives.WriteUInt16LittleEndian(record.AsSpan(4, 2), charIdMain);
        record[10] = (byte)((canRename ? 1 : 0) | (raceChange ? 2 : 0));
        Encoding.ASCII.GetBytes(name).CopyTo(record, 12);
        Encoding.ASCII.GetBytes(worldName).CopyTo(record, 28);
        BinaryPrimitives.WriteUInt16LittleEndian(record.AsSpan(44, 2), race); // mon_no
        record[46] = mainJob;                                                 // mjob_no
        record[47] = subJob;                                                  // sjob_no
        record[44 + 28] = zone;                                               // zone_no
        record[44 + 29] = mainJobLevel;                                       // mjob_level
        return record;
    }

    private static byte[] BuildPacket(params byte[][] records)
    {
        var packet = new byte[HeaderSize + 4 + records.Sum(r => r.Length)];
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(HeaderSize, 4), (uint)records.Length);
        int offset = HeaderSize + 4;
        foreach (byte[] record in records)
        {
            record.CopyTo(packet, offset);
            offset += record.Length;
        }
        return packet;
    }

    [Fact]
    public void ParseCharacters_ZeroCount_ReturnsEmpty()
    {
        byte[] packet = BuildPacket();
        Assert.Empty(FfxiRosterClient.ParseCharacters(packet));
    }

    [Fact]
    public void ParseCharacters_SingleCharacter_DecodesAllFields()
    {
        byte[] record = BuildRecord(contentId: 12345, charIdMain: 42, name: "Tagban", worldName: "PortJeuno", race: 1, mainJob: 5, mainJobLevel: 75, subJob: 20, zone: 100, canRename: true, raceChange: false);
        byte[] packet = BuildPacket(record);

        var characters = FfxiRosterClient.ParseCharacters(packet);

        FfxiCharacter character = Assert.Single(characters);
        Assert.Equal(12345u, character.ContentId);
        Assert.Equal((ushort)42, character.CharIdMain);
        Assert.Equal("Tagban", character.Name);
        Assert.Equal("PortJeuno", character.WorldName);
        Assert.Equal((ushort)1, character.Race);
        Assert.Equal((byte)5, character.MainJob);
        Assert.Equal((byte)75, character.MainJobLevel);
        Assert.Equal((byte)20, character.SubJob);
        Assert.Equal((byte)100, character.Zone);
        Assert.True(character.CanRename);
        Assert.False(character.EligibleForRaceChange);
    }

    [Fact]
    public void ParseCharacters_MultipleCharacters_DecodesInOrder()
    {
        byte[] first = BuildRecord(1, 1, "First", "World", 0, 1, 1, 0, 0, false, false);
        byte[] second = BuildRecord(2, 2, "Second", "World", 0, 2, 2, 0, 0, false, true);
        byte[] packet = BuildPacket(first, second);

        var characters = FfxiRosterClient.ParseCharacters(packet);

        Assert.Equal(2, characters.Count);
        Assert.Equal("First", characters[0].Name);
        Assert.Equal("Second", characters[1].Name);
        Assert.True(characters[1].EligibleForRaceChange);
    }

    [Fact]
    public void ParseCharacters_CountExceedsActualData_StopsAtWhatFits()
    {
        byte[] record = BuildRecord(1, 1, "Only", "World", 0, 1, 1, 0, 0, false, false);
        byte[] packet = BuildPacket(record);
        // Lie about the count the way a truncated/corrupt read might.
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(HeaderSize, 4), 5);

        var characters = FfxiRosterClient.ParseCharacters(packet);

        Assert.Single(characters);
    }

    [Fact]
    public void BuildDataKeepAliveRequest_PlacesOpcodeAndHashAtServerExpectedOffsets()
    {
        byte[] hash = Enumerable.Range(0, 16).Select(i => (byte)(i + 2)).ToArray();
        byte[] request = FfxiRosterClient.BuildDataKeepAliveRequest(hash);

        Assert.Equal(0xFE, request[0]);
        Assert.Equal(hash, request.AsSpan(FfxiConstants.PacketIdentifierOffset, 16).ToArray());
    }

    [Fact]
    public void BuildDataCharacterListRequest_PlacesFieldsAtServerExpectedOffsets()
    {
        byte[] hash = Enumerable.Range(0, 16).Select(i => (byte)i).ToArray();
        byte[] request = FfxiRosterClient.BuildDataCharacterListRequest(accountId: 0xAABBCCDD, serverIp: 0x11223344, hash);

        Assert.Equal(0xA1, request[0]);
        Assert.Equal(0xAABBCCDDu, BinaryPrimitives.ReadUInt32LittleEndian(request.AsSpan(1, 4)));
        Assert.Equal(0x11223344u, BinaryPrimitives.ReadUInt32LittleEndian(request.AsSpan(5, 4)));
        Assert.Equal(hash, request.AsSpan(FfxiConstants.PacketIdentifierOffset, 16).ToArray());
    }

    [Fact]
    public void BuildViewAcquirePlayerDataRequest_PlacesOpcodeAndHashAtServerExpectedOffsets()
    {
        byte[] hash = Enumerable.Range(0, 16).Select(i => (byte)(i + 1)).ToArray();
        byte[] request = FfxiRosterClient.BuildViewAcquirePlayerDataRequest(hash);

        Assert.Equal(0x1F, request[8]);
        Assert.Equal(hash, request.AsSpan(FfxiConstants.PacketIdentifierOffset, 16).ToArray());
    }
}
