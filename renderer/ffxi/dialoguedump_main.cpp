// Prints what the people in a zone say.
//
//   ffxi-dialogue <dialogue.DAT>            every entry, numbered
//   ffxi-dialogue <dialogue.DAT> <n>        just entry n
//   ffxi-dialogue <dialogue.DAT> --menus    only the entries offering a choice
//
// The file is `6420 + zone` in the file table. An event names an entry by its
// number, so the numbering is the part that matters and it is printed even for
// the empty ones.

#include "dialogue.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: ffxi-dialogue <dialogue.DAT> [n | --menus]\n");
        return 1;
    }

    const std::vector<ffxi::DialogueEntry> entries = ffxi::readDialogue(argv[1]);
    if (entries.empty())
    {
        std::printf("no dialogue in %s\n", argv[1]);
        return 1;
    }
    std::printf("%zu entries\n", entries.size());

    const bool menusOnly = argc > 2 && std::strcmp(argv[2], "--menus") == 0;
    const long only = argc > 2 && !menusOnly ? std::strtol(argv[2], nullptr, 10) : -1;

    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (only >= 0 && static_cast<size_t>(only) != i)
        {
            continue;
        }
        if (menusOnly && entries[i].options.empty())
        {
            continue;
        }
        if (entries[i].text.empty() && entries[i].options.empty())
        {
            continue;
        }
        std::printf("\n[%zu] %s\n", i, entries[i].text.c_str());
        for (size_t o = 0; o < entries[i].options.size(); ++o)
        {
            std::printf("      %zu) %s\n", o, entries[i].options[o].c_str());
        }
    }
    return 0;
}
