#include "wayland_interpose_patch.hh"

#include "logger.hh"

#include <wayland-client.h>

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
    constexpr int kMaxCopies = 8;

    struct PatchedCopy
    {
        uintptr_t lo;
        uintptr_t hi;
        void* real;
    };

    PatchedCopy g_copies[kMaxCopies];
    std::atomic<int> g_copyCount{0};
    std::mutex g_installMutex;
    std::atomic<int64_t> g_lastScanNs{0};

    bool nameMatches(const char* path, const char* needle)
    {
        return path && std::strstr(path, needle) != nullptr;
    }

    // glibc rewrites DT_SYMTAB/DT_STRTAB/DT_JMPREL values in place to absolute
    // addresses at load time; a rare configuration leaves them relative.
    // Accept both; the caller validates the resulting address against PT_LOAD.
    uintptr_t normalize(uintptr_t base, uintptr_t value)
    {
        if (value < base)
            return value <= UINTPTR_MAX - base ? base + value : 0;
        return value;
    }

    // restore the protection the page had before we touched it, read straight
    // from /proc/self/maps (RELRO got pages are read-only)
    void restoreProtection(uintptr_t pageAddr, size_t pageSize)
    {
        int flags = PROT_READ;
        if (FILE* maps = std::fopen("/proc/self/maps", "re"))
        {
            char line[512];
            while (std::fgets(line, sizeof(line), maps))
            {
                unsigned long start = 0;
                unsigned long end = 0;
                char perms[8] = {0};
                if (std::sscanf(line, "%lx-%lx %7s", &start, &end, perms) == 3 && start <= pageAddr && pageAddr < end)
                {
                    flags = 0;
                    if (perms[0] == 'r') flags |= PROT_READ;
                    if (perms[1] == 'w') flags |= PROT_WRITE;
                    if (perms[2] == 'x') flags |= PROT_EXEC;
                    if (flags == 0)
                        flags = PROT_READ;
                    break;
                }
            }
            std::fclose(maps);
        }
        mprotect(reinterpret_cast<void*>(pageAddr), pageSize, flags);
    }

    struct Scan
    {
        const char* symbol;
        void* replacement;
        bool probeOnly;
        bool stopped;
        void** foundSlot;
        void** foundReal;
    };

    void tryPatchObject(struct dl_phdr_info* info, Scan& scan)
    {
        const uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);

        // already gated? (same base means same load)
        const int known = g_copyCount.load(std::memory_order_acquire);
        for (int i = 0; i < known; ++i)
        {
            if (g_copies[i].lo == base)
                return;
        }

        // object load ranges + dynamic segment
        uintptr_t lo = UINTPTR_MAX;
        uintptr_t hi = 0;
        const ElfW(Phdr)* dynPhdr = nullptr;
        for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i)
        {
            const ElfW(Phdr)& ph = info->dlpi_phdr[i];
            if (ph.p_type == PT_LOAD && ph.p_memsz)
            {
                if (ph.p_vaddr > UINTPTR_MAX - base)
                    return;
                const uintptr_t segLo = base + static_cast<uintptr_t>(ph.p_vaddr);
                if (ph.p_memsz > UINTPTR_MAX - segLo)
                    return;
                const uintptr_t segHi = segLo + static_cast<uintptr_t>(ph.p_memsz);
                lo = segLo < lo ? segLo : lo;
                hi = segHi > hi ? segHi : hi;
            }
            else if (ph.p_type == PT_DYNAMIC)
            {
                dynPhdr = &ph;
            }
        }
        if (!dynPhdr || lo >= hi)
            return;

        auto inRange = [&](uintptr_t p, size_t len) {
            if (p < lo || len > hi - lo || p > hi - len)
                return false;
            // The envelope can contain unmapped gaps between load segments.
            for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i)
            {
                const ElfW(Phdr)& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_LOAD || !ph.p_memsz || !(ph.p_flags & PF_R))
                    continue;
                const uintptr_t start = base + static_cast<uintptr_t>(ph.p_vaddr);
                if (p >= start && len <= ph.p_memsz && p - start <= ph.p_memsz - len)
                    return true;
            }
            return false;
        };
        auto ptrIn = [&](const void* p, size_t len) {
            return inRange(reinterpret_cast<uintptr_t>(p), len);
        };

        if (dynPhdr->p_vaddr > UINTPTR_MAX - base)
            return;
        const auto* dynamic = reinterpret_cast<const ElfW(Dyn)*>(base + static_cast<uintptr_t>(dynPhdr->p_vaddr));
        if (!ptrIn(dynamic, dynPhdr->p_memsz))
            return;

        const ElfW(Sym)* symtab = nullptr;
        const char* strtab = nullptr;
        const void* jmprel = nullptr;
        const ElfW(Rel)* rels = nullptr;
        const ElfW(Rela)* relas = nullptr;
        size_t strsz = 0;
        size_t pltrelsz = 0;
        size_t relsz = 0;
        size_t relasz = 0;
        size_t relent = 0;
        size_t relaent = 0;
        ElfW(Xword) pltrel = DT_NULL;
        bool terminated = false;
        const size_t dynamicCount = dynPhdr->p_memsz / sizeof(ElfW(Dyn));
        for (size_t i = 0; i < dynamicCount; ++i)
        {
            const ElfW(Dyn)& dyn = dynamic[i];
            if (dyn.d_tag == DT_NULL)
            {
                terminated = true;
                break;
            }
            switch (dyn.d_tag)
            {
                case DT_SYMTAB: symtab = reinterpret_cast<const ElfW(Sym)*>(normalize(base, dyn.d_un.d_ptr)); break;
                case DT_STRTAB: strtab = reinterpret_cast<const char*>(normalize(base, dyn.d_un.d_ptr)); break;
                case DT_STRSZ: strsz = static_cast<size_t>(dyn.d_un.d_val); break;
                case DT_JMPREL: jmprel = reinterpret_cast<const void*>(normalize(base, dyn.d_un.d_ptr)); break;
                case DT_PLTRELSZ: pltrelsz = static_cast<size_t>(dyn.d_un.d_val); break;
                case DT_PLTREL: pltrel = dyn.d_un.d_val; break;
                case DT_REL: rels = reinterpret_cast<const ElfW(Rel)*>(normalize(base, dyn.d_un.d_ptr)); break;
                case DT_RELSZ: relsz = static_cast<size_t>(dyn.d_un.d_val); break;
                case DT_RELENT: relent = static_cast<size_t>(dyn.d_un.d_val); break;
                case DT_RELA: relas = reinterpret_cast<const ElfW(Rela)*>(normalize(base, dyn.d_un.d_ptr)); break;
                case DT_RELASZ: relasz = static_cast<size_t>(dyn.d_un.d_val); break;
                case DT_RELAENT: relaent = static_cast<size_t>(dyn.d_un.d_val); break;
                default: break;
            }
        }

        if (!terminated || !symtab || !strtab || strsz == 0 ||
            !ptrIn(symtab, sizeof(ElfW(Sym))) || !ptrIn(strtab, strsz))
            return;

        // dl_iterate_phdr exposes only the process's native ELF class. i386
        // normally uses REL and x86_64 RELA; DT_PLTREL, not the CPU, selects
        // the actual PLT entry layout.
