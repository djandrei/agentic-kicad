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
#include <agentic/core/core_tools.h>
#include <agentic/core/file_store.h>
#include <agentic/core/services.h>
#include <agentic/sch/sch_doc_session.h>
#include <agentic/sch/sch_items.h>
#include <agentic/sch/sim_tools.h>

#include <font/font.h>
#include <locale_io.h>
#include <page_info.h>
#include <plotters/plotter_png.h>
#include <project/project_file.h>
#include <reporter.h>
#include <richio.h>
#include <sch_render_settings.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <schematic_settings.h>
#include <sim/sim_model.h>
#include <sim/spice_circuit_model.h>
#include <sim/spice_settings.h>
#include <sim/spice_simulator.h>

#include <wx/base64.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>


namespace AGENTIC
{

namespace fs = std::filesystem;

namespace
{

    std::string utf8( const wxString& aText )
    {
        return aText.ToStdString( wxConvUTF8 );
    }


    wxString wx( const std::string& aText )
    {
        return wxString::FromUTF8( aText );
    }


    std::string lower( std::string aText )
    {
        std::transform( aText.begin(), aText.end(), aText.begin(),
                        []( unsigned char c )
                        {
                            return static_cast<char>( std::tolower( c ) );
                        } );
        return aText;
    }


    /// Refuse a built-in device or type KiCad does not know: SIM_MODEL would fall back to an untyped
    /// line without a .model card, which ngspice cannot load.
    void checkDeviceType( const std::string& aDevice, const std::string& aType )
    {
        std::vector<std::string>                        devices;
        std::map<std::string, std::vector<std::string>> types;

        for( SIM_MODEL::TYPE type : SIM_MODEL::TYPE_ITERATOR() )
        {
            const SIM_MODEL::INFO info = SIM_MODEL::TypeInfo( type );
            const std::string     device = SIM_MODEL::DeviceInfo( info.deviceType ).fieldValue;

            if( device.empty() )
                continue;

            if( !types.count( device ) )
                devices.push_back( device );

            std::vector<std::string>& list = types[device];

            if( std::find( list.begin(), list.end(), info.fieldValue ) == list.end() )
                list.push_back( info.fieldValue );
        }

        auto join = []( const std::vector<std::string>& aList )
        {
            std::string text;

            for( const std::string& item : aList )
            {
                if( !item.empty() )
                    text += ( text.empty() ? "" : ", " ) + item;
            }

            return text;
        };

        if( !types.count( aDevice ) )
            throw TOOL_ERROR( "Unknown device '" + aDevice + "'. Built-in devices: " + join( devices ) + "." );

        const std::vector<std::string>& valid = types[aDevice];

        if( std::find( valid.begin(), valid.end(), aType ) != valid.end() )
            return;

        if( aType.empty() )
            throw TOOL_ERROR( aDevice + " needs a type: " + join( valid ) + "." );

        throw TOOL_ERROR( "'" + aType + "' is not a type of " + aDevice + "; its types are " + join( valid )
                          + ( std::find( valid.begin(), valid.end(), "" ) != valid.end() ? " (or none)." : "." ) );
    }


    /// A SPICE number from JSON: a number, or a string such as "1u" or "10k".
    std::string spiceValue( const JSON& aValue, const std::string& aField )
    {
        if( aValue.is_number() )
        {
            std::ostringstream out;
            out.imbue( std::locale::classic() );
            out << aValue.get<double>();
            return out.str();
        }

        if( aValue.is_string()
            && std::regex_match( aValue.get<std::string>(), std::regex( "[0-9.eE+\\-]+[a-zA-Z]*" ) ) )
            return aValue.get<std::string>();

        throw TOOL_ERROR( aField + " must be a number or a SPICE value such as 1u or 10k." );
    }


    std::string spiceName( const JSON& aValue, const std::string& aField )
    {
        if( aValue.is_string() && std::regex_match( aValue.get<std::string>(), std::regex( "[A-Za-z0-9_/.+\\-]+" ) ) )
            return aValue.get<std::string>();

        throw TOOL_ERROR( aField + " must be a source or node name." );
    }


    const std::set<std::string> ANALYSES = { ".op", ".dc", ".ac", ".tran", ".noise", ".pz", ".sp", ".fft" };


    /**
     * Check one analysis line: one of the analysis commands with plain arguments.  ngspice's
     * control language (.control, shell, source, write) is not accepted; it can run programs
     * and write files.
     */
    std::string checkedCommand( const std::string& aCommand )
    {
        std::string command = aCommand;
        command.erase( 0, command.find_first_not_of( " \t" ) );
        command.erase( command.find_last_not_of( " \t\r\n" ) + 1 );

        if( command.find_first_of( "\n\r;\"'`{}$" ) != std::string::npos )
            throw TOOL_ERROR( "An analysis is one line of plain SPICE (no quotes, braces or ;)." );

        const std::string first = lower( command.substr( 0, command.find_first_of( " \t" ) ) );

        if( !ANALYSES.count( first ) )
            throw TOOL_ERROR( "The analysis is one of .op, .dc, .ac, .tran, .noise, .pz, .sp, .fft; '" + first
                              + "' is not accepted (no .control blocks or ngspice commands)." );

        if( !std::regex_match( command, std::regex( "[A-Za-z0-9_.+\\-(),=/ \\t]*" ) ) )
            throw TOOL_ERROR( "The analysis has characters SPICE analyses do not use." );

        return command;
    }


