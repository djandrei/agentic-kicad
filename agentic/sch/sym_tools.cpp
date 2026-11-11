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


#include <agentic/core/core_tools.h>
#include <agentic/core/handles.h>
#include <agentic/core/services.h>
#include <agentic/sch/sch_items.h>
#include <agentic/sch/sym_lib_session.h>
#include <agentic/sch/sym_tools.h>

#include <base_units.h>
#include <lib_symbol.h>
#include <libraries/library_manager.h>
#include <plotters/plotter_png.h>
#include <sch_field.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr_lib_cache.h>
#include <sch_pin.h>
#include <sch_plotter.h>
#include <sch_render_settings.h>
#include <sch_shape.h>
#include <sch_text.h>
#include <settings/color_settings.h>
#include <settings/settings_manager.h>
#include <units_provider.h>
#include <locale_io.h>

#include <wx/image.h>
#include <wx/mstream.h>

#include <cmath>
#include <map>


// Defined in eeschema/symbol_checker.cpp.
void CheckLibSymbol( LIB_SYMBOL* aSymbol, std::vector<wxString>& aMessages, int aGridForPins,
                     UNITS_PROVIDER* aUnitsProvider );


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


    const double PITCH_MM = 2.54;


    JSON symSchema( JSON aProperties, JSON aRequired = JSON::array() )
    {
        aProperties["library"] = {
            { "type", "string" }, { "description", "Project symbol library: nickname (lib_tables) or .kicad_sym path." }
        };
        aProperties["force"] = { { "type", "boolean" } };
        aRequired.push_back( "library" );
        return ProjectToolSchema( aProperties, aRequired );
    }


    const std::map<std::string, ELECTRICAL_PINTYPE> PIN_TYPES = {
        { "input", ELECTRICAL_PINTYPE::PT_INPUT },
        { "output", ELECTRICAL_PINTYPE::PT_OUTPUT },
        { "bidirectional", ELECTRICAL_PINTYPE::PT_BIDI },
        { "tri_state", ELECTRICAL_PINTYPE::PT_TRISTATE },
        { "passive", ELECTRICAL_PINTYPE::PT_PASSIVE },
        { "free", ELECTRICAL_PINTYPE::PT_NIC },
        { "unspecified", ELECTRICAL_PINTYPE::PT_UNSPECIFIED },
        { "power_in", ELECTRICAL_PINTYPE::PT_POWER_IN },
        { "power_out", ELECTRICAL_PINTYPE::PT_POWER_OUT },
        { "open_collector", ELECTRICAL_PINTYPE::PT_OPENCOLLECTOR },
        { "open_emitter", ELECTRICAL_PINTYPE::PT_OPENEMITTER },
        { "no_connect", ELECTRICAL_PINTYPE::PT_NC },
    };

    const std::map<std::string, GRAPHIC_PINSHAPE> PIN_SHAPES = {
        { "line", GRAPHIC_PINSHAPE::LINE },
        { "inverted", GRAPHIC_PINSHAPE::INVERTED },
        { "clock", GRAPHIC_PINSHAPE::CLOCK },
        { "inverted_clock", GRAPHIC_PINSHAPE::INVERTED_CLOCK },
        { "input_low", GRAPHIC_PINSHAPE::INPUT_LOW },
        { "clock_low", GRAPHIC_PINSHAPE::CLOCK_LOW },
        { "output_low", GRAPHIC_PINSHAPE::OUTPUT_LOW },
        { "edge_clock_high", GRAPHIC_PINSHAPE::FALLING_EDGE_CLOCK },
        { "non_logic", GRAPHIC_PINSHAPE::NONLOGIC },
    };


    template <typename T>
    T lookup( const std::map<std::string, T>& aMap, const std::string& aKey, const std::string& aWhat )
    {
        auto it = aMap.find( aKey );

        if( it == aMap.end() )
        {
            std::string known;

            for( const auto& [name, value] : aMap )
                known += ( known.empty() ? "" : ", " ) + name;

            throw TOOL_ERROR( "Unknown " + aWhat + " '" + aKey + "'. Known: " + known + "." );
        }

        return it->second;
    }


    JSON symbolJson( const LIB_SYMBOL& aSymbol )
    {
        JSON pins = JSON::array();

        for( const SCH_PIN* pin : aSymbol.GetGraphicalPins( 0, 0 ) )
        {
            pins.push_back( { { "number", utf8( pin->GetNumber() ) },
                              { "name", utf8( pin->GetName() ) },
                              { "type", utf8( pin->GetCanonicalElectricalTypeName() ) },
                              { "unit", pin->GetUnit() },
                              { "bodyStyle", pin->GetBodyStyle() },
                              { "position", SchPointJson( pin->GetPosition() ) } } );
        }

        JSON out{ { "name", utf8( aSymbol.GetName() ) },
                  { "reference", utf8( aSymbol.GetReferenceField().GetText() ) },
                  { "value", utf8( aSymbol.GetValueField().GetText() ) },
                  { "description", utf8( aSymbol.GetDescription() ) },
                  { "keywords", utf8( aSymbol.GetKeyWords() ) },
                  { "footprint", utf8( aSymbol.GetFootprintField().GetText() ) },
                  { "units", aSymbol.GetUnitCount() },
                  { "bodyStyles", aSymbol.GetBodyStyleCount() },
                  { "power", aSymbol.IsPower() },
                  { "pins", pins } };

        if( aSymbol.IsDerived() )
        {
            if( std::shared_ptr<LIB_SYMBOL> parent = aSymbol.GetParent().lock() )
                out["derivedFrom"] = utf8( parent->GetName() );
        }

        return out;
    }


    /// Run one change to a symbol library: \a aEdit changes it, the library is saved (D5).
    JSON symMutate( AGENT_SERVICES& aServices, const JSON& aArgs, const std::string& aLabel,
                    const std::function<JSON( SYMBOL_LIB_SESSION& )>& aEdit )
    {
        AGENT_PROJECT&      project = aServices.Sessions().Resolve( aArgs );
        SYMBOL_LIB_SESSION& session = SymbolLibSession( project, aArgs["library"].get<std::string>() );

        JSON result = project.Mutate( aLabel, aArgs.value( "force", false ),
                                      [&]() -> JSON
                                      {
                                          JSON out = aEdit( session );
                                          session.MarkModified();
                                          session.RecordChange(
                                                  { { "summary", aLabel }, { "symbol", session.CurrentSymbol() } } );
                                          return out;
                                      } );

        // Searches and placements read the library through the project's adapter.
        if( LIBRARY_MANAGER_ADAPTER* adapter = project.LibraryAdapter( LIBRARY_TABLE_TYPE::SYMBOL ) )
        {
            if( fs::path( aArgs["library"].get<std::string>() ).extension() != ".kicad_sym" )
                project.Libraries().ReloadLibraryEntry( LIBRARY_TABLE_TYPE::SYMBOL, wx( aArgs["library"] ) );
        }

        return result;
    }


    void setField( LIB_SYMBOL& aSymbol, const std::string& aName, const std::string& aValue )
    {
        if( aName == "Reference" )
            aSymbol.GetReferenceField().SetText( wx( aValue ) );
        else if( aName == "Value" )
            aSymbol.GetValueField().SetText( wx( aValue ) );
        else if( aName == "Footprint" )
            aSymbol.GetFootprintField().SetText( wx( aValue ) );
        else if( aName == "Datasheet" )
            aSymbol.GetDatasheetField().SetText( wx( aValue ) );
        else if( aName == "Description" )
            aSymbol.SetDescription( wx( aValue ) );
        else if( aName == "Keywords" )
            aSymbol.SetKeyWords( wx( aValue ) );
        else
        {
            SCH_FIELD* field = aSymbol.GetField( wx( aName ) );

            if( !field )
            {
                field = new SCH_FIELD( &aSymbol, FIELD_T::USER, wx( aName ) );
                field->SetVisible( false );
                aSymbol.AddField( field );
            }

            field->SetText( wx( aValue ) );
        }
    }


    /// Plot one unit and body style of a symbol to an in-memory PNG; returns the PNG bytes.
    std::string plotSymbolPng( LIB_SYMBOL& aSymbol, int aUnit, int aBodyStyle, int aWidth, double& aPxPerMm )
    {
        LIB_SYMBOL*                 drawSymbol = &aSymbol;
        std::shared_ptr<LIB_SYMBOL> root;

        if( aSymbol.IsDerived() )
        {
            root = aSymbol.GetRootSymbol();
            drawSymbol = root.get();
        }

        BOX2I bbox = GetSymbolPlotBBox( aSymbol, aUnit, aBodyStyle, false );
        bbox.Inflate( schIUScale.mmToIU( 1.27 ) );

        const double widthMm = schIUScale.IUTomm( bbox.GetWidth() );
        aPxPerMm = aWidth / widthMm;
        const int dpi = std::clamp( static_cast<int>( std::lround( aPxPerMm * 25.4 ) ), 20, 2400 );

        PAGE_INFO page( PAGE_SIZE_TYPE::User );
        page.SetWidthMils( schIUScale.IUToMils( bbox.GetWidth() ) );
        page.SetHeightMils( schIUScale.IUToMils( bbox.GetHeight() ) );

        SCH_RENDER_SETTINGS renderSettings;
        renderSettings.LoadColors( ::GetColorSettings( wxEmptyString ) );
        renderSettings.SetDefaultPenWidth( DEFAULT_LINE_WIDTH_MILS * schIUScale.IU_PER_MILS );

        PNG_PLOTTER plotter;
        plotter.SetPixelSize( KiROUND( page.GetWidthMils() * dpi / 1000.0 ),
                              KiROUND( page.GetHeightMils() * dpi / 1000.0 ) );
        plotter.SetResolution( dpi );
        plotter.SetAntialias( true );
        plotter.SetRenderSettings( &renderSettings );
        plotter.SetPageSettings( page );
        plotter.SetColorMode( true );
        plotter.SetViewport( bbox.GetOrigin(), schIUScale.IU_PER_MILS / 10, 1.0, false );

        const fs::path file = fs::temp_directory_path() / ( "agentic-sym-" + std::to_string( ::getpid() ) + ".png" );

        if( !plotter.OpenFile( wx( file.string() ) ) )
            throw TOOL_ERROR( "Cannot plot the symbol." );

        LOCALE_IO     toggle;
        SCH_PLOT_OPTS options;
        plotter.StartPlot( wxS( "1" ) );
        plotter.SetColor( renderSettings.GetLayerColor( LAYER_SCHEMATIC_BACKGROUND ) );
        plotter.Rect( bbox.GetOrigin(), bbox.GetEnd(), FILL_T::FILLED_SHAPE, 1.0, 0 );
        drawSymbol->Plot( &plotter, true, options, aUnit, aBodyStyle, VECTOR2I( 0, 0 ), false );
        drawSymbol->Plot( &plotter, false, options, aUnit, aBodyStyle, VECTOR2I( 0, 0 ), false );
        aSymbol.PlotFields( &plotter, false, options, aUnit, aBodyStyle, VECTOR2I( 0, 0 ), false );
        plotter.EndPlot();

        std::string png = FILE_STORE::ReadFile( file );
        fs::remove( file );
        return png;
    }


    std::string base64( const std::string& aData )
    {
        static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string        out;
        int                val = 0, bits = -6;

        for( unsigned char c : aData )
        {
            val = ( val << 8 ) + c;
            bits += 8;

            while( bits >= 0 )
            {
                out.push_back( table[( val >> bits ) & 0x3F] );
                bits -= 6;
            }
        }

        if( bits > -6 )
            out.push_back( table[( ( val << 8 ) >> ( bits + 8 ) ) & 0x3F] );

        while( out.size() % 4 )
            out.push_back( '=' );

        return out;
    }

} // namespace


void RegisterSymbolEditorTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY& tools = aServices.Tools();

    tools.Add( { "sym_list", "The symbols of a project symbol library, with their pins.", "sym",
                 symSchema( WithPageArgs( JSON::object() ) ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&      project = aServices.Sessions().Resolve( aArgs );
                     SYMBOL_LIB_SESSION& session = SymbolLibSession( project, aArgs["library"] );
                     JSON                symbols = JSON::array();

                     for( const auto& [name, symbol] : session.Library().GetSymbolMap() )
                         symbols.push_back( symbolJson( *symbol ) );

                     return TOOL_RESULT::Data( JSON{ { "library", session.Path() },
                                                     { "symbols", Page( symbols, aArgs ) },
                                                     { "total", symbols.size() } } );
                 } } );

    tools.Add( { "sym_create",
                 "Create a new symbol in a project symbol library (created empty if the file is new): name, "
                 "reference prefix (R, U, ...), value, description, keywords, footprint, footprintFilters, units "
                 "(count), power. Add pins with sym_pins_set.",
                 "sym",
                 symSchema( { { "name", { { "type", "string" } } },
                              { "reference", { { "type", "string" } } },
                              { "value", { { "type", "string" } } },
                              { "description", { { "type", "string" } } },
                              { "keywords", { { "type", "string" } } },
                              { "footprint", { { "type", "string" } } },
                              { "footprintFilters", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                              { "units", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 64 } } },
                              { "power", { { "type", "boolean" } } } },
                            { "name" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     const std::string name = aArgs["name"];

                     return TOOL_RESULT::Data( symMutate(
                             aServices, aArgs, "sym_create " + name,
                             [&]( SYMBOL_LIB_SESSION& aSession ) -> JSON
                             {
                                 if( aSession.Library().GetSymbol( wx( name ) ) )
                                     throw TOOL_ERROR( "A symbol '" + name + "' is already in the library." );

                                 auto symbol = std::make_unique<LIB_SYMBOL>( wx( name ) );
                                 symbol->GetReferenceField().SetText( wx( aArgs.value( "reference", "U" ) ) );
                                 symbol->GetValueField().SetText( wx( aArgs.value( "value", name ) ) );
                                 symbol->SetDescription( wx( aArgs.value( "description", "" ) ) );
                                 symbol->SetKeyWords( wx( aArgs.value( "keywords", "" ) ) );
                                 symbol->GetFootprintField().SetText( wx( aArgs.value( "footprint", "" ) ) );

                                 wxArrayString filters;

                                 for( const JSON& filter : aArgs.value( "footprintFilters", JSON::array() ) )
                                     filters.Add( wx( filter.get<std::string>() ) );

                                 symbol->SetFPFilters( filters );
                                 symbol->SetUnitCount( aArgs.value( "units", 1 ), false );

                                 if( aArgs.value( "power", false ) )
                                     symbol->SetGlobalPower();

                                 JSON out = symbolJson( *symbol );
                                 aSession.Add( std::move( symbol ) );
                                 aSession.SetCurrentSymbol( name );
                                 return out;
                             } ) );
                 } } );

    tools.Add( { "sym_derive",
                 "Create a symbol derived from another in the same library (same drawing and pins, its own name "
                 "and value; it starts with the parent's reference, footprint, datasheet and user fields).",
                 "sym",
                 symSchema( { { "name", { { "type", "string" } } },
                              { "parent", { { "type", "string" } } },
                              { "value", { { "type", "string" } } },
                              { "description", { { "type", "string" } } } },
                            { "name", "parent" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     const std::string name = aArgs["name"];

                     return TOOL_RESULT::Data( symMutate(
                             aServices, aArgs, "sym_derive " + name,
                             [&]( SYMBOL_LIB_SESSION& aSession ) -> JSON
                             {
                                 if( aSession.Library().GetSymbol( wx( name ) ) )
                                     throw TOOL_ERROR( "A symbol '" + name + "' is already in the library." );

                                 LIB_SYMBOL& parent = aSession.Symbol( aArgs["parent"] );

                                 if( parent.IsDerived() )
                                     throw TOOL_ERROR( "Derive from a root symbol, not from a derived one." );

                                 auto symbol = std::make_unique<LIB_SYMBOL>( wx( name ), &parent );

                                 // As the symbol editor derives: the parent's fields (reference, footprint,
                                 // datasheet and user fields) with their text, then the new value.
                                 for( FIELD_T fieldId : MANDATORY_FIELDS )
                                 {
                                     SCH_FIELD* field = symbol->GetField( fieldId );
                                     *field = *parent.GetField( fieldId );
                                     field->SetParent( symbol.get() );
                                 }

                                 std::vector<SCH_FIELD*> parentFields;
                                 parent.GetFields( parentFields );

                                 for( SCH_FIELD* field : parentFields )
                                 {
                                     if( field->GetId() == FIELD_T::USER )
                                     {
                                         SCH_FIELD* copy = new SCH_FIELD( *field );
                                         copy->SetParent( symbol.get() );
                                         symbol->AddField( copy );
                                     }
                                 }

                                 symbol->GetValueField().SetText( wx( aArgs.value( "value", name ) ) );

                                 if( aArgs.contains( "description" ) )
                                     symbol->SetDescription( wx( aArgs["description"] ) );

                                 JSON out = symbolJson( *symbol );
                                 aSession.Add( std::move( symbol ) );
                                 aSession.SetCurrentSymbol( name );
                                 return out;
                             } ) );
                 } } );

    tools.Add( { "sym_units_set", "Set a symbol's unit count and optional unit names {\"1\": \"A\", ...}.", "sym",
                 symSchema( { { "symbol", { { "type", "string" } } },
                              { "count", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 64 } } },
                              { "names", { { "type", "object" } } } },
                            { "symbol", "count" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     return TOOL_RESULT::Data( symMutate(
                             aServices, aArgs, "sym_units_set " + aArgs["symbol"].get<std::string>(),
                             [&]( SYMBOL_LIB_SESSION& aSession ) -> JSON
                             {
                                 LIB_SYMBOL& symbol = aSession.Symbol( aArgs["symbol"] );
                                 symbol.SetUnitCount( aArgs["count"], false );
                                 const JSON names = aArgs.value( "names", JSON::object() );

                                 for( const auto& [unit, label] : names.items() )
                                     symbol.GetUnitDisplayNames()[std::stoi( unit )] = wx( label.get<std::string>() );

                                 aSession.SetCurrentSymbol( aArgs["symbol"] );
                                 return symbolJson( symbol );
                             } ) );
                 } } );

    tools.Add(
            { "sym_pins_set",
              "Set a symbol's pins from a table [{number, name, type, shape, side, unit, bodyStyle, position?, "
              "length?}]. type: input, output, bidirectional, tri_state, passive, free, unspecified, power_in, "
              "power_out, open_collector, open_emitter, no_connect. side: left, right, top, bottom. Pins without "
              "position are laid out on a 2.54 mm pitch around a body rectangle drawn for each unit. replace "
              "(default true) removes the units' old pins and bodies first.",
              "sym",
              symSchema( { { "symbol", { { "type", "string" } } },
                           { "pins", { { "type", "array" }, { "items", { { "type", "object" } } } } },
                           { "replace", { { "type", "boolean" } } },
                           { "body",
                             { { "type", "boolean" },
                               { "description", "Draw a body rectangle per unit; default true." } } } },
                         { "symbol", "pins" } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  return TOOL_RESULT::Data( symMutate(
                          aServices, aArgs, "sym_pins_set " + aArgs["symbol"].get<std::string>(),
                          [&]( SYMBOL_LIB_SESSION& aSession ) -> JSON
                          {
                              LIB_SYMBOL& symbol = aSession.Symbol( aArgs["symbol"] );

                              if( symbol.IsDerived() )
                                  throw TOOL_ERROR( "A derived symbol takes its pins from its parent." );

                              struct SPEC
                              {
                                  JSON        json;
                                  std::string side;
                                  int         unit;
                                  int         bodyStyle;
                              };

                              std::map<std::pair<int, int>, std::map<std::string, std::vector<SPEC>>> layout;
                              std::set<std::pair<int, int>>                                           units;

                              for( const JSON& pin : aArgs["pins"] )
                              {
                                  SPEC spec{ pin, pin.value( "side", "left" ), pin.value( "unit", 1 ),
                                             pin.value( "bodyStyle", 1 ) };

                                  if( spec.unit < 1 || spec.unit > symbol.GetUnitCount() )
                                      throw TOOL_ERROR( "Pin " + pin.value( "number", "?" ) + ": unit "
                                                        + std::to_string( spec.unit )
                                                        + " does not exist (sym_units_set first)." );

                                  if( !pin.contains( "number" ) )
                                      throw TOOL_ERROR( "Every pin needs a number." );

                                  layout[{ spec.unit, spec.bodyStyle }][spec.side].push_back( spec );
                                  units.insert( { spec.unit, spec.bodyStyle } );
                              }

                              if( aArgs.value( "replace", true ) )
                              {
                                  std::vector<SCH_ITEM*> remove;

                                  for( SCH_ITEM& item : symbol.GetDrawItems() )
                                  {
                                      if( item.Type() == SCH_FIELD_T )
                                          continue;

                                      if( units.count( { item.GetUnit(), item.GetBodyStyle() } )
                                          || ( item.GetUnit() == 0 && item.Type() == SCH_PIN_T ) )
                                      {
                                          remove.push_back( &item );
                                      }
                                  }

                                  for( SCH_ITEM* item : remove )
                                      symbol.RemoveDrawItem( item );
                              }

                              const int pitch = SchIu( PITCH_MM, false );
                              const int length = SchIu( 2.54, false );
                              const int textSize = SchIu( 1.27, false );

                              int  maxHalfW = 0;
                              int  maxHalfH = 0;
                              bool anyTopBottom = false;

                              for( auto& [key, sides] : layout )
                              {
                                  const auto& [unit, bodyStyle] = key;
                                  size_t rows =
                                          std::max( { size_t( 1 ), sides["left"].size(), sides["right"].size() } );
                                  size_t cols = std::max( sides["top"].size(), sides["bottom"].size() );
                                  size_t longest = 1;

                                  for( const auto& [side, pins] : sides )
                                  {
                                      for( const SPEC& spec : pins )
                                          longest = std::max( longest, spec.json.value( "name", "" ).size() );
                                  }

                                  // Body half sizes, on the grid, wide enough for two names.
                                  const int halfH = pitch * static_cast<int>( ( rows + 1 ) / 2 + 1 );
                                  const int halfW = std::max( pitch * static_cast<int>( ( cols + 1 ) / 2 + 1 ),
                                                              pitch * static_cast<int>( ( longest + 2 ) / 2 + 1 ) );

                                  auto place = [&]( const std::string& aSide, size_t aIndex, size_t aCount ) -> VECTOR2I
                                  {
                                      const int start = -pitch * static_cast<int>( aCount - 1 ) / 2;
                                      const int offset = ( start / pitch ) * pitch + pitch * static_cast<int>( aIndex );

                                      if( aSide == "left" )
                                          return VECTOR2I( -halfW - length, offset );

                                      if( aSide == "right" )
                                          return VECTOR2I( halfW + length, offset );

                                      if( aSide == "top" )
                                          return VECTOR2I( offset, -halfH - length );

                                      return VECTOR2I( offset, halfH + length );
                                  };

                                  for( auto& [side, pins] : sides )
                                  {
                                      PIN_ORIENTATION orientation = side == "left"    ? PIN_ORIENTATION::PIN_RIGHT
                                                                    : side == "right" ? PIN_ORIENTATION::PIN_LEFT
                                                                    : side == "top"   ? PIN_ORIENTATION::PIN_DOWN
                                                                                      : PIN_ORIENTATION::PIN_UP;

                                      if( side != "left" && side != "right" && side != "top" && side != "bottom" )
                                          throw TOOL_ERROR( "Pin side is left, right, top or bottom; got '" + side
                                                            + "'." );

                                      for( size_t i = 0; i < pins.size(); ++i )
                                      {
                                          const JSON& spec = pins[i].json;
                                          VECTOR2I    pos = spec.contains( "position" )
                                                                    ? SchPoint( spec["position"], "position" )
                                                                    : place( side, i, pins.size() );
                                          const int   pinLength =
                                                  spec.contains( "length" )
                                                            ? SchIu( ParseLengthMm( spec["length"], "length" ), false )
                                                            : length;

                                          auto* pin = new SCH_PIN(
                                                  &symbol, wx( spec.value( "name", "~" ) ),
                                                  wx( spec["number"].get<std::string>() ), orientation,
                                                  lookup( PIN_TYPES, spec.value( "type", "passive" ), "pin type" ),
                                                  pinLength, textSize, textSize, bodyStyle, pos, unit );
                                          pin->SetShape(
                                                  lookup( PIN_SHAPES, spec.value( "shape", "line" ), "pin shape" ) );
                                          symbol.AddDrawItem( pin );
                                      }
                                  }

                                  if( aArgs.value( "body", true ) )
                                  {
                                      auto* body = new SCH_SHAPE( SHAPE_T::RECTANGLE, LAYER_DEVICE );
                                      body->SetStart( VECTOR2I( -halfW, -halfH ) );
                                      body->SetEnd( VECTOR2I( halfW, halfH ) );
                                      body->SetUnit( unit );
                                      body->SetBodyStyle( bodyStyle );
                                      body->SetFillMode( FILL_T::FILLED_WITH_BG_BODYCOLOR );
                                      symbol.AddDrawItem( body );
                                  }

                                  maxHalfW = std::max( maxHalfW, halfW );
                                  maxHalfH = std::max( maxHalfH, halfH );
                                  anyTopBottom |= cols > 0;
                              }

                              // The fields are the symbol's, shared by every unit: above and below
                              // the largest body, clear of top and bottom pins.
                              const int fieldX = anyTopBottom ? -maxHalfW : 0;
                              symbol.GetReferenceField().SetPosition( VECTOR2I( fieldX, -maxHalfH - pitch ) );
                              symbol.GetValueField().SetPosition( VECTOR2I( fieldX, maxHalfH + pitch ) );
                              symbol.GetReferenceField().SetHorizJustify( anyTopBottom ? GR_TEXT_H_ALIGN_LEFT
                                                                                       : GR_TEXT_H_ALIGN_CENTER );
                              symbol.GetValueField().SetHorizJustify( anyTopBottom ? GR_TEXT_H_ALIGN_LEFT
                                                                                   : GR_TEXT_H_ALIGN_CENTER );

                              aSession.SetCurrentSymbol( aArgs["symbol"] );
                              return symbolJson( symbol );
                          } ) );
              } } );

    tools.Add( { "sym_graphic_add",
                 "Draw on a symbol: kind rectangle {start, end}, circle {center, radius}, polyline {points}, text "
                 "{position, text}; unit (0 = all units), fill none|outline|background.",
                 "sym",
                 symSchema( { { "symbol", { { "type", "string" } } },
                              { "kind",
                                { { "type", "string" }, { "enum", { "rectangle", "circle", "polyline", "text" } } } },
                              { "start", { { "type", { "object", "array" } } } },
                              { "end", { { "type", { "object", "array" } } } },
                              { "center", { { "type", { "object", "array" } } } },
                              { "radius", { { "type", { "number", "string" } } } },
                              { "points", { { "type", "array" } } },
                              { "position", { { "type", { "object", "array" } } } },
                              { "text", { { "type", "string" } } },
                              { "unit", { { "type", "integer" }, { "minimum", 0 } } },
                              { "fill", { { "type", "string" }, { "enum", { "none", "outline", "background" } } } } },
                            { "symbol", "kind" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     return TOOL_RESULT::Data( symMutate(
                             aServices, aArgs, "sym_graphic_add " + aArgs["symbol"].get<std::string>(),
                             [&]( SYMBOL_LIB_SESSION& aSession ) -> JSON
                             {
                                 LIB_SYMBOL&       symbol = aSession.Symbol( aArgs["symbol"] );
                                 const std::string kind = aArgs["kind"];
                                 const std::string fill = aArgs.value( "fill", "none" );
                                 SCH_ITEM*         item = nullptr;

                                 if( kind == "text" )
                                 {
                                     auto* text = new SCH_TEXT( SchPoint( aArgs.at( "position" ), "position", false ),
                                                                wx( aArgs.value( "text", "" ) ), LAYER_DEVICE );
                                     item = text;
                                 }
                                 else
                                 {
                                     auto* shape = new SCH_SHAPE( kind == "rectangle" ? SHAPE_T::RECTANGLE
                                                                  : kind == "circle"  ? SHAPE_T::CIRCLE
                                                                                      : SHAPE_T::POLY,
                                                                  LAYER_DEVICE );

                                     if( kind == "rectangle" )
                                     {
                                         shape->SetStart( SchPoint( aArgs.at( "start" ), "start", false ) );
                                         shape->SetEnd( SchPoint( aArgs.at( "end" ), "end", false ) );
                                     }
                                     else if( kind == "circle" )
                                     {
                                         VECTOR2I  center = SchPoint( aArgs.at( "center" ), "center", false );
                                         const int radius =
                                                 SchIu( ParseLengthMm( aArgs.at( "radius" ), "radius" ), false );
                                         shape->SetStart( center );
                                         shape->SetEnd( center + VECTOR2I( radius, 0 ) );
                                     }
                                     else
                                     {
                                         for( const JSON& point : aArgs.at( "points" ) )
                                             shape->AddPoint( SchPoint( point, "points", false ) );
                                     }

                                     shape->SetFillMode( fill == "outline"      ? FILL_T::FILLED_SHAPE
                                                         : fill == "background" ? FILL_T::FILLED_WITH_BG_BODYCOLOR
                                                                                : FILL_T::NO_FILL );
                                     item = shape;
                                 }

                                 item->SetParent( &symbol );
                                 item->SetUnit( aArgs.value( "unit", 0 ) );
                                 symbol.AddDrawItem( item );
                                 aSession.SetCurrentSymbol( aArgs["symbol"] );
                                 return symbolJson( symbol );
                             } ) );
                 } } );

    tools.Add( { "sym_field_set",
                 "Set a symbol's fields: Reference, Value, Footprint, Datasheet, Description, Keywords, or any user "
                 "field (created hidden).",
                 "sym",
                 symSchema( { { "symbol", { { "type", "string" } } }, { "fields", { { "type", "object" } } } },
                            { "symbol", "fields" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     return TOOL_RESULT::Data( symMutate( aServices, aArgs,
                                                          "sym_field_set " + aArgs["symbol"].get<std::string>(),
                                                          [&]( SYMBOL_LIB_SESSION& aSession ) -> JSON
                                                          {
                                                              LIB_SYMBOL& symbol = aSession.Symbol( aArgs["symbol"] );
                                                              const JSON  fields = aArgs["fields"];

                                                              for( const auto& [name, value] : fields.items() )
                                                                  setField( symbol, name, value.get<std::string>() );

                                                              aSession.SetCurrentSymbol( aArgs["symbol"] );
                                                              return symbolJson( symbol );
                                                          } ) );
                 } } );

    tools.Add( { "sym_delete", "Delete a symbol from a project symbol library.", "sym",
                 symSchema( { { "symbol", { { "type", "string" } } } }, { "symbol" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     return TOOL_RESULT::Data( symMutate( aServices, aArgs,
                                                          "sym_delete " + aArgs["symbol"].get<std::string>(),
                                                          [&]( SYMBOL_LIB_SESSION& aSession ) -> JSON
                                                          {
                                                              aSession.Symbol( aArgs["symbol"] );
                                                              aSession.Library().DeleteSymbol( wx( aArgs["symbol"] ) );
                                                              aSession.SetCurrentSymbol( "" );
                                                              return JSON{ { "deleted", aArgs["symbol"] } };
                                                          } ) );
                 } } );

    tools.Add( { "sym_check",
                 "Run KiCad's symbol checker on a symbol (or every symbol): duplicate or off-grid pins, missing pin "
                 "names, bad reference prefix, overlapping pins, hidden power pins and more.",
                 "sym", symSchema( { { "symbol", { { "type", "string" } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&      project = aServices.Sessions().Resolve( aArgs );
                     SYMBOL_LIB_SESSION& session = SymbolLibSession( project, aArgs["library"] );
                     UNITS_PROVIDER      units( schIUScale, EDA_UNITS::MM );
                     JSON                results = JSON::array();
                     size_t              problems = 0;

                     for( const auto& [name, symbol] : session.Library().GetSymbolMap() )
                     {
                         if( aArgs.contains( "symbol" ) && utf8( name ) != aArgs["symbol"].get<std::string>() )
                             continue;

                         std::vector<wxString> messages;
                         CheckLibSymbol( symbol, messages, schIUScale.MilsToIU( 50 ), &units );
                         JSON list = JSON::array();

                         for( wxString message : messages )
                         {
                             // The checker writes HTML for its dialog.
                             wxString plain;
                             bool     inTag = false;

                             for( wxUniChar c : message )
                             {
                                 if( c == '<' )
                                     inTag = true;
                                 else if( c == '>' )
                                     inTag = false;
                                 else if( !inTag )
                                     plain += c;
                             }

                             list.push_back( utf8( plain ) );
                         }

                         problems += list.size();
                         results.push_back(
                                 { { "symbol", utf8( name ) }, { "ok", list.empty() }, { "problems", list } } );
                     }

                     if( results.empty() )
                         throw TOOL_ERROR( "No such symbol in the library." );

                     return TOOL_RESULT::Data( JSON{ { "ok", problems == 0 }, { "symbols", results } } );
                 } } );

    tools.Add( { "sym_capture", "A PNG picture of one unit and body style of a library symbol.", "sym",
                 symSchema( { { "symbol", { { "type", "string" } } },
                              { "unit", { { "type", "integer" }, { "minimum", 1 } } },
                              { "bodyStyle", { { "type", "integer" }, { "minimum", 1 } } },
                              { "width", { { "type", "integer" }, { "minimum", 100 }, { "maximum", 4096 } } } },
                            { "symbol" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&      project = aServices.Sessions().Resolve( aArgs );
                     SYMBOL_LIB_SESSION& session = SymbolLibSession( project, aArgs["library"] );
                     LIB_SYMBOL&         symbol = session.Symbol( aArgs["symbol"] );
                     double              pxPerMm = 0;
                     std::string png = plotSymbolPng( symbol, aArgs.value( "unit", 1 ), aArgs.value( "bodyStyle", 1 ),
                                                      aArgs.value( "width", 1024 ), pxPerMm );

                     wxMemoryInputStream stream( png.data(), png.size() );
                     wxImage             image( stream, wxBITMAP_TYPE_PNG );

                     TOOL_RESULT result;
                     result.content.push_back( ImageContent( base64( png ), "image/png" ) );
                     JSON info{ { "symbol", aArgs["symbol"] },
                                { "unit", aArgs.value( "unit", 1 ) },
                                { "width", image.IsOk() ? image.GetWidth() : 0 },
                                { "height", image.IsOk() ? image.GetHeight() : 0 },
                                { "pixelsPerMm", std::round( pxPerMm * 1000 ) / 1000 } };
                     result.content.push_back( TextContent( info.dump() ) );
                     result.structuredContent = info;
                     session.SetCurrentSymbol( aArgs["symbol"] );
                     return result;
                 } } );

    tools.Add( { "sym_export_svg", "Export SVG files of a symbol (one per unit) inside the project, default output/.",
                 "sym",
                 symSchema( { { "symbol", { { "type", "string" } } }, { "output", { { "type", "string" } } } },
                            { "symbol" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&      project = aServices.Sessions().Resolve( aArgs );
                     SYMBOL_LIB_SESSION& session = SymbolLibSession( project, aArgs["library"] );
                     LIB_SYMBOL&         symbol = session.Symbol( aArgs["symbol"] );
                     const fs::path      dir = project.Files().Resolve( aArgs.value( "output", "output" ) );
                     fs::create_directories( dir );

                     SCH_RENDER_SETTINGS renderSettings;
                     renderSettings.LoadColors( ::GetColorSettings( wxEmptyString ) );
                     renderSettings.SetDefaultPenWidth( DEFAULT_LINE_WIDTH_MILS * schIUScale.IU_PER_MILS );

                     std::shared_ptr<LIB_SYMBOL> root = symbol.IsDerived() ? symbol.GetRootSymbol() : nullptr;
                     LIB_SYMBOL&                 drawSymbol = root ? *root : symbol;
                     JSON                        files = JSON::array();

                     for( int unit = 1; unit <= symbol.GetUnitCount(); ++unit )
                     {
                         const fs::path file =
                                 dir / ( utf8( symbol.GetName() ) + "_unit" + std::to_string( unit ) + ".svg" );
                         BOX2I bbox = GetSymbolPlotBBox( symbol, unit, 1, false );

                         if( !PlotSymbolToSVG( drawSymbol, symbol, unit, 1, bbox, renderSettings, false,
                                               wx( file.string() ), &aServices.Reporter() ) )
                             throw TOOL_ERROR( "Writing " + file.filename().string() + " failed." );

                         files.push_back( project.Files().Relative( file ) );
                     }

                     return TOOL_RESULT::Data( JSON{ { "files", files } } );
                 } } );
}

} // namespace AGENTIC
