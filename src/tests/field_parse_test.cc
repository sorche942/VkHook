// The numeric field editors parse typed values with std::from_chars. These pin
// the grammar: exact component count, no partial application, no negative for
// unsigned, no trailing garbage.

#include "field_parse.hh"

#include <cstdio>
#include <string>

using namespace VkHook::FieldParse;

namespace
{
    int failures = 0;
    void expect(bool condition, const char* what, int line)
    {
        if (!condition)
        {
            ++failures;
            std::printf("FAIL:%d: %s\n", line, what);
        }
    }
}
#define CHECK(c) expect((c), #c, __LINE__)

int main()
{
    {
        uint32_t v[4] = {7, 7, 7, 7};
        CHECK(!parseComponents<uint32_t>("-1", v, 1));
        CHECK(v[0] == 7);
        CHECK(!parseComponents<uint32_t>("1, -2", v, 2));
        CHECK(v[0] == 7 && v[1] == 7);
        CHECK(parseComponents<uint32_t>("4294967295", v, 1) && v[0] == 4294967295u);
        CHECK(!parseComponents<uint32_t>("4294967296", v, 1));  // overflow
        CHECK(parseComponents<uint32_t>(" 1 ,2  3", v, 3) && v[0] == 1 && v[1] == 2 && v[2] == 3);
        CHECK(!parseComponents<uint32_t>("1, 2", v, 3));        // too few
        CHECK(!parseComponents<uint32_t>("1, 2, 3", v, 2));     // too many
        CHECK(!parseComponents<uint32_t>("12abc", v, 1));
        CHECK(!parseComponents<uint32_t>("", v, 1));
    }
    {
        int v[4] = {};
        CHECK(parseComponents<int>("-5", v, 1) && v[0] == -5);
        CHECK(parseComponents<int>("-1,-2,-3,-4", v, 4) && v[3] == -4);
        CHECK(!parseComponents<int>("1.5", v, 1));
        CHECK(!parseComponents<int>("+3", v, 1));
    }
    {
        float v[4] = {};
        CHECK(parseComponents<float>("0.25", v, 1) && v[0] == 0.25f);
        CHECK(parseComponents<float>("1e-2, -3.5", v, 2) && v[1] == -3.5f);
        CHECK(!parseComponents<float>("nan", v, 1));
        CHECK(!parseComponents<float>("inf", v, 1));
        CHECK(!parseComponents<float>("0.5x", v, 1));
    }
    {
        const unsigned values[3] = {1, 22, 333};
        CHECK(formatComponents(values, 3) == "1, 22, 333");
        unsigned back[3] = {};
        CHECK(parseComponents<unsigned>(formatComponents(values, 3), back, 3) && back[2] == 333);
        const float f[2] = {0.1f, -2.0f};
        float fb[2] = {};
        CHECK(parseComponents<float>(formatComponents(f, 2), fb, 2) && fb[0] == 0.1f && fb[1] == -2.0f);
    }

    if (failures)
        return 1;
    std::printf("ok\n");
    return 0;
}
