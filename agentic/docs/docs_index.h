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


#ifndef AGENTIC_DOCS_INDEX_H
#define AGENTIC_DOCS_INDEX_H

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace AGENTIC
{

/// One section of a manual: a heading and the text under it.
struct DOC_PAGE
{
    std::string id;     ///< "<manual>#<anchor>", stable across downloads.
    std::string manual; ///< "pcbnew", "eeschema", ...
    std::string title;
    std::string text;
};


/**
 * Split an HTML manual (as docs.kicad.org serves them) into pages at its h1 to h4 headings.
 * Scripts, styles and tags are dropped, entities decoded, whitespace folded.
 */
std::vector<DOC_PAGE> SplitHtmlManual( const std::string& aManual, const std::string& aHtml );


/**
 * The offline documentation: pages, an inverted index over their words, BM25 ranking.  Kept
 * on disk as one JSON file per cache folder.
 */
class DOCS_INDEX
{
public:
    void Clear();

    /// Replace the pages of \a aManual.
    void SetManual( const std::string& aManual, std::vector<DOC_PAGE> aPages );

    size_t PageCount() const { return m_pages.size(); }

    /// {manual: pages}.
    std::map<std::string, size_t> Manuals() const;

    struct HIT
    {
        const DOC_PAGE* page;
        double          score;
        std::string     snippet;
    };

    std::vector<HIT> Search( const std::string& aQuery, size_t aLimit, const std::string& aManual = "" ) const;

    const DOC_PAGE* Find( const std::string& aId ) const;

    const std::vector<DOC_PAGE>& Pages() const { return m_pages; }

    bool Save( const std::filesystem::path& aFile, const nlohmann::json& aMeta ) const;

    /// @return the meta object saved with the pages, or null when there is no usable index.
    nlohmann::json Load( const std::filesystem::path& aFile );

    static std::vector<std::string> Tokenize( const std::string& aText );

private:
    void rebuild();

    std::vector<DOC_PAGE>                                      m_pages;
    std::map<std::string, std::vector<std::pair<size_t, int>>> m_postings; ///< word -> (page, count)
    std::vector<size_t>                                        m_lengths;
    double                                                     m_avgLength = 0;
};

} // namespace AGENTIC

#endif // AGENTIC_DOCS_INDEX_H
