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


#include <agentic/core/file_store.h>
#include <agentic/protocol/registry.h>

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif


namespace AGENTIC
{

namespace fs = std::filesystem;

namespace
{

    bool isUnder( const fs::path& aPath, const fs::path& aRoot )
    {
        auto [rootEnd, pathIt] = std::mismatch( aRoot.begin(), aRoot.end(), aPath.begin(), aPath.end() );
        return rootEnd == aRoot.end();
    }


    std::string extensionOf( const std::string& aRelative )
    {
        const size_t slash = aRelative.find_last_of( '/' );
        const size_t dot = aRelative.find_last_of( '.' );

        if( dot == std::string::npos || ( slash != std::string::npos && dot < slash ) )
            return std::string();

        std::string ext = aRelative.substr( dot + 1 );
        std::transform( ext.begin(), ext.end(), ext.begin(),
                        []( unsigned char c )
                        {
                            return static_cast<char>( std::tolower( c ) );
                        } );
        return ext;
    }


    std::string fileName( const std::string& aRelative )
    {
        const size_t slash = aRelative.find_last_of( '/' );
        return slash == std::string::npos ? aRelative : aRelative.substr( slash + 1 );
    }


    /// Unique per process and call: a forked child must not reuse its parent's names.
    std::string tempSuffix()
    {
        static std::atomic<unsigned> counter{ 0 };
        std::random_device           random;
        std::ostringstream           out;
#ifndef _WIN32
        out << std::hex << ::getpid() << '-';
#endif
        out << std::hex << counter++ << '-' << random();
        return out.str();
    }


