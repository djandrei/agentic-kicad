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


#ifndef AGENTIC_DS_SESSION_H
#define AGENTIC_DS_SESSION_H

#include <memory>
#include <string>

#include <agentic/core/doc_session.h>

class DS_DATA_MODEL;

namespace AGENTIC
{

class AGENT_PROJECT;

/// One drawing sheet (.kicad_wks): a DS_DATA_MODEL of its own, not KiCad's process-wide one.
class SHEET_SESSION : public DOC_SESSION
{
public:
    SHEET_SESSION( AGENT_PROJECT& aProject, const std::string& aPath );
    ~SHEET_SESSION() override;

    DS_DATA_MODEL& Model() const { return *m_model; }

    /// Start a new sheet in memory (saved by the first change): KiCad's default or empty.
    void SetNew( bool aDefault );

protected:
    void load() override;
    void save( const std::vector<std::string>& aFiles ) override;

private:
    std::unique_ptr<DS_DATA_MODEL> m_model;
};


/// The session of drawing sheet \a aPath (project-relative .kicad_wks), opened on first use.
SHEET_SESSION& SheetSession( AGENT_PROJECT& aProject, const std::string& aPath );

} // namespace AGENTIC

#endif // AGENTIC_DS_SESSION_H
