using System.Buffers.Binary;

namespace PortJeuno.Core.Ffxi;

/// <summary>Mode field of <see cref="FfxiLogoutPacket"/> - GP_CLI_COMMAND_REQLOGOUT_MODE.</summary>
public enum FfxiLogoutMode : ushort
{
    Toggle = 0x00,
    LogoutOn = 0x01,
    Off = 0x02,
    ShutdownOn = 0x03,
}

/// <summary>Kind field - GP_CLI_COMMAND_REQLOGOUT_KIND. Logout returns to character select; Shutdown exits entirely.</summary>
public enum FfxiLogoutKind : ushort
{
    Logout = 0x01,
    Shutdown = 0x03,
}

/// <summary>
/// GP_CLI_COMMAND_REQLOGOUT (C2S 0x0E7, 8 bytes) - asking to log out, the way
/// a real client does when you type /logout.
///
/// Worth having rather than just dropping the connection. Abandoning a session
/// leaves the server to reap it on a timeout roughly a minute later, and the
/// `accounts_sessions` row survives until then - which blocks the next login
/// for that character with error 201 and makes repeated testing slow. A clean
/// logout releases it immediately.
///
/// The server applies a `Leavegame` status effect rather than disconnecting at
/// once, so the usual countdown still applies; GM characters are exempt and
/// leave instantly.
///
/// Validation rejects this while the character is in an event, has an abnormal
/// status, is crafting, or is otherwise action-blocked - so a logout can
/// legitimately fail and the caller should not assume it succeeded.
/// </summary>
public static class FfxiLogoutPacket
{
    public const ushort PacketId = 0x0E7;
    public const int PacketSize = 8;

    private const int OffsetIdAndSize = 0;
    private const int OffsetSync = 2;
    private const int OffsetMode = 4;
    private const int OffsetKind = 6;

    public static byte[] Build(ushort sync, FfxiLogoutMode mode = FfxiLogoutMode.LogoutOn, FfxiLogoutKind kind = FfxiLogoutKind.Logout)
    {
        var packet = new byte[PacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetIdAndSize, 2), FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetSync, 2), sync);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetMode, 2), (ushort)mode);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetKind, 2), (ushort)kind);
        return packet;
    }
}
