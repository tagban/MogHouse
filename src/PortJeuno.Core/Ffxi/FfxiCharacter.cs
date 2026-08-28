namespace PortJeuno.Core.Ffxi;

/// <summary>
/// One character slot decoded from a LandSandBoat lpkt_chr_info2 packet
/// (login_packets.h's lpkt_chr_info_sub2 / TC_OPERATION_MAKE). Only the
/// fields useful for a login/roster screen are exposed here - the full
/// struct carries far more (fame, gold, chat/party counters, etc.) that a
/// client that only lists characters and logs in doesn't need.
/// </summary>
public sealed record FfxiCharacter(
    uint ContentId,
    ushort CharIdMain,
    string Name,
    string WorldName,
    ushort Race,
    byte MainJob,
    byte MainJobLevel,
    byte SubJob,
    byte Zone,
    bool CanRename,
    bool EligibleForRaceChange);
