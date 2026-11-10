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


#ifndef AGENTIC_FILE_HISTORY_H
#define AGENTIC_FILE_HISTORY_H

#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace AGENTIC
{

/**
 * Undo and redo through saved file versions (D7).  Every mutating call is one step: for each
 * file it wrote, the content before and after the call is kept in
 * <project>/.agentic/history/ (content-addressed, so a version kept twice is stored once).
 * Undo writes the "before" content back, redo the "after" content.
 *
 * The caller (AGENT_PROJECT) reloads the documents whose files changed.
 */
class FILE_HISTORY
{
public:
    /// Steps kept for undo; older steps are dropped with their unreferenced versions.
    static constexpr size_t MAX_STEPS = 50;

    explicit FILE_HISTORY( std::filesystem::path aProjectRoot );

    struct CHANGE
    {
        std::string path;   ///< Project-relative.
        std::string before; ///< Version id before the call, empty when the file did not exist.
        std::string after;  ///< Version id after the call, empty when the call deleted it.
    };

    struct STEP
    {
        std::string         label; ///< The tool and a short summary.
        std::vector<CHANGE> changes;

        nlohmann::json ToJson() const;
    };

    /// Start recording a call.  Nested calls are folded into the outer step.
    void Begin( const std::string& aLabel );

    /// Record the current content of \a aRelative before it is overwritten or deleted.
    void Before( const std::string& aRelative );

    /// Record the content of \a aRelative after the write (or its absence after a delete).
    void After( const std::string& aRelative );

    /// End the call.  A step that changed nothing is dropped; otherwise redo is cleared.
    void End();

    /// Drop the step being recorded (the call failed and its files were restored).
    void Abandon();

    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }

    const std::deque<STEP>& UndoSteps() const { return m_undo; }

    /**
     * Restore the files of the last step to their "before" content.  When \a aForce is false
     * and a file no longer has the step's "after" content (someone else changed it), nothing
     * is written and a conflict is thrown.
     *
     * @return the step undone.
     */
    STEP Undo( bool aForce );

    /// The reverse of Undo().
    STEP Redo( bool aForce );

private:
    std::filesystem::path absolute( const std::string& aRelative ) const;
    std::string           snapshot( const std::string& aRelative );
    void                  restore( const std::string& aRelative, const std::string& aVersion );
    std::string           currentVersion( const std::string& aRelative ) const;
    void                  apply( const STEP& aStep, bool aToAfter, bool aForce );
    void                  collectGarbage();
    void                  saveIndex() const;
    void                  loadIndex();

    std::filesystem::path m_root;
    std::filesystem::path m_dir;
    std::deque<STEP>      m_undo;
    std::deque<STEP>      m_redo;
    std::optional<STEP>   m_current;
    int                   m_depth = 0;
};

} // namespace AGENTIC

#endif // AGENTIC_FILE_HISTORY_H
