namespace PortJeuno.Core.Ffxi;

/// <summary>
/// The lpkt_next_login (S2C 0x0B) reply to selecting a character - tells the
/// client which zone server to actually connect to next (a separate,
/// Blowfish-encrypted protocol this project hasn't implemented yet) plus the
/// search-server address. Received once, right before the login server
/// closes the view socket - see FfxiRosterClient.SelectCharacterAsync.
/// </summary>
public sealed record FfxiZoneHandoff(
    uint ContentId,
    uint CharIdMain,
    string CharacterName,
    uint ServerId,
    uint ZoneServerIp,
    uint ZoneServerPort,
    uint SearchServerIp,
    uint SearchServerPort);
