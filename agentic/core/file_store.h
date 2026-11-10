/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#ifndef AGENTIC_FILE_STORE_H
#define AGENTIC_FILE_STORE_H

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/**
 * The project sandbox (D8).  Every path a tool names is relative to the project root and is
 * resolved here; symlinks are followed and nothing outside the root is reachable.  Writes
 * are atomic (sibling temporary file, fsync, rename) and files are versioned by a hash of
 * their content, so a writer can say which version it expects to replace.
 *
 * No wx dependency.  All paths given to and returned by the public methods are relative to
 * the root and use '/'.
 */
namespace AGENTIC
{

class FILE_STORE
{
public:
    /// @throw TOOL_ERROR when \a aRoot is not an existing directory.
    explicit FILE_STORE( const std::filesystem::path& aRoot );

    const std::filesystem::path& Root() const { return m_root; }

    /**
     * The absolute path of \a aRelative, which must stay inside the root once symlinks are
     * followed.  The file need not exist.
     *
     * @throw TOOL_ERROR for absolute paths, ".." escapes and symlinks out of the root.
     */
    std::filesystem::path Resolve( const std::string& aRelative ) const;

    /// The root-relative form of an absolute path inside the root, with '/' separators.
    std::string Relative( const std::filesystem::path& aAbsolute ) const;

    /// KiCad design and settings files: readable, but written only by structured tools.
    static bool IsDesignFile( const std::string& aRelative );

    /// Fork state that file tools must not touch (the history of D7).
    static bool IsReserved( const std::string& aRelative );

    struct ENTRY
    {
        std::string path; ///< Relative to the root.
        bool        directory = false;
        uintmax_t   size = 0;
        bool        design = false;
    };

    /// Entries under \a aDir (relative, "" for the root), sorted; lock files and the
    /// reserved history are left out.
    std::vector<ENTRY> List( const std::string& aDir, bool aRecursive ) const;

    /// Content version of a file: the first 16 hex digits of its SHA-256; empty if missing.
    std::string Version( const std::string& aRelative ) const;

    static std::string VersionOf( const std::string& aContent );

    /// @throw TOOL_ERROR when the file is missing or unreadable.
    std::string Read( const std::string& aRelative ) const;

    /**
     * Replace or create a file atomically.  When \a aBaseVersion is given, the file's current
     * version must equal it (empty for "must not exist"), or a conflict is thrown with the
     * current version in the error data.
     *
     * @return the new version.
     * @throw TOOL_ERROR on conflict or I/O failure; the old file is then untouched.
     */
    std::string Write( const std::string& aRelative, const std::string& aContent,
                       const std::optional<std::string>& aBaseVersion = std::nullopt );

    void Rename( const std::string& aFrom, const std::string& aTo, bool aOverwrite );
    void Remove( const std::string& aRelative );

    /// Atomic write of an absolute path, used for design files by their sessions and history.
    static void WriteAtomic( const std::filesystem::path& aPath, const std::string& aContent );

    /// Whole content of an absolute path.  @throw TOOL_ERROR when unreadable.
    static std::string ReadFile( const std::filesystem::path& aPath );

private:
    std::filesystem::path m_root;
};

} // namespace AGENTIC

#endif // AGENTIC_FILE_STORE_H
