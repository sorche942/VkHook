// ConfigStore: the settings schema (defaults, clamping, load/save round trip,
// read-modify-write of a hand-edited file), the published snapshots and the
// config-directory migrations.

#include "config.hh"
#include "ini.hh"
#include "migrate.hh"
#include "paths.hh"
#include "persist.hh"
#include "settings_schema.hh"
#include "store.hh"
#include "util.hh"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace VkHook;

    int failures = 0;

    void expect(bool condition, const std::string& message)
    {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }

    std::string readFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    void writeFile(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << text;
    }

    std::string confPath()
    {
        return Paths::userSettingsFile();
    }

    void testDefaultsComeFromTheSchema()
    {
        const AppSettings defaults;
        for (const SettingDesc& row : Schema::settings())
            expect(Schema::get(defaults, row) == row.defaultValue, std::string("default of ") + row.key);
        expect(defaults.enableOnLaunch && defaults.depthCapture && defaults.maxEffects == 10,
               "the shipped defaults: launch on, depth capture on, ten effects");
        expect(Schema::find("maxEffects") != nullptr && Schema::find("noSuchKey") == nullptr, "lookup by key");
    }

    void testClampingIsOneRule()
    {
        AppSettings s;
        const SettingDesc* maxEffects = Schema::find("maxEffects");
        Schema::set(s, *maxEffects, 9999);
        expect(s.maxEffects == 200, "maxEffects clamps to its schema maximum");
        Schema::set(s, *maxEffects, -4);
        expect(s.maxEffects == 1, "maxEffects clamps to its schema minimum");
        expect(!Schema::set(s, *maxEffects, true), "a value of the wrong type is refused");

        const SettingDesc* scale = Schema::find("uiScale");
        Schema::set(s, *scale, 0.1f);
        expect(s.uiScale == 0.75f, "a small uiScale clamps up to the manual minimum");
        Schema::set(s, *scale, 7.0f);
        expect(s.uiScale == 3.0f, "a large uiScale clamps down");
        Schema::set(s, *scale, -1.0f);
        expect(s.uiScale == 0.0f, "zero or negative uiScale means automatic");

        AppSettings loaded;
        Schema::load(loaded, {{"maxEffects", "5000"}, {"depthCaptureMethod", "9"}, {"depthSourceChannel", "x"},
                              {"autoApply", "off"}, {"depthCapture", "on"}, {"unknownKey", "1"}});
        expect(loaded.maxEffects == 200, "load clamps through the same table");
        expect(loaded.depthCaptureMethod == 2, "load clamps depthCaptureMethod");
        expect(loaded.depthSourceChannel == 0, "an unparsable value keeps the default");
        expect(!loaded.autoApply && loaded.depthCapture, "bool spellings on/off");
    }

    void testFileRoundTripAndReadModifyWrite()
    {
        ConfigStore::writeFile(AppSettings{});
        expect(readFile(confPath()).find("enableOnLaunch = true") != std::string::npos,
               "a generated file writes enableOnLaunch");
        expect(readFile(confPath()).find("depthCapture = on") != std::string::npos,
               "depthCapture keeps its on/off spelling");

        AppSettings settings;
        settings.maxEffects = 23;
        settings.toggleKey = "F8";
        settings.uiScale = 1.5f;
        settings.depthManualPin = "image-view-42";
        settings.depthFlop = true;
        expect(ConfigStore::writeFile(settings), "save settings");
        const AppSettings loaded = ConfigStore::loadFile();
        expect(loaded.maxEffects == 23 && loaded.toggleKey == "F8" && loaded.uiScale == 1.5f
                   && loaded.depthManualPin == "image-view-42" && loaded.depthFlop,
               "settings round trip through the file");

        // keys VkHook does not own and the user's comments survive, in order
        writeFile(confPath(), "# my notes about this setup\n"
                              "reshadeIncludePath = /opt/my shaders/include\n"
                              "maxEffects = 5\n"
                              "# keep this too\n"
                              "someFutureKey = \"a # b\"\n");
        AppSettings s = ConfigStore::loadFile();
        s.maxEffects = 31;
        expect(ConfigStore::writeFile(s), "save over a hand-edited VkHook.conf");
        const std::string text = readFile(confPath());
        const auto at = [&](const char* needle) { return text.find(needle); };
        expect(at("# my notes about this setup") == 0, "leading comment preserved in place");
        expect(at("reshadeIncludePath = /opt/my shaders/include") != std::string::npos, "unknown key preserved");
        expect(at("someFutureKey = \"a # b\"") != std::string::npos && at("# keep this too") != std::string::npos,
               "later comment and unknown key preserved");
        expect(at("maxEffects = 31") != std::string::npos && at("maxEffects = 5\n") == std::string::npos,
               "owned key updated in place");
        expect(at("reshadeIncludePath") < at("maxEffects = 31") && at("maxEffects = 31") < at("someFutureKey"),
               "key order preserved");
        const std::string before = text;
        expect(ConfigStore::writeFile(ConfigStore::loadFile()), "re-save unchanged settings");
        expect(readFile(confPath()) == before, "saving unchanged settings is idempotent");

        // CRLF endings stay CRLF
        writeFile(confPath(), "maxEffects = 4\r\nkeep = me\r\n");
        expect(ConfigStore::writeFile(ConfigStore::loadFile()), "save over a CRLF file");
        expect(readFile(confPath()).find("keep = me\r\n") != std::string::npos, "CRLF line preserved");

        // one tokenizer: Config and the store read the same file the same way
        writeFile(confPath(), "maxEffects = \"17\"   # quoted number\n"
                              "toggleKey = \"Home\"\n"
                              "enableOnLaunch = Off # comment\n"
                              "depthManualPin = \"pin # one\"\n"
                              "uiScale = 1.25\n");
        Config config(confPath());
        const AppSettings fromFile = ConfigStore::loadFile();
        expect(config.getOption<int>("maxEffects") == 17 && fromFile.maxEffects == 17, "quoted number with a comment");
        expect(config.getOption<std::string>("toggleKey") == fromFile.toggleKey && fromFile.toggleKey == "Home",
               "quoted string");
        expect(config.getOption<bool>("enableOnLaunch") == false && !fromFile.enableOnLaunch, "bool");
        expect(fromFile.depthManualPin == "pin # one", "a '#' inside quotes is not a comment");
        expect(fromFile.uiScale == 1.25f, "float");

        writeFile(confPath(), "enableOnLaunch = True\n");
        expect(ConfigStore::loadFile().enableOnLaunch, "capitalized True from older bundled configs");

        // the hot-reload watcher ignores VkHook's own writes but sees external edits
        writeFile(confPath(), "maxEffects = 9\n");
        Config watched(confPath());
        expect(!watched.hasConfigChanged(), "fresh config reports no change");
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        expect(writeAtomically(confPath(), "maxEffects = 9\n"), "own write");
        expect(!watched.hasConfigChanged(), "the watcher ignores VkHook's own write");
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        writeFile(confPath(), "maxEffects = 10\n");
        expect(watched.hasConfigChanged(), "the watcher sees an external edit");
        fs::remove(confPath());
    }

    void testSnapshotsAndQueuedSave()
    {
        fs::remove(confPath());
        ConfigStore store;
        store.initialize();
        const auto before = store.snapshot();
        const int oldMax = before->maxEffects;
        expect(store.set("maxEffects", oldMax == 17 ? 18 : 17), "set by key");
        const auto after = store.snapshot();
        expect(before->maxEffects == oldMax, "a snapshot taken earlier never changes");
        expect(after->maxEffects != oldMax && store.getMaxEffects() == after->maxEffects,
               "set publishes a new snapshot the accessors agree with");
        expect(before.get() != after.get(), "each publish is a distinct immutable object");
        store.set("maxEffects", 5000);
        expect(store.getMaxEffects() == 200, "the store clamps through the schema");
        expect(!store.set("noSuchKey", 1), "an unknown key is refused");

        std::future<bool> last;
        for (int i = 1; i <= 20; ++i)
        {
            store.set("autoApplyDelay", i * 10);
            last = store.save();
        }
        expect(last.get(), "the final queued settings save succeeds");
        PersistenceQueue::instance().waitForIdle();
        expect(ConfigStore::loadFile().autoApplyDelay == 200, "the newest settings reached the file");

        store.set("autoApplyDelay", 777);
        store.reload();
        expect(store.snapshot()->autoApplyDelay == 200, "reload re-reads the file and publishes it");
    }

    void testMigrations()
    {
        const fs::path base = Paths::baseConfigDir();
        fs::remove_all(base);

        // a pre-schema layout: packages tree, @default.conf, comma-separated manager lists
        writeFile(base / "reshade/packages/Pack/Shaders/a.fx", "legacy a");
        writeFile(base / "reshade/packages/Pack/Shaders/b.fx", "legacy b");
        writeFile(base / "reshade/Pack/Shaders/b.fx", "new b");
        writeFile(base / "configs/oot@default.conf", "effects = Bloom\n");
        writeFile(base / "configs/vkcube@default.conf", "effects = Old\n");
        writeFile(base / "configs/vkcube.conf", "effects = New\n");
        writeFile(base / "shader_manager.conf", "parentDir = /a\nshaderPath = /x, /y\ntexturePath = /t\n");
        writeFile(base / "VkHook.conf", "# mine\nmaxEffects = 12\n");

        expect(Migrate::storedVersion(base.string()) == 0, "no schemaVersion reads as version 0");
        expect(Migrate::run(base.string()) == Schema::kCurrentVersion, "migrations reach the current version");

        expect(readFile((base / "reshade/Pack/Shaders/a.fx").string()) == "legacy a", "packages merged into reshade/");
        expect(readFile((base / "reshade/Pack/Shaders/b.fx").string()) == "new b", "the newer layout wins");
        expect(!fs::exists(base / "reshade/packages"), "the legacy packages tree is removed");
        expect(fs::exists(base / "configs/oot.conf") && !fs::exists(base / "configs/oot@default.conf"),
               "<game>@default.conf renamed");
        expect(readFile((base / "configs/vkcube.conf").string()) == "effects = New\n",
               "an existing <game>.conf is never replaced");
        const std::string manager = readFile((base / "shader_manager.conf").string());
        expect(manager.find("shaderPath = /x\nshaderPath = /y\n") != std::string::npos,
               "comma-separated lists split one per line");
        const std::string conf = readFile((base / "VkHook.conf").string());
        expect(conf.find("# mine\nmaxEffects = 12\n") == 0 && conf.find("schemaVersion = 1") != std::string::npos,
               "the stamp is added without disturbing the user's file");
        expect(Migrate::storedVersion(base.string()) == 1, "the version is stored");

        // run once: a re-created legacy artifact is left alone at the current version
        writeFile(base / "configs/late@default.conf", "x");
        expect(Migrate::run(base.string()) == Schema::kCurrentVersion && fs::exists(base / "configs/late@default.conf"),
               "steps at or below the stored version do not run again");

        // order, stamping and stopping at the first failure
        static std::vector<int>* trace;
        std::vector<int> ran;
        trace = &ran;
        const Migrate::Step custom[] = {
            {2, "two", [](const std::string&) { trace->push_back(2); return true; }},
            {3, "three", [](const std::string&) { trace->push_back(3); return false; }},
            {4, "four", [](const std::string&) { trace->push_back(4); return true; }},
        };
        expect(Migrate::run(base.string(), custom) == 2, "a failing step stops the chain at the last good version");
        expect(ran == std::vector<int>({2, 3}), "steps run in ascending order and stop at the failure");
        expect(Migrate::storedVersion(base.string()) == 2, "the last good version is stamped");

        // a fresh install has nothing to migrate and still gets stamped
        fs::remove_all(base);
        expect(Migrate::run(base.string()) == Schema::kCurrentVersion, "a fresh config dir migrates cleanly");
        expect(Migrate::storedVersion(base.string()) == Schema::kCurrentVersion, "and is stamped");

    }
} // namespace

int main()
{
    char temporary[] = "/tmp/vkhook-config-store-tests-XXXXXX";
    char* tempDir = mkdtemp(temporary);
    if (!tempDir)
        return 1;
    setenv("XDG_CONFIG_HOME", tempDir, 1);
    setenv("HOME", tempDir, 1);
    setenv("VKHOOK_LOG_LEVEL", "none", 1);

    testDefaultsComeFromTheSchema();
    testClampingIsOneRule();
    testFileRoundTripAndReadModifyWrite();
    testSnapshotsAndQueuedSave();
    testMigrations();

    PersistenceQueue::instance().shutdown();
    fs::remove_all(tempDir);
    if (failures)
        std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
