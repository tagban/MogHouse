using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// The identifying head of an entity update - GP_SERV_COMMAND_CHAR_PC (0x00D,
/// another player) and GP_SERV_COMMAND_CHAR_NPC (0x00E, an NPC or mob) both
/// open with the same GP_SERV_POS_HEAD block that GP_SERV_COMMAND_LOGIN uses,
/// so the fields that say *who* an update is about are read the same way for
/// all three.
///
/// This exists to make "does client A know about client B?" a thing code can
/// assert rather than something a person has to eyeball. Reading these by hand
/// out of hex is exactly how I got it wrong twice: an 0x00D in a login reply
/// looks like proof another player is visible, but its UniqueNo is usually the
/// receiving character's own id - the server describes you to yourself first.
/// Compare UniqueNo against your own before concluding anything.
///
/// Only the head is parsed. The rest of an 0x00D (appearance, equipment, name)
/// is a large structure with no compiled reference read yet, and guessing at
/// it would produce fields that look authoritative while being wrong.
/// </summary>
/// <param name="Name">
/// Present only for player updates long enough to carry it - NPC updates use a
/// different layout past the shared head, so this is never read for them.
/// </param>
/// <param name="ModelSize">
/// `Flags1.GraphSize`, the model scale the server sends from `char_look.size`.
/// Surfaced because a zero here is worth noticing: that column defaults to 0
/// and isn't populated by the character-insert trigger, and a zero-scale
/// character is present and targetable while plausibly drawing as nothing.
/// </param>
/// <summary>What an entity is, for anything that has to tell them apart.</summary>
public enum FfxiEntityKind
{
    /// <summary>Another player.</summary>
    Player,

    /// <summary>A friendly NPC - a shopkeeper, a guard, a moogle.</summary>
    Npc,

    /// <summary>Something that fights back.</summary>
    Enemy,
}

