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


/**
 * The upstream calculator panels, driven with the same inputs as the fork's formulas (D9):
 * each panel is built in a hidden frame, its input controls are filled, its own compute
 * handler runs, and its displayed outputs are read back in the fork's units.  Used by
 * qa_agentic_e2e through the debug_calc_panel test hook (viewer mode, AGENTIC_TEST_HOOKS).
 */

#include <agentic/calc/calc_tools.h>
#include <agentic/protocol/registry.h>

#include <calculator_panels/panel_board_class.h>
#include <calculator_panels/panel_cable_size.h>
#include <calculator_panels/panel_electrical_spacing_ipc2221.h>
#include <calculator_panels/panel_fusing_current.h>
#include <calculator_panels/panel_regulator.h>
#include <calculator_panels/panel_track_width.h>
#include <calculator_panels/panel_via_size.h>
#include <calculator_panels/panel_wavelength.h>

#include <wx/frame.h>
#include <wx/grid.h>
#include <wx/radiobut.h>
#include <wx/slider.h>

#include <memory>


namespace AGENTIC
{

namespace
{

    constexpr double MM = 1e-3;

    using JSON = nlohmann::json;


    double get( const JSON& aIn, const char* aKey, double aDefault )
    {
        return aIn.contains( aKey ) ? aIn[aKey].get<double>() : aDefault;
    }


    wxString text( double aValue )
    {
        return wxString::Format( wxS( "%.17g" ), aValue );
    }


    double read( const wxString& aText )
    {
        double v = 0;

        if( !aText.ToCDouble( &v ) && !aText.ToDouble( &v ) )
            return std::nan( "" );

        return v;
    }


    /// The hidden frame the panels are built in; destroyed with its panel.
    struct HOST
    {
        HOST() :
                frame( new wxFrame( nullptr, wxID_ANY, wxS( "calc pin" ) ) )
        {
        }
        ~HOST() { frame->Destroy(); }
        wxFrame* frame;
    };


    struct TRACK_WIDTH_PIN : public PANEL_TRACK_WIDTH
    {
        using PANEL_TRACK_WIDTH::PANEL_TRACK_WIDTH;

        JSON Run( const JSON& aIn )
        {
            const double t = get( aIn, "thickness", 0.035 );
            m_TrackDeltaTValue->SetValue( text( get( aIn, "temperatureRise", 10 ) ) );
            m_ExtTrackThicknessValue->SetValue(
                    text( get( aIn, "externalThickness", t ) * MM / m_ExtTrackThicknessUnit->GetUnitScale() ) );
            m_IntTrackThicknessValue->SetValue(
                    text( get( aIn, "internalThickness", t ) * MM / m_IntTrackThicknessUnit->GetUnitScale() ) );
            m_TrackCurrentValue->SetValue( text( get( aIn, "current", 1 ) ) );

            wxCommandEvent event;
            OnTWCalculateFromCurrent( event );

            return { { "externalWidth",
                       read( m_ExtTrackWidthValue->GetValue() ) * m_TW_ExtTrackWidth_choiceUnit->GetUnitScale() / MM },
                     { "internalWidth", read( m_IntTrackWidthValue->GetValue() )
                                                * m_TW_IntTrackWidth_choiceUnit->GetUnitScale() / MM } };
        }
    };


    struct VIA_PIN : public PANEL_VIA_SIZE
    {
        using PANEL_VIA_SIZE::PANEL_VIA_SIZE;

