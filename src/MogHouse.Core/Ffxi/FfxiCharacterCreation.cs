using System.Buffers.Binary;
using System.Text;

namespace MogHouse.Core.Ffxi;

/// <summary>Where a new character starts. view_session validates 0 to 2.</summary>
public enum FfxiNation : byte
{
    SanDOria = 0,
    Bastok = 1,
    Windurst = 2,
}

/// <summary>
/// The six jobs a character can start as. Anything else is clamped by the
/// server rather than refused, so sending one is a silent mistake.
/// </summary>
public enum FfxiStartingJob : byte
{
    Warrior = 1,
    Monk = 2,
    WhiteMage = 3,
    BlackMage = 4,
    RedMage = 5,
    Thief = 6,
}

/// <summary>How big the character is. Small, medium, large.</summary>
public enum FfxiBodySize : byte
{
    Small = 0,
    Medium = 1,
    Large = 2,
}

/// <summary>
/// What a new character is: everything the lobby server stores about how they
/// look and where they begin.
///
/// Race carries gender - HumeMale and HumeFemale are separate races here, which
/// is why there is no gender of its own. Face is a face *and* a hair colour;
/// the game presents them as one choice because that is what they are.
/// </summary>
public sealed record FfxiNewCharacter(
    string Name,
    FfxiRaceId Race,
    byte Face,
    FfxiBodySize Size,
    FfxiStartingJob Job,
    FfxiNation Nation)
{
    /// <summary>The look string the renderer takes, for showing them before they exist.</summary>
    public string ToLookString() => FfxiAppearance.LookString((byte)Race, Face);
}

/// <summary>Races, as the lobby server numbers them. 1 to 8 or it refuses.</summary>
public enum FfxiRaceId : byte
{
    HumeMale = 1,
    HumeFemale = 2,
    ElvaanMale = 3,
    ElvaanFemale = 4,
    TarutaruMale = 5,
    TarutaruFemale = 6,
    Mithra = 7,
    Galka = 8,
}

/// <summary>
/// Making a character, which is two exchanges on the view socket rather than
/// one.
///
/// First 0x22 asks whether a name may be used. The server holds the answer -
/// `session.requestedNewCharacterName` - so the name is not sent again; then
/// 0x21 says what the character looks like and the server builds it from the
/// name it already has. Sending 0x21 without a successful 0x22 first creates a
/// character with no name.
///
/// A refused name comes back as one error code whatever the reason. Taken,
/// containing a banned word, containing a digit, too short - all
/// CHARACTER_NAME_UNAVAILABLE, with the actual reason written only to the
/// server's own log. A client cannot tell someone why, so it is worth checking
/// what can be checked here first.
/// </summary>
public static class FfxiCharacterCreation
{
    public const byte ViewOpcodeCheckName = 0x22;
    public const byte ViewOpcodeCreateCharacter = 0x21;

    /// <summary>The reply the server writes for a name it will allow.</summary>
    public const byte NameAcceptedResult = 0x03;

    private const int RequestSize = 0x30;
    private const int CreateRequestSize = 0x50;
    private const int OffsetName = 32;
    private const int OffsetRace = 48;
    private const int OffsetJob = 50;
    private const int OffsetNation = 54;
    private const int OffsetSize = 57;
    private const int OffsetFace = 60;

    /// <summary>
    /// What is wrong with a name, as far as can be known without asking, or
    /// null if nothing is. The server applies these same rules and several
    /// more it will not explain.
    /// </summary>
    public static string? WhyNameIsInvalid(string name)
    {
        if (name.Length < 3 || name.Length > 15)
        {
            return "A name is between 3 and 15 characters.";
        }

        foreach (char letter in name)
        {
            if (!char.IsLetter(letter))
            {
                return "A name is letters only - no spaces, digits or punctuation.";
            }
        }

        return null;
    }

    /// <summary>view_session.cpp case 0x22: the name at offset 32.</summary>
    public static byte[] BuildCheckNameRequest(string name, ReadOnlySpan<byte> sessionHash)
    {
        var packet = new byte[RequestSize];
        packet[8] = ViewOpcodeCheckName;
        sessionHash.CopyTo(packet.AsSpan(FfxiConstants.PacketIdentifierOffset, FfxiConstants.PacketIdentifierLength));

        Span<byte> field = packet.AsSpan(OffsetName, 15);
        Encoding.ASCII.GetBytes(name.AsSpan(0, Math.Min(name.Length, 15)), field);
        return packet;
    }

    /// <summary>
    /// view_session.cpp case 0x21, read by loginHelpers::createCharacter. The
    /// name is not here - the server kept it from the 0x22 that came first.
    /// </summary>
    public static byte[] BuildCreateRequest(FfxiNewCharacter character, ReadOnlySpan<byte> sessionHash)
    {
        var packet = new byte[CreateRequestSize];
        packet[8] = ViewOpcodeCreateCharacter;
        sessionHash.CopyTo(packet.AsSpan(FfxiConstants.PacketIdentifierOffset, FfxiConstants.PacketIdentifierLength));

        packet[OffsetRace] = (byte)character.Race;
        packet[OffsetJob] = (byte)character.Job;
        packet[OffsetNation] = (byte)character.Nation;
        packet[OffsetSize] = (byte)character.Size;
        packet[OffsetFace] = character.Face;
        return packet;
    }

    /// <summary>
    /// Whether a reply says the name may be used. The server answers "IXFF"
    /// with a result byte for yes, and an error packet for no.
    /// </summary>
    public static bool NameWasAccepted(ReadOnlySpan<byte> reply) =>
        reply.Length > 8 && reply[4] == (byte)'I' && reply[5] == (byte)'X' &&
        reply[6] == (byte)'F' && reply[7] == (byte)'F' && reply[8] == NameAcceptedResult;
}