    /// Refuse a netlist with ngspice control code (it may come from schematic text directives).
    void checkNetlist( const std::string& aNetlist )
    {
        std::istringstream lines( aNetlist );
        std::string        line;

        while( std::getline( lines, line ) )
        {
            const std::string l = lower( line );
            const size_t      start = l.find_first_not_of( " \t" );

            if( start == std::string::npos )
                continue;

            if( l.compare( start, 8, ".control" ) == 0 || l.compare( start, 5, "shell" ) == 0 )
            {
                throw TOOL_ERROR( "The schematic has an ngspice .control block or shell command in a text directive. "
                                  "The agent does not run them (they can run programs); remove the directive or "
                                  "use sim_analysis_set." );
            }
        }
    }


    std::shared_ptr<SPICE_SIMULATOR> simulator()
    {
        std::shared_ptr<SPICE_SIMULATOR> sim = SIMULATOR::CreateInstance( "ngspice" );

        if( !sim )
            throw TOOL_ERROR( "ngspice is not available: install libngspice0 (and ngspice for XSPICE code models)." );

        return sim;
    }


    /// The circuit model of the project's schematic, read as the simulator frame reads it.
    std::shared_ptr<SPICE_CIRCUIT_MODEL> circuit( SCH_DOC_SESSION& aSession, std::string& aMessages )
    {
        auto               model = std::make_shared<SPICE_CIRCUIT_MODEL>( &aSession.Schematic(), nullptr );
        WX_STRING_REPORTER reporter;
        model->ReadSchematicAndLibraries( NETLIST_EXPORTER_SPICE::OPTION_DEFAULT_FLAGS, reporter );
        aMessages = utf8( reporter.GetMessages() );
        return model;
    }


    std::string analysisOf( SIM_STATE& aState, SPICE_CIRCUIT_MODEL& aModel, const JSON& aArgs )
    {
        if( aArgs.contains( "analysis" ) )
            return checkedCommand( aArgs["analysis"] );

        if( !aState.analysis.empty() )
            return aState.analysis;

        const std::string directive = utf8( aModel.GetSchTextSimCommand() );

        if( directive.empty() )
            throw TOOL_ERROR(
                    "No analysis: set one with sim_analysis_set, or put a directive such as .tran on the sheet." );

        return checkedCommand( directive );
    }


    std::string netlistText( SPICE_CIRCUIT_MODEL& aModel, const std::string& aCommand, std::string& aMessages )
    {
        STRING_FORMATTER   formatter;
        WX_STRING_REPORTER reporter;

        if( !aModel.GetNetlist( wx( aCommand ), NETLIST_EXPORTER_SPICE::OPTION_DEFAULT_FLAGS, &formatter, reporter ) )
            throw TOOL_ERROR( "The SPICE netlist could not be made:\n" + utf8( reporter.GetMessages() ) );

        aMessages += utf8( reporter.GetMessages() );
        return formatter.GetString();
    }


    /// Find an ngspice vector by name: exact, then without case, then as a net (v(/out) for out).
    std::string vectorName( SPICE_SIMULATOR& aSim, const std::string& aName )
    {
        const std::vector<std::string> all = aSim.AllVectors();

        for( const std::string& v : all )
        {
            if( v == aName )
                return v;
        }

        for( const std::string& v : all )
        {
            if( lower( v ) == lower( aName ) )
                return v;
        }

        for( const std::string& candidate : { "v(" + aName + ")", "v(/" + aName + ")", "/" + aName } )
        {
            for( const std::string& v : all )
            {
                if( lower( v ) == lower( candidate ) )
                    return v;
            }
        }

        std::string known;

        for( const std::string& v : all )
            known += ( known.empty() ? "" : ", " ) + v;

        throw TOOL_ERROR( "No vector '" + aName + "'. The last run has: " + known + "." );
    }


    bool isFrequencyDomain( SIM_TYPE aType )
    {
        return aType == ST_AC || aType == ST_SP;
    }


    struct SERIES
    {
        std::vector<double> x;
        std::vector<double> y;
        std::string         quantity;
    };


    /// One vector of the last run against the x axis.
    SERIES series( SPICE_SIMULATOR& aSim, const SIM_STATE& aState, const std::string& aName, std::string aQuantity )
    {
        const std::string name = vectorName( aSim, aName );
        const SIM_TYPE    type = SPICE_CIRCUIT_MODEL::CommandToSimType( wx( aState.lastCommand ) );
        SERIES            out;

        if( aQuantity.empty() )
            aQuantity = isFrequencyDomain( type ) ? "gain_db" : "value";

        if( aQuantity == "gain_db" || aQuantity == "magnitude" )
        {
            out.y = aSim.GetGainVector( name );

            if( aQuantity == "gain_db" )
            {
                for( double& v : out.y )
                    v = 20 * std::log10( std::max( v, 1e-300 ) );
            }
        }
        else if( aQuantity == "phase" )
        {
            out.y = aSim.GetPhaseVector( name );

            for( double& v : out.y )
                v = v * 180 / M_PI;
        }
        else
        {
            out.y = aSim.GetRealVector( name );
        }

        if( !aState.xAxis.empty() )
        {
            out.x = aSim.GetRealVector( aState.xAxis );

            if( out.x.size() != out.y.size() )
                out.x = aSim.GetGainVector( aState.xAxis );
        }

        if( out.x.size() != out.y.size() )
        {
            out.x.resize( out.y.size() );

            for( size_t i = 0; i < out.x.size(); ++i )
                out.x[i] = static_cast<double>( i );
        }

        out.quantity = aQuantity;
        return out;
    }


    /// The part of \a aSeries with x in [from, to].
    SERIES window( const SERIES& aSeries, const JSON& aArgs )
    {
        if( !aArgs.contains( "from" ) && !aArgs.contains( "to" ) )
            return aSeries;

        const double from = aArgs.value( "from", -HUGE_VAL );
        const double to = aArgs.value( "to", HUGE_VAL );
        SERIES       out;
        out.quantity = aSeries.quantity;

        for( size_t i = 0; i < aSeries.x.size(); ++i )
        {
            if( aSeries.x[i] >= from && aSeries.x[i] <= to )
            {
                out.x.push_back( aSeries.x[i] );
                out.y.push_back( aSeries.y[i] );
            }
        }

        if( out.x.empty() )
            throw TOOL_ERROR( "No points between from and to." );

        return out;
    }


