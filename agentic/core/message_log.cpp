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


#include <agentic/core/message_log.h>

#include <wx/log.h>


namespace AGENTIC
{

namespace
{

    /// Turns wxLogError/Warning/Message calls made by engines into call records.
    class CAPTURE_LOG : public wxLog
    {
    public:
        explicit CAPTURE_LOG( CALL_MESSAGES& aOwner ) :
                m_owner( aOwner )
        {
        }

    protected:
        void DoLogRecord( wxLogLevel aLevel, const wxString& aMsg, const wxLogRecordInfo& ) override
        {
            SEVERITY severity = RPT_SEVERITY_INFO;

            if( aLevel <= wxLOG_Error )
                severity = RPT_SEVERITY_ERROR;
            else if( aLevel == wxLOG_Warning )
                severity = RPT_SEVERITY_WARNING;
            else if( aLevel >= wxLOG_Debug )
                return; // traces and debug output are not the agent's business

            m_owner.Add( severity, aMsg.ToStdString( wxConvUTF8 ) );
        }

    private:
        CALL_MESSAGES& m_owner;
    };

} // namespace


nlohmann::json LOG_RECORD::ToJson() const
{
    nlohmann::json out{ { "seq", seq }, { "severity", severity }, { "text", text } };

    if( !tool.empty() )
        out["tool"] = tool;

    if( !item.empty() )
        out["item"] = item;

    if( !file.empty() )
        out["file"] = file;

    return out;
}


std::string MESSAGE_LOG::SeverityName( SEVERITY aSeverity )
{
    switch( aSeverity )
    {
    case RPT_SEVERITY_ERROR: return "error";
    case RPT_SEVERITY_WARNING: return "warning";
    case RPT_SEVERITY_ACTION: return "action";
    case RPT_SEVERITY_DEBUG: return "debug";
    case RPT_SEVERITY_EXCLUSION: return "exclusion";
    default: return "info";
    }
}


uint64_t MESSAGE_LOG::Add( LOG_RECORD aRecord )
{
    aRecord.seq = m_next++;
    m_records.push_back( std::move( aRecord ) );

    while( m_records.size() > CAPACITY )
        m_records.pop_front();

    return m_records.back().seq;
}


std::vector<LOG_RECORD> MESSAGE_LOG::Since( uint64_t aSince, size_t aLimit ) const
{
    std::vector<LOG_RECORD> out;

    for( const LOG_RECORD& record : m_records )
    {
        if( record.seq <= aSince )
            continue;

        if( out.size() >= aLimit )
            break;

        out.push_back( record );
    }

    return out;
}


CALL_MESSAGES::CALL_MESSAGES( std::string aTool ) :
        m_tool( std::move( aTool ) ),
        m_capture( std::make_unique<CAPTURE_LOG>( *this ) )
{
    m_previousLog = wxLog::SetActiveTarget( m_capture.get() );
}


CALL_MESSAGES::~CALL_MESSAGES()
{
    wxLog::SetActiveTarget( m_previousLog );
}


REPORTER& CALL_MESSAGES::Report( const wxString& aText, SEVERITY aSeverity )
{
    REPORTER::Report( aText, aSeverity );
    Add( aSeverity, aText.ToStdString( wxConvUTF8 ) );
    return *this;
}


void CALL_MESSAGES::Add( SEVERITY aSeverity, const std::string& aText, const std::string& aItem,
                         const std::string& aFile )
{
    std::string text = aText;

    while( !text.empty() && ( text.back() == '\n' || text.back() == '\r' ) )
        text.pop_back();

    if( text.empty() )
        return;

    LOG_RECORD record;
    record.severity = MESSAGE_LOG::SeverityName( aSeverity );
    record.text = text;
    record.tool = m_tool;
    record.item = aItem;
    record.file = aFile;
    m_records.push_back( std::move( record ) );
}


nlohmann::json CALL_MESSAGES::Flush( MESSAGE_LOG& aLog )
{
    nlohmann::json out = nlohmann::json::array();

    for( LOG_RECORD& record : m_records )
    {
        record.seq = aLog.Add( record );
        out.push_back( record.ToJson() );
    }

    m_records.clear();
    return out;
}

} // namespace AGENTIC
