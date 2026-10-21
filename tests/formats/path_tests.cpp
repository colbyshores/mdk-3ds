// ============================================================================
// path_tests.cpp — the ports of the C# PathTests and LocalPathsTests.
//
// The C# originals are tests/Mdk.Formats.Tests/PathTests.cs and
// LocalPathsTests.cs. The expected values are THEIRS, unchanged.
//
// These two need no game data, so they run anywhere.
// ============================================================================

#include "test_support.h"

#include "formats/case_insensitive_path.h"
#include "formats/local_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace mdk::formats;

namespace {

// A scratch directory, removed when the test binary exits.
std::string makeTempDir() {
    char pattern[] = "/tmp/mdk-paths-XXXXXX";
    const char* made = mkdtemp(pattern);
    return made ? std::string(made) : std::string();
}

void removeTree(const std::string& path) {
    // The test trees are two levels deep and hold a handful of files, so
    // this is enough; a full recursive walk would be more machinery than
    // the test needs.
    const std::string command = "rm -rf '" + path + "'";
    if (system(command.c_str()) != 0) {
        // Nothing to do: the directory is in /tmp.
    }
}

} // namespace

// Data paths match the files whatever their case: the game asks for
// MISC/MDKFONT.FTI, the installation has MISC/mdkfont.fti (case-sensitive
// file systems).
MDK_TEST(PartsMatchWhateverTheirCase) {
    const std::string dir = makeTempDir();
    const std::string folder = dir + "/Misc/FLIC";
    mkdir((dir + "/Misc").c_str(), 0777);
    mkdir(folder.c_str(), 0777);
    FILE* file = fopen((dir + "/Misc/mdkfont.fti").c_str(), "wb");
    if (file) fclose(file);
    file = fopen((folder + "/Mdk12.flc").c_str(), "wb");
    if (file) fclose(file);

    const mdk::formats::FileSystem* system = mdk::test::host::fileSystem();
    MDK_CHECK_STR_EQ(caseInsensitiveResolve(system, dir, "MISC/MDKFONT.FTI"),
                     dir + "/Misc/mdkfont.fti");
    MDK_CHECK_STR_EQ(caseInsensitiveResolve(system, dir, "MISC/FLIC/MDK12.FLC"),
                     folder + "/Mdk12.flc");

    removeTree(dir);
}

MDK_TEST(MissingFilesKeepTheirName) {
    const std::string dir = makeTempDir();
    const mdk::formats::FileSystem* system = mdk::test::host::fileSystem();
    MDK_CHECK_STR_EQ(caseInsensitiveResolve(system, dir, "MISC/NONE.GIF"),
                     dir + "/MISC/NONE.GIF");
    removeTree(dir);
}

// mdk_paths.cfg (the Godot port's ConfigFile): the folders of the game and
// the demo.
MDK_TEST(KeysOfThePathsSectionAreRead) {
    const std::string config =
        "; my folders\n"
        "[other]\n"
        "mdk=\"C:/Wrong\"\n"
        "\n"
        "[paths]\n"
        "mdk=\"C:/GOG Games/MDK\"\n"
        "beta = \"C:/Games/MDK (1996-08-06) (beta demo)\"\n"
        "plain=D:/MDK\n"
        "escaped=\"C:\\\\Games\\\\\\\"MDK\\\"\"\n";

    MDK_CHECK_STR_EQ(localPathsParse(config, kLocalPathsGame), "C:/GOG Games/MDK");
    MDK_CHECK_STR_EQ(localPathsParse(config, kLocalPathsBeta), "C:/Games/MDK (1996-08-06) (beta demo)");
    MDK_CHECK_STR_EQ(localPathsParse(config, "plain"), "D:/MDK");
    // The C# expects "C:\Games\"MDK\"" -- wait: the C# literal
    // "C:\\Games\\\"MDK\"" decodes to C:\Games\"MDK" (13 characters), and the
    // escaped-quote replacement runs BEFORE the escaped-backslash one, and
    // the order is observable. This test caught the C++ port adding a
    // trailing backslash the C# does not produce.
    MDK_CHECK_STR_EQ(localPathsParse(config, "escaped"), "C:\\Games\\\"MDK\"");
    MDK_CHECK_STR_EQ(localPathsParse(config, "missing"), "");
}

MDK_TEST(WindowsLineEndingsAreRead) {
    MDK_CHECK_STR_EQ(localPathsParse("[paths]\r\nmdk=\"C:/MDK\"\r\n", kLocalPathsGame), "C:/MDK");
}

MDK_TEST(NoFileNoPath) {
    MDK_CHECK_STR_EQ(localPathsParse("", kLocalPathsGame), "");
}