    double interpolate( const SERIES& aSeries, double aX )
    {
        const std::vector<double>& x = aSeries.x;

        if( aX <= x.front() )
            return aSeries.y.front();

        for( size_t i = 1; i < x.size(); ++i )
        {
            if( aX <= x[i] )
            {
                const double t = ( aX - x[i - 1] ) / ( x[i] - x[i - 1] );
                return aSeries.y[i - 1] + t * ( aSeries.y[i] - aSeries.y[i - 1] );
            }
        }

        return aSeries.y.back();
    }


    /// x of the first crossing of \a aLevel (rising, falling or either), interpolated.
    std::optional<double> crossing( const SERIES& aSeries, double aLevel, const std::string& aDirection )
    {
        for( size_t i = 1; i < aSeries.y.size(); ++i )
        {
            const double a = aSeries.y[i - 1] - aLevel;
            const double b = aSeries.y[i] - aLevel;
            const bool   rising = a < 0 && b >= 0;
            const bool   falling = a > 0 && b <= 0;

            if( ( rising && aDirection != "falling" ) || ( falling && aDirection != "rising" ) )
            {
                const double t = a / ( a - b );
                return aSeries.x[i - 1] + t * ( aSeries.x[i] - aSeries.x[i - 1] );
            }
        }

        return std::nullopt;
    }


    /// \a aCount points of \a aSeries, evenly spaced by index (the first and last kept).
    SERIES decimate( const SERIES& aSeries, size_t aCount )
    {
        if( aSeries.x.size() <= aCount || aCount < 2 )
            return aSeries;

        SERIES out;
        out.quantity = aSeries.quantity;

        for( size_t i = 0; i < aCount; ++i )
        {
            const size_t index = i * ( aSeries.x.size() - 1 ) / ( aCount - 1 );
            out.x.push_back( aSeries.x[index] );
            out.y.push_back( aSeries.y[index] );
        }

        return out;
    }


    void requireRun( const SIM_STATE& aState )
    {
        if( aState.runs == 0 )
            throw TOOL_ERROR( "Nothing simulated yet; call sim_run." );
    }


    std::string engineering( double aValue )
    {
        static const char* prefixes[] = { "f", "p", "n", "u", "m", "", "k", "M", "G", "T" };

        if( aValue == 0 || !std::isfinite( aValue ) )
            return aValue == 0 ? "0" : "?";

        int exponent = static_cast<int>( std::floor( std::log10( std::fabs( aValue ) ) / 3 ) );
        exponent = std::clamp( exponent, -5, 4 );
        const double scaled = aValue / std::pow( 1000.0, exponent );
        char         text[32];
        std::snprintf( text, sizeof( text ), "%.3g%s", scaled, prefixes[exponent + 5] );
        return text;
    }