        JSON Run( const JSON& aIn )
        {
            auto set = [&]( wxTextCtrl* aCtrl, double aSi, UNIT_SELECTOR* aUnit )
            {
                aCtrl->SetValue( text( aUnit ? aSi / aUnit->GetUnitScale() : aSi ) );
            };

            set( m_textCtrlHoleDia, aIn["holeDiameter"].get<double>() * MM, m_choiceHoleDia );
            set( m_textCtrlPlatingThickness, get( aIn, "platingThickness", 0.035 ) * MM, m_choicePlatingThickness );
            set( m_textCtrlViaLength, get( aIn, "length", 1.6 ) * MM, m_choiceViaLength );
            set( m_textCtrlViaPadDia, aIn["padDiameter"].get<double>() * MM, m_choiceViaPadDia );
            set( m_textCtrlClearanceDia, aIn["clearanceDiameter"].get<double>() * MM, m_choiceClearanceDia );
            set( m_textCtrlImpedance, get( aIn, "impedance", 50 ), m_choiceImpedance );
            set( m_textCtrlAppliedCurrent, get( aIn, "current", 1 ), nullptr );
            set( m_textCtrlPlatingResistivity, get( aIn, "resistivity", 1.72e-8 ), nullptr );
            set( m_textCtrlPlatingPermittivity, get( aIn, "permittivity", 4.5 ), nullptr );
            set( m_textCtrlTemperatureDiff, get( aIn, "temperatureRise", 10 ), nullptr );
            set( m_textCtrlRiseTime, get( aIn, "riseTime", 1 ), nullptr );

            wxCommandEvent event;
            OnViaCalculate( event );

            return { { "resistance", read( m_ViaResistance->GetLabel() ) },
                     { "voltageDrop", read( m_ViaVoltageDrop->GetLabel() ) },
                     { "powerLoss", read( m_ViaPowerLoss->GetLabel() ) },
                     { "ampacity", read( m_ViaAmpacity->GetLabel() ) },
                     { "thermalResistance", read( m_ViaThermalResistance->GetLabel() ) },
                     { "capacitance", read( m_ViaCapacitance->GetLabel() ) },
                     { "riseTimeDegradation", read( m_RiseTimeOutput->GetLabel() ) },
                     { "inductance", read( m_Inductance->GetLabel() ) },
                     { "reactance", read( m_Reactance->GetLabel() ) } };
        }
    };


    struct IPC2221_PIN : public PANEL_ELECTRICAL_SPACING_IPC2221
    {
        using PANEL_ELECTRICAL_SPACING_IPC2221::PANEL_ELECTRICAL_SPACING_IPC2221;

        JSON Run( const JSON& aIn )
        {
            m_ElectricalSpacingVoltage->SetValue( text( get( aIn, "voltage", 500 ) ) );

            wxCommandEvent event;
            OnElectricalSpacingRefresh( event );

            const double scale = m_ElectricalSpacingUnitsSelector->GetUnitScale();
            JSON         rows = JSON::array();

            for( int r = 0; r < m_gridElectricalSpacingValues->GetNumberRows(); ++r )
            {
                JSON row = JSON::array();

                for( int c = 0; c < m_gridElectricalSpacingValues->GetNumberCols(); ++c )
                    row.push_back( read( m_gridElectricalSpacingValues->GetCellValue( r, c ) ) * scale / MM );

                rows.push_back( row );
            }

            return { { "rows", rows } };
        }
    };


    struct FUSING_PIN : public PANEL_FUSING_CURRENT
    {
        using PANEL_FUSING_CURRENT::PANEL_FUSING_CURRENT;

        JSON Run( const JSON& aIn )
        {
            const std::string solve = aIn.value( "solve", "current" );
            m_meltingValue->SetValue( text( get( aIn, "melting", 1084 ) ) );
            m_ambientValue->SetValue( text( get( aIn, "ambient", 25 ) ) );
            m_currentValue->SetValue( text( get( aIn, "current", 1 ) ) );
            m_widthValue->SetValue( text( get( aIn, "width", 1 ) * MM / m_widthUnit->GetUnitScale() ) );
            m_thicknessValue->SetValue( text( get( aIn, "thickness", 0.035 ) * MM / m_thicknessUnit->GetUnitScale() ) );
            m_timeValue->SetValue( text( get( aIn, "time", 1 ) ) );
            m_widthRadio->SetValue( solve == "width" );
            m_thicknessRadio->SetValue( solve == "thickness" );
            m_currentRadio->SetValue( solve == "current" );
            m_timeRadio->SetValue( solve == "time" );

            wxCommandEvent event;
            m_onCalculateClick( event );

            double v = solve == "width" ? read( m_widthValue->GetValue() ) * m_widthUnit->GetUnitScale() / MM
                       : solve == "thickness"
                               ? read( m_thicknessValue->GetValue() ) * m_thicknessUnit->GetUnitScale() / MM
                       : solve == "time" ? read( m_timeValue->GetValue() )
                                         : read( m_currentValue->GetValue() );
            return { { solve, v } };
        }
    };


