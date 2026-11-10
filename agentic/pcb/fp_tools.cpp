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
#include <agentic/core/library_tools.h>
#include <agentic/core/services.h>
#include <agentic/pcb/footprint_lib_session.h>
#include <agentic/pcb/fp_tools.h>
#include <agentic/pcb/pcb_items.h>
#include <agentic/pcb/pcb_tools.h>

#include <base_units.h>
#include <board.h>
#include <drc/drc_item.h>
#include <footprint.h>
#include <footprint_library_adapter.h>
#include <footprint_utils.h>
#include <libraries/library_manager.h>
#include <pad.h>
#include <pcb_field.h>
#include <pcb_plot_params.h>
#include <pcb_plotter.h>
#include <pcb_shape.h>
#include <reporter.h>
#include <units_provider.h>

#include <cmath>
#include <map>


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


    double mm( long long aIu )
    {
        return std::round( pcbIUScale.IUTomm( static_cast<double>( aIu ) ) * 1e4 ) / 1e4;
    }


    const JSON POINT = { { "type", { "object", "array" } }, { "description", "{x, y} in mm (or [x, y])." } };
    const JSON LENGTH = { { "type", { "number", "string" } }, { "description", "mm, or a string with units." } };


    JSON fpSchema( JSON aProperties, JSON aRequired = JSON::array(), bool aFootprint = true )
    {
        aProperties["library"] = {
            { "type", "string" }, { "description", "Project footprint library: nickname (lib_tables) or .pretty path." }
        };
        aProperties["force"] = { { "type", "boolean" } };
        aRequired.push_back( "library" );

        if( aFootprint )
        {
            aProperties["footprint"] = { { "type", "string" }, { "description", "Footprint name in the library." } };
            aRequired.push_back( "footprint" );
        }

        return ProjectToolSchema( aProperties, aRequired );
    }


    VECTOR2I point( const JSON& aValue, const std::string& aField )
    {
        if( aValue.is_array() && aValue.size() == 2 )
            return VECTOR2I( PcbLength( aValue[0], aField + ".x" ), PcbLength( aValue[1], aField + ".y" ) );

        if( !aValue.is_object() || !aValue.contains( "x" ) || !aValue.contains( "y" ) )
            throw TOOL_ERROR( "'" + aField + "' must be a point {x, y} in mm." );

        return VECTOR2I( PcbLength( aValue["x"], aField + ".x" ), PcbLength( aValue["y"], aField + ".y" ) );
    }


    /// A size: one number (square or round), [w, h] or {width, height}.
    VECTOR2I size( const JSON& aValue, const std::string& aField )
    {
        if( aValue.is_array() && aValue.size() == 2 )
            return VECTOR2I( PcbLength( aValue[0], aField ), PcbLength( aValue[1], aField ) );

        if( aValue.is_object() )
            return VECTOR2I( PcbLength( aValue.at( "width" ), aField ), PcbLength( aValue.at( "height" ), aField ) );

        const int side = PcbLength( aValue, aField );
        return VECTOR2I( side, side );
    }


    const std::map<std::string, PAD_SHAPE> PAD_SHAPES = { { "circle", PAD_SHAPE::CIRCLE },
                                                          { "rect", PAD_SHAPE::RECTANGLE },
                                                          { "oval", PAD_SHAPE::OVAL },
                                                          { "trapezoid", PAD_SHAPE::TRAPEZOID },
                                                          { "roundrect", PAD_SHAPE::ROUNDRECT } };

    const std::map<std::string, PAD_ATTRIB> PAD_TYPES = { { "smd", PAD_ATTRIB::SMD },
                                                          { "thru_hole", PAD_ATTRIB::PTH },
                                                          { "np_thru_hole", PAD_ATTRIB::NPTH },
                                                          { "connect", PAD_ATTRIB::CONN } };


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


    template <typename T>
    std::string nameOf( const std::map<std::string, T>& aMap, T aValue )
    {
        for( const auto& [name, value] : aMap )
        {
            if( value == aValue )
                return name;
        }

        return "custom";
    }


    std::string layerName( const BOARD& aBoard, PCB_LAYER_ID aLayer )
    {
        return utf8( aBoard.GetLayerName( aLayer ) );
    }


    JSON padJson( const BOARD& aBoard, const PAD& aPad )
    {
        const VECTOR2I sz = aPad.GetSize( PADSTACK::ALL_LAYERS );
        JSON           layers = JSON::array();

        for( PCB_LAYER_ID layer : aPad.GetLayerSet().Seq() )
        {
            // A through-hole pad is on every copper layer; list the ones the board has.
            if( IsCopperLayer( layer ) && !aBoard.IsLayerEnabled( layer ) )
                continue;

            layers.push_back( layerName( aBoard, layer ) );
        }

        JSON out{ { "number", utf8( aPad.GetNumber() ) },
                  { "type", nameOf( PAD_TYPES, aPad.GetAttribute() ) },
                  { "shape", nameOf( PAD_SHAPES, aPad.GetShape( PADSTACK::ALL_LAYERS ) ) },
                  { "position", PcbPointJson( aPad.GetFPRelativePosition() ) },
                  { "size", { mm( sz.x ), mm( sz.y ) } },
                  { "rotation", aPad.GetFPRelativeOrientation().AsDegrees() },
                  { "layers", layers } };

        if( aPad.HasHole() )
            out["drill"] = { mm( aPad.GetDrillSize().x ), mm( aPad.GetDrillSize().y ) };

        return out;
    }


    std::string typeOf( const FOOTPRINT& aFootprint )
    {
        if( aFootprint.GetAttributes() & FP_SMD )
            return "smd";

        if( aFootprint.GetAttributes() & FP_THROUGH_HOLE )
            return "through_hole";

        return "other";
    }


    JSON footprintJson( const FOOTPRINT& aFootprint, bool aDetail )
    {
        const BOARD& board = *aFootprint.GetBoard();
        JSON         out{ { "name", utf8( aFootprint.GetFPID().GetLibItemName().wx_str() ) },
                          { "description", utf8( aFootprint.GetLibDescription() ) },
                          { "keywords", utf8( aFootprint.GetKeywords() ) },
                          { "type", typeOf( aFootprint ) },
                          { "pads", aFootprint.Pads().size() } };

        if( !aDetail )
            return out;

        out["reference"] = utf8( aFootprint.GetReference() );
        out["value"] = utf8( aFootprint.GetValue() );
        out["excludeFromBom"] = aFootprint.IsExcludedFromBOM();
        out["excludeFromPos"] = aFootprint.IsExcludedFromPosFiles();
        out["boardOnly"] = aFootprint.IsBoardOnly();

        JSON pads = JSON::array();

        for( const PAD* pad : aFootprint.Pads() )
            pads.push_back( padJson( board, *pad ) );

        out["pads"] = pads;

        std::map<std::string, int> graphics;

        for( const BOARD_ITEM* item : aFootprint.GraphicalItems() )
            graphics[layerName( board, item->GetLayer() )]++;

        out["graphics"] = graphics;

        JSON fields = JSON::object();

        for( const PCB_FIELD* field : aFootprint.GetFields() )
        {
            if( !field->IsMandatory() )
                fields[utf8( field->GetName() )] = utf8( field->GetText() );
        }

        out["fields"] = fields;

        JSON models = JSON::array();

        for( const FP_3DMODEL& model : aFootprint.Models() )
        {
            models.push_back( { { "path", utf8( model.m_Filename ) },
                                { "offset", { model.m_Offset.x, model.m_Offset.y, model.m_Offset.z } },
                                { "rotation", { model.m_Rotation.x, model.m_Rotation.y, model.m_Rotation.z } },
                                { "scale", { model.m_Scale.x, model.m_Scale.y, model.m_Scale.z } } } );
        }

        out["models"] = models;
        return out;
    }


    /// One change to a footprint library: \a aEdit changes it and touches what it changed.
    JSON fpMutate( AGENT_SERVICES& aServices, const JSON& aArgs, const std::string& aLabel,
                   const std::function<JSON( FOOTPRINT_LIB_SESSION& )>& aEdit )
    {
        AGENT_PROJECT&         project = aServices.Sessions().Resolve( aArgs );
        const std::string      library = aArgs["library"];
        FOOTPRINT_LIB_SESSION& session = FootprintLibSession( project, library );

        JSON result = project.Mutate( aLabel, aArgs.value( "force", false ),
                                      [&]() -> JSON
                                      {
                                          JSON out = aEdit( session );
                                          session.RecordChange(
                                                  { { "summary", aLabel }, { "footprint", session.Current() } } );
                                          return out;
                                      } );

        // Placements and searches read the library through the project's adapter.
        if( fs::path( library ).extension() != ".pretty" )
            project.Libraries().ReloadLibraryEntry( LIBRARY_TABLE_TYPE::FOOTPRINT, wx( library ) );

        return result;
    }


    /// Change footprint aArgs.footprint: \a aEdit changes it.
    JSON fpEdit( AGENT_SERVICES& aServices, const JSON& aArgs, const std::string& aLabel,
                 const std::function<JSON( FOOTPRINT_LIB_SESSION&, FOOTPRINT& )>& aEdit )
    {
        return fpMutate( aServices, aArgs, aLabel + " " + aArgs["footprint"].get<std::string>(),
                         [&]( FOOTPRINT_LIB_SESSION& aSession ) -> JSON
                         {
                             const std::string name = aArgs["footprint"];
                             FOOTPRINT&        footprint = aSession.Footprint( name );
                             JSON              out = aEdit( aSession, footprint );
                             aSession.Touch( name );
                             aSession.SetCurrent( name );
                             return out;
                         } );
    }


    void applyPadShape( PAD& aPad, const JSON& aArgs )
    {
        if( aArgs.contains( "type" ) )
        {
            const PAD_ATTRIB type = lookup( PAD_TYPES, aArgs["type"], "pad type" );
            aPad.SetAttribute( type );

            switch( type )
            {
            case PAD_ATTRIB::SMD: aPad.SetLayerSet( PAD::SMDMask() ); break;
            case PAD_ATTRIB::CONN: aPad.SetLayerSet( PAD::ConnSMDMask() ); break;
            case PAD_ATTRIB::NPTH: aPad.SetLayerSet( PAD::UnplatedHoleMask() ); break;
            default: aPad.SetLayerSet( PAD::PTHMask() ); break;
            }
        }

        if( aArgs.contains( "side" ) && aArgs["side"] == "back" && aPad.IsOnLayer( F_Cu ) && !aPad.IsOnLayer( B_Cu ) )
        {
            LSET layers = aPad.GetLayerSet();
            aPad.SetLayerSet( layers.FlipStandardLayers() );
        }

        if( aArgs.contains( "shape" ) )
            aPad.SetShape( PADSTACK::ALL_LAYERS, lookup( PAD_SHAPES, aArgs["shape"], "pad shape" ) );

        if( aArgs.contains( "size" ) )
            aPad.SetSize( PADSTACK::ALL_LAYERS, size( aArgs["size"], "size" ) );

        if( aArgs.contains( "roundrectRatio" ) )
            aPad.SetRoundRectRadiusRatio( PADSTACK::ALL_LAYERS, aArgs["roundrectRatio"].get<double>() );
        else if( aArgs.contains( "shape" ) && aArgs["shape"] == "roundrect" )
            aPad.SetRoundRectRadiusRatio( PADSTACK::ALL_LAYERS, 0.25 );

        if( aArgs.contains( "drill" ) )
        {
            const VECTOR2I drill = size( aArgs["drill"], "drill" );
            aPad.SetDrillShape( drill.x == drill.y ? PAD_DRILL_SHAPE::CIRCLE : PAD_DRILL_SHAPE::OBLONG );
            aPad.SetDrillSize( drill );
        }

        if( aArgs.contains( "rotation" ) )
            aPad.SetFPRelativeOrientation( EDA_ANGLE( aArgs["rotation"].get<double>(), DEGREES_T ) );

        if( aPad.HasHole() && aPad.GetDrillSize().x <= 0 )
            throw TOOL_ERROR( "A through-hole pad needs a drill." );

        const VECTOR2I sz = aPad.GetSize( PADSTACK::ALL_LAYERS );

        if( aPad.GetAttribute() != PAD_ATTRIB::NPTH && ( sz.x <= 0 || sz.y <= 0 ) )
            throw TOOL_ERROR( "A pad needs a size." );
    }


    /// The positions and numbers of a pad array: linear (count, pitch, direction) or grid.
    std::vector<std::pair<std::string, VECTOR2I>> arrayPads( const JSON& aArray )
    {
        std::vector<std::pair<std::string, VECTOR2I>> out;
        const VECTOR2I                                start = point( aArray.at( "start" ), "array.start" );
        const int                                     first = aArray.value( "firstNumber", 1 );
        const int                                     increment = aArray.value( "increment", 1 );
        const std::string                             prefix = aArray.value( "prefix", "" );

        if( aArray.contains( "rows" ) || aArray.contains( "columns" ) )
        {
            const int rows = aArray.value( "rows", 1 );
            const int columns = aArray.value( "columns", 1 );
            const int rowPitch = PcbLength( aArray.at( "rowPitch" ), "rowPitch" );
            const int columnPitch = PcbLength( aArray.at( "columnPitch" ), "columnPitch" );

            if( rows < 1 || columns < 1 || rows * columns > 10000 )
                throw TOOL_ERROR( "A grid has 1 to 10000 pads." );

            int n = first;

            for( int r = 0; r < rows; ++r )
            {
                for( int c = 0; c < columns; ++c, n += increment )
                    out.emplace_back( prefix + std::to_string( n ), start + VECTOR2I( c * columnPitch, r * rowPitch ) );
            }

            return out;
        }

        const int         count = aArray.at( "count" );
        const int         pitch = PcbLength( aArray.at( "pitch" ), "pitch" );
        const std::string direction = aArray.value( "direction", "down" );
        VECTOR2I          step;

        if( direction == "down" )
            step = VECTOR2I( 0, pitch );
        else if( direction == "up" )
            step = VECTOR2I( 0, -pitch );
        else if( direction == "right" )
            step = VECTOR2I( pitch, 0 );
        else if( direction == "left" )
            step = VECTOR2I( -pitch, 0 );
        else
            throw TOOL_ERROR( "direction is down, up, right or left." );

        if( count < 1 || count > 10000 )
            throw TOOL_ERROR( "count is 1 to 10000." );

        for( int i = 0; i < count; ++i )
            out.emplace_back( prefix + std::to_string( first + i * increment ), start + step * i );

        return out;
    }


    /// The footprint's body: its pads and its fabrication drawing.
    BOX2I bodyBox( const FOOTPRINT& aFootprint, bool aBack )
    {
        BOX2I box;
        bool  any = false;

        auto add = [&]( const BOX2I& aBox )
        {
            if( any )
                box.Merge( aBox );
            else
                box = aBox;

            any = true;
        };

        for( const PAD* pad : aFootprint.Pads() )
            add( pad->GetBoundingBox() );

        for( const BOARD_ITEM* item : aFootprint.GraphicalItems() )
        {
            if( item->GetLayer() == ( aBack ? B_Fab : F_Fab ) && item->Type() == PCB_SHAPE_T )
            {
                // The drawn outline, not the stroke around it.
                BOX2I outline = item->GetBoundingBox();
                outline.Inflate( -static_cast<const PCB_SHAPE*>( item )->GetWidth() / 2 );
                add( outline );
            }
        }

        if( !any )
            throw TOOL_ERROR( "The footprint has no pads and no fabrication drawing to put a courtyard around." );

        return box;
    }


    int roundTo( int aValue, int aGrid, bool aUp )
    {
        const double steps = static_cast<double>( aValue ) / aGrid;
        return static_cast<int>( ( aUp ? std::ceil( steps - 1e-9 ) : std::floor( steps + 1e-9 ) ) * aGrid );
    }


    /// A copy of the footprint on a board of its own, at \a aOffset, for plotting.
    std::unique_ptr<BOARD> plotBoard( AGENT_PROJECT& aProject, const FOOTPRINT& aFootprint, const VECTOR2I& aOffset )
    {
        auto board = std::make_unique<BOARD>();
        board->SetProject( &aProject.KicadProject(), true );

        FOOTPRINT* copy = static_cast<FOOTPRINT*>( aFootprint.Duplicate( IGNORE_PARENT_GROUP ) );
        copy->SetParent( board.get() );
        copy->SetPosition( aOffset );
        board->Add( copy );
        return board;
    }

} // namespace


void RegisterFootprintEditorTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY& tools = aServices.Tools();

    tools.Add( { "fp_list", "The footprints of a project footprint library: name, description, type, pad count.", "fp",
                 fpSchema( WithPageArgs( JSON::object() ), {}, false ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&         project = aServices.Sessions().Resolve( aArgs );
                     FOOTPRINT_LIB_SESSION& session = FootprintLibSession( project, aArgs["library"] );
                     JSON                   out = JSON::array();

                     for( const std::string& name : session.Names() )
                         out.push_back( footprintJson( session.Footprint( name ), false ) );

                     return TOOL_RESULT::Data( { { "library", session.Path() },
                                                 { "footprints", Page( out, aArgs ) },
                                                 { "total", out.size() } } );
                 } } );

    tools.Add( { "fp_get",
                 "One footprint in full: fields, type, pads (number, type, shape, position, size, drill, rotation, "
                 "layers), graphics per layer, 3D models. Positions are relative to the footprint origin, in mm.",
                 "fp", fpSchema( {} ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&         project = aServices.Sessions().Resolve( aArgs );
                     FOOTPRINT_LIB_SESSION& session = FootprintLibSession( project, aArgs["library"] );
                     return TOOL_RESULT::Data( footprintJson( session.Footprint( aArgs["footprint"] ), true ) );
                 } } );

    tools.Add(
            { "fp_create",
              "Create a footprint in a project library: empty (REF** on F.SilkS, the name as value on F.Fab), or "
              "a copy of another footprint (copyFrom: a name in this library or Library:Footprint from any "
              "library).",
              "fp",
              fpSchema( { { "name", { { "type", "string" } } },
                          { "description", { { "type", "string" } } },
                          { "keywords", { { "type", "string" } } },
                          { "type", { { "type", "string" }, { "enum", { "smd", "through_hole", "other" } } } },
                          { "copyFrom", { { "type", "string" } } } },
                        { "name" }, false ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
              {
                  const std::string name = aArgs["name"];

                  if( name.empty() || name.find_first_of( "/\\:" ) != std::string::npos )
                      throw TOOL_ERROR( "A footprint name has no / \\ or :." );

                  std::unique_ptr<FOOTPRINT> source;

                  if( aArgs.contains( "copyFrom" )
                      && aArgs["copyFrom"].get<std::string>().find( ':' ) != std::string::npos )
                  {
                      AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                      LIB_ID         id;

                      if( id.Parse( wx( aArgs["copyFrom"] ) ) >= 0 || !id.IsValid() )
                          throw TOOL_ERROR( "copyFrom is a name in this library or Library:Footprint." );

                      JSON  errors = JSON::array();
                      auto& adapter = static_cast<FOOTPRINT_LIBRARY_ADAPTER&>(
                              LoadLibraries( project, LIBRARY_TABLE_TYPE::FOOTPRINT, aContext, errors ) );
                      source.reset( adapter.LoadFootprint( id, false ) );

                      if( !source )
                          throw TOOL_ERROR( "No footprint '" + aArgs["copyFrom"].get<std::string>() + "'." );
                  }

                  JSON result = fpMutate(
                          aServices, aArgs, "fp_create " + name,
                          [&]( FOOTPRINT_LIB_SESSION& aSession ) -> JSON
                          {
                              if( aSession.Has( name ) )
                                  throw TOOL_ERROR( "'" + name + "' already exists in " + aSession.Path() + "." );

                              std::unique_ptr<FOOTPRINT> footprint;

                              if( source )
                              {
                                  footprint = std::move( source );
                              }
                              else if( aArgs.contains( "copyFrom" ) )
                              {
                                  footprint.reset( static_cast<FOOTPRINT*>(
                                          aSession.Footprint( aArgs["copyFrom"] ).Duplicate( IGNORE_PARENT_GROUP ) ) );
                              }
                              else
                              {
                                  footprint = std::make_unique<FOOTPRINT>( &aSession.Holder() );
                                  footprint->SetReference( wxS( "REF**" ) );
                                  footprint->Reference().SetLayer( F_SilkS );
                                  footprint->Reference().SetFPRelativePosition( VECTOR2I( 0, PcbIu( -2 ) ) );
                                  footprint->SetValue( wx( name ) );
                                  footprint->Value().SetLayer( F_Fab );
                                  footprint->Value().SetFPRelativePosition( VECTOR2I( 0, PcbIu( 2 ) ) );
                                  footprint->SetAttributes( FP_SMD );
                              }

                              footprint->SetFPID( LIB_ID( wx( aSession.LibraryName() ), wx( name ) ) );

                              if( aArgs.contains( "copyFrom" ) && !footprint->GetValue().IsEmpty() )
                                  footprint->SetValue( wx( name ) );

                              if( aArgs.contains( "description" ) )
                                  footprint->SetLibDescription( wx( aArgs["description"] ) );

                              if( aArgs.contains( "keywords" ) )
                                  footprint->SetKeywords( wx( aArgs["keywords"] ) );

                              if( aArgs.contains( "type" ) )
                              {
                                  int attributes = footprint->GetAttributes() & ~( FP_SMD | FP_THROUGH_HOLE );

                                  if( aArgs["type"] == "smd" )
                                      attributes |= FP_SMD;
                                  else if( aArgs["type"] == "through_hole" )
                                      attributes |= FP_THROUGH_HOLE;

                                  footprint->SetAttributes( attributes );
                              }

                              aSession.Add( std::move( footprint ) );
                              aSession.SetCurrent( name );
                              return JSON{ { "created", footprintJson( aSession.Footprint( name ), true ) } };
                          } );

                  return TOOL_RESULT::Data( result );
              } } );

    const JSON padProperties = {
        { "type", { { "type", "string" }, { "enum", { "smd", "thru_hole", "np_thru_hole", "connect" } } } },
        { "shape", { { "type", "string" }, { "enum", { "circle", "rect", "oval", "trapezoid", "roundrect" } } } },
        { "size", { { "description", "mm: one number, [w, h] or {width, height}." } } },
        { "drill", { { "description", "Hole diameter in mm, or [w, h] for an oblong hole." } } },
        { "roundrectRatio", { { "type", "number" }, { "minimum", 0 }, { "maximum", 0.5 } } },
        { "rotation", { { "type", "number" }, { "description", "Degrees." } } },
        { "side", { { "type", "string" }, { "enum", { "front", "back" } } } }
    };

    JSON padsAddProps = padProperties;
    padsAddProps["pads"] = { { "type", "array" },
                             { "description", "A table: [{number, x, y}] in mm from the footprint origin." } };
    padsAddProps["array"] = {
        { "type", "object" },
        { "description",
          "Linear: {start, count, pitch, direction (down|up|right|left)}; grid: {start, rows, columns, rowPitch, "
          "columnPitch}, numbered row by row. Both: firstNumber (1), increment (1), prefix (\"\")." }
    };

    tools.Add( { "fp_pads_add",
                 "Add pads with one shape: a table of numbered positions, or a linear or grid array. Type smd "
                 "(F.Cu, F.Paste, F.Mask), thru_hole (all copper and both masks, needs drill), np_thru_hole, connect. "
                 "For a dual-row part call it once per row.",
                 "fp", fpSchema( padsAddProps, { "type", "shape", "size" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     std::vector<std::pair<std::string, VECTOR2I>> placements;

                     if( aArgs.contains( "pads" ) )
                     {
                         const JSON& table = aArgs["pads"];

                         for( const JSON& row : table )
                         {
                             placements.emplace_back( row.at( "number" ).is_string()
                                                              ? row["number"].get<std::string>()
                                                              : std::to_string( row["number"].get<long>() ),
                                                      point( row, "pads[].position" ) );
                         }
                     }
                     else if( aArgs.contains( "array" ) )
                     {
                         placements = arrayPads( aArgs["array"] );
                     }
                     else
                     {
                         throw TOOL_ERROR( "Give pads (a table) or array." );
                     }

                     // A new pad has a default drill; a hole's size must be given.
                     if( ( aArgs["type"] == "thru_hole" || aArgs["type"] == "np_thru_hole" )
                         && !aArgs.contains( "drill" ) )
                         throw TOOL_ERROR( "A through-hole pad needs a drill (mm)." );

                     JSON result = fpEdit( aServices, aArgs, "fp_pads_add",
                                           [&]( FOOTPRINT_LIB_SESSION&, FOOTPRINT& aFootprint ) -> JSON
                                           {
                                               JSON added = JSON::array();

                                               for( const auto& [number, position] : placements )
                                               {
                                                   auto pad = std::make_unique<PAD>( &aFootprint );
                                                   pad->SetNumber( wx( number ) );
                                                   applyPadShape( *pad, aArgs );
                                                   pad->SetFPRelativePosition( position );
                                                   added.push_back( padJson( *aFootprint.GetBoard(), *pad ) );
                                                   aFootprint.Add( pad.release() );
                                               }

                                               return JSON{ { "added", added } };
                                           } );

                     return TOOL_RESULT::Data( result );
                 } } );

    JSON padSetProps = padProperties;
    padSetProps["pads"] = { { "type", "array" },
                            { "items", { { "type", "string" } } },
                            { "description", "Pad numbers to change; [\"*\"] for all." } };
    padSetProps["position"] = POINT;
    padSetProps["number"] = { { "type", "string" }, { "description", "Renumber (one pad only)." } };

    tools.Add( { "fp_pad_set", "Change pads: type, shape, size, drill, rotation, position or number.", "fp",
                 fpSchema( padSetProps, { "pads" } ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     JSON result = fpEdit(
                             aServices, aArgs, "fp_pad_set",
                             [&]( FOOTPRINT_LIB_SESSION&, FOOTPRINT& aFootprint ) -> JSON
                             {
                                 const JSON        numbers = aArgs["pads"];
                                 std::vector<PAD*> pads;

                                 for( PAD* pad : aFootprint.Pads() )
                                 {
                                     for( const JSON& number : numbers )
                                     {
                                         if( number == "*" || utf8( pad->GetNumber() ) == number.get<std::string>() )
                                         {
                                             pads.push_back( pad );
                                             break;
                                         }
                                     }
                                 }

                                 if( pads.empty() )
                                     throw TOOL_ERROR( "No pad with those numbers; fp_get lists them." );

                                 if( ( aArgs.contains( "number" ) || aArgs.contains( "position" ) )
                                     && pads.size() != 1 )
                                     throw TOOL_ERROR( "number and position change one pad at a time." );

                                 JSON changed = JSON::array();

                                 for( PAD* pad : pads )
                                 {
                                     applyPadShape( *pad, aArgs );

                                     if( aArgs.contains( "position" ) )
                                         pad->SetFPRelativePosition( point( aArgs["position"], "position" ) );

                                     if( aArgs.contains( "number" ) )
                                         pad->SetNumber( wx( aArgs["number"] ) );

                                     changed.push_back( padJson( *aFootprint.GetBoard(), *pad ) );
                                 }

                                 return JSON{ { "changed", changed } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add(
            { "fp_graphic_add",
              "Draw on a footprint layer (F.SilkS, F.Fab, F.CrtYd, ...): line (start, end), rectangle (start, "
              "end), circle (center, radius), arc (start, mid, end) or polygon (points). Width in mm.",
              "fp",
              fpSchema( { { "kind",
                            { { "type", "string" }, { "enum", { "line", "rectangle", "circle", "arc", "polygon" } } } },
                          { "layer", { { "type", "string" } } },
                          { "start", POINT },
                          { "end", POINT },
                          { "mid", POINT },
                          { "center", POINT },
                          { "radius", LENGTH },
                          { "points", { { "type", "array" } } },
                          { "width", LENGTH },
                          { "filled", { { "type", "boolean" } } } },
                        { "kind", "layer" } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  JSON result = fpEdit(
                          aServices, aArgs, "fp_graphic_add",
                          [&]( FOOTPRINT_LIB_SESSION&, FOOTPRINT& aFootprint ) -> JSON
                          {
                              BOARD&             board = *aFootprint.GetBoard();
                              const std::string  kind = aArgs["kind"];
                              const PCB_LAYER_ID layer = PcbLayer( board, aArgs["layer"] );
                              const SHAPE_T      type = kind == "line"        ? SHAPE_T::SEGMENT
                                                        : kind == "rectangle" ? SHAPE_T::RECTANGLE
                                                        : kind == "circle"    ? SHAPE_T::CIRCLE
                                                        : kind == "arc"       ? SHAPE_T::ARC
                                                                              : SHAPE_T::POLY;

                              auto shape = std::make_unique<PCB_SHAPE>( &aFootprint, type );
                              shape->SetLayer( layer );

                              const bool courtyard = layer == F_CrtYd || layer == B_CrtYd;
                              const bool fab = layer == F_Fab || layer == B_Fab;
                              shape->SetWidth( aArgs.contains( "width" ) ? PcbLength( aArgs["width"], "width" )
                                                                         : PcbIu( courtyard ? 0.05
                                                                                  : fab     ? 0.1
                                                                                            : 0.12 ) );

                              if( type == SHAPE_T::CIRCLE )
                              {
                                  const VECTOR2I center = point( aArgs.at( "center" ), "center" );
                                  shape->SetCenter( center );
                                  shape->SetEnd( center + VECTOR2I( PcbLength( aArgs.at( "radius" ), "radius" ), 0 ) );
                              }
                              else if( type == SHAPE_T::ARC )
                              {
                                  shape->SetArcGeometry( point( aArgs.at( "start" ), "start" ),
                                                         point( aArgs.at( "mid" ), "mid" ),
                                                         point( aArgs.at( "end" ), "end" ) );
                              }
                              else if( type == SHAPE_T::POLY )
                              {
                                  std::vector<VECTOR2I> points;

                                  for( const JSON& p : aArgs.at( "points" ) )
                                      points.push_back( point( p, "points" ) );

                                  if( points.size() < 3 )
                                      throw TOOL_ERROR( "A polygon has at least 3 points." );

                                  shape->SetPolyPoints( points );
                              }
                              else
                              {
                                  shape->SetStart( point( aArgs.at( "start" ), "start" ) );
                                  shape->SetEnd( point( aArgs.at( "end" ), "end" ) );
                              }

                              if( aArgs.value( "filled", false ) )
                                  shape->SetFilled( true );

                              // Shapes are kept in footprint coordinates; the footprint sits at the origin.
                              const std::string kiid = utf8( shape->m_Uuid.AsString() );
                              aFootprint.Add( shape.release() );
                              return JSON{ { "added", kiid } };
                          } );

                  return TOOL_RESULT::Data( result );
              } } );

    tools.Add( { "fp_courtyard_set",
                 "Replace the courtyard: a rectangle around the body (pads and F.Fab drawing) plus clearance (0.25 mm, "
                 "IPC-7351 nominal), rounded out to 0.01 mm; or an explicit rectangle {x1, y1, x2, y2}.",
                 "fp",
                 fpSchema( { { "clearance", LENGTH },
                             { "rectangle", { { "type", "object" } } },
                             { "side", { { "type", "string" }, { "enum", { "front", "back" } } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     JSON result = fpEdit(
                             aServices, aArgs, "fp_courtyard_set",
                             [&]( FOOTPRINT_LIB_SESSION&, FOOTPRINT& aFootprint ) -> JSON
                             {
                                 const bool         back = aArgs.value( "side", "front" ) == "back";
                                 const PCB_LAYER_ID layer = back ? B_CrtYd : F_CrtYd;
                                 BOX2I              box;

                                 if( aArgs.contains( "rectangle" ) )
                                 {
                                     const JSON& r = aArgs["rectangle"];
                                     VECTOR2I    p1( PcbLength( r.at( "x1" ), "x1" ), PcbLength( r.at( "y1" ), "y1" ) );
                                     VECTOR2I    p2( PcbLength( r.at( "x2" ), "x2" ), PcbLength( r.at( "y2" ), "y2" ) );
                                     box = BOX2I( p1, p2 - p1 );
                                     box.Normalize();
                                 }
                                 else
                                 {
                                     const int clearance = aArgs.contains( "clearance" )
                                                                   ? PcbLength( aArgs["clearance"], "clearance" )
                                                                   : PcbIu( 0.25 );
                                     box = bodyBox( aFootprint, back );
                                     box.Inflate( clearance );

                                     const int grid = PcbIu( 0.01 );
                                     VECTOR2I  p1( roundTo( box.GetLeft(), grid, false ),
                                                   roundTo( box.GetTop(), grid, false ) );
                                     VECTOR2I  p2( roundTo( box.GetRight(), grid, true ),
                                                   roundTo( box.GetBottom(), grid, true ) );
                                     box = BOX2I( p1, p2 - p1 );
                                 }

                                 std::vector<BOARD_ITEM*> old;

                                 for( BOARD_ITEM* item : aFootprint.GraphicalItems() )
                                 {
                                     if( item->GetLayer() == layer )
                                         old.push_back( item );
                                 }

                                 for( BOARD_ITEM* item : old )
                                 {
                                     aFootprint.Remove( item );
                                     delete item;
                                 }

                                 auto shape = std::make_unique<PCB_SHAPE>( &aFootprint, SHAPE_T::RECTANGLE );
                                 shape->SetLayer( layer );
                                 shape->SetWidth( PcbIu( 0.05 ) );
                                 shape->SetStart( box.GetOrigin() );
                                 shape->SetEnd( box.GetEnd() );
                                 aFootprint.Add( shape.release() );

                                 return JSON{ { "courtyard",
                                                { { "x1", mm( box.GetLeft() ) },
                                                  { "y1", mm( box.GetTop() ) },
                                                  { "x2", mm( box.GetRight() ) },
                                                  { "y2", mm( box.GetBottom() ) } } },
                                              { "replaced", old.size() } };
                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "fp_field_set",
                 "Set a footprint's description, keywords, type (smd, through_hole, other), reference and value text, "
                 "attributes (excludeFromBom, excludeFromPos, boardOnly) and user fields {name: text} (empty text "
                 "removes a field).",
                 "fp",
                 fpSchema( { { "description", { { "type", "string" } } },
                             { "keywords", { { "type", "string" } } },
                             { "type", { { "type", "string" }, { "enum", { "smd", "through_hole", "other" } } } },
                             { "reference", { { "type", "string" } } },
                             { "value", { { "type", "string" } } },
                             { "excludeFromBom", { { "type", "boolean" } } },
                             { "excludeFromPos", { { "type", "boolean" } } },
                             { "boardOnly", { { "type", "boolean" } } },
                             { "fields", { { "type", "object" } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     JSON result =
                             fpEdit( aServices, aArgs, "fp_field_set",
                                     [&]( FOOTPRINT_LIB_SESSION&, FOOTPRINT& aFootprint ) -> JSON
                                     {
                                         if( aArgs.contains( "description" ) )
                                             aFootprint.SetLibDescription( wx( aArgs["description"] ) );

                                         if( aArgs.contains( "keywords" ) )
                                             aFootprint.SetKeywords( wx( aArgs["keywords"] ) );

                                         if( aArgs.contains( "reference" ) )
                                             aFootprint.SetReference( wx( aArgs["reference"] ) );

                                         if( aArgs.contains( "value" ) )
                                             aFootprint.SetValue( wx( aArgs["value"] ) );

                                         int attributes = aFootprint.GetAttributes();

                                         auto flag = [&]( const char* aName, int aBit )
                                         {
                                             if( aArgs.contains( aName ) )
                                                 attributes = aArgs[aName].get<bool>() ? ( attributes | aBit )
                                                                                       : ( attributes & ~aBit );
                                         };

                                         flag( "excludeFromBom", FP_EXCLUDE_FROM_BOM );
                                         flag( "excludeFromPos", FP_EXCLUDE_FROM_POS_FILES );
                                         flag( "boardOnly", FP_BOARD_ONLY );

                                         if( aArgs.contains( "type" ) )
                                         {
                                             attributes &= ~( FP_SMD | FP_THROUGH_HOLE );

                                             if( aArgs["type"] == "smd" )
                                                 attributes |= FP_SMD;
                                             else if( aArgs["type"] == "through_hole" )
                                                 attributes |= FP_THROUGH_HOLE;
                                         }

                                         aFootprint.SetAttributes( attributes );

                                         const JSON fields = aArgs.value( "fields", JSON::object() );

                                         for( const auto& [name, text] : fields.items() )
                                         {
                                             if( name == "Reference" || name == "Value" || name == "Footprint"
                                                 || name == "Datasheet" || name == "Description" )
                                             {
                                                 throw TOOL_ERROR( "'" + name
                                                                   + "' is a built-in field; use its own argument." );
                                             }

                                             PCB_FIELD* field = aFootprint.GetField( wx( name ) );

                                             if( text.get<std::string>().empty() )
                                             {
                                                 if( field )
                                                 {
                                                     aFootprint.Remove( field );
                                                     delete field;
                                                 }

                                                 continue;
                                             }

                                             if( !field )
                                             {
                                                 field = new PCB_FIELD( &aFootprint, FIELD_T::USER, wx( name ) );
                                                 field->SetLayer( F_Fab );
                                                 field->SetVisible( false );
                                                 aFootprint.Add( field );
                                             }

                                             field->SetText( wx( text ) );
                                         }

                                         return JSON{ { "footprint", footprintJson( aFootprint, true ) } };
                                     } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add(
            { "fp_model_set",
              "Set the footprint's 3D models (replaces the list; [] removes them): each {path, offset [x,y,z] mm, "
              "rotation [x,y,z] degrees, scale [x,y,z]}. Paths may use ${KICAD9_3DMODEL_DIR} or be project-relative.",
              "fp", fpSchema( { { "models", { { "type", "array" } } } }, { "models" } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  JSON result =
                          fpEdit( aServices, aArgs, "fp_model_set",
                                  [&]( FOOTPRINT_LIB_SESSION&, FOOTPRINT& aFootprint ) -> JSON
                                  {
                                      std::vector<FP_3DMODEL> models;

                                      auto triple = []( const JSON& aModel, const char* aKey, VECTOR3D aDefault )
                                      {
                                          if( !aModel.contains( aKey ) )
                                              return aDefault;

                                          const JSON& v = aModel[aKey];

                                          if( !v.is_array() || v.size() != 3 )
                                              throw TOOL_ERROR( std::string( aKey ) + " is [x, y, z]." );

                                          return VECTOR3D( v[0].get<double>(), v[1].get<double>(), v[2].get<double>() );
                                      };

                                      for( const JSON& m : aArgs["models"] )
                                      {
                                          FP_3DMODEL model;
                                          model.m_Filename = wx( m.at( "path" ) );
                                          model.m_Offset = triple( m, "offset", { 0, 0, 0 } );
                                          model.m_Rotation = triple( m, "rotation", { 0, 0, 0 } );
                                          model.m_Scale = triple( m, "scale", { 1, 1, 1 } );
                                          models.push_back( model );
                                      }

                                      aFootprint.Models() = models;
                                      return JSON{ { "models", models.size() } };
                                  } );

                  return TOOL_RESULT::Data( result );
              } } );

    tools.Add(
            { "fp_check",
              "Run KiCad's footprint checker: courtyard (missing, malformed), type against pads, pads (size, "
              "drill, layers), shorting pads, net ties, silkscreen over pads. Returns the issues; none means clean.",
              "fp", fpSchema( {} ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT&         project = aServices.Sessions().Resolve( aArgs );
                  FOOTPRINT_LIB_SESSION& session = FootprintLibSession( project, aArgs["library"] );
                  FOOTPRINT&             footprint = session.Footprint( aArgs["footprint"] );
                  JSON                   issues = JSON::array();

                  auto report = [&]( const BOARD_ITEM* aA, const BOARD_ITEM* aB, int aCode, const wxString& aMsg,
                                     const VECTOR2I& aPos )
                  {
                      std::shared_ptr<DRC_ITEM> item = DRC_ITEM::Create( aCode );
                      JSON                      items = JSON::array();
                      UNITS_PROVIDER            units( pcbIUScale, EDA_UNITS::MM );

                      for( const BOARD_ITEM* i : { aA, aB } )
                      {
                          if( i )
                              items.push_back( utf8( i->GetItemDescription( &units, true ) ) );
                      }

                      issues.push_back( { { "type", utf8( item->GetSettingsKey() ) },
                                          { "description", utf8( item->GetErrorText( true ) ) },
                                          { "detail", utf8( aMsg ) },
                                          { "items", items },
                                          { "position", PcbPointJson( aPos ) } } );
                  };

                  OUTLINE_ERROR_HANDLER outline =
                          [&]( const wxString& aMsg, BOARD_ITEM* aA, BOARD_ITEM* aB, const VECTOR2I& aPos )
                  {
                      report( aA ? aA : aB, aA ? aB : nullptr, DRCE_MALFORMED_COURTYARD, aMsg, aPos );
                  };

                  footprint.BuildCourtyardCaches( &outline );

                  if( !footprint.AllowMissingCourtyard() && footprint.GetCourtyard( F_CrtYd ).OutlineCount() == 0
                      && footprint.GetCourtyard( B_CrtYd ).OutlineCount() == 0 )
                  {
                      report( &footprint, nullptr, DRCE_MISSING_COURTYARD, wxEmptyString, { 0, 0 } );
                  }

                  FOOTPRINT_UTILS::CheckFootprintAttributes( footprint,
                                                             [&]( const wxString& aMsg )
                                                             {
                                                                 report( &footprint, nullptr,
                                                                         DRCE_FOOTPRINT_TYPE_MISMATCH, aMsg, { 0, 0 } );
                                                             } );

                  UNITS_PROVIDER units( pcbIUScale, EDA_UNITS::MM );
                  FOOTPRINT_UTILS::CheckPads( footprint, units,
                                              [&]( const PAD& aPad, const PAD_UTILS::PAD_CHECK_RESULT& aResult )
                                              {
                                                  report( &aPad, nullptr, ConvertPadCheckToDrcCode( aResult.code ),
                                                          aResult.message, aPad.GetPosition() );
                                              } );

                  FOOTPRINT_UTILS::CheckShortingPads(
                          footprint,
                          [&]( const PAD* aA, const PAD* aB, int aCode, const VECTOR2I& aPos )
                          {
                              report( aA, aB, aCode, wxEmptyString, aPos );
                          } );

                  if( footprint.IsNetTie() )
                  {
                      FOOTPRINT_UTILS::CheckNetTiePadGroups( footprint,
                                                             [&]( const wxString& aMsg )
                                                             {
                                                                 report( &footprint, nullptr, DRCE_FOOTPRINT, aMsg,
                                                                         { 0, 0 } );
                                                             } );

                      FOOTPRINT_UTILS::CheckNetTies(
                              footprint,
                              [&]( const BOARD_ITEM* aA, const BOARD_ITEM* aB, const BOARD_ITEM*, const VECTOR2I& aPos )
                              {
                                  report( aA, aB, DRCE_SHORTING_ITEMS, wxEmptyString, aPos );
                              } );
                  }

                  FOOTPRINT_UTILS::CheckClippedSilk( footprint,
                                                     [&]( BOARD_ITEM* aA, BOARD_ITEM* aB, const VECTOR2I& aPos )
                                                     {
                                                         report( aA, aB, DRCE_SILK_MASK_CLEARANCE, wxEmptyString,
                                                                 aPos );
                                                     } );

                  return TOOL_RESULT::Data(
                          { { "footprint", aArgs["footprint"] }, { "issues", issues }, { "clean", issues.empty() } } );
              } } );

    tools.Add( { "fp_delete", "Delete a footprint from a project library (undo brings it back).", "fp", fpSchema( {} ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     const std::string name = aArgs["footprint"];
                     JSON              result = fpMutate( aServices, aArgs, "fp_delete " + name,
                                                          [&]( FOOTPRINT_LIB_SESSION& aSession ) -> JSON
                                                          {
                                                 aSession.Remove( name );
                                                 return JSON{ { "deleted", name } };
                                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "fp_capture",
                 "Look at a footprint: a PNG of its layers (default copper, paste, mask, silkscreen, fab, courtyard). "
                 "The area returned is in footprint coordinates (mm).",
                 "fp",
                 fpSchema( { { "layers", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                             { "width", { { "type", "integer" }, { "minimum", 200 }, { "maximum", 4096 } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&         project = aServices.Sessions().Resolve( aArgs );
                     FOOTPRINT_LIB_SESSION& session = FootprintLibSession( project, aArgs["library"] );
                     FOOTPRINT&             footprint = session.Footprint( aArgs["footprint"] );

                     // Plotted on a page, so placed well inside it.
                     const VECTOR2I         offset( PcbIu( 100 ), PcbIu( 100 ) );
                     std::unique_ptr<BOARD> board = plotBoard( project, footprint, offset );

                     LSEQ layers;

                     if( aArgs.contains( "layers" ) )
                     {
                         for( const JSON& name : aArgs["layers"] )
                             layers.push_back( PcbLayer( *board, name ) );
                     }
                     else
                     {
                         layers = { B_Cu, F_Cu, F_Paste, F_Mask, B_SilkS, F_SilkS, F_Fab, F_CrtYd };
                     }

                     BOX2I area = board->GetFirstFootprint()->GetBoundingBox( true );
                     area.Inflate( PcbIu( 1 ) );

                     TOOL_RESULT result = PcbCapturePng( project, *board, area, layers, aArgs.value( "width", 1024 ) );
                     JSON&       info = result.structuredContent;
                     info["area"] = { { "x1", mm( area.GetLeft() - offset.x ) },
                                      { "y1", mm( area.GetTop() - offset.y ) },
                                      { "x2", mm( area.GetRight() - offset.x ) },
                                      { "y2", mm( area.GetBottom() - offset.y ) } };
                     result.content.back() = TextContent( info.dump() );
                     return result;
                 } } );

    tools.Add( { "fp_export_svg",
                 "Write an SVG of a footprint (origin at the SVG origin), like kicad-cli fp export svg. Returns the "
                 "file (project-relative).",
                 "fp", fpSchema( { { "output", { { "type", "string" }, { "description", "Folder (output/)." } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&         project = aServices.Sessions().Resolve( aArgs );
                     FOOTPRINT_LIB_SESSION& session = FootprintLibSession( project, aArgs["library"] );
                     const std::string      name = aArgs["footprint"];
                     FOOTPRINT&             footprint = session.Footprint( name );

                     const fs::path dir = project.Files().Resolve( aArgs.value( "output", "output" ) );
                     fs::create_directories( dir );
                     const fs::path file = dir / ( name + ".svg" );

                     PCB_PLOT_PARAMS opts;
                     opts.SetFormat( PLOT_FORMAT::SVG );
                     opts.SetBlackAndWhite( false );

                     LSEQ               layers = { F_Cu, B_Cu, F_Paste, F_Mask, F_SilkS, B_SilkS, F_Fab, F_CrtYd };
                     WX_STRING_REPORTER reporter;

                     if( !PlotFootprintToSVG( footprint, project.KicadProject(), nullptr, opts, layers, LSEQ(),
                                              wx( file.string() ), &reporter ) )
                     {
                         throw TOOL_ERROR( "Plotting the footprint failed: " + utf8( reporter.GetMessages() ) );
                     }

                     return TOOL_RESULT::Data( { { "file", project.Files().Relative( file ) } } );
                 } } );
}

} // namespace AGENTIC
