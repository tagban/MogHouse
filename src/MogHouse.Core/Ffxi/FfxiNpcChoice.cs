namespace MogHouse.Core.Ffxi;

/// <summary>
/// A question an NPC asked, and the answers it offered.
///
/// <para>
/// The choices are not a separate packet or a separate table. A line of
/// dialogue holds them inside itself - 0x0B opens the list and each 0x07 after
/// it starts the next answer - so "Set this as your home point?" carries its
/// own Yes and No, and <see cref="FfxiDialogueTable.Options"/> has already
/// separated them by the time one of these is made.
/// </para>
/// </summary>
/// <param name="UniqueNo">The entity that asked.</param>
/// <param name="MessageId">The line id, which is what an answer refers back to.</param>
/// <param name="Speaker">Who asked, or empty for narration.</param>
/// <param name="Text">The question itself, without the answers.</param>
/// <param name="Choices">The answers, in the order the line lists them.</param>
public sealed record FfxiNpcChoice(
    uint UniqueNo,
    int MessageId,
    string Speaker,
    string Text,
    IReadOnlyList<string> Choices);
