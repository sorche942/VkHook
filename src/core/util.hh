#ifndef UTIL_HPP_INCLUDED
#define UTIL_HPP_INCLUDED

#include <string>
#include <sstream>
#include <vector>

namespace VkHook
{
    void addUniqueCString(std::vector<const char*>& stringVector, const char* addString);

    // Opens a file or url with the system handler: xdg-open normally, or
    // `flatpak-spawn --host xdg-open` inside a flatpak sandbox. never runs a
    // shell, so a url can't smuggle in extra arguments.
    bool openInShell(const char* url);

    // Writes contents to a sibling temp file and renames it over path, so a
    // crash mid-write leaves the previous file intact instead of truncated.
    bool writeAtomically(const std::string& path, const std::string& contents);

    // True when the file on disk is, byte for byte and by nanosecond mtime,
    // what the most recent writeAtomically() to that path produced. File
    // watchers use it to tell VkHook's own saves from the user's edits.
    bool isOwnWrite(const std::string& path);

    // XDG desktop portal file chooser, wrapped so the D-Bus plumbing lives in
    // one place. the request runs asynchronously; poll* is pumped once per
    // frame. start* returns false when no portal can be started (no GIO at
    // compile time, or no session bus), and the caller falls back to its own
    // in-process picker.
    enum class FileDialogResult
    {
        Success,
        Cancelled,
        Unavailable
    };

    enum class FileDialogKind
    {
        OpenFile,
        OpenDirectory
    };

    bool startOpenFileDialog(const std::string& title, const std::vector<std::string>& globFilters);
    bool startOpenDirectoryDialog(const std::string& title);
    // only reports a request of the given kind, so a dialog started from
    // another tab can't be mistaken for this one's result.
    bool pollFileDialog(FileDialogKind kind, FileDialogResult& outResult, std::string& outPath);
    bool fileDialogPending();

    template<typename T>
    std::string convertToString(T object)
    {
        std::stringstream ss;
        ss << object;
        return ss.str();
    }
} // namespace VkHook

#endif // UTIL_HPP_INCLUDED
