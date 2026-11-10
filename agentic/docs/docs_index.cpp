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
#include <agentic/docs/docs_index.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>


namespace AGENTIC
{

namespace
{

    std::string lower( std::string aText )
    {
        for( char& c : aText )
            c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );

        return aText;
    }


    void appendUtf8( std::string& aOut, unsigned long aCode )
    {
        if( aCode < 0x80 )
        {
            aOut += static_cast<char>( aCode );
        }
        else if( aCode < 0x800 )
        {
            aOut += static_cast<char>( 0xC0 | ( aCode >> 6 ) );
            aOut += static_cast<char>( 0x80 | ( aCode & 0x3F ) );
        }
        else if( aCode < 0x10000 )
        {
            aOut += static_cast<char>( 0xE0 | ( aCode >> 12 ) );
            aOut += static_cast<char>( 0x80 | ( ( aCode >> 6 ) & 0x3F ) );
            aOut += static_cast<char>( 0x80 | ( aCode & 0x3F ) );
        }
        else
        {
            aOut += static_cast<char>( 0xF0 | ( aCode >> 18 ) );
            aOut += static_cast<char>( 0x80 | ( ( aCode >> 12 ) & 0x3F ) );
            aOut += static_cast<char>( 0x80 | ( ( aCode >> 6 ) & 0x3F ) );
            aOut += static_cast<char>( 0x80 | ( aCode & 0x3F ) );
        }
    }


    /// HTML fragment to plain text: tags dropped (block tags become line breaks), entities
    /// decoded, runs of blank space folded.
    std::string htmlText( const std::string& aHtml )
    {
        static const std::set<std::string> blocks = {
            "p",  "div", "br", "li", "tr", "pre", "table", "ul", "ol",
            "dt", "dd",  "h1", "h2", "h3", "h4",  "h5",    "h6", "blockquote"
        };
        std::string raw;
        size_t      i = 0;

        while( i < aHtml.size() )
        {
            char c = aHtml[i];

            if( c == '<' )
            {
                size_t end = aHtml.find( '>', i );

                if( end == std::string::npos )
                    break;

                std::string tag = lower( aHtml.substr( i + 1, end - i - 1 ) );
                bool        closing = !tag.empty() && tag[0] == '/';
                size_t      nameStart = closing ? 1 : 0;
                size_t      nameEnd = tag.find_first_of( " \t\r\n/>", nameStart );
                std::string name =
                        tag.substr( nameStart, nameEnd == std::string::npos ? std::string::npos : nameEnd - nameStart );

                if( !closing && ( name == "script" || name == "style" ) )
                {
                    size_t close = lower( aHtml ).find( "</" + name, end );
                    i = close == std::string::npos ? aHtml.size() : aHtml.find( '>', close ) + 1;
                    continue;
                }

                if( blocks.count( name ) )
                    raw += '\n';
                else if( name == "td" || name == "th" )
                    raw += ' ';

                i = end + 1;
            }
            else if( c == '&' )
            {
                size_t      semi = aHtml.find( ';', i );
                std::string entity =
                        semi == std::string::npos || semi - i > 10 ? "" : aHtml.substr( i + 1, semi - i - 1 );

                if( entity == "amp" )
                    raw += '&';
                else if( entity == "lt" )
                    raw += '<';
                else if( entity == "gt" )
                    raw += '>';
                else if( entity == "quot" )
                    raw += '"';
                else if( entity == "apos" || entity == "#39" )
                    raw += '\'';
                else if( entity == "nbsp" )
                    raw += ' ';
                else if( !entity.empty() && entity[0] == '#' )
                    appendUtf8( raw, entity.size() > 1 && ( entity[1] == 'x' || entity[1] == 'X' )
                                             ? std::strtoul( entity.c_str() + 2, nullptr, 16 )
                                             : std::strtoul( entity.c_str() + 1, nullptr, 10 ) );
                else
                {
                    raw += c;
                    i++;
                    continue;
                }

                i = semi + 1;
            }
            else
            {
                raw += c;
                i++;
            }
        }

        // Fold spaces within lines and blank lines between them.
        std::string out;
        bool        space = false;
        int         newlines = 0;

        for( char ch : raw )
        {
            if( ch == '\n' )
            {
                newlines++;
                space = false;
                continue;
            }

            if( ch == ' ' || ch == '\t' || ch == '\r' )
            {
                space = true;
                continue;
            }

            if( !out.empty() )
            {
                if( newlines )
                    out += newlines > 1 ? "\n\n" : "\n";
                else if( space )
                    out += ' ';
            }

            newlines = 0;
            space = false;
            out += ch;
        }

        return out;
    }