public sealed record FfxiEntityUpdate(
    ushort PacketId,
    uint UniqueNo,
    ushort ActIndex,
    sbyte Direction,
    float X,
    float Vertical,
    float Depth,
    uint EntityFlags = 0,
    string? Name = null,
    byte? ModelSize = null,
    uint? RawFlags1 = null,
    uint? RawFlags0 = null,
    byte? Allegiance = null,
    byte? HealthPercent = null,
    byte? BattleFlags = null,
    byte? RenderFlags = null,
    FfxiEntityLook? Look = null,
    byte? NameVis = null,
    byte SendFlags = 0)
{
    public const ushort PlayerPacketId = 0x00D;
    public const ushort NpcPacketId = 0x00E;

    /// <summary>
    /// The server says this one is not to be seen.
    ///
    /// Plenty of entities exist to be targeted rather than looked at - an
    /// auction counter, a ??? on the ground - and the game draws none of them.
    /// Drawing them anyway fills a room with bodies that are not there.
    ///
    /// Two bits, both in the second flags word: hide at 1, invisible at 29.
    /// </summary>
    /// <summary>
    /// The sender's GM level, 0 for an ordinary player.
    ///
    /// Bits 24 to 26 of the second flags word. Confirmed by toggling the flag
    /// on a live character and watching the word: 0x05008400 with it on,
    /// 0x00008400 with it off, everything else unchanged.
    /// </summary>
    public int GmLevel => RawFlags1 is uint flags ? (int)((flags >> 24) & 7) : 0;

    public bool IsHidden =>
        RawFlags1 is uint flags && (((flags >> 1) & 1) != 0 || ((flags >> 29) & 1) != 0);

    /// <summary>
    /// Whether this is a player, a friendly NPC or something hostile.
    ///
    /// Three plausible discriminators are wrong, each measured against live
    /// packets from a zone whose contents the server data already states:
    ///
    /// - The target index. The widely repeated ranges have it backwards -
    ///   zone_entities.cpp sends targid below 0x400 to the mob *and* NPC
    ///   lists - so it cannot separate a shopkeeper from a crab at all.
    /// - Allegiance. Both packet branches write it, but a plain NPC defaults
    ///   to Mob just as a crab does. It separates factions, not kinds. This
    ///   classifier used it and called six Zeruhn Mines NPCs enemies.
    /// - Status, at 0x20. It is Normal, Update or Disappear - transient
    ///   state, and a mob standing still reads the same as an NPC.
    ///
    /// What does work is <see cref="BattleFlags"/>, which only the mob branch
    /// of the builder ever writes.
    /// </summary>
    public FfxiEntityKind Kind =>
        PacketId == PlayerPacketId ? FfxiEntityKind.Player
        : IsLivingMob ? FfxiEntityKind.Enemy
        : FfxiEntityKind.Npc;

    /// <summary>
    /// The bit the server sets for a mob that is alive, and never for an NPC:
    /// `ref&lt;uint8&gt;(0x25) = PMob-&gt;health.hp &gt; 0 ? 0x08 : 0`.
    ///
    /// Note what it literally means - a mob that is *dead* clears it and reads
    /// as an NPC. FfxiEntityTracker compensates by remembering: once an entity
    /// has shown this bit it stays an enemy for the rest of the zone.
    /// </summary>
    /// <remarks>
    /// Pattern-matched rather than written as (BattleFlags &amp; MobAliveFlag) != 0.
    /// On a nullable byte that reads naturally and is wrong: the lifted !=
    /// returns true when the operand is null, so every update too short to
    /// carry the byte would report a mob.
    /// </remarks>
    /// <remarks>
    /// Any non-zero value, not the 0x08 bit alone.
    ///
    /// entity_update.cpp assigns `0x08` there and nothing else writes the
    /// byte, so 0x08 exactly is what it should hold - but live captures say
    /// otherwise: a Savanna Rarab arrives with 0x0B, another mob with 0x0C,
    /// and a Carrion Crow with 0x11, which has no 0x08 in it at all and so
    /// came out the same green as a shopkeeper. Something past that
    /// assignment is contributing bits this client cannot yet account for.
    ///
    /// What holds across every capture is the weaker claim: the byte is
    /// written only in the mob branch, so a real NPC arrives with 0x00 and
    /// anything that fights arrives with something. Testing for written-at-
    /// all is the part that is actually known to be true.
    /// </remarks>
    public bool IsLivingMob => BattleFlags is byte flags && flags != 0;

    /// <summary>The bit the server assigns for a living mob. See IsLivingMob for why it is not tested alone.</summary>
    public const byte MobAliveFlag = 0x08;

    /// <summary>Set when the server will accept a trigger on this entity.</summary>
    public const byte TriggerableFlag = 0x40;

    /// <summary>
    /// Whether clicking this does anything. A signpost, a door and a
    /// shopkeeper are triggerable; the auction counter beside them is not,
    /// and neither is a mob that is already dead.
    /// </summary>
    public bool IsTriggerable => RenderFlags is byte flags && (flags & TriggerableFlag) != 0;

    /// <summary>
    /// This update says the entity has gone - killed, walked out of range,
    /// logged out.
    ///
    /// Both packet types carry it the same way, which is worth knowing because
    /// almost nothing else about them is shared past the position block: the
    /// server sets SendFlg.Despawn in char_update.cpp for players and in
    /// entity_update.cpp for everything else, and SendFlg is in the block they
    /// do share.
    /// </summary>
    public bool IsDespawn => (SendFlags & DespawnFlag) != 0;

    /// <summary>sendflags_t bit 5. The whole byte reads 0x30 on an NPC despawn
    /// - the despawn bit and the model bit together - so testing the bit rather
    /// than the byte is what makes this work for players too.</summary>
    public const byte DespawnFlag = 0x20;

    /// <summary>xi::Allegiance - 0 is Mob, 1 Player, 2-6 the nations. Kept for
    /// information; it does not say what kind of entity this is.</summary>
    public const byte MobAllegiance = 0;

    private const int Body = 4; // id/size/sync sub-packet header
    private const int OffsetUniqueNo = Body + 0;
    private const int OffsetActIndex = Body + 4;

    /// <summary>Absolute 0x0A - sendflags_t, in the shared position block.</summary>
    private const int OffsetSendFlags = 0x0A;

    /// What an update actually carries. The server truncates by update type and
    /// says which parts it sent in the byte at 0x0A - base_entity.h's
    /// updatemask. Length alone cannot tell you: a short update is long enough
    /// to reach the status fields and holds zeros there, and reading those as
    /// real values is what stripped a GM's level, a player's name and a
    /// character's whole appearance on every step they took.
    private const byte UpdatePosition = 0x01;
    // sendflags_t, named as the server names it. char_update.cpp and
    // entity_update.cpp declare the same bitfield, and which block a field
    // lives in decides whether reading it means anything: a flag that is not
    // set is a block the server never wrote, so the bytes are whatever the
    // buffer happened to hold.
    private const byte UpdateClaimStatus = 0x02;
    private const byte UpdateGeneral = 0x04;
    private const byte UpdateName = 0x08;
    private const byte UpdateModel = 0x10;
    private const int OffsetDirection = Body + 7;
    private const int OffsetX = Body + 8;
    private const int OffsetVertical = Body + 12; // engine Y - see FfxiPositionPacket
    private const int OffsetDepth = Body + 16;

    /// <summary>
    /// Absolute 0x29, which is where the server writes allegiance for both
    /// NPCs and mobs. Only present once the update carries the HP block, so
    /// short updates leave it null rather than reading a zero and calling
    /// every entity hostile.
    /// </summary>
    private const int OffsetAllegiance = 0x29;

    /// <summary>Absolute 0x1E - real HP percent for a mob, hardcoded 100 for an NPC.</summary>
    private const int OffsetHealthPercent = 0x1E;

    /// <summary>Absolute 0x25 - see <see cref="IsLivingMob"/>.</summary>
    private const int OffsetBattleFlags = 0x25;

    /// <summary>
    /// Presentation flags, of which one is worth having: 0x40 says the
    /// entity can be clicked. entity_update.cpp sets it for an NPC whose
    /// `triggerable()` is true, for a mob that is spawned and in a normal
    /// state, and for trusts.
    ///
    /// The rest of the byte is presentation this client does not do yet -
    /// terror, a death animation, a render priority hack for Pso'Xja.
    /// </summary>
    private const int OffsetRenderFlags = 0x28;

    /// <summary>Minimum bytes needed for the head this parses.</summary>
    public const int MinimumSize = Body + 20;

    // Offsets confirmed against the compiled GP_SERV_CHAR_PC, not counted by
    // hand: the struct threads several bitfield blocks and a misaligned
    // uint8 between the head and the name.
    /// look_t, at a fixed offset whatever kind it turns out to be. Only the
    /// first two bytes are always present: the equipment form fills all twenty
    /// and grows the packet to 0x48, while a fixed model writes four and a door
    /// writes an id or a name instead.
    private const int OffsetLook = 0x30;

    /// A player's look sits somewhere else entirely, and without the leading
    /// size field: face, race, then the eight equipment slots, running up to
    /// the name at 0x5A. Found by looking for the slot tags - equipment ids
    /// carry theirs in the high nibble, so a run reading 0x1xxx 0x2xxx 0x3xxx
    /// 0x4xxx 0x5xxx can only be one thing.
    private const int OffsetPlayerLook = 0x48;

    /// xi::NameVis. Doors, zone lines and the like carry HideName: they have a
    /// name, and the game shows it only once you target them.
    /// xi::EntityFlags, as a u32 - entity_update.cpp writes
    /// `ref&lt;uint32&gt;(0x21) = PNpc-&gt;m_flags`. The bits are named in
    /// LandSandBoat's scripts/enum/entity_flags.codegen.lua.
    ///
    /// These change while you play: an event can reveal something that was
    /// hidden, so they are read on every update rather than remembered.
    private const int OffsetEntityFlags = 0x21;

    private const uint EntityFlagHideName = 0x00000008;
    private const uint EntityFlagHideModel = 0x00000080;
    private const uint EntityFlagUntargetable = 0x00000800;

    private const int OffsetNameVis = 0x2B;

    private const byte NameVisHideName = 0x08;

    private const int OffsetFlags1 = 32;
    private const int OffsetName = 90;
    private const int NameLength = 16;

    /// <summary>Flags1 bits 9-10 - see <see cref="ModelSize"/>.</summary>
    private const int GraphSizeBitOffset = 9;
    private const uint GraphSizeMask = 0b11;

    /// <summary>True for the two ids that carry a GP_SERV_POS_HEAD.</summary>
    public static bool IsEntityUpdate(ushort id) => id is PlayerPacketId or NpcPacketId;

    /// <summary>
    /// Parses the head of one entity-update sub-packet, or returns null if
    /// it's not one or is too short to hold the block.
    /// </summary>
    public static FfxiEntityUpdate? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < MinimumSize)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket[..2]));
        if (!IsEntityUpdate(id))
        {
            return null;
        }

        // Only player updates share the layout past the head, so name and
        // model size are read for those alone - and only when the packet is
        // actually long enough, since these are truncated by update type.
        string? name = null;
        byte? modelSize = null;
        uint? rawFlags1 = null;
        uint? rawFlags0 = null;
        byte? allegiance = null;
        byte? healthPercent = null;
        byte? battleFlags = null;
        byte? renderFlags = null;

        if (id == NpcPacketId && subPacket.Length > OffsetAllegiance)
        {
            allegiance = subPacket[OffsetAllegiance];

            // Health and the mob flag are written only under UPDATE_HP.
            //
            // entity_update.cpp puts every one of them inside
            // `if (updatemask & UPDATE_HP)`, so on an update without that
            // bit those bytes are whatever the packet happened to contain -
            // usually zero. Reading them anyway makes a living mob look like
            // an NPC that merely has no hit points, which is exactly what an
            // NPC is, so a Carrion Crow came out the same colour as a
            // shopkeeper. Long enough to hold the byte is not the same
            // question as the server having written it.
            if ((subPacket[OffsetSendFlags] & UpdateGeneral) != 0)
            {
                healthPercent = subPacket[OffsetHealthPercent];
                battleFlags = subPacket[OffsetBattleFlags];
                renderFlags = subPacket.Length > OffsetRenderFlags ? subPacket[OffsetRenderFlags] : null;
            }
        }

        // Flags1 - which carries HideFlag - is written under General, not
        // under ClaimStatus. ClaimStatus writes one field, BtTargetID, and
        // nothing else. Reading Flags1 whenever ClaimStatus was set meant
        // reading a block the server had not touched, and a HideFlag that came
        // back set from whatever was in the buffer hid a player permanently:
        // they appeared on spawn, when every flag is set, and vanished on the
        // first ordinary update afterwards.
        if (id == PlayerPacketId && (subPacket[OffsetSendFlags] & UpdateGeneral) != 0 &&
            subPacket.Length >= OffsetFlags1 + 4)
        {
            uint flags1 = BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetFlags1, 4));
            rawFlags1 = flags1;
            rawFlags0 = BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(24, 4));
            modelSize = (byte)((flags1 >> GraphSizeBitOffset) & GraphSizeMask);
        }

        // And the name stands on its own flag rather than inside another one.
        if (id == PlayerPacketId && (subPacket[OffsetSendFlags] & UpdateName) != 0 &&
            subPacket.Length >= OffsetName + 1)
        {
            int available = Math.Min(NameLength, subPacket.Length - OffsetName);
            name = ReadFixedString(subPacket.Slice(OffsetName, available));
        }

        // A player's GrapIDTbl is written under Model; an NPC's look_t rides
        // with the status block. Different packets, different questions, and
        // a position-only update holds zeros where either would be.
        FfxiEntityLook? look =
            id == PlayerPacketId
                ? ((subPacket[OffsetSendFlags] & UpdateModel) != 0 ? ReadPlayerLook(subPacket) : null)
                : ((subPacket[OffsetSendFlags] & UpdateClaimStatus) != 0 ? ReadLook(subPacket) : null);
        uint? entityFlags = (subPacket[OffsetSendFlags] & UpdateClaimStatus) != 0 &&
                            subPacket.Length >= OffsetEntityFlags + 4 && id == NpcPacketId
            ? BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetEntityFlags, 4))
            : null;

        byte? nameVis = (subPacket[OffsetSendFlags] & UpdateClaimStatus) != 0 && subPacket.Length > OffsetNameVis
            ? subPacket[OffsetNameVis]
            : null;

        return new FfxiEntityUpdate(
            SendFlags: subPacket[OffsetSendFlags],
            PacketId: id,
            UniqueNo: BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetUniqueNo, 4)),
            ActIndex: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetActIndex, 2)),
            Direction: (sbyte)subPacket[OffsetDirection],
            X: BinaryPrimitives.ReadSingleLittleEndian(subPacket.Slice(OffsetX, 4)),
            Vertical: BinaryPrimitives.ReadSingleLittleEndian(subPacket.Slice(OffsetVertical, 4)),
            Depth: BinaryPrimitives.ReadSingleLittleEndian(subPacket.Slice(OffsetDepth, 4)),
            Name: name,
            ModelSize: modelSize,
            RawFlags1: rawFlags1,
            RawFlags0: rawFlags0,
            EntityFlags: entityFlags ?? 0,
            Allegiance: allegiance,
            HealthPercent: healthPercent,
            BattleFlags: battleFlags,
            RenderFlags: renderFlags,
            Look: look,
            NameVis: nameVis);
    }

    /// <summary>
    /// The server says to keep this one's name off screen until it is
    /// targeted. Doors, zone lines and scenery carry it - they are named, and
    /// the game shows the name only on target.
    /// </summary>
    /// <summary>
    /// Only the namevis byte decides this.
    ///
    /// EntityFlags bit 0x08 is called HIDE_NAME in LandSandBoat's enum, and
    /// acting on it hid almost every NPC in the zone: 203 of Windurst Waters'
    /// NPCs carry flags 0x1B, which contains it, and they are ordinary
    /// shopkeepers whose names the game shows. So either the field at 0x21 is
    /// not what arrives, or that bit means something else once it is on the
    /// wire. Parsed and left alone until something can say which.
    /// </summary>
    public bool IsNameHidden => (NameVis & NameVisHideName) != 0;

    /// <summary>
    /// The server says not to draw this at all - a trigger, or something an
    /// event has yet to reveal.
    /// </summary>
    public bool IsModelHidden => (EntityFlags & EntityFlagHideModel) != 0;

    /// <summary>Nothing the player can click. Warp triggers carry this.</summary>
    public bool IsUntargetable => (EntityFlags & EntityFlagUntargetable) != 0;

    /// <summary>
    /// The look_t at 0x30, as much of it as this packet actually carries.
    /// Returns null when the packet stops short of it, which the smaller
    /// position-only updates do.
    /// </summary>
    /// <summary>
    /// A player's look, which the 0x00D packet lays out differently: no size
    /// field, and further along. A player is always the equipment form - there
    /// is no other way to describe one - so the kind is not read, it is known.
    /// </summary>
    private static FfxiEntityLook? ReadPlayerLook(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < OffsetPlayerLook + 18)
        {
            return null;
        }

        // Race zero is not a race. A partial update - a gear change, a flag
        // toggled - carries this field as zeros rather than omitting it, and
        // taking that as a look replaces a real character with nothing.
        if (subPacket[OffsetPlayerLook + 1] == 0)
        {
            return null;
        }

        ReadOnlySpan<byte> slots = subPacket.Slice(OffsetPlayerLook + 2, 16);
        return new FfxiEntityLook(FfxiLookKind.Equipped, 0,
                                  Race: subPacket[OffsetPlayerLook + 1],
                                  Face: subPacket[OffsetPlayerLook],
                                  Head: BinaryPrimitives.ReadUInt16LittleEndian(slots[..2]),
                                  Body: BinaryPrimitives.ReadUInt16LittleEndian(slots[2..4]),
                                  Hands: BinaryPrimitives.ReadUInt16LittleEndian(slots[4..6]),
                                  Legs: BinaryPrimitives.ReadUInt16LittleEndian(slots[6..8]),
                                  Feet: BinaryPrimitives.ReadUInt16LittleEndian(slots[8..10]),
                                  Main: BinaryPrimitives.ReadUInt16LittleEndian(slots[10..12]),
                                  Sub: BinaryPrimitives.ReadUInt16LittleEndian(slots[12..14]),
                                  Ranged: BinaryPrimitives.ReadUInt16LittleEndian(slots[14..16]));
    }

    private static FfxiEntityLook? ReadLook(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < OffsetLook + 4)
        {
            return null;
        }

        var kind = (FfxiLookKind)BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetLook, 2));

        // Doors and transport put an id or a name where the model would be, so
        // there is nothing here to read as an appearance.
        if (kind is FfxiLookKind.Door or FfxiLookKind.Elevator or FfxiLookKind.Ship)
        {
            return new FfxiEntityLook(kind, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        }

        ushort modelId = BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetLook + 2, 2));

        // The equipment form is the only one that fills the whole structure,
        // and only then is the packet long enough to hold it.
        if (kind is FfxiLookKind.Equipped or FfxiLookKind.Chocobo && subPacket.Length >= OffsetLook + 20)
        {
            if (subPacket[OffsetLook + 3] == 0)
            {
                return null;   // as above: an equipment look with no race is a blank field
            }

            // Read one at a time rather than through a helper: a local
            // function cannot capture the span this all comes from.
            ReadOnlySpan<byte> slots = subPacket.Slice(OffsetLook + 4, 16);
            return new FfxiEntityLook(kind, modelId,
                                      Race: subPacket[OffsetLook + 3],
                                      Face: subPacket[OffsetLook + 2],
                                      Head: BinaryPrimitives.ReadUInt16LittleEndian(slots[..2]),
                                      Body: BinaryPrimitives.ReadUInt16LittleEndian(slots[2..4]),
                                      Hands: BinaryPrimitives.ReadUInt16LittleEndian(slots[4..6]),
                                      Legs: BinaryPrimitives.ReadUInt16LittleEndian(slots[6..8]),
                                      Feet: BinaryPrimitives.ReadUInt16LittleEndian(slots[8..10]),
                                      Main: BinaryPrimitives.ReadUInt16LittleEndian(slots[10..12]),
                                      Sub: BinaryPrimitives.ReadUInt16LittleEndian(slots[12..14]),
                                      Ranged: BinaryPrimitives.ReadUInt16LittleEndian(slots[14..16]));
        }

        return new FfxiEntityLook(kind, modelId, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    }

    private static string ReadFixedString(ReadOnlySpan<byte> field)
    {
        int nul = field.IndexOf((byte)0);
        return System.Text.Encoding.ASCII.GetString(nul >= 0 ? field[..nul] : field);
    }
}
