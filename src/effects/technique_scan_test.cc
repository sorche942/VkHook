// Technique scan: the text-level list of techniques a .fx file declares.

#include "effects/technique_scan.hh"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace VkHook;

namespace
{
    int failures = 0;

    void expect(bool condition, const std::string& message)
    {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }

    using Names = std::vector<std::string>;

    void testPlainDeclarations()
    {
        expect(scanTechniqueNamesInText("technique A { pass { } }\ntechnique B{pass{}}\n") == Names({"A", "B"}),
               "techniques come back in file order");
        expect(scanTechniqueNamesInText("technique\n\tLuma_Pre\n{\n}\n") == Names({"Luma_Pre"}),
               "the name may sit on another line");
        expect(scanTechniqueNamesInText("technique<ui_label=\"x\";>").empty(),
               "a technique without a name is not a declaration");
        expect(scanTechniqueNamesInText("techniques foo; mytechnique Bar; technique_x Baz;").empty(),
               "only the whole word `technique` counts");
        expect(scanTechniqueNamesInText("").empty(), "empty text has no techniques");
    }

    void testCommentsAndStrings()
    {
        const std::string text =
            "// technique Commented { }\n"
            "/* technique Block\n"
            "   technique Block2 */\n"
            "uniform string s < ui_label = \"technique InString\"; > = \"technique Other\";\n"
            "uniform string t = \"quote \\\" technique Escaped\";\n"
            "technique Real // technique Trailing\n"
            "{\n"
            "    pass { }\n"
            "}\n";
        expect(scanTechniqueNamesInText(text) == Names({"Real"}),
               "comments and string literals declare nothing");
        expect(scanTechniqueNamesInText("/* unterminated technique A") .empty(), "an open comment swallows the rest");
        expect(scanTechniqueNamesInText("\"unterminated technique A\ntechnique B { }") == Names({"B"}),
               "a string ends at its line");
    }

    void testAnnotationsAndDuplicates()
    {
        const std::string text =
            "technique First < ui_label = \"First pass\"; ui_tooltip = \"technique Fake\"; >\n"
            "{ pass { VertexShader = V; } }\n"
            "technique Second < enabled = true; > { }\n"
            "technique First { }\n";
        expect(scanTechniqueNamesInText(text) == Names({"First", "Second"}),
               "annotations are skipped and repeated names collapse");
    }

    void testPreprocessor()
    {
        const std::string text =
            "#include \"technique Z.fxh\"\n"
            "#define MAKE(n) technique n { }\\\n"
            "    technique Continued { }\n"
            "  # define X technique Indented\n"
            "#if USE_B\n"
            "technique B { }\n"
            "#else\n"
            "technique C { }\n"
            "#endif\n";
        expect(scanTechniqueNamesInText(text) == Names({"B", "C"}),
               "directives are ignored and #if gating is not evaluated");
    }

    void testFile()
    {
        const auto path = std::filesystem::temp_directory_path() / "vkhook_technique_scan_test.fx";
        {
            std::ofstream file(path, std::ios::binary);
            file << "technique One { }\r\ntechnique Two { }\r\n";
        }
        expect(scanTechniqueNames(path.string()) == Names({"One", "Two"}), "a file is read and scanned");
        std::filesystem::remove(path);
        expect(scanTechniqueNames(path.string()).empty(), "an unreadable file has no techniques");
    }
} // namespace

int main()
{
    testPlainDeclarations();
    testCommentsAndStrings();
    testAnnotationsAndDuplicates();
    testPreprocessor();
    testFile();
    if (failures != 0)
    {
        std::cerr << failures << " technique scan test(s) failed\n";
        return 1;
    }
    std::cout << "Technique scan tests passed\n";
    return 0;
}