    std::string attribute( const std::string& aTag, const std::string& aName )
    {
        const std::string lowerTag = lower( aTag );
        size_t            at = lowerTag.find( aName + "=" );

        if( at == std::string::npos )
            return std::string();

        at += aName.size() + 1;

        if( at < aTag.size() && ( aTag[at] == '"' || aTag[at] == '\'' ) )
        {
            char   quote = aTag[at];
            size_t end = aTag.find( quote, at + 1 );
            return end == std::string::npos ? std::string() : aTag.substr( at + 1, end - at - 1 );
        }

        size_t end = aTag.find_first_of( " >", at );
        return aTag.substr( at, end == std::string::npos ? std::string::npos : end - at );
    }

} // namespace


std::vector<DOC_PAGE> SplitHtmlManual( const std::string& aManual, const std::string& aHtml )
{
    std::vector<DOC_PAGE> pages;
    const std::string     lowerHtml = lower( aHtml );

    // Headings h1..h4, in document order.
    struct HEADING
    {
        size_t      start;
        size_t      bodyStart;
        std::string title;
        std::string anchor;
    };

    std::vector<HEADING> headings;
    size_t               pos = 0;

    while( ( pos = lowerHtml.find( "<h", pos ) ) != std::string::npos )
    {
        if( pos + 2 >= lowerHtml.size() || lowerHtml[pos + 2] < '1' || lowerHtml[pos + 2] > '4' )
        {
            pos += 2;
            continue;
        }

        const char   level = lowerHtml[pos + 2];
        const size_t tagEnd = aHtml.find( '>', pos );
        const size_t close = lowerHtml.find( std::string( "</h" ) + level, tagEnd );

        if( tagEnd == std::string::npos || close == std::string::npos )
            break;

        HEADING heading;
        heading.start = pos;
        heading.bodyStart = aHtml.find( '>', close ) + 1;
        heading.title = htmlText( aHtml.substr( tagEnd + 1, close - tagEnd - 1 ) );
        heading.anchor = attribute( aHtml.substr( pos, tagEnd - pos ), "id" );

        if( heading.anchor.empty() )
        {
            for( char c : lower( heading.title ) )
                heading.anchor += std::isalnum( static_cast<unsigned char>( c ) ) ? c : '_';
        }

        headings.push_back( heading );
        pos = heading.bodyStart;
    }

    std::set<std::string> used;

    for( size_t h = 0; h < headings.size(); ++h )
    {
        const size_t end = h + 1 < headings.size() ? headings[h + 1].start : aHtml.size();
        std::string  text = htmlText( aHtml.substr( headings[h].bodyStart, end - headings[h].bodyStart ) );

        if( headings[h].title.empty() )
            continue;

        std::string id = aManual + "#" + headings[h].anchor;

        for( int n = 2; used.count( id ); ++n )
            id = aManual + "#" + headings[h].anchor + "-" + std::to_string( n );

        used.insert( id );
        pages.push_back( DOC_PAGE{ id, aManual, headings[h].title, text } );
    }

    return pages;
}


std::vector<std::string> DOCS_INDEX::Tokenize( const std::string& aText )
{
    std::vector<std::string> out;
    std::string              word;

    auto flush = [&]()
    {
        if( word.size() >= 2 )
            out.push_back( word );

        word.clear();
    };

    for( char c : aText )
    {
        if( std::isalnum( static_cast<unsigned char>( c ) ) || c == '_' || ( c & 0x80 ) )
            word += static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) );
        else
            flush();
    }

    flush();
    return out;
}


void DOCS_INDEX::Clear()
{
    m_pages.clear();
    rebuild();
}


void DOCS_INDEX::SetManual( const std::string& aManual, std::vector<DOC_PAGE> aPages )
{
    std::erase_if( m_pages,
                   [&]( const DOC_PAGE& aPage )
                   {
                       return aPage.manual == aManual;
                   } );

    for( DOC_PAGE& page : aPages )
        m_pages.push_back( std::move( page ) );

    rebuild();
}


std::map<std::string, size_t> DOCS_INDEX::Manuals() const
{
    std::map<std::string, size_t> out;

    for( const DOC_PAGE& page : m_pages )
        out[page.manual]++;

    return out;
}