    [[noreturn]] void ioError( const std::string& aWhat, const fs::path& aPath, int aErrno )
    {
        throw TOOL_ERROR( aWhat + " " + aPath.string() + ": " + std::strerror( aErrno ) );
    }

} // namespace


FILE_STORE::FILE_STORE( const fs::path& aRoot )
{
    std::error_code ec;
    m_root = fs::canonical( aRoot, ec );

    if( ec || !fs::is_directory( m_root ) )
        throw TOOL_ERROR( "Project folder " + aRoot.string() + " does not exist or is not a folder." );
}


fs::path FILE_STORE::Resolve( const std::string& aRelative ) const
{
    const fs::path relative( aRelative );

    if( relative.is_absolute() || relative.has_root_name() )
        throw TOOL_ERROR( "Paths are relative to the project folder; '" + aRelative + "' is absolute." );

    std::error_code ec;
    fs::path        resolved = fs::weakly_canonical( m_root / relative, ec );

    if( ec )
        throw TOOL_ERROR( "Cannot resolve '" + aRelative + "': " + ec.message() );

    if( !isUnder( resolved, m_root ) )
        throw TOOL_ERROR( "'" + aRelative
                          + "' is outside the project folder. Tools can only reach files "
                            "inside the project." );

    return resolved;
}


std::string FILE_STORE::Relative( const fs::path& aAbsolute ) const
{
    fs::path    rel = aAbsolute.lexically_relative( m_root );
    std::string text = rel.generic_string();
    return text == "." ? std::string() : text;
}


bool FILE_STORE::IsDesignFile( const std::string& aRelative )
{
    static const std::array<const char*, 11> exts = { "kicad_pro",    "kicad_sch", "kicad_pcb", "kicad_sym",
                                                      "kicad_mod",    "kicad_wks", "kicad_prl", "kicad_dru",
                                                      "kicad_jobset", "sch",       "brd" };

    const std::string ext = extensionOf( aRelative );

    if( std::find( exts.begin(), exts.end(), ext ) != exts.end() )
        return true;

    const std::string name = fileName( aRelative );
    return name == "sym-lib-table" || name == "fp-lib-table" || name == "design-block-lib-table";
}


bool FILE_STORE::IsReserved( const std::string& aRelative )
{
    return aRelative == ".agentic/history" || aRelative.rfind( ".agentic/history/", 0 ) == 0;
}


std::vector<FILE_STORE::ENTRY> FILE_STORE::List( const std::string& aDir, bool aRecursive ) const
{
    const fs::path dir = Resolve( aDir );

    if( !fs::is_directory( dir ) )
        throw TOOL_ERROR( "'" + aDir + "' is not a folder in the project." );

    std::vector<ENTRY> entries;

    auto add = [&]( const fs::directory_entry& aEntry )
    {
        const std::string rel = Relative( aEntry.path() );
        const std::string name = fileName( rel );

        const bool leftover = name.size() > 4 && name[0] == '.' && name.ends_with( ".tmp" );

        if( IsReserved( rel ) || name.rfind( "~", 0 ) == 0 || extensionOf( rel ) == "lck" || leftover )
            return;

        ENTRY entry;
        entry.path = rel;
        entry.directory = aEntry.is_directory();
        entry.size = entry.directory ? 0 : aEntry.file_size();
        entry.design = !entry.directory && IsDesignFile( rel );
        entries.push_back( entry );
    };

    std::error_code ec;

    if( aRecursive )
    {
        for( auto it = fs::recursive_directory_iterator( dir, ec ); !ec && it != fs::recursive_directory_iterator();
             it.increment( ec ) )
        {
            if( IsReserved( Relative( it->path() ) ) )
            {
                it.disable_recursion_pending();
                continue;
            }

            add( *it );
        }
    }
    else
    {
        for( const fs::directory_entry& entry : fs::directory_iterator( dir, ec ) )
            add( entry );
    }

    std::sort( entries.begin(), entries.end(),
               []( const ENTRY& a, const ENTRY& b )
               {
                   return a.path < b.path;
               } );

    return entries;
}


std::string FILE_STORE::VersionOf( const std::string& aContent )
{
    return picosha2::hash256_hex_string( aContent ).substr( 0, 16 );
}


std::string FILE_STORE::Version( const std::string& aRelative ) const
{
    const fs::path path = Resolve( aRelative );

    if( !fs::is_regular_file( path ) )
        return std::string();

    return VersionOf( ReadFile( path ) );
}


std::string FILE_STORE::ReadFile( const fs::path& aPath )
{
    std::ifstream in( aPath, std::ios::binary );

    if( !in )
        throw TOOL_ERROR( "Cannot read " + aPath.filename().string() + ": no such file or not readable." );

    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}


std::string FILE_STORE::Read( const std::string& aRelative ) const
{
    const fs::path path = Resolve( aRelative );

    if( fs::is_directory( path ) )
        throw TOOL_ERROR( "'" + aRelative + "' is a folder; use file_list." );

    if( !fs::exists( path ) )
        throw TOOL_ERROR( "'" + aRelative + "' does not exist in the project." );

    return ReadFile( path );
}


void FILE_STORE::WriteAtomic( const fs::path& aPath, const std::string& aContent )
{
    std::error_code ec;
    fs::create_directories( aPath.parent_path(), ec );

    fs::path temp;

#ifndef _WIN32
    int fd = -1;

    // A name taken by a leftover from a killed writer is skipped, not fatal.
    for( int attempt = 0; attempt < 8 && fd < 0; ++attempt )
    {
        temp = aPath.parent_path() / ( "." + aPath.filename().string() + "." + tempSuffix() + ".tmp" );
        fd = ::open( temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644 );

        if( fd < 0 && errno != EEXIST )
            break;
    }

    if( fd < 0 )
        ioError( "Cannot create", temp, errno );

    // Keep the target's permissions when it exists.
    struct stat st;

    if( ::stat( aPath.c_str(), &st ) == 0 )
        ::fchmod( fd, st.st_mode & 07777 );

    const char* data = aContent.data();
    size_t      left = aContent.size();

    while( left > 0 )
    {
        ssize_t n = ::write( fd, data, left );

        if( n < 0 )
        {
            if( errno == EINTR )
                continue;

            int err = errno;
            ::close( fd );
            ::unlink( temp.c_str() );
            ioError( "Cannot write", aPath, err );
        }

        data += n;
        left -= static_cast<size_t>( n );
    }

    if( ::fsync( fd ) != 0 || ::close( fd ) != 0 )
    {
        int err = errno;
        ::unlink( temp.c_str() );
        ioError( "Cannot flush", aPath, err );
    }

    if( ::rename( temp.c_str(), aPath.c_str() ) != 0 )
    {
        int err = errno;
        ::unlink( temp.c_str() );
        ioError( "Cannot replace", aPath, err );
    }

    int dirFd = ::open( aPath.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC );

    if( dirFd >= 0 )
    {
        ::fsync( dirFd );
        ::close( dirFd );
    }
#else
    temp = aPath.parent_path() / ( "." + aPath.filename().string() + "." + tempSuffix() + ".tmp" );

    {
        std::ofstream out( temp, std::ios::binary | std::ios::trunc );
        out.write( aContent.data(), static_cast<std::streamsize>( aContent.size() ) );
        out.flush();

        if( !out )
        {
            fs::remove( temp, ec );
            throw TOOL_ERROR( "Cannot write " + aPath.string() );
        }
    }

    fs::rename( temp, aPath, ec );

    if( ec )
    {
        fs::remove( temp, ec );
        throw TOOL_ERROR( "Cannot replace " + aPath.string() + ": " + ec.message() );
    }
#endif
}


std::string FILE_STORE::Write( const std::string& aRelative, const std::string& aContent,
                               const std::optional<std::string>& aBaseVersion )
{
    const fs::path    path = Resolve( aRelative );
    const std::string rel = Relative( path );

    if( IsReserved( rel ) )
        throw TOOL_ERROR( "'" + aRelative + "' is the agent's undo history; it is managed by undo and redo." );

    if( IsDesignFile( rel ) )
        throw TOOL_ERROR( "'" + aRelative
                          + "' is a KiCad design file. Change it with the structured tools of "
                            "its program, not with file_write." );

    if( fs::is_directory( path ) )
        throw TOOL_ERROR( "'" + aRelative + "' is a folder." );

    if( aBaseVersion )
    {
        const std::string current = Version( rel );

        if( current != *aBaseVersion )
        {
            throw TOOL_ERROR( "'" + aRelative + "' changed since version "
                                      + ( aBaseVersion->empty() ? "(none)" : *aBaseVersion ) + "; it is now "
                                      + ( current.empty() ? "missing" : current )
                                      + ". Read it again and redo the edit.",
                              JSON{ { "conflict", true }, { "path", rel }, { "version", current } } );
        }
    }

    WriteAtomic( path, aContent );
    return VersionOf( aContent );
}


void FILE_STORE::Rename( const std::string& aFrom, const std::string& aTo, bool aOverwrite )
{
    const fs::path from = Resolve( aFrom );
    const fs::path to = Resolve( aTo );

    for( const std::string& rel : { Relative( from ), Relative( to ) } )
    {
        if( IsReserved( rel ) )
            throw TOOL_ERROR( "'" + rel + "' is the agent's undo history." );

        if( IsDesignFile( rel ) )
            throw TOOL_ERROR( "'" + rel + "' is a KiCad design file; file tools do not rename design files." );
    }

    if( !fs::exists( from ) )
        throw TOOL_ERROR( "'" + aFrom + "' does not exist in the project." );

    if( fs::exists( to ) && !aOverwrite )
        throw TOOL_ERROR( "'" + aTo + "' exists; pass overwrite:true to replace it." );

    std::error_code ec;
    fs::create_directories( to.parent_path(), ec );
    fs::rename( from, to, ec );

    if( ec )
        throw TOOL_ERROR( "Cannot rename '" + aFrom + "' to '" + aTo + "': " + ec.message() );
}


void FILE_STORE::Remove( const std::string& aRelative )
{
    const fs::path    path = Resolve( aRelative );
    const std::string rel = Relative( path );

    if( rel.empty() )
        throw TOOL_ERROR( "The project folder itself cannot be deleted." );

    if( IsReserved( rel ) || rel == ".agentic" )
        throw TOOL_ERROR( "'" + rel + "' is the agent's own state." );

    if( IsDesignFile( rel ) )
        throw TOOL_ERROR( "'" + rel + "' is a KiCad design file; file tools do not delete design files." );

    if( !fs::exists( path ) )
        throw TOOL_ERROR( "'" + aRelative + "' does not exist in the project." );

    if( fs::is_directory( path ) && !fs::is_empty( path ) )
        throw TOOL_ERROR( "'" + aRelative + "' is a folder that is not empty." );

    std::error_code ec;
    fs::remove( path, ec );

    if( ec )
        throw TOOL_ERROR( "Cannot delete '" + aRelative + "': " + ec.message() );
}

} // namespace AGENTIC