    /// A plot of \a aSignals as a PNG, drawn with KiCad's PNG plotter (headless).
    std::string plotPng( AGENT_PROJECT& aProject, const std::vector<SIM_SIGNAL>& aSignals, bool aLogX, int aWidth,
                         int aHeight )
    {
        constexpr int DPI = 50;
        const double  iuPerPixel = schIUScale.IU_PER_MILS * 1000.0 / DPI;
        const int     left = 70, right = 20, top = 20, bottom = 45;
        const COLOR4D palette[] = { COLOR4D( 0.12, 0.47, 0.71, 1 ), COLOR4D( 0.84, 0.15, 0.16, 1 ),
                                    COLOR4D( 0.17, 0.63, 0.17, 1 ), COLOR4D( 0.58, 0.40, 0.74, 1 ),
                                    COLOR4D( 1.00, 0.50, 0.05, 1 ) };

        auto px = [&]( double aX, double aY )
        {
            return VECTOR2I( KiROUND( aX * iuPerPixel ), KiROUND( aY * iuPerPixel ) );
        };

        double xMin = HUGE_VAL, xMax = -HUGE_VAL, yMin = HUGE_VAL, yMax = -HUGE_VAL;

        auto xOf = [&]( double aX )
        {
            return aLogX ? std::log10( std::max( aX, 1e-30 ) ) : aX;
        };

        for( const SIM_SIGNAL& s : aSignals )
        {
            for( size_t i = 0; i < s.x.size(); ++i )
            {
                xMin = std::min( xMin, xOf( s.x[i] ) );
                xMax = std::max( xMax, xOf( s.x[i] ) );
                yMin = std::min( yMin, s.y[i] );
                yMax = std::max( yMax, s.y[i] );
            }
        }

        if( !( xMax > xMin ) )
            xMax = xMin + 1;

        if( !( yMax > yMin ) )
        {
            yMin -= 1;
            yMax += 1;
        }

        const double margin = ( yMax - yMin ) * 0.05;
        yMin -= margin;
        yMax += margin;

        const double plotW = aWidth - left - right;
        const double plotH = aHeight - top - bottom;

        auto map = [&]( double aX, double aY )
        {
            return px( left + ( xOf( aX ) - xMin ) / ( xMax - xMin ) * plotW,
                       top + ( yMax - aY ) / ( yMax - yMin ) * plotH );
        };

        PAGE_INFO page( PAGE_SIZE_TYPE::User );
        page.SetWidthMils( aWidth * 1000.0 / DPI );
        page.SetHeightMils( aHeight * 1000.0 / DPI );

        SCH_RENDER_SETTINGS settings;
        PNG_PLOTTER         plotter;
        plotter.SetPixelSize( aWidth, aHeight );
        plotter.SetResolution( DPI );
        plotter.SetAntialias( true );
        plotter.SetRenderSettings( &settings );
        plotter.SetPageSettings( page );
        plotter.SetColorMode( true );
        plotter.SetViewport( VECTOR2I( 0, 0 ), schIUScale.IU_PER_MILS / 10, 1.0, false );

        const fs::path file = aProject.Root() / ".agentic" / "captures" / "sim.png";
        fs::create_directories( file.parent_path() );

        if( !plotter.OpenFile( wx( file.string() ) ) )
            throw TOOL_ERROR( "Plotting failed." );

        plotter.StartPlot( wxS( "1" ) );
        plotter.SetColor( COLOR4D::WHITE );
        plotter.Rect( px( 0, 0 ), px( aWidth, aHeight ), FILL_T::FILLED_SHAPE, 0, 0 );

        KIFONT::FONT*  font = KIFONT::FONT::GetFont();
        const VECTOR2I textSize = px( 7, 7 );
        const COLOR4D  grey( 0.85, 0.85, 0.85, 1 );
        const COLOR4D  ink( 0.2, 0.2, 0.2, 1 );
        const int      thin = KiROUND( iuPerPixel );

        auto text = [&]( const VECTOR2I& aPos, const std::string& aText, GR_TEXT_H_ALIGN_T aH, GR_TEXT_V_ALIGN_T aV )
        {
            plotter.Text( aPos, ink, wx( aText ), ANGLE_0, textSize, aH, aV, thin, false, false, false, font,
                          KIFONT::METRICS::Default() );
        };

        for( int i = 0; i <= 5; ++i )
        {
            const double gx = xMin + ( xMax - xMin ) * i / 5;
            const double gy = yMin + ( yMax - yMin ) * i / 5;
            const double sx = left + plotW * i / 5;
            const double sy = top + plotH - plotH * i / 5;

            plotter.SetColor( grey );
            plotter.MoveTo( px( sx, top ) );
            plotter.FinishTo( px( sx, top + plotH ) );
            plotter.MoveTo( px( left, sy ) );
            plotter.FinishTo( px( left + plotW, sy ) );

            text( px( sx, top + plotH + 6 ), engineering( aLogX ? std::pow( 10, gx ) : gx ), GR_TEXT_H_ALIGN_CENTER,
                  GR_TEXT_V_ALIGN_TOP );
            text( px( left - 6, sy ), engineering( gy ), GR_TEXT_H_ALIGN_RIGHT, GR_TEXT_V_ALIGN_CENTER );
        }

        plotter.SetColor( ink );
        plotter.Rect( px( left, top ), px( left + plotW, top + plotH ), FILL_T::NO_FILL, thin, 0 );

        for( size_t n = 0; n < aSignals.size(); ++n )
        {
            const SIM_SIGNAL&     s = aSignals[n];
            std::vector<VECTOR2I> points;

            for( size_t i = 0; i < s.x.size(); ++i )
                points.push_back( map( s.x[i], s.y[i] ) );

            plotter.SetColor( palette[n % 5] );

            if( points.size() > 1 )
                plotter.PlotPoly( points, FILL_T::NO_FILL, 2 * thin );

            text( px( left + 8, top + 6 + 12 * n ), s.name + ( s.quantity == "value" ? "" : " (" + s.quantity + ")" ),
                  GR_TEXT_H_ALIGN_LEFT, GR_TEXT_V_ALIGN_TOP );
        }

        plotter.EndPlot();
        return FILE_STORE::ReadFile( file );
    }

} // namespace


SIM_STATE& SimState( AGENT_PROJECT& aProject )
{
    static std::map<std::string, SIM_STATE> states;
    return states[aProject.Root().string()];
}


void RegisterSimulatorTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY& tools = aServices.Tools();

    auto notify = [&aServices]( AGENT_PROJECT& aProject, const std::string& aEvent )
    {
        SCH_DOC_SESSION& session = SchematicSession( aProject );
        aServices.Observers().Notify(
                [&]( SESSION_OBSERVER& aObserver )
                {
                    aObserver.OnResults( session, JSON{ { "checker", "sim" }, { "event", aEvent } } );
                } );
    };

    aServices.AddStatus( "simulator",
                         []() -> JSON
                         {
                             std::shared_ptr<SPICE_SIMULATOR> sim = SIMULATOR::CreateInstance( "ngspice" );

                             if( !sim )
                                 return JSON{ { "engine", "ngspice" }, { "available", false } };

                             WX_STRING_REPORTER console;
                             sim->SetReporter( &console );
                             sim->Command( "version -s" );
                             sim->SetReporter( nullptr );

                             std::string       version;
                             std::smatch       match;
                             const std::string text = utf8( console.GetMessages() );

                             if( std::regex_search( text, match, std::regex( "ngspice-([0-9][0-9.+a-z]*)" ) ) )
                                 version = match[1];

                             return JSON{ { "engine", "ngspice" }, { "available", true }, { "version", version } };
                         } );