void DOCS_INDEX::rebuild()
{
    m_postings.clear();
    m_lengths.assign( m_pages.size(), 0 );
    double total = 0;

    for( size_t p = 0; p < m_pages.size(); ++p )
    {
        std::map<std::string, int> counts;

        // The title counts three times: a heading that names the subject ranks high.
        for( const std::string& word : Tokenize( m_pages[p].title ) )
            counts[word] += 3;

        for( const std::string& word : Tokenize( m_pages[p].text ) )
            counts[word]++;

        size_t length = 0;

        for( const auto& [word, count] : counts )
        {
            m_postings[word].emplace_back( p, count );
            length += count;
        }

        m_lengths[p] = length;
        total += length;
    }

    m_avgLength = m_pages.empty() ? 0 : total / m_pages.size();
}


std::vector<DOCS_INDEX::HIT> DOCS_INDEX::Search( const std::string& aQuery, size_t aLimit,
                                                 const std::string& aManual ) const
{
    const double k1 = 1.2;
    const double b = 0.75;
    const double n = static_cast<double>( m_pages.size() );

    std::map<size_t, double> scores;
    std::vector<std::string> query = Tokenize( aQuery );

    for( const std::string& word : query )
    {
        auto it = m_postings.find( word );

        if( it == m_postings.end() )
            continue;

        const double df = static_cast<double>( it->second.size() );
        const double idf = std::log( 1.0 + ( n - df + 0.5 ) / ( df + 0.5 ) );

        for( const auto& [page, count] : it->second )
        {
            if( !aManual.empty() && m_pages[page].manual != aManual )
                continue;

            const double tf = count;
            const double norm = 1.0 - b + b * ( m_avgLength > 0 ? m_lengths[page] / m_avgLength : 1.0 );
            scores[page] += idf * tf * ( k1 + 1 ) / ( tf + k1 * norm );
        }
    }

    std::vector<HIT> hits;

    for( const auto& [page, score] : scores )
        hits.push_back( HIT{ &m_pages[page], score, std::string() } );

    std::sort( hits.begin(), hits.end(),
               []( const HIT& a, const HIT& b )
               {
                   if( a.score != b.score )
                       return a.score > b.score;

                   return a.page->id < b.page->id;
               } );

    if( hits.size() > aLimit )
        hits.resize( aLimit );

    // A snippet around the first query word in the text.
    for( HIT& hit : hits )
    {
        const std::string lowerText = lower( hit.page->text );
        size_t            at = std::string::npos;

        for( const std::string& word : query )
            at = std::min( at, lowerText.find( word ) );

        const size_t start = at == std::string::npos || at < 80 ? 0 : at - 80;
        hit.snippet = hit.page->text.substr( start, 240 );

        for( char& c : hit.snippet )
        {
            if( c == '\n' )
                c = ' ';
        }
    }

    return hits;
}


const DOC_PAGE* DOCS_INDEX::Find( const std::string& aId ) const
{
    for( const DOC_PAGE& page : m_pages )
    {
        if( page.id == aId )
            return &page;
    }

    return nullptr;
}


bool DOCS_INDEX::Save( const std::filesystem::path& aFile, const nlohmann::json& aMeta ) const
{
    nlohmann::json pages = nlohmann::json::array();

    for( const DOC_PAGE& page : m_pages )
        pages.push_back(
                { { "id", page.id }, { "manual", page.manual }, { "title", page.title }, { "text", page.text } } );

    try
    {
        FILE_STORE::WriteAtomic( aFile, nlohmann::json{ { "meta", aMeta }, { "pages", pages } }.dump() );
        return true;
    }
    catch( const std::exception& )
    {
        return false;
    }
}


nlohmann::json DOCS_INDEX::Load( const std::filesystem::path& aFile )
{
    Clear();

    if( !std::filesystem::exists( aFile ) )
        return nullptr;

    try
    {
        nlohmann::json data = nlohmann::json::parse( FILE_STORE::ReadFile( aFile ) );

        for( const nlohmann::json& page : data.at( "pages" ) )
        {
            m_pages.push_back( DOC_PAGE{ page.at( "id" ).get<std::string>(), page.at( "manual" ).get<std::string>(),
                                         page.at( "title" ).get<std::string>(),
                                         page.at( "text" ).get<std::string>() } );
        }

        rebuild();
        return data.value( "meta", nlohmann::json::object() );
    }
    catch( const std::exception& )
    {
        Clear();
        return nullptr;
    }
}

} // namespace AGENTIC
