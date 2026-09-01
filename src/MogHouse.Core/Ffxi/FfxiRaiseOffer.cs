using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>Which menu the server is putting in front of a dead character.</summary>
public enum FfxiResurrectionKind : ushort
{
    /// <summary>The home point offer, which arrives the moment you die.</summary>
    HomePoint = 0,

    /// <summary>Somebody has cast Raise, or a Reraise has come due.</summary>
    Raise = 1,

    /// <summary>Somebody has cast Tractor. Not answered by this client yet.</summary>
    Tractor = 2,
}

/// <summary>
/// GP_SERV_COMMAND_RES (S2C 0x0F9) - the server adjusting the menu a dead
/// character is looking at.
///
/// A raise is not something a client can infer. Nothing about the corpse
/// changes: the hit points stay at zero, the entity update says the same thing
/// it said a second ago, and the only evidence that someone has cast Raise
/// over you is this packet. Without reading it the second button on the death
/// box could never light up, and a player would be left waiting for something
/// that had already happened.
///
/// It is the same packet the home point offer arrives on - the type says which
/// - so it also marks the point at which the server is willing to be told
/// where you want to go. LandSandBoat sends the Raise type from
/// CDeathState::Update, twelve seconds after the raise lands, and again from
/// sendRaise when somebody casts it.
/// </summary>
public sealed record FfxiRaiseOffer(uint UniqueNo, ushort ActIndex, FfxiResurrectionKind Kind)
{
    public const ushort PacketId = 0x0F9;

    private const int Body = 4; // id/size/sync sub-packet header
    private const int OffsetUniqueNo = Body + 0;
    private const int OffsetActIndex = Body + 4;
    private const int OffsetType = Body + 6;

    /// <summary>Parses a 0x0F9 sub-packet, or returns null if it is not one.</summary>
    public static FfxiRaiseOffer? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < OffsetType + 2)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        return new FfxiRaiseOffer(
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetUniqueNo, 4)),
            BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetActIndex, 2)),
            (FfxiResurrectionKind)BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetType, 2)));
    }
}