#if defined(__i386__)
        constexpr unsigned jumpSlotType = R_386_JMP_SLOT;
        constexpr unsigned globalDataType = R_386_GLOB_DAT;
#elif defined(__x86_64__) && UINTPTR_MAX == UINT64_MAX
        constexpr unsigned jumpSlotType = R_X86_64_JUMP_SLOT;
        constexpr unsigned globalDataType = R_X86_64_GLOB_DAT;
#else
#error "Wayland GOT interposition requires i386 or x86_64 ELF"
#endif

        const size_t wanted = std::strlen(scan.symbol);
        auto scanRelocations = [&](const auto* relocations, size_t bytes, unsigned slotType) {
            const size_t entrySize = sizeof(*relocations);
            const size_t count = bytes / entrySize;
            if (!relocations || bytes % entrySize != 0 || count == 0 ||
                count > 65536 || !ptrIn(relocations, bytes))
                return false;

            for (size_t i = 0; i < count; ++i)
            {
                const auto& rel = relocations[i];
#if defined(__i386__)
                const unsigned type = ELF32_R_TYPE(rel.r_info);
                const size_t symbolIndex = ELF32_R_SYM(rel.r_info);
#else
                const unsigned type = ELF64_R_TYPE(rel.r_info);
                const size_t symbolIndex = ELF64_R_SYM(rel.r_info);
#endif
                if (type != slotType)
                    continue;

                const uintptr_t symtabAddr = reinterpret_cast<uintptr_t>(symtab);
                if (symbolIndex > (hi - symtabAddr) / sizeof(ElfW(Sym)))
                    continue;
                const auto* sym = reinterpret_cast<const ElfW(Sym)*>(symtabAddr + symbolIndex * sizeof(ElfW(Sym)));
                if (!ptrIn(sym, sizeof(ElfW(Sym))) || sym->st_name >= strsz ||
                    wanted >= strsz - sym->st_name)
                    continue;

                const char* name = strtab + sym->st_name;
                if (std::memcmp(name, scan.symbol, wanted) != 0 || name[wanted] != '\0')
                    continue;

                if (rel.r_offset > UINTPTR_MAX - base)
                    continue;
                const uintptr_t slotAddr = base + static_cast<uintptr_t>(rel.r_offset);
                if (slotAddr % alignof(void*) != 0 ||
                    !ptrIn(reinterpret_cast<const void*>(slotAddr), sizeof(void*)))
                    continue;

                auto* slot = reinterpret_cast<void**>(slotAddr);
                void* current = *slot;

                // Only gate a resolved entry into this copy of libwayland.
                // A lazy PLT stub also belongs to this object, so require the
                // actual symbol address rather than just a matching load base.
                Dl_info resolved{};
                if (sym->st_shndx == SHN_UNDEF || sym->st_value > UINTPTR_MAX - base ||
                    reinterpret_cast<uintptr_t>(current) != base + sym->st_value ||
                    dladdr(current, &resolved) == 0 ||
                    reinterpret_cast<uintptr_t>(resolved.dli_fbase) != base)
                    continue;

                if (scan.probeOnly)
                {
                    if (scan.foundSlot)
                        *scan.foundSlot = slot;
                    if (scan.foundReal)
                        *scan.foundReal = current;
                    scan.stopped = true;
                    return true;
                }

                const int index = g_copyCount.load(std::memory_order_relaxed);
                if (index >= kMaxCopies)
                    return true;

                const long pageSizeLong = sysconf(_SC_PAGESIZE);
                const size_t pageSize = pageSizeLong > 0 ? static_cast<size_t>(pageSizeLong) : 4096;
                const uintptr_t pageAddr = slotAddr & ~static_cast<uintptr_t>(pageSize - 1);
                if (mprotect(reinterpret_cast<void*>(pageAddr), pageSize, PROT_READ | PROT_WRITE) != 0)
                    return true;

                // publish the real entry before any thread can reach the patched slot
                g_copies[index] = {lo, hi, current};
                g_copyCount.store(index + 1, std::memory_order_release);
                *slot = scan.replacement;

                restoreProtection(pageAddr, pageSize);

                VkHook::Logger::info("Wayland interpose: gated the marshal entry in " + std::string(info->dlpi_name));
                return true;
            }
            return false;
        };

        if (pltrel == DT_REL &&
            scanRelocations(static_cast<const ElfW(Rel)*>(jmprel), pltrelsz, jumpSlotType))
            return;
        if (pltrel == DT_RELA &&
            scanRelocations(static_cast<const ElfW(Rela)*>(jmprel), pltrelsz, jumpSlotType))
            return;

        // -fno-plt builds call through GLOB_DAT GOT entries instead of JUMP_SLOT.
        if (relent == sizeof(ElfW(Rel)) && scanRelocations(rels, relsz, globalDataType))
            return;
        if (relaent == sizeof(ElfW(Rela)))
            scanRelocations(relas, relasz, globalDataType);
    }

    int scanCallback(struct dl_phdr_info* info, size_t, void* data)
    {
        auto* scan = static_cast<Scan*>(data);
        if (nameMatches(info->dlpi_name, "libwayland-client.so"))
            tryPatchObject(info, *scan);
        return scan->stopped ? 1 : 0;
    }
} // namespace