    tools.Add( { "sim_model_get",
                 "The simulation model of a symbol: its Sim.* fields (device, type, params, pins, library, name), or "
                 "that KiCad infers it from the reference and value (R, C, L with a value), or that it has none.",
                 "sim",
                 ProjectToolSchema( { { "item", { { "type", "string" }, { "description", "Reference, e.g. R1." } } } },
                                    { "item" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&   project = aServices.Sessions().Resolve( aArgs );
                     SCH_DOC_SESSION& session = SchematicSession( project );
                     SCH_FOUND        found = SchFindOne( session.Schematic(), aArgs["item"] );

                     if( found.item->Type() != SCH_SYMBOL_T )
                         throw TOOL_ERROR( "Give a symbol." );

                     SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( found.item );
                     JSON        fields = JSON::object();

                     for( const char* name : { "Sim.Device", "Sim.Type", "Sim.Params", "Sim.Pins", "Sim.Library",
                                               "Sim.Name", "Sim.Enable" } )
                     {
                         if( SCH_FIELD* field = symbol->GetField( wx( name ) ) )
                             fields[std::string( name ).substr( 4 )] = utf8( field->GetText() );
                     }

                     const std::string prefix = utf8( symbol->GetPrefix() );
                     std::string       source = "none";

                     if( !fields.empty() )
                         source = fields.contains( "Library" ) ? "library" : "built-in";
                     else if( ( prefix == "R" || prefix == "C" || prefix == "L" )
                              && !symbol->GetValue( &found.path, FOR_GUI ).IsEmpty() )
                         source = "inferred";

                     return TOOL_RESULT::Data( { { "item", aArgs["item"] },
                                                 { "model", source },
                                                 { "fields", fields },
                                                 { "value", utf8( symbol->GetValue( &found.path, FOR_GUI ) ) },
                                                 { "excludedFromSim", symbol->GetExcludedFromSim() } } );
                 } } );

    tools.Add(
            { "sim_model_set",
              "Set a symbol's simulation model as its Sim.* fields: a built-in device (device R, C, L, V, I, D, NPN, "
              "PNP, NMOS, PMOS, ...; type such as DC, SIN, PULSE for sources or GUMMELPOON, VBIC for transistors, "
              "required where a device has several models; params \"dc=0 ac=1\" or an object), "
              "or a library model (library file, name), with pins {symbol pin: model pin}. clear removes them.",
              "sim",
              ProjectToolSchema( { { "item", { { "type", "string" } } },
                                   { "device", { { "type", "string" } } },
                                   { "type", { { "type", "string" } } },
                                   { "params", { { "type", { "string", "object" } } } },
                                   { "library", { { "type", "string" } } },
                                   { "name", { { "type", "string" } } },
                                   { "pins", { { "type", "object" } } },
                                   { "clear", { { "type", "boolean" } } } },
                                 { "item" } ),
              [&tools]( const JSON& aArgs, TOOL_CONTEXT& aContext )
              {
                  JSON fields = JSON::object();

                  for( const char* name :
                       { "Sim.Device", "Sim.Type", "Sim.Params", "Sim.Pins", "Sim.Library", "Sim.Name" } )
                      fields[name] = nullptr;

                  if( !aArgs.value( "clear", false ) )
                  {
                      if( !aArgs.contains( "device" ) && !aArgs.contains( "library" ) )
                          throw TOOL_ERROR( "Give device (built-in model) or library and name, or clear." );

                      if( aArgs.contains( "device" ) )
                      {
                          checkDeviceType( aArgs["device"], aArgs.value( "type", "" ) );
                          fields["Sim.Device"] = aArgs["device"];
                      }

                      if( aArgs.contains( "type" ) )
                          fields["Sim.Type"] = aArgs["type"];

                      if( aArgs.contains( "library" ) )
                          fields["Sim.Library"] = aArgs["library"];

                      if( aArgs.contains( "name" ) )
                          fields["Sim.Name"] = aArgs["name"];

                      if( aArgs.contains( "params" ) )
                      {
                          const JSON& params = aArgs["params"];

                          if( params.is_string() )
                          {
                              fields["Sim.Params"] = params;
                          }
                          else
                          {
                              std::string text;

                              for( const auto& [key, value] : params.items() )
                                  text += ( text.empty() ? "" : " " ) + key + "=" + spiceValue( value, key );

                              fields["Sim.Params"] = text;
                          }
                      }

                      if( aArgs.contains( "pins" ) )
                      {
                          std::string text;

                          for( const auto& [pin, modelPin] : aArgs["pins"].items() )
                              text += ( text.empty() ? "" : " " ) + pin + "=" + modelPin.get<std::string>();

                          fields["Sim.Pins"] = text;
                      }
                  }

                  // The fields are set through the schematic's own symbol tool (one SCH_COMMIT).
                  const TOOL_DEF* set = tools.Find( "sch_symbol_set" );
                  JSON            args{ { "item", aArgs["item"] }, { "fields", fields } };

                  if( aArgs.contains( "project" ) )
                      args["project"] = aArgs["project"];

                  return set->handler( args, aContext );
              } } );

    // A number, or a SPICE value with a suffix ("1u", "10Meg").
    const JSON SPICE_NUMBER = { { "type", { "number", "string" } } };

    tools.Add(
            { "sim_analysis_set",
              "Set the analysis sim_run runs (kept until changed; the schematic's text directive is used when none "
              "is set): op; dc {source, start, stop, step}; ac {sweep dec|oct|lin, points, start, stop}; tran "
              "{step, stop, start}; noise {output, reference, source, points, start, stop}; pz; sp; fft; or "
              "command (one line, e.g. \".tran 1u 10m\"). ngspice .control blocks and commands are refused.",
              "sim",
              ProjectToolSchema( { { "type",
                                     { { "type", "string" },
                                       { "enum", { "op", "dc", "ac", "tran", "noise", "pz", "sp", "fft" } } } },
                                   { "command", { { "type", "string" } } },
                                   { "source", { { "type", "string" } } },
                                   { "sweep", { { "type", "string" }, { "enum", { "dec", "oct", "lin" } } } },
                                   { "points", { { "type", "integer" }, { "minimum", 1 } } },
                                   { "start", SPICE_NUMBER },
                                   { "stop", SPICE_NUMBER },
                                   { "step", SPICE_NUMBER },
                                   { "output", { { "type", "string" } } },
                                   { "reference", { { "type", "string" } } },
                                   { "nodes", { { "type", "array" } } } } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                  std::string    command;

                  if( aArgs.contains( "command" ) )
                  {
                      command = aArgs["command"];
                  }
                  else
                  {
                      const std::string type = aArgs.value( "type", "" );

                      auto need = [&]( const char* aKey ) -> const JSON&
                      {
                          if( !aArgs.contains( aKey ) )
                              throw TOOL_ERROR( "A " + type + " analysis needs " + aKey + "." );

                          return aArgs[aKey];
                      };

                      if( type == "op" )
                          command = ".op";
                      else if( type == "dc" )
                          command = ".dc " + spiceName( need( "source" ), "source" ) + " "
                                    + spiceValue( need( "start" ), "start" ) + " "
                                    + spiceValue( need( "stop" ), "stop" ) + " " + spiceValue( need( "step" ), "step" );
                      else if( type == "ac" || type == "sp" )
                          command = "." + type + " " + aArgs.value( "sweep", "dec" ) + " "
                                    + std::to_string( aArgs.value( "points", 10 ) ) + " "
                                    + spiceValue( need( "start" ), "start" ) + " "
                                    + spiceValue( need( "stop" ), "stop" );
                      else if( type == "tran" )
                          command = ".tran " + spiceValue( need( "step" ), "step" ) + " "
                                    + spiceValue( need( "stop" ), "stop" )
                                    + ( aArgs.contains( "start" ) ? " " + spiceValue( aArgs["start"], "start" ) : "" );
                      else if( type == "noise" )
                          command =
                                  ".noise v(" + spiceName( need( "output" ), "output" )
                                  + ( aArgs.contains( "reference" ) ? "," + spiceName( aArgs["reference"], "reference" )
                                                                    : "" )
                                  + ") " + spiceName( need( "source" ), "source" ) + " " + aArgs.value( "sweep", "dec" )
                                  + " " + std::to_string( aArgs.value( "points", 10 ) ) + " "
                                  + spiceValue( need( "start" ), "start" ) + " " + spiceValue( need( "stop" ), "stop" );
                      else if( type == "pz" )
                      {
                          const JSON& nodes = need( "nodes" );

                          if( !nodes.is_array() || nodes.size() != 4 )
                              throw TOOL_ERROR( "A pz analysis needs nodes [in+, in-, out+, out-]." );

                          command = ".pz";

                          for( const JSON& n : nodes )
                              command += " " + spiceName( n, "nodes" );

                          command += " vol pz";
                      }
                      else if( type == "fft" )
                          command = ".fft " + spiceName( need( "output" ), "output" );
                      else
                          throw TOOL_ERROR( "Give type or command." );
                  }

                  SimState( project ).analysis = checkedCommand( command );
                  return TOOL_RESULT::Data( { { "analysis", SimState( project ).analysis } } );
              } } );

    tools.Add( { "sim_netlist", "The SPICE netlist sim_run would simulate, with the analysis line.", "sim",
                 ProjectToolSchema( { { "analysis", { { "type", "string" } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&    project = aServices.Sessions().Resolve( aArgs );
                     SCH_DOC_SESSION&  session = SchematicSession( project );
                     std::string       messages;
                     auto              model = circuit( session, messages );
                     const std::string command = analysisOf( SimState( project ), *model, aArgs );
                     const std::string netlist = netlistText( *model, command, messages );

                     return TOOL_RESULT::Data(
                             { { "analysis", command }, { "netlist", netlist }, { "messages", messages } } );
                 } } );

    tools.Add(
            { "sim_run",
              "Simulate the project's schematic with ngspice: the analysis set by sim_analysis_set, an analysis "
              "given here, or the sheet's directive. Returns the vectors made (an op analysis returns its values). "
              "Cancel or timeoutSeconds stops ngspice.",
              "sim",
              ProjectToolSchema( { { "analysis", { { "type", "string" } } },
                                   { "timeoutSeconds", { { "type", "number" }, { "minimum", 0 } } } } ),
              [&aServices, notify]( const JSON& aArgs, TOOL_CONTEXT& aContext )
              {
                  AGENT_PROJECT&   project = aServices.Sessions().Resolve( aArgs );
                  SCH_DOC_SESSION& session = SchematicSession( project );
                  SIM_STATE&       state = SimState( project );

                  if( aArgs.contains( "timeoutSeconds" ) )
                      aContext.SetTimeout( aArgs["timeoutSeconds"] );

                  std::string       messages;
                  auto              model = circuit( session, messages );
                  const std::string command = analysisOf( state, *model, aArgs );

                  // Text directives on the sheet go into the netlist too.
                  checkNetlist( netlistText( *model, command, messages ) );

                  std::shared_ptr<SPICE_SIMULATOR>  sim = simulator();
                  std::shared_ptr<NGSPICE_SETTINGS> settings =
                          project.KicadProject().GetProjectFile().m_SchematicSettings->m_NgspiceSettings;

                  if( settings->GetWorkbookFilename().IsEmpty() )
                      settings->SetCompatibilityMode( NGSPICE_COMPATIBILITY_MODE::LT_PSPICE );

                  sim->Settings() = settings;
                  sim->Init();

                  WX_STRING_REPORTER console;
                  WX_STRING_REPORTER reporter;
                  sim->SetReporter( &console );

                  const bool attached = sim->Attach( model, wx( command ), NETLIST_EXPORTER_SPICE::OPTION_DEFAULT_FLAGS,
                                                     wx( project.Root().string() ), reporter );

                  bool cancelled = false;

                  if( attached && sim->Run() )
                  {
                      do
                      {
                          wxMilliSleep( 5 );

                          if( aContext.IsCancelled() && !cancelled )
                          {
                              sim->Stop();
                              cancelled = true;
                          }
                      } while( sim->IsRunning() );
                  }

                  // Detaching waits for a callback in flight on ngspice's thread (S0.g).
                  sim->SetReporter( nullptr );

                  state.console = utf8( console.GetMessages() );
                  state.netlist = sim->GetNetlist();
                  state.signals.clear();
                  state.measurements = JSON::array();

                  if( !attached )
                      throw TOOL_ERROR( "ngspice did not accept the circuit:\n" + utf8( reporter.GetMessages() ) );

                  if( cancelled )
                      throw TOOL_ERROR( "Cancelled; ngspice was stopped.", JSON{ { "cancelled", true } } );

                  if( state.console.find( "MIF-ERROR" ) != std::string::npos )
                  {
                      throw TOOL_ERROR( "A model needs an XSPICE code model that is not installed (install the ngspice "
                                        "package for its code models):\n"
                                                + state.console,
                                        JSON{ { "missingDependency", "ngspice code models" } } );
                  }

                  // A circuit ngspice cannot load leaves its constants plot (planck, boltz, ...) current.
                  if( utf8( sim->CurrentPlotName() ) == "const" )
                  {
                      throw TOOL_ERROR( "ngspice could not load the circuit, so no analysis ran (see sim_netlist):\n"
                                        + state.console );
                  }

                  const std::vector<std::string> vectors = sim->AllVectors();

                  if( vectors.empty() )
                      throw TOOL_ERROR( "ngspice produced no results:\n" + state.console );

                  const SIM_TYPE type = SPICE_CIRCUIT_MODEL::CommandToSimType( wx( command ) );
                  state.lastCommand = command;
                  state.xAxis = utf8( sim->GetXAxis( type ) );
                  state.runs++;

                  JSON out{ { "analysis", command },
                            { "type", utf8( SPICE_SIMULATOR::TypeToName( type, true ) ) },
                            { "xAxis", state.xAxis } };
                  JSON list = JSON::array();
                  JSON values = JSON::object();

                  for( const std::string& name : vectors )
                  {
                      std::vector<double> real = sim->GetRealVector( name );
                      std::vector<double> gain = real.empty() ? sim->GetGainVector( name ) : real;
                      list.push_back( { { "name", name }, { "points", gain.size() } } );

                      if( type == ST_OP && !real.empty() )
                          values[name] = real.front();
                  }

                  out["vectors"] = list;

                  if( type == ST_OP )
                      out["operatingPoint"] = values;

                  if( !messages.empty() )
                      out["netlistMessages"] = messages;

                  notify( project, "run" );
                  return TOOL_RESULT::Data( out );
              } } );

    tools.Add( { "sim_vectors", "The vectors of the last run: names and point counts, and the x axis.", "sim",
                 ProjectToolSchema( WithPageArgs( JSON::object() ) ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                     SIM_STATE&     state = SimState( project );
                     requireRun( state );

                     std::shared_ptr<SPICE_SIMULATOR> sim = simulator();
                     JSON                             list = JSON::array();

                     for( const std::string& name : sim->AllVectors() )
                         list.push_back( { { "name", name }, { "points", sim->GetGainVector( name ).size() } } );

                     return TOOL_RESULT::Data( { { "analysis", state.lastCommand },
                                                 { "xAxis", state.xAxis },
                                                 { "vectors", Page( list, aArgs ) },
                                                 { "total", list.size() } } );
                 } } );

    const JSON SIGNAL_PROPERTIES = {
        { "vector", { { "type", "string" }, { "description", "A vector or net name, e.g. v(/out) or out." } } },
        { "quantity",
          { { "type", "string" },
            { "enum", { "value", "gain_db", "phase", "magnitude" } },
            { "description", "AC default gain_db (dB of the magnitude); otherwise value." } } },
        { "from", { { "type", "number" } } },
        { "to", { { "type", "number" } } }
    };

    JSON signalProps = SIGNAL_PROPERTIES;
    signalProps["points"] = { { "type", "integer" }, { "minimum", 2 }, { "maximum", 10000 } };

    tools.Add( { "sim_signal",
                 "A vector of the last run against its x axis (time, frequency, sweep), decimated to points (200), "
                 "optionally between from and to. The simulator viewer plots it.",
                 "sim", ProjectToolSchema( signalProps, { "vector" } ),
                 [&aServices, notify]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                     SIM_STATE&     state = SimState( project );
                     requireRun( state );

                     std::shared_ptr<SPICE_SIMULATOR> sim = simulator();
                     SERIES                           full =
                             window( series( *sim, state, aArgs["vector"], aArgs.value( "quantity", "" ) ), aArgs );
                     SERIES shown = decimate( full, aArgs.value( "points", 200 ) );

                     SIM_SIGNAL signal{ vectorName( *sim, aArgs["vector"] ), shown.quantity, shown.x, shown.y };
                     auto       same = std::find_if( state.signals.begin(), state.signals.end(),
                                                     [&]( const SIM_SIGNAL& s )
                                                     {
                                                   return s.name == signal.name && s.quantity == signal.quantity;
                                               } );

                     if( same != state.signals.end() )
                         *same = signal;
                     else
                         state.signals.push_back( signal );

                     notify( project, "signal" );
                     return TOOL_RESULT::Data( { { "vector", signal.name },
                                                 { "quantity", shown.quantity },
                                                 { "xAxis", state.xAxis },
                                                 { "pointsTotal", full.x.size() },
                                                 { "x", shown.x },
                                                 { "y", shown.y } } );
                 } } );

    JSON measureProps = SIGNAL_PROPERTIES;
    measureProps["kind"] = {
        { "type", "string" }, { "enum", { "min", "max", "average", "rms", "at", "crossing", "rise_time", "fall_time" } }
    };
    measureProps["x"] = { { "type", "number" }, { "description", "For at: the x to read." } };
    measureProps["level"] = { { "type", "number" }, { "description", "For crossing: the y level." } };
    measureProps["direction"] = { { "type", "string" }, { "enum", { "rising", "falling", "either" } } };
    measureProps["low"] = { { "type", "number" }, { "description", "rise/fall: low fraction (0.1)." } };
    measureProps["high"] = { { "type", "number" }, { "description", "rise/fall: high fraction (0.9)." } };

    tools.Add( { "sim_measure",
                 "Measure a vector of the last run over its x axis (optionally between from and to): min, max, "
                 "average, rms (time-weighted), at (value at x), crossing (first x where it crosses level, e.g. the "
                 "-3 dB point of gain_db), rise_time and fall_time (10% to 90% of the swing by default).",
                 "sim", ProjectToolSchema( measureProps, { "vector", "kind" } ),
                 [&aServices, notify]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                     SIM_STATE&     state = SimState( project );
                     requireRun( state );

                     std::shared_ptr<SPICE_SIMULATOR> sim = simulator();
                     const SERIES                     s =
                             window( series( *sim, state, aArgs["vector"], aArgs.value( "quantity", "" ) ), aArgs );
                     const std::string kind = aArgs["kind"];
                     JSON              out{ { "vector", vectorName( *sim, aArgs["vector"] ) },
                                            { "quantity", s.quantity },
                                            { "kind", kind } };

                     if( s.y.empty() )
                         throw TOOL_ERROR( "The vector is empty." );

                     if( kind == "min" || kind == "max" )
                     {
                         auto it = kind == "min" ? std::min_element( s.y.begin(), s.y.end() )
                                                 : std::max_element( s.y.begin(), s.y.end() );
                         out["value"] = *it;
                         out["at"] = s.x[it - s.y.begin()];
                     }
                     else if( kind == "average" || kind == "rms" )
                     {
                         double area = 0, span = s.x.back() - s.x.front();

                         for( size_t i = 1; i < s.x.size(); ++i )
                         {
                             const double a = kind == "rms" ? s.y[i - 1] * s.y[i - 1] : s.y[i - 1];
                             const double b = kind == "rms" ? s.y[i] * s.y[i] : s.y[i];
                             area += ( a + b ) / 2 * ( s.x[i] - s.x[i - 1] );
                         }

                         const double mean = span > 0 ? area / span : ( kind == "rms" ? s.y[0] * s.y[0] : s.y[0] );
                         out["value"] = kind == "rms" ? std::sqrt( mean ) : mean;
                     }
                     else if( kind == "at" )
                     {
                         if( !aArgs.contains( "x" ) )
                             throw TOOL_ERROR( "at needs x." );

                         out["x"] = aArgs["x"];
                         out["value"] = interpolate( s, aArgs["x"] );
                     }
                     else if( kind == "crossing" )
                     {
                         if( !aArgs.contains( "level" ) )
                             throw TOOL_ERROR( "crossing needs level." );

                         std::optional<double> x = crossing( s, aArgs["level"], aArgs.value( "direction", "either" ) );

                         if( !x )
                             throw TOOL_ERROR( "The vector does not cross that level." );

                         out["level"] = aArgs["level"];
                         out["value"] = *x;
                     }
                     else
                     {
                         const double          lo = *std::min_element( s.y.begin(), s.y.end() );
                         const double          hi = *std::max_element( s.y.begin(), s.y.end() );
                         const double          low = lo + ( hi - lo ) * aArgs.value( "low", 0.1 );
                         const double          high = lo + ( hi - lo ) * aArgs.value( "high", 0.9 );
                         const std::string     dir = kind == "rise_time" ? "rising" : "falling";
                         std::optional<double> a = crossing( s, dir == "rising" ? low : high, dir );
                         std::optional<double> b = crossing( s, dir == "rising" ? high : low, dir );

                         if( !a || !b || *b < *a )
                             throw TOOL_ERROR( "No complete " + std::string( dir == "rising" ? "rise" : "fall" )
                                               + " in the range." );

                         out["value"] = *b - *a;
                         out["from"] = *a;
                         out["to"] = *b;
                     }

                     state.measurements.push_back( out );
                     notify( project, "measure" );
                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add(
            { "sim_plot_capture",
              "A PNG plot of vectors of the last run (default: the signals already asked for with sim_signal). "
              "Frequency is on a log axis.",
              "sim",
              ProjectToolSchema( { { "vectors", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                                   { "quantity", SIGNAL_PROPERTIES["quantity"] },
                                   { "width", { { "type", "integer" }, { "minimum", 200 }, { "maximum", 4096 } } },
                                   { "height", { { "type", "integer" }, { "minimum", 150 }, { "maximum", 4096 } } } } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                  SIM_STATE&     state = SimState( project );
                  requireRun( state );

                  std::vector<SIM_SIGNAL> signals;

                  if( aArgs.contains( "vectors" ) )
                  {
                      std::shared_ptr<SPICE_SIMULATOR> sim = simulator();

                      for( const JSON& name : aArgs["vectors"] )
                      {
                          SERIES s = decimate( series( *sim, state, name, aArgs.value( "quantity", "" ) ), 2000 );
                          signals.push_back( { vectorName( *sim, name ), s.quantity, s.x, s.y } );
                      }
                  }
                  else
                  {
                      signals = state.signals;
                  }

                  if( signals.empty() )
                      throw TOOL_ERROR( "Nothing to plot: give vectors, or ask for signals with sim_signal first." );

                  const bool  logX = state.xAxis == "frequency";
                  std::string png =
                          plotPng( project, signals, logX, aArgs.value( "width", 1024 ), aArgs.value( "height", 614 ) );

                  TOOL_RESULT result;
                  result.content.push_back(
                          ImageContent( wxBase64Encode( png.data(), png.size() ).ToStdString(), "image/png" ) );
                  JSON info{ { "xAxis", state.xAxis }, { "logX", logX }, { "signals", signals.size() } };
                  result.content.push_back( TextContent( info.dump() ) );
                  result.structuredContent = info;
                  return result;
              } } );
}

} // namespace AGENTIC
