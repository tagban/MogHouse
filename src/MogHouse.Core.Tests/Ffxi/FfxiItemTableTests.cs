using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class FfxiItemTableTests : IDisposable
{
    private readonly string _root = Directory.CreateTempSubdirectory("moghouse-items").FullName;

    /// <summary>File id 73 is the English general-items DAT, whose header is 0x18 bytes.</summary>
    private const int GeneralItems = 73;

    private const int RecordSize = 0xC00;
    private const int TableOffset = 0x18;

    /// <summary>
    /// Builds an install holding one item DAT with two records - a blank one
    /// and the item asked for, because record 0 is where the real files keep
    /// their placeholder and the reader takes the first id from record 1.
    /// </summary>
    private FfxiItemTable Build(ushort id, string name, string description, byte[]? icon = null)
    {
        var file = new byte[RecordSize * 2];
        var record = new byte[RecordSize];

        BinaryPrimitives.WriteUInt32LittleEndian(record.AsSpan(0, 4), id);
        BinaryPrimitives.WriteUInt16LittleEndian(record.AsSpan(0x06, 2), 12);   // stack size
        BinaryPrimitives.WriteUInt16LittleEndian(record.AsSpan(0x08, 2), 1);    // type
        BinaryPrimitives.WriteUInt32LittleEndian(record.AsSpan(TableOffset, 4), 5);

        // Five strings: the name, a blank, two log forms, the description.
        string[] text = [name, "", name.ToLowerInvariant(), name.ToLowerInvariant() + "s", description];
        int at = TableOffset + 4 + (5 * 8);
        for (int i = 0; i < 5; i++)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(
                record.AsSpan(TableOffset + 4 + (i * 8), 4), (uint)(at - TableOffset));

            // A block is a count of 24-byte attributes, then the text.
            BinaryPrimitives.WriteUInt32LittleEndian(record.AsSpan(at, 4), 1);
            at += 4 + 0x18;
            foreach (char c in text[i])
            {
                record[at++] = (byte)c;
            }
            at++;   // the NUL
        }

        if (icon is not null)
        {
            icon.CopyTo(record, 0x280);
        }

        // Every byte is stored rotated left by five, which the reader undoes.
        for (int i = 0; i < record.Length; i++)
        {
            record[i] = (byte)((record[i] << 5) | (record[i] >> 3));
        }

        record.CopyTo(file, RecordSize);

        // FTABLE packs a directory and a file number; ROM 1 is the folder
        // called plain "ROM".
        Directory.CreateDirectory(Path.Combine(_root, "ROM", "9"));
        File.WriteAllBytes(Path.Combine(_root, "ROM", "9", "1.DAT"), file);

        var vtable = new byte[GeneralItems + 1];
        vtable[GeneralItems] = 1;
        var ftable = new byte[(GeneralItems + 1) * 2];
        BinaryPrimitives.WriteUInt16LittleEndian(ftable.AsSpan(GeneralItems * 2, 2), (9 << 7) | 1);
        File.WriteAllBytes(Path.Combine(_root, "VTABLE.DAT"), vtable);
        File.WriteAllBytes(Path.Combine(_root, "FTABLE.DAT"), ftable);

        return new FfxiItemTable(new FfxiFileTable(_root));
    }

    /// <summary>A 32x32 icon whose palette entry 1 is opaque red, and every pixel entry 1.</summary>
    private static byte[] RedIcon()
    {
        var icon = new byte[4 + 1 + 16 + 40 + 1024 + 1024];
        BinaryPrimitives.WriteUInt32LittleEndian(icon.AsSpan(0, 4), (uint)(icon.Length - 4));
        int header = 4 + 1 + 16;
        BinaryPrimitives.WriteUInt32LittleEndian(icon.AsSpan(header, 4), 40);
        BinaryPrimitives.WriteInt32LittleEndian(icon.AsSpan(header + 4, 4), 32);
        BinaryPrimitives.WriteInt32LittleEndian(icon.AsSpan(header + 8, 4), 32);
        BinaryPrimitives.WriteUInt16LittleEndian(icon.AsSpan(header + 12, 2), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(icon.AsSpan(header + 14, 2), 8);

        int palette = header + 40;
        icon[palette + 4] = 0x00;   // blue
        icon[palette + 5] = 0x00;   // green
        icon[palette + 6] = 0xFF;   // red
        icon[palette + 7] = 0x80;   // alpha, at the game's half scale

        int pixels = palette + 1024;
        for (int i = 0; i < 1024; i++)
        {
            icon[pixels + i] = 1;
        }

        return icon;
    }

    [Fact]
    public void ReadsTheNameAndTheDescription()
    {
        using FfxiItemTable table = Build(2, "Simple Bed", "Furnishing:\nA crude bed.");

        FfxiItem? item = table.Item(2);

        Assert.NotNull(item);
        Assert.Equal("Simple Bed", item.Name);
        Assert.Equal("simple bed", item.LogSingular);
        Assert.Equal("simple beds", item.LogPlural);
        Assert.Equal("Furnishing:\nA crude bed.", item.Description);
        Assert.Equal(12, item.StackSize);
    }

    [Fact]
    public void AnIdOutsideTheFileIsNotThere()
    {
        using FfxiItemTable table = Build(2, "Simple Bed", "");

        Assert.True(table.IsLoaded);
        Assert.Null(table.Item(9999));
    }

    [Fact]
    public void ResistancesAreWrittenWithASymbolNotAWord()
    {
        // 0xEF then 0x1F to 0x26 is Fire through Dark. Ice is 0x20, dark 0x26.
        using FfxiItemTable table = Build(2, "Scorpion Harness", "DEF:40 ï -20 ï&+15");

        FfxiItem? item = table.Item(2);

        Assert.NotNull(item);
        Assert.Equal("DEF:40 Ice-20 Dark+15", item.Description);
    }

    [Fact]
    public void ExpandsThePalettedIcon()
    {
        using FfxiItemTable table = Build(2, "Simple Bed", "", RedIcon());

        FfxiItemIcon? icon = table.Icon(2);

        Assert.NotNull(icon);
        Assert.Equal(32, icon.Width);
        Assert.Equal(32, icon.Height);
        Assert.Equal(32 * 32 * 4, icon.Rgba.Length);

        // Red, and opaque: the DAT's 0x80 alpha is the top of its range.
        Assert.Equal(0xFF, icon.Rgba[0]);
        Assert.Equal(0x00, icon.Rgba[1]);
        Assert.Equal(0x00, icon.Rgba[2]);
        Assert.Equal(0xFF, icon.Rgba[3]);
    }

    [Fact]
    public void AnInstallWithoutTheItemDatsLoadsNothing()
    {
        File.WriteAllBytes(Path.Combine(_root, "VTABLE.DAT"), new byte[128]);
        File.WriteAllBytes(Path.Combine(_root, "FTABLE.DAT"), new byte[256]);

        using var table = new FfxiItemTable(new FfxiFileTable(_root));

        Assert.False(table.IsLoaded);
        Assert.Null(table.Item(2));
    }

    public void Dispose() => Directory.Delete(_root, recursive: true);
}
