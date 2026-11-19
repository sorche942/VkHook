// ShaderCatalog: the stem/file/relative-path index that replaces the
// recursive filesystem search.

#include "shader_catalog.hh"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

using namespace VkHook;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        if (!condition)
        {
            ++failures;
            std::printf("FAILED: %s\n", message);
        }
    }

    void touch(const fs::path& path)
    {
        fs::create_directories(path.parent_path());
        std::ofstream(path) << "// shader\n";
    }
}

int main()
{
    const fs::path root = fs::temp_directory_path() / ("vkhook-catalog-test-" + std::to_string(::getpid()));
    fs::remove_all(root);
    touch(root / "a" / "Shaders" / "Clarity.fx");
    touch(root / "a" / "Shaders" / "Sub" / "Vibrance.FX");
    touch(root / "a" / "Shaders" / "Common.fxh");
    touch(root / "b" / "Clarity.fx");
    touch(root / "b" / "Only_B.fx");

    ShaderCatalog catalog;
    catalog.setRoots({(root / "a").string(), (root / "b").string()});
    expect(catalog.size() == 4, "four .fx files indexed (the .fxh is not a shader)");

    const std::string clarityA = (root / "a" / "Shaders" / "Clarity.fx").string();
    expect(catalog.find("Clarity.fx") == clarityA, "file name; the first root wins");
    expect(catalog.find("clarity") == clarityA, "stem, case-insensitive");
    expect(catalog.find("CLARITY.FX") == clarityA, "file name, case-insensitive");
    expect(catalog.find("Shaders/Clarity.fx") == clarityA, "relative path");
    expect(catalog.find("Only_B") == (root / "b" / "Only_B.fx").string(), "second root");
    expect(catalog.find("Vibrance").find("Vibrance.FX") != std::string::npos, "upper-case extension");
    expect(catalog.find("Clarity.2") == clarityA, "a legacy copy name resolves by its stem");
    expect(catalog.find("Missing").empty() && catalog.find("").empty(), "misses are empty");

    // setRoots with the same list is a no-op; a different list rescans
    const uint64_t generation = catalog.generation();
    catalog.setRoots({(root / "a").string(), (root / "b").string()});
    expect(catalog.generation() == generation, "unchanged roots do not rescan");
    catalog.setRoots({(root / "b").string()});
    expect(catalog.generation() > generation && catalog.find("Clarity.fx") == (root / "b" / "Clarity.fx").string(),
           "new roots rebuild the index");

    // a shader added later is found after a rescan, which a miss rate-limits
    touch(root / "b" / "Late.fx");
    expect(catalog.find("Late").empty(), "not visible before a rescan");
    expect(!catalog.rescan(false, 60'000), "a rescan inside the interval is skipped");
    expect(catalog.rescan(true) && !catalog.find("Late").empty(), "a forced rescan sees it");

    fs::remove_all(root);
    if (failures == 0)
        std::printf("shader catalog tests passed\n");
    return failures == 0 ? 0 : 1;
}
