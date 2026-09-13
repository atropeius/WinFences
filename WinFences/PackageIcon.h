#pragma once
// ============================================================================
// PackageIcon.h — Logo assets of a packaged (Store/MSIX) app, read from disk.
//
// Why this exists: for a packaged app the shell serves the app-list icon, and
// Windows prefers the "altform-unplated" variant for that. Publishers often
// ship only a small unplated file — Claude ships one at 24x24 — so at anything
// above a small icon size the shell hands back an upscale, while a far better
// asset sits unused in the same folder (Claude also ships 88x88, and Owlfiles,
// WhatsApp and Solitaire each ship a 256x256 app-list logo).
//
// So for packaged items the package itself is the better source, and unlike the
// shell it states its resolution: the file's own pixel size. That number is the
// only reliable one available — asking the shell how big its source is does not
// work, it reports 244px for an icon that is visibly an upscale.
//
// Everything here is read-only and local; nothing is installed or modified.
// ============================================================================
#include "pch.h"
#include <appmodel.h>

class PackageIcon
{
public:
    // Best logo file for an Application User Model ID ("Family_hash!AppId"),
    // or empty when the item is not a packaged app or ships nothing usable.
    // `px` receives the asset's pixel width.
    static std::wstring FindLogo(const std::wstring& aumid, int& px)
    {
        px = 0;

        const size_t bang = aumid.find(L'!');
        if (bang == std::wstring::npos || bang == 0) return {};
        const std::wstring family = aumid.substr(0, bang);

        std::wstring root = InstallPathForFamily(family);
        if (root.empty()) return {};

        std::wstring declared = DeclaredSquare44Logo(root);
        if (declared.empty()) return {};

        return LargestVariant(root, declared, px);
    }

private:
    // Install folder of the first package in the family, via the packaging API.
    static std::wstring InstallPathForFamily(const std::wstring& family)
    {
        UINT32 count = 0, bufLen = 0;
        if (GetPackagesByPackageFamily(family.c_str(), &count, nullptr, &bufLen, nullptr)
                != ERROR_INSUFFICIENT_BUFFER || count == 0)
            return {};

        std::vector<PWSTR>   names(count);
        std::vector<wchar_t> buf(bufLen);
        if (GetPackagesByPackageFamily(family.c_str(), &count, names.data(),
                &bufLen, buf.data()) != ERROR_SUCCESS || count == 0)
            return {};

        UINT32 pathLen = 0;
        if (GetPackagePathByFullName(names[0], &pathLen, nullptr)
                != ERROR_INSUFFICIENT_BUFFER)
            return {};

        std::wstring path(pathLen, L'\0');
        if (GetPackagePathByFullName(names[0], &pathLen, path.data()) != ERROR_SUCCESS)
            return {};
        if (!path.empty() && path.back() == L'\0') path.pop_back();
        return path;
    }

    // The Square44x44Logo attribute of <uap:VisualElements>, e.g.
    // "Assets\Square44x44Logo.png". Deliberately a targeted text scan rather
    // than a full XML parse: one attribute does not justify pulling MSXML in,
    // and the attribute name is unique within an AppxManifest.
    static std::wstring DeclaredSquare44Logo(const std::wstring& root)
    {
        std::ifstream f(std::filesystem::path(root) / L"AppxManifest.xml",
                        std::ios::binary);
        if (!f) return {};
        std::string xml((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

        const std::string key = "Square44x44Logo=\"";
        size_t p = xml.find(key);
        if (p == std::string::npos) return {};
        p += key.size();
        size_t end = xml.find('"', p);
        if (end == std::string::npos) return {};

        const std::string value = xml.substr(p, end - p);
        return std::wstring(value.begin(), value.end()); // manifest paths are ASCII
    }

    // Largest sibling of the declared logo. Publishers place the high-resolution
    // forms next to it as "<stem>.targetsize-256.png", "<stem>.scale-200.png",
    // "<stem>.altform-lightunplated_targetsize-256.png" and so on, so every file
    // starting with the same stem is a candidate. The winner is decided by
    // actually decoding each candidate's size rather than by parsing the name:
    // the naming is not consistent between publishers, and the file knows best.
    static std::wstring LargestVariant(const std::wstring& root,
                                       const std::wstring& declared, int& px)
    {
        std::filesystem::path rel(declared);
        const std::filesystem::path dir  = std::filesystem::path(root) / rel.parent_path();
        const std::wstring stem = rel.stem().wstring();
        const std::wstring ext  = rel.extension().wstring();

        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return {};

        std::wstring best;
        int bestPx = 0;

        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;

            const std::wstring name = e.path().filename().wstring();
            if (name.size() < stem.size()
                || _wcsnicmp(name.c_str(), stem.c_str(), stem.size()) != 0)
                continue;
            if (_wcsicmp(e.path().extension().wstring().c_str(), ext.c_str()) != 0)
                continue;

            int w = PngWidth(e.path().wstring());
            if (w > bestPx) { bestPx = w; best = e.path().wstring(); }
        }

        px = bestPx;
        return best;
    }

    // Width from the PNG header — 8 byte signature, then an IHDR whose width is
    // a big-endian uint32 at offset 16. Cheap enough to run over every candidate.
    static int PngWidth(const std::wstring& file)
    {
        std::ifstream f(std::filesystem::path(file), std::ios::binary);
        if (!f) return 0;
        unsigned char h[24] = {};
        f.read(reinterpret_cast<char*>(h), sizeof(h));
        if (f.gcount() < static_cast<std::streamsize>(sizeof(h))) return 0;

        static const unsigned char sig[8] =
            { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (memcmp(h, sig, sizeof(sig)) != 0) return 0;

        return (h[16] << 24) | (h[17] << 16) | (h[18] << 8) | h[19];
    }
};
