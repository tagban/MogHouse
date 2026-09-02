using System.Runtime.InteropServices;
using MogHouse.Core.Interop;

namespace MogHouse.Core.Tests.Interop;

/// <summary>
/// The form boundary, which is declared twice - once in C as MhFormRow and once
/// here as NativeFormRowData. Neither side fails to build when they disagree;
/// it just reads the wrong bytes. The C++ side pins the same numbers with
/// static_assert, so these two tests fail together or not at all.
/// </summary>
public class NativeFormTests
{
    [Fact]
    public void FormRowData_MatchesTheLayoutTheNativeSideAsserts()
    {
        Assert.Equal(200, Marshal.SizeOf<NativeFormRowData>());

        Assert.Equal(0, (int)Marshal.OffsetOf<NativeFormRowData>(nameof(NativeFormRowData.Kind)));
        Assert.Equal(4, (int)Marshal.OffsetOf<NativeFormRowData>(nameof(NativeFormRowData.Enabled)));
        Assert.Equal(8, (int)Marshal.OffsetOf<NativeFormRowData>(nameof(NativeFormRowData.Text)));
        Assert.Equal(72, (int)Marshal.OffsetOf<NativeFormRowData>(nameof(NativeFormRowData.Value)));
    }

    [Fact]
    public void RowKinds_MatchTheNumbersTheRendererCastsThemTo()
    {
        Assert.Equal(0, (int)NativeFormRowKind.Label);
        Assert.Equal(1, (int)NativeFormRowKind.Field);
        Assert.Equal(2, (int)NativeFormRowKind.Secret);
        Assert.Equal(3, (int)NativeFormRowKind.Button);
    }

    [Fact]
    public void SplitValues_ReadsOneEntryPerRowInOrder()
    {
        byte[] packed = Packed("127.0.0.1", "tagban", "hunter2");

        IReadOnlyList<string> values = NativeViewer.SplitValues(packed, 3);

        Assert.Equal(["127.0.0.1", "tagban", "hunter2"], values);
    }

    [Fact]
    public void SplitValues_LeadingEmptyRow_DoesNotEndTheList()
    {
        // The reason the count is passed rather than marked in the buffer. A
        // label row produces an empty value, an empty value is a lone NUL, and
        // a terminated list would stop dead on the form's own caption - so a
        // login screen would come back with nothing anyone typed.
        byte[] packed = Packed("", "127.0.0.1", "", "tagban");

        IReadOnlyList<string> values = NativeViewer.SplitValues(packed, 4);

        Assert.Equal(["", "127.0.0.1", "", "tagban"], values);
    }

    [Fact]
    public void SplitValues_ReadsOnlyAsManyAsTheRendererWrote()
    {
        // A real buffer is stack memory holding whatever was there before, and
        // nothing marks where the values stop.
        var buffer = new byte[64];
        Array.Fill(buffer, (byte)'X');
        Packed("hello").CopyTo(buffer, 0);

        IReadOnlyList<string> values = NativeViewer.SplitValues(buffer, 1);

        Assert.Equal(["hello"], values);
    }

    [Fact]
    public void SplitValues_NoRows_ComesBackEmptyRatherThanThrowing()
    {
        Assert.Empty(NativeViewer.SplitValues(new byte[8], 0));
        Assert.Empty(NativeViewer.SplitValues(ReadOnlySpan<byte>.Empty, 3));
    }

    [Fact]
    public void SplitValues_ShortBuffer_KeepsWhatIsThereRatherThanThrowing()
    {
        // Claiming more values than were written should not walk off the end:
        // the buffer runs out and what was read is kept.
        IReadOnlyList<string> values = NativeViewer.SplitValues(Packed("only"), 5);

        Assert.Equal(["only"], values);
    }

    [Fact]
    public unsafe void WriteFixed_TerminatesWhatItWrites()
    {
        byte* target = stackalloc byte[16];

        NativeViewer.WriteFixed(target, 16, "tagban");

        Assert.Equal("tagban", Marshal.PtrToStringUTF8((IntPtr)target));
    }

    [Fact]
    public unsafe void WriteFixed_TruncatesRatherThanRunningPastTheEnd()
    {
        // One past the array, so an off-by-one shows up as a changed guard.
        byte* target = stackalloc byte[9];
        target[8] = 0x7F;

        NativeViewer.WriteFixed(target, 8, "abcdefghijkl");

        Assert.Equal("abcdefg", Marshal.PtrToStringUTF8((IntPtr)target));
        Assert.Equal(0x7F, target[8]);
    }

    [Fact]
    public unsafe void WriteFixed_ReplacesWhatTheRenderersFontCannotDraw()
    {
        byte* target = stackalloc byte[16];

        NativeViewer.WriteFixed(target, 16, "café");

        // The font has no accents and turns what it does not know into a
        // space, so the substitution happens here where it is visible.
        Assert.Equal("caf ", Marshal.PtrToStringUTF8((IntPtr)target));
    }

    [Fact]
    public unsafe void WriteFixed_NullIsAnEmptyString()
    {
        byte* target = stackalloc byte[4];
        target[0] = 0x7F;

        NativeViewer.WriteFixed(target, 4, null);

        Assert.Equal(string.Empty, Marshal.PtrToStringUTF8((IntPtr)target));
    }

    [Fact]
    public void Result_IndexerAnswersForRowsThatHaveNoValue()
    {
        var result = new NativeFormResult(3, ["", "127.0.0.1"]);

        Assert.Equal("127.0.0.1", result[1]);
        Assert.Equal(string.Empty, result[0]);
        Assert.Equal(string.Empty, result[99]);
        Assert.Equal(string.Empty, result[-1]);
    }

    private static byte[] Packed(params string[] values)
    {
        var bytes = new List<byte>();
        foreach (string value in values)
        {
            bytes.AddRange(System.Text.Encoding.UTF8.GetBytes(value));
            bytes.Add(0);
        }
        return [.. bytes];
    }
}
