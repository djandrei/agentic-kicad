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


#include <agentic/core/agent_project.h>
#include <agentic/sch/sim_tools.h>
#include <agentic/sch/sim_viewer.h>

#include <sim/spice_value.h>
#include <widgets/mathplot.h>

#include <wx/listctrl.h>
#include <wx/textctrl.h>

#include <cmath>


namespace AGENTIC
{

namespace
{

    /// mathplot's scales leave their tick labels empty (the simulator frame's own scales format
    /// them): label each tick with an SI-prefixed number (500m, 1k).
    template <class SCALE>
    class LABELLED_SCALE : public SCALE
    {
    public:
        using SCALE::SCALE;

    private:
        void formatLabels() override
        {
            for( mpScaleBase::TICK_LABEL& label : SCALE::m_tickLabels )
            {
                label.label = std::abs( label.pos ) < 1e-15 ? wxString( wxS( "0" ) )
                                                            : SPICE_VALUE( label.pos ).ToSpiceString();
                label.visible = true;
            }
        }
    };


    class SIM_VIEWER : public AGENT_VIEWER
    {
    public:
        SIM_VIEWER( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject ) :
                AGENT_VIEWER( aServices, aProject, "sim", _( "Simulator" ) )
        {
            m_plot = new mpWindow( this, wxID_ANY );
            m_plot->SetMinSize( wxSize( 700, 450 ) );
            m_plot->SetMargins( 30, 30, 45, 80 );
            SetCentralView( m_plot );

            m_analysis = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize( 360, 200 ),
                                         wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP );
            m_measurements = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 360, 160 ), wxLC_REPORT );
            m_measurements->AppendColumn( _( "Vector" ), wxLIST_FORMAT_LEFT, 120 );
            m_measurements->AppendColumn( _( "Measure" ), wxLIST_FORMAT_LEFT, 90 );
            m_measurements->AppendColumn( _( "Value" ), wxLIST_FORMAT_RIGHT, 120 );

            AddDock( m_analysis, wxS( "analysis" ), _( "Analysis and netlist" ), wxRIGHT );
            AddDock( m_measurements, wxS( "measurements" ), _( "Measurements" ), wxRIGHT );
            FinishLayout();

            redraw();
        }

        nlohmann::json UiState() const override
        {
            nlohmann::json state = AGENT_VIEWER::UiState();
            state["traces"] = m_traces;
            state["measurementRows"] = m_measurements->GetItemCount();
            state["analysis"] = m_shownAnalysis;
            state["logX"] = m_logX;
            return state;
        }

        void OnResults( DOC_SESSION& aSession, const nlohmann::json& aResults ) override
        {
            if( &aSession.Project() == &Project() && aResults.value( "checker", "" ) == "sim" )
                redraw();
        }

    protected:
        bool shows( const DOC_SESSION& aSession ) const override
        {
            return &aSession.Project() == &Project() && aSession.Program() == "sch";
        }

    private:
        void redraw()
        {
            const SIM_STATE&      state = SimState( Project() );
            static const wxColour colours[] = { wxColour( 31, 119, 180 ), wxColour( 214, 39, 40 ),
                                                wxColour( 44, 160, 44 ), wxColour( 148, 103, 189 ),
                                                wxColour( 255, 127, 14 ) };

            m_plot->DelAllLayers( true, false );
            m_logX = state.xAxis == "frequency";

            mpScaleXBase* axisX = nullptr;

            if( m_logX )
                axisX = new LABELLED_SCALE<mpScaleXLog>( wxS( "frequency (Hz)" ), mpALIGN_BOTTOM );
            else
                axisX = new LABELLED_SCALE<mpScaleX>( wx( state.xAxis.empty() ? "x" : state.xAxis ), mpALIGN_BOTTOM );

            mpScaleY* axisY = new LABELLED_SCALE<mpScaleY>( wxEmptyString, mpALIGN_LEFT );
            m_plot->AddLayer( axisX, false );
            m_plot->AddLayer( axisY, false );

            m_traces = 0;

            for( const SIM_SIGNAL& signal : state.signals )
            {
                auto* trace = new mpFXYVector( wx( signal.name + " " + signal.quantity ) );
                trace->SetData( signal.x, signal.y );
                trace->SetContinuity( true );
                trace->SetPen( wxPen( colours[m_traces % 5], 2 ) );
                trace->SetScale( axisX, axisY );
                m_plot->AddLayer( trace, false );
                m_traces++;
            }

            if( m_traces )
                m_plot->Fit();

            m_plot->UpdateAll();

            m_shownAnalysis = state.lastCommand;
            m_analysis->SetValue( wx( ( state.analysis.empty() ? "(the sheet's directive)" : state.analysis ) + "\n\n"
                                      + state.netlist ) );

            m_measurements->DeleteAllItems();

            for( const nlohmann::json& m : state.measurements )
            {
                long row = m_measurements->InsertItem( m_measurements->GetItemCount(), wx( m.value( "vector", "" ) ) );
                m_measurements->SetItem( row, 1, wx( m.value( "kind", "" ) ) );
                m_measurements->SetItem( row, 2, wxString::Format( wxS( "%.6g" ), m.value( "value", 0.0 ) ) );
            }
        }

        static wxString wx( const std::string& aText ) { return wxString::FromUTF8( aText ); }

        mpWindow*   m_plot = nullptr;
        wxTextCtrl* m_analysis = nullptr;
        wxListCtrl* m_measurements = nullptr;
        int         m_traces = 0;
        bool        m_logX = false;
        std::string m_shownAnalysis;
    };

} // namespace


AGENT_VIEWER* CreateSimViewer( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject )
{
    return new SIM_VIEWER( aServices, aProject );
}

} // namespace AGENTIC
