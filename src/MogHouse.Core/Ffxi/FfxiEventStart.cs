using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_SERV_COMMAND_EVENT (S2C 0x032) - "a cutscene is starting".
///
/// The client is expected to play it and say when it is done. Until it does,
/// the character is InEvent, and a character in an event is never spawned for
/// anyone else - present, addressable, and invisible to every other player.
///
/// The field names are a trap worth documenting, because they cost a long
/// detour. From the server's own builder:
///
///     packet.EventNum  = the zone id
///     packet.EventPara = the event id
///
/// So EventNum is not the event. A login reply that appeared to carry a
/// nonsense event number of 235 was carrying the zone id in exactly the field
/// named for the event, and reading it as an id produced
/// "Event ID mismatch 0 != 152" from the server.
///
/// UniqueNo and ActIndex belong to whatever the event is about - usually an
/// NPC, not the player - and are echoed back unchanged when ending it.
/// </summary>
public sealed record FfxiEventStart(
    uint UniqueNo,
    ushort ActIndex,
    ushort ZoneNo,
    ushort EventId,
    ushort Mode)
{
    public const ushort PacketId = 0x032;

    private const int Body = 4; // id/size/sync sub-packet header
    private const int OffsetUniqueNo = Body + 0;
    private const int OffsetActIndex = Body + 4;
    private const int OffsetEventNum = Body + 6;  // the zone, despite the name
    private const int OffsetEventPara = Body + 8; // the event
    private const int OffsetMode = Body + 10;

    /// <summary>Reads one, or null if the sub-packet is too short to hold it.</summary>
    public static FfxiEventStart? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < OffsetMode + 2)
        {
            return null;
        }

        // Which packet this is, before reading it as one.
        //
        // Without this every sub-packet long enough to reach Mode parses as an
        // event, and the numbers that come out are whatever happened to be at
        // those offsets - 11348, 45321, 13758 for a character whose actual
        // event was 305. The server answers each with "Event ID mismatch" and
        // the real event is never ended, so a new character stays in its
        // opening cutscene forever.
        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        return new FfxiEventStart(
            UniqueNo: BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetUniqueNo, 4)),
            ActIndex: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetActIndex, 2)),
            ZoneNo: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetEventNum, 2)),
            EventId: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetEventPara, 2)),
            Mode: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetMode, 2)));
    }
}