namespace VkHook
{
    void* waylandMarshalArrayFlagsForCaller(void* caller)
    {
        const uintptr_t pc = reinterpret_cast<uintptr_t>(caller);
        const int count = g_copyCount.load(std::memory_order_acquire);
        for (int i = 0; i < count; ++i)
        {
            if (pc >= g_copies[i].lo && pc < g_copies[i].hi)
                return g_copies[i].real;
        }
        return count > 0 ? g_copies[0].real : nullptr;
    }

    void installWaylandInterposeHooks()
    {
        // called before every present to catch libwayland copies that load
        // after us; scanning walks every loaded object, so keep it to a
        // once-a-second cadence instead of once a frame
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        int64_t last = g_lastScanNs.load(std::memory_order_relaxed);
        if (last != 0 && now - last < 1'000'000'000LL)
            return;
        if (!g_lastScanNs.compare_exchange_strong(last, now, std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> lock(g_installMutex);
        Scan scan{"wl_proxy_marshal_array_flags",
                  reinterpret_cast<void*>(&wl_proxy_marshal_array_flags),
                  false, false, nullptr, nullptr};
        dl_iterate_phdr(scanCallback, &scan);
    }

    bool waylandMarshalSlotForTests(void** slotOut, void** realOut)
    {
        std::lock_guard<std::mutex> lock(g_installMutex);
        Scan scan{"wl_proxy_marshal_array_flags", nullptr, true, false, slotOut, realOut};
        dl_iterate_phdr(scanCallback, &scan);
        return scan.stopped;
    }
} // namespace VkHook
