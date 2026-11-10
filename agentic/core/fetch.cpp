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


#include <agentic/core/fetch.h>
#include <agentic/core/file_store.h>
#include <agentic/protocol/registry.h>

#include <kicad_curl/kicad_curl_easy.h>

#include <filesystem>
#include <sstream>


namespace AGENTIC
{

std::string FileUrlPath( const std::string& aUrl )
{
    const std::string prefix = "file://";

    if( aUrl.rfind( prefix, 0 ) != 0 )
        return std::string();

    return aUrl.substr( prefix.size() );
}


std::string Fetch( const std::string& aUrl, size_t aMaxBytes, const std::function<bool( double )>& aProgress )
{
    if( const std::string path = FileUrlPath( aUrl ); !path.empty() )
    {
        std::error_code ec;

        if( !std::filesystem::is_regular_file( path, ec ) )
            throw TOOL_ERROR( "No file at " + aUrl + "." );

        if( std::filesystem::file_size( path, ec ) > aMaxBytes )
            throw TOOL_ERROR( aUrl + " is larger than the " + std::to_string( aMaxBytes ) + " bytes allowed." );

        return FILE_STORE::ReadFile( path );
    }

    if( aUrl.rfind( "https://", 0 ) != 0 && aUrl.rfind( "http://", 0 ) != 0 )
        throw TOOL_ERROR( "Only http(s) and file:// URLs can be fetched; got " + aUrl + "." );

    std::stringstream out;
    bool              tooLarge = false;
    bool              aborted = false;

    KICAD_CURL_EASY curl;
    curl.SetURL( aUrl );
    curl.SetFollowRedirects( true );
    curl.SetOutputStream( &out );
    curl.SetConnectTimeout( 20 );
    curl.SetStallTimeout( 100L, 30L );
    curl.SetTransferCallback(
            [&]( size_t aTotal, size_t aNow, size_t, size_t ) -> int
            {
                if( aNow > aMaxBytes || aTotal > aMaxBytes )
                {
                    tooLarge = true;
                    return 1;
                }

                if( aProgress && !aProgress( aTotal > 0 ? double( aNow ) / aTotal : 0.0 ) )
                {
                    aborted = true;
                    return 1;
                }

                return 0;
            },
            250000L );

    const int code = curl.Perform();

    if( tooLarge )
        throw TOOL_ERROR( aUrl + " is larger than the " + std::to_string( aMaxBytes ) + " bytes allowed." );

    if( aborted )
        throw TOOL_ERROR( "Cancelled while downloading " + aUrl + "." );

    if( code != 0 )
        throw TOOL_ERROR( "Downloading " + aUrl + " failed: " + curl.GetErrorText( code ) );

    const int status = curl.GetResponseStatusCode();

    if( status >= 400 )
        throw TOOL_ERROR( "Downloading " + aUrl + " failed: HTTP " + std::to_string( status ) + "." );

    return out.str();
}

} // namespace AGENTIC
