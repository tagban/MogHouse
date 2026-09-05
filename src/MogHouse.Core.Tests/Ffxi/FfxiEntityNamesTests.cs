using System.Buffers.Binary;
using System.Text;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class FfxiEntityNamesTests : IDisposable
{
    private readonly string _root = Directory.CreateTempSubdirectory("moghouse-names").FullName;

    private const int RecordLength = 32;
    private const int NameLength = 28;

    /// <summary>
    /// Builds an install holding one zone's name table.
    ///
    /// The first record is deliberately the blank placeholder the retail files
    /// open with, so the reader is asked to skip one on every call.
    /// </summary>
    private FfxiEntityNames Build(int zoneId, params (uint Id, string Name)[] entries)
    {
        var file = new byte[RecordLength * (entries.Length + 1)];

        for (int i = 0; i < entries.Length; i++)
        {
            int at = RecordLength * (i + 1);
            byte[] name = Encoding.UTF8.GetBytes(entries[i].Name);
            name.AsSpan(0, Math.Min(name.Length, NameLength)).CopyTo(file.AsSpan(at, NameLength));
            BinaryPrimitives.WriteUInt32LittleEndian(file.AsSpan(at + NameLength, 4), entries[i].Id);
        }

        int fileId = FfxiEntityNames.EntityNameFileIdOffset + zoneId;
        Directory.CreateDirectory(Path.Combine(_root, "ROM", "9"));
        File.WriteAllBytes(Path.Combine(_root, "ROM", "9", "1.DAT"), file);

        var vtable = new byte[fileId + 1];
        vtable[fileId] = 1;
        var ftable = new byte[(fileId + 1) * 2];
        BinaryPrimitives.WriteUInt16LittleEndian(ftable.AsSpan(fileId * 2, 2), (9 << 7) | 1);
        File.WriteAllBytes(Path.Combine(_root, "VTABLE.DAT"), vtable);
        File.WriteAllBytes(Path.Combine(_root, "FTABLE.DAT"), ftable);

        return FfxiEntityNames.Load(new FfxiFileTable(_root), zoneId);
    }

    /// <summary>Entity ids are 0x1000000 | zone &lt;&lt; 12 | targid.</summary>
    private static uint EntityId(int zone, int targid) =>
        0x1000000u | ((uint)zone << 12) | (uint)targid;

    [Fact]
    public void ReadsANameByEntityId()
    {
        uint id = EntityId(230, 0x11);
        FfxiEntityNames names = Build(230, (id, "Clainomille"));

        Assert.Equal("Clainomille", names.Lookup(id));
    }

    [Fact]
    public void SkipsRecordsWithNoIdAndNoName()
    {
        uint real = EntityId(230, 0x12);
        FfxiEntityNames names = Build(230, (0, "unreachable"), (real, "Ailevia"), (EntityId(230, 0x13), ""));

        Assert.Equal(1, names.Count);
        Assert.Equal("Ailevia", names.Lookup(real));
    }

    [Fact]
    public void StopsAtTheNulRatherThanReadingThePadding()
    {
        uint id = EntityId(100, 0x07);
        FfxiEntityNames names = Build(100, (id, "Ambrotien"));

        // The record is 28 bytes of name; anything past the terminator is
        // padding and must not come back as trailing NULs.
        Assert.Equal("Ambrotien", names.Lookup(id));
    }

    [Fact]
    public void AnUnknownIdIsNullRatherThanAFault()
    {
        FfxiEntityNames names = Build(230, (EntityId(230, 1), "Somebody"));

        // Players are never in this table, so a miss is the normal case.
        Assert.Null(names.Lookup(EntityId(230, 999)));
    }

    [Fact]
    public void AMissingFileLeavesItEmpty()
    {
        File.WriteAllBytes(Path.Combine(_root, "VTABLE.DAT"), new byte[128]);
        File.WriteAllBytes(Path.Combine(_root, "FTABLE.DAT"), new byte[256]);

        FfxiEntityNames names = FfxiEntityNames.Load(new FfxiFileTable(_root), 230);

        Assert.Equal(0, names.Count);
        Assert.Null(names.Lookup(EntityId(230, 1)));
    }

    public void Dispose() => Directory.Delete(_root, recursive: true);
}
