namespace PortJeuno.Core.Ffxi;

/// <summary>
/// Wire-format constants for a LandSandBoat login server, grounded directly
/// in LandSandBoat/server's src/login/*.{h,cpp} (branch `base`, read
/// 2026-08-28 via the GitHub API - not guessed, not carried over from any
/// prior session). See FfxiAuthClient/FfxiRosterClient for citations of
/// which specific file backs each piece. NOT live-tested against a real
/// server from this codebase.
/// </summary>
public static class FfxiConstants
{
    /// <summary>auth_session - real TLS (SslStream), JSON request/response.</summary>
    public const int AuthPort = 54231;

    /// <summary>
    /// data_session - despite the server wrapping this socket in an
    /// asio::ssl::stream type, handler_session::start()/do_read()/do_write()
    /// all operate on socket_.next_layer() and never call async_handshake,
    /// so no TLS handshake ever happens here. Plain TCP.
    /// </summary>
    public const int DataPort = 54230;

    /// <summary>view_session - same plain-TCP-under-an-unused-SSL-type situation as data_session.</summary>
    public const int ViewPort = 54001;

    /// <summary>
    /// auth_session.cpp only checks major.minor (SupportedXiloaderVersion),
    /// ignoring patch - reject anything else client-side too so the server's
    /// LOGIN_ERROR_VERSION_UNSUPPORTED path isn't the first time this is caught.
    /// </summary>
    public static readonly byte[] SupportedXiloaderVersion = [2, 1, 0];

    /// <summary>
    /// Every packet_t-shaped message (login_packets.h) reserves a 16-byte
    /// `identifer` field at this offset. On the auth socket's JSON replies
    /// it's the `session_hash` array; on every data/view socket packet
    /// afterwards, the client must stamp those same 16 bytes here so
    /// loginHelpers::getHashFromPacket (login_helpers.cpp) can correlate the
    /// connection back to an authenticated session.
    /// </summary>
    public const int PacketIdentifierOffset = 12;
    public const int PacketIdentifierLength = 16;

    /// <summary>sizeof(packet_t): packet_size(4) + terminator(4) + command(4) + identifer(16).</summary>
    public const int PacketHeaderSize = 28;
}

/// <summary>auth_session.h's login_cmd, byte-for-byte.</summary>
public enum FfxiLoginCommand : byte
{
    Noop = 0x00,
    Attempt = 0x10,
    Create = 0x20,
    ChangePassword = 0x30,
    CreateTotp = 0x31,
    RemoveTotp = 0x32,
    RegenerateRecovery = 0x33,
    VerifyTotp = 0x34,
}

/// <summary>auth_session.h's login_result, byte-for-byte.</summary>
public enum FfxiLoginResult : byte
{
    Fail = 0x00,
    Success = 0x01,
    Error = 0x02,
    SuccessCreate = 0x03,
    ErrorCreateTaken = 0x04,
    RequestNewPassword = 0x05,
    SuccessChangePassword = 0x06,
    ErrorChangePassword = 0x07,
    ErrorCreateDisabled = 0x08,
    ErrorCreate = 0x09,
    ErrorAlreadyLoggedIn = 0x0A,
    ErrorVersionUnsupported = 0x0B,
    SuccessCreateTotp = 0x10,
    SuccessVerifyTotp = 0x11,
    SuccessRemoveTotp = 0x12,
    ErrorTrustTokenInvalid = 0x13,
}