    struct WAVE_PIN : public PANEL_WAVELENGTH
    {
        using PANEL_WAVELENGTH::PANEL_WAVELENGTH;

        JSON Run( const JSON& aIn )
        {
            // Text events recompute, in this order.
            m_permittivityCtrl->SetValue( text( get( aIn, "permittivity", 1 ) ) );
            m_permeabilityCtrl->SetValue( text( get( aIn, "permeability", 1 ) ) );
            m_frequencyCtrl->SetValue( text( aIn["frequency"].get<double>() / m_frequencyUnit->GetUnitScale() ) );

            return { { "period", read( m_periodCtrl->GetValue() ) * m_periodUnit->GetUnitScale() },
                     { "wavelengthVacuum",
                       read( m_wavelengthVacuumCtrl->GetValue() ) * m_wavelengthVacuumUnit->GetUnitScale() },
                     { "wavelengthMedium",
                       read( m_wavelengthMediumCtrl->GetValue() ) * m_wavelengthMediumUnit->GetUnitScale() },
                     { "speed", read( m_speedCtrl->GetValue() ) * m_speedUnit->GetUnitScale() } };
        }
    };


    struct CABLE_PIN : public PANEL_CABLE_SIZE
    {
        using PANEL_CABLE_SIZE::PANEL_CABLE_SIZE;

        JSON Run( const JSON& aIn )
        {
            m_textCtrlConductorResistivity->SetValue( text( get( aIn, "resistivity", 1.72e-8 ) ) );
            m_textCtrlConductorThermCoef->SetValue( text( get( aIn, "thermalCoefficient", 3.93e-3 ) ) );
            m_conductorTempCtrl->SetValue( text( get( aIn, "temperature", 20 ) ) );
            m_slCurrentDensity->SetValue( (int) get( aIn, "currentDensity", 3 ) );

            wxScrollEvent scroll;
            onUpdateCurrentDensity( scroll );

            m_currentCtrl->SetValue( text( get( aIn, "current", 1 ) ) );
            m_lengthCtrl->SetValue( text( get( aIn, "length", 1 ) / m_lengthUnit->GetUnitScale() ) );
            m_diameterCtrl->SetValue( text( aIn["diameter"].get<double>() * MM / m_diameterUnit->GetUnitScale() ) );

            return { { "area", read( m_areaCtrl->GetValue() ) },
                     { "linearResistance",
                       read( m_linResistanceCtrl->GetValue() ) * m_linResistanceUnit->GetUnitScale() },
                     { "maxFrequency", read( m_frequencyCtrl->GetValue() ) * m_frequencyUnit->GetUnitScale() },
                     { "ampacity", read( m_AmpacityCtrl->GetValue() ) },
                     { "resistanceDc", read( m_resistanceDcCtrl->GetValue() ) },
                     // These two selectors scale the other way: shown = value * scale.
                     { "voltageDrop", read( m_vDropCtrl->GetValue() ) / m_vDropUnit->GetUnitScale() },
                     { "power", read( m_powerCtrl->GetValue() ) / m_powerUnit->GetUnitScale() } };
        }
    };


    struct REGULATOR_PIN : public PANEL_REGULATOR
    {
        using PANEL_REGULATOR::PANEL_REGULATOR;

