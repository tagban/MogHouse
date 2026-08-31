using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_CLI_COMMAND_EVENTEND (C2S 0x05B, 20 bytes) - "the cutscene is over".
///
/// A character the server considers to be in an event is not spawned for
/// anyone else. They are in the zone, they are in the logged-in list, a GM can
/// !goto them, they receive every other player's position - and nobody can see
/// them. That is exactly what MogHouse looked like from a second client, and
/// the server was saying so the whole time:
///
///     [map][warn] Invalid GP_CLI_COMMAND_REQLOGOUT packet from Quest:
///                 Invalid state: InEvent
///
/// The login reply carries the event the server has started, and the real
/// client answers it. This one never did, so `currentEvent-&gt;eventId` stayed
/// set from zone-in onward and `isInEvent()` never went false.
///
/// The server validates the mode and checks the event id against the one it is
/// running, so EventPara has to be the id from the login reply rather than
/// zero.
/// </summary>
public static class FfxiEventEndPacket
{
    public const ushort PacketId = 0x05B;
    public const int PacketSize = 20;

    /// <summary>Ends the event outright, rather than answering a pending one.</summary>
    public const ushort ModeEnd = 0;

    private const int OffsetIdAndSize = 0;
    private const int OffsetSync = 2;
    private const int OffsetUniqueNo = 4;
    private const int OffsetEndPara = 8;
    private const int OffsetActIndex = 12;
    private const int OffsetMode = 14;
    private const int OffsetEventNum = 16;
    private const int OffsetEventPara = 18;

    public static byte[] Build(uint uniqueNo, ushort actIndex, ushort eventNum, ushort eventPara, ushort sync)
    {
        var packet = new byte[PacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetIdAndSize, 2), FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetSync, 2), sync);

        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(OffsetUniqueNo, 4), uniqueNo);

        // EndPara is the option a player picked out of a menu. Nothing was
        // asked, so nothing was chosen.
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(OffsetEndPara, 4), 0);

        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetActIndex, 2), actIndex);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetMode, 2), ModeEnd);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetEventNum, 2), eventNum);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetEventPara, 2), eventPara);
        return packet;
    }
}
