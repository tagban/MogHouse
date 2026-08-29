namespace PortJeuno.Core.Ffxi;

/// <summary>
/// The lpkt_next_login (S2C 0x0B) reply to selecting a character - tells the
/// client which zone server to actually connect to next plus the search-server
/// address. Received once, right before the login server closes the view
/// socket - see FfxiRosterClient.SelectCharacterAsync.
/// </summary>
/// <param name="SessionKey">
/// The 5-word Blowfish session key for the zone connection. This does NOT
/// come from the handoff packet - the *client* originates it, in the `key3`
/// field of the 0xA2 request it sent moments earlier, and the login server
/// simply stores it in `accounts_sessions.session_key` for the map server to
/// read back later. So it's reconstructed here from what was sent rather than
/// parsed from what was received. See FfxiRosterClient.DeriveSessionKey.
/// </param>
public sealed record FfxiZoneHandoff(
    uint ContentId,
    uint CharIdMain,
    string CharacterName,
    uint ServerId,
    uint ZoneServerIp,
    uint ZoneServerPort,
    uint SearchServerIp,
    uint SearchServerPort,
    uint[] SessionKey);

/// <summary>
/// The login server answered with an error packet (command 0x24) instead of
/// the thing we asked for. Carries the server's own numeric code so callers
/// can react to specific, expected conditions - a stale session row, say -
/// rather than pattern-matching on a message.
/// </summary>
public sealed class FfxiLoginErrorException : Exception
{
    public uint Code { get; }

    public FfxiLoginErrorException(uint code, string description)
        : base($"Login server returned error {code}: {description}")
    {
        Code = code;
    }
}
