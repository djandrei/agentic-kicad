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


#ifndef AGENTIC_MESSAGE_LOG_H
#define AGENTIC_MESSAGE_LOG_H

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <reporter.h>

class wxLog;

namespace AGENTIC
{

/**
 * The messages of a process (D11): what KiCad's engines reported while tools ran.  Each
 * record has a sequence number, so a call can return exactly its own records and log_get
 * can page the rest.
 */
struct LOG_RECORD
{
    uint64_t    seq = 0;
    std::string severity; ///< "error", "warning", "info", "action" or "debug".
    std::string text;
    std::string tool; ///< The tool call that produced it.
    std::string item; ///< Source item (KIID or handle), when known.
    std::string file; ///< Source file (project-relative), when known.

    nlohmann::json ToJson() const;
};


class MESSAGE_LOG
{
public:
    /// Records beyond this are dropped oldest first.
    static constexpr size_t CAPACITY = 5000;

    uint64_t Add( LOG_RECORD aRecord );

    /// Records with seq > \a aSince, at most \a aLimit of them.
    std::vector<LOG_RECORD> Since( uint64_t aSince, size_t aLimit ) const;

    uint64_t LastSeq() const { return m_next - 1; }

    static std::string SeverityName( SEVERITY aSeverity );

private:
    std::deque<LOG_RECORD> m_records;
    uint64_t               m_next = 1;
};


/**
 * Collects what is reported during one tool call: a REPORTER to hand to engine calls, plus
 * a wxLog target installed for the life of the capture.  The records are kept in the
 * capture and copied to a MESSAGE_LOG when it ends.
 */
class CALL_MESSAGES : public REPORTER
{
public:
    explicit CALL_MESSAGES( std::string aTool );
    ~CALL_MESSAGES() override;

    REPORTER& Report( const wxString& aText, SEVERITY aSeverity = RPT_SEVERITY_UNDEFINED ) override;

    bool HasMessage() const override { return !m_records.empty(); }

    /// Record with an item or file attached.
    void Add( SEVERITY aSeverity, const std::string& aText, const std::string& aItem = "",
              const std::string& aFile = "" );

    /// Move the records into \a aLog (assigning sequence numbers) and return them as JSON.
    nlohmann::json Flush( MESSAGE_LOG& aLog );

    const std::vector<LOG_RECORD>& Records() const { return m_records; }

private:
    std::string             m_tool;
    std::vector<LOG_RECORD> m_records;
    wxLog*                  m_previousLog = nullptr;
    std::unique_ptr<wxLog>  m_capture;
};

} // namespace AGENTIC

#endif // AGENTIC_MESSAGE_LOG_H