        JSON Run( const JSON& aIn )
        {
            const std::string solve = aIn.value( "solve", "vout" );
            const double      vref = get( aIn, "vref", 1.25 );
            m_choiceRegType->SetSelection( aIn.value( "threeTerminal", false ) ? 1 : 0 );
            m_resTolVal->SetValue( text( get( aIn, "tolerance", 1 ) ) );
            m_r1TypVal->SetValue( text( get( aIn, "r1", 0 ) ) );
            m_r2TypVal->SetValue( text( get( aIn, "r2", 0 ) ) );
            m_vrefMinVal->SetValue( text( get( aIn, "vrefMin", vref ) ) );
            m_vrefTypVal->SetValue( text( vref ) );
            m_vrefMaxVal->SetValue( text( get( aIn, "vrefMax", vref ) ) );
            m_voutTypVal->SetValue( text( get( aIn, "vout", 0 ) ) );
            m_iadjTypVal->SetValue( text( get( aIn, "iadj", 0 ) ) );
            m_iadjMaxVal->SetValue( text( get( aIn, "iadjMax", get( aIn, "iadj", 0 ) ) ) );
            m_rbRegulR1->SetValue( solve == "r1" );
            m_rbRegulR2->SetValue( solve == "r2" );
            m_rbRegulVout->SetValue( solve == "vout" );
            m_RegulMessage->SetLabel( wxEmptyString );

            wxCommandEvent event;
            OnRegulatorCalcButtonClick( event );

            if( !m_RegulMessage->GetLabel().IsEmpty() )
                return { { "error", m_RegulMessage->GetLabel().ToStdString( wxConvUTF8 ) } };

            return {
                { "r1",
                  { read( m_r1MinVal->GetValue() ), read( m_r1TypVal->GetValue() ), read( m_r1MaxVal->GetValue() ) } },
                { "r2",
                  { read( m_r2MinVal->GetValue() ), read( m_r2TypVal->GetValue() ), read( m_r2MaxVal->GetValue() ) } },
                { "vout",
                  { read( m_voutMinVal->GetValue() ), read( m_voutTypVal->GetValue() ),
                    read( m_voutMaxVal->GetValue() ) } },
                { "tolerance", { read( m_tolTotalMin->GetValue() ), read( m_TolTotalMax->GetValue() ) } }
            };
        }
    };


    struct BOARD_CLASS_PIN : public PANEL_BOARD_CLASS
    {
        using PANEL_BOARD_CLASS::PANEL_BOARD_CLASS;

        JSON Run( const JSON& )
        {
            const double scale = m_BoardClassesUnitsSelector->GetUnitScale();
            BoardClassesUpdateData( scale );

            static const char* rows[5] = { "trackWidth", "clearance", "viaDiameterMinusDrill", "platedPadMinusDrill",
                                           "unplatedPadMinusDrill" };
            JSON               classes = JSON::array();

            for( int c = 0; c < 6; ++c )
            {
                JSON entry{ { "class", c + 1 } };

                for( int r = 0; r < 5; ++r )
                {
                    const double v = read( m_gridClassesValuesDisplay->GetCellValue( r, c ) );
                    entry[rows[r]] = std::isfinite( v ) ? JSON( v * scale / MM ) : JSON( nullptr );
                }

                classes.push_back( entry );
            }

            return { { "classes", classes } };
        }
    };


    template <typename PIN>
    JSON run( const JSON& aIn )
    {
        HOST host;
        PIN* panel = new PIN( host.frame, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL, wxEmptyString );
        return panel->Run( aIn );
    }

} // namespace


JSON RunCalculatorPanel( const std::string& aName, const JSON& aInputs )
{
    if( aName == "track_width" )
        return run<TRACK_WIDTH_PIN>( aInputs );

    if( aName == "via_size" )
        return run<VIA_PIN>( aInputs );

    if( aName == "electrical_spacing_ipc2221" )
        return run<IPC2221_PIN>( aInputs );

    if( aName == "fusing_current" )
        return run<FUSING_PIN>( aInputs );

    if( aName == "wavelength" )
        return run<WAVE_PIN>( aInputs );

    if( aName == "cable_size" )
        return run<CABLE_PIN>( aInputs );

    if( aName == "regulator" )
        return run<REGULATOR_PIN>( aInputs );

    if( aName == "board_class" )
        return run<BOARD_CLASS_PIN>( aInputs );

    throw TOOL_ERROR( "No panel pin for '" + aName + "' (it calls upstream code directly, or has no formula)." );
}

} // namespace AGENTIC
