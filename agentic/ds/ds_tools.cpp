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
#include <agentic/core/services.h>
#include <agentic/ds/ds_session.h>
#include <agentic/ds/ds_tools.h>

#include <base_units.h>
#include <drawing_sheet/ds_data_item.h>
#include <drawing_sheet/ds_data_model.h>
#include <drawing_sheet/ds_painter.h>
#include <page_info.h>
#include <plotters/plotter.h>
#include <plotters/plotter_png.h>
#include <title_block.h>

#include <wx/base64.h>
#include <wx/mstream.h>

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


    const std::map<std::string, int> ANCHORS = { { "rb", RB_CORNER },
                                                 { "rt", RT_CORNER },
                                                 { "lb", LB_CORNER },
                                                 { "lt", LT_CORNER } };

    const std::map<std::string, PAGE_OPTION> PAGES = { { "all", ALL_PAGES },
                                                       { "first", FIRST_PAGE_ONLY },
                                                       { "subsequent", SUBSEQUENT_PAGES } };

    const std::map<std::string, DS_DATA_ITEM::DS_ITEM_TYPE> TYPES = { { "text", DS_DATA_ITEM::DS_TEXT },
                                                                      { "line", DS_DATA_ITEM::DS_SEGMENT },
                                                                      { "rect", DS_DATA_ITEM::DS_RECT },
                                                                      { "polygon", DS_DATA_ITEM::DS_POLYPOLYGON },
                                                                      { "bitmap", DS_DATA_ITEM::DS_BITMAP } };


    template <typename T>
    std::string nameOf( const std::map<std::string, T>& aMap, T aValue )
    {
        for( const auto& [name, value] : aMap )
        {
            if( value == aValue )
                return name;
        }

        return "?";
    }


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


    JSON coordJson( const POINT_COORD& aCoord )
    {
        return { { "x", aCoord.m_Pos.x }, { "y", aCoord.m_Pos.y }, { "anchor", nameOf( ANCHORS, aCoord.m_Anchor ) } };
    }


    JSON itemJson( DS_DATA_ITEM* aItem, size_t aIndex )
    {
        JSON out{ { "index", aIndex },
                  { "type", nameOf( TYPES, aItem->GetType() ) },
                  { "name", utf8( aItem->m_Name ) },
                  { "start", coordJson( aItem->m_Pos ) },
                  { "page", nameOf( PAGES, aItem->GetPage1Option() ) },
                  { "repeat", aItem->m_RepeatCount } };

        if( !aItem->m_Info.IsEmpty() )
            out["info"] = utf8( aItem->m_Info );

        if( aItem->GetType() == DS_DATA_ITEM::DS_SEGMENT || aItem->GetType() == DS_DATA_ITEM::DS_RECT )
            out["end"] = coordJson( aItem->m_End );

        if( aItem->GetType() != DS_DATA_ITEM::DS_TEXT && aItem->GetType() != DS_DATA_ITEM::DS_BITMAP )
            out["width"] = aItem->m_LineWidth;

        if( aItem->m_RepeatCount > 1 )
            out["increment"] = { { "x", aItem->m_IncrementVector.x }, { "y", aItem->m_IncrementVector.y } };

        if( aItem->GetType() == DS_DATA_ITEM::DS_TEXT )
        {
            auto* text = static_cast<DS_DATA_ITEM_TEXT*>( aItem );
            out["text"] = utf8( text->m_TextBase );
            out["size"] = { text->m_TextSize.x, text->m_TextSize.y };
            out["bold"] = text->m_Bold;
            out["italic"] = text->m_Italic;
            out["rotation"] = text->m_Orient;
            out["justify"] = text->m_Hjustify == GR_TEXT_H_ALIGN_LEFT    ? "left"
                             : text->m_Hjustify == GR_TEXT_H_ALIGN_RIGHT ? "right"
                                                                         : "center";
        }

        return out;
    }


    DS_DATA_ITEM* findItem( DS_DATA_MODEL& aModel, const JSON& aWhich, size_t* aIndex = nullptr )
    {
        std::vector<DS_DATA_ITEM*>& items = aModel.GetItems();

        if( aWhich.is_number_integer() )
        {
            const int index = aWhich;

            if( index < 0 || index >= (int) items.size() )
                throw TOOL_ERROR( "No item " + std::to_string( index ) + "; ds_items lists them." );

            if( aIndex )
                *aIndex = index;

            return items[index];
        }

        for( size_t i = 0; i < items.size(); ++i )
        {
            if( utf8( items[i]->m_Name ) == aWhich.get<std::string>() )
            {
                if( aIndex )
                    *aIndex = i;

                return items[i];
            }
        }

        throw TOOL_ERROR( "No item named '" + aWhich.get<std::string>() + "'; ds_items lists them." );
    }


    void readCoord( POINT_COORD& aCoord, const JSON& aValue, const std::string& aField )
    {
        if( !aValue.is_object() || !aValue.contains( "x" ) || !aValue.contains( "y" ) )
            throw TOOL_ERROR( aField + " is {x, y, anchor} in mm from a page corner (anchor rb, rt, lb, lt)." );

        aCoord.m_Pos = VECTOR2D( aValue["x"].get<double>(), aValue["y"].get<double>() );

        if( aValue.contains( "anchor" ) )
            aCoord.m_Anchor = lookup( ANCHORS, aValue["anchor"], "anchor" );
    }


    /// Apply the item fields of \a aArgs to \a aItem.
    void apply( DS_DATA_ITEM* aItem, const JSON& aArgs )
    {
        if( aArgs.contains( "name" ) )
            aItem->m_Name = wx( aArgs["name"] );

        if( aArgs.contains( "info" ) )
            aItem->m_Info = wx( aArgs["info"] );

        if( aArgs.contains( "start" ) )
            readCoord( aItem->m_Pos, aArgs["start"], "start" );

        if( aArgs.contains( "end" ) )
            readCoord( aItem->m_End, aArgs["end"], "end" );

        if( aArgs.contains( "width" ) )
            aItem->m_LineWidth = aArgs["width"];

        if( aArgs.contains( "page" ) )
            aItem->SetPage1Option( lookup( PAGES, aArgs["page"], "page" ) );

        if( aArgs.contains( "repeat" ) )
        {
            const int repeat = aArgs["repeat"];

            if( repeat < 1 || repeat > 100 )
                throw TOOL_ERROR( "repeat is 1 to 100." );

            aItem->m_RepeatCount = repeat;
        }

        if( aArgs.contains( "increment" ) )
            aItem->m_IncrementVector =
                    VECTOR2D( aArgs["increment"].at( "x" ).get<double>(), aArgs["increment"].at( "y" ).get<double>() );

        if( aItem->GetType() == DS_DATA_ITEM::DS_TEXT )
        {
            auto* text = static_cast<DS_DATA_ITEM_TEXT*>( aItem );

            if( aArgs.contains( "text" ) )
                text->m_TextBase = wx( aArgs["text"] );

            if( aArgs.contains( "size" ) )
            {
                const JSON&  size = aArgs["size"];
                const double h = size.is_array() ? size[1].get<double>() : size.get<double>();
                const double w = size.is_array() ? size[0].get<double>() : size.get<double>();
                text->m_TextSize = VECTOR2D( w, h );
            }

            if( aArgs.contains( "bold" ) )
                text->m_Bold = aArgs["bold"];

            if( aArgs.contains( "italic" ) )
                text->m_Italic = aArgs["italic"];

            if( aArgs.contains( "rotation" ) )
                text->m_Orient = aArgs["rotation"];

            if( aArgs.contains( "justify" ) )
            {
                const std::string j = aArgs["justify"];
                text->m_Hjustify = j == "left"    ? GR_TEXT_H_ALIGN_LEFT
                                   : j == "right" ? GR_TEXT_H_ALIGN_RIGHT
                                                  : GR_TEXT_H_ALIGN_CENTER;
            }
        }
        else if( aArgs.contains( "text" ) || aArgs.contains( "size" ) || aArgs.contains( "bold" ) )
        {
            throw TOOL_ERROR( "text, size and bold are for text items." );
        }
    }


    const JSON COORD = { { "type", "object" },
                         { "description", "{x, y, anchor}: mm from a page corner (rb default)." } };


    JSON itemProperties()
    {
        return { { "name", { { "type", "string" } } },
                 { "info", { { "type", "string" } } },
                 { "text",
                   { { "type", "string" }, { "description", "May use ${TITLE}, ${REVISION}, %K style fields." } } },
                 { "start", COORD },
                 { "end", COORD },
                 { "width", { { "type", "number" }, { "description", "Line width, mm (0: the sheet default)." } } },
                 { "size", { { "description", "Text size, mm: one number or [w, h]." } } },
                 { "bold", { { "type", "boolean" } } },
                 { "italic", { { "type", "boolean" } } },
                 { "rotation", { { "type", "number" } } },
                 { "justify", { { "type", "string" }, { "enum", { "left", "center", "right" } } } },
                 { "page", { { "type", "string" }, { "enum", { "all", "first", "subsequent" } } } },
                 { "repeat", { { "type", "integer" } } },
                 { "increment", { { "type", "object" } } } };
    }


    JSON dsSchema( JSON aProperties, JSON aRequired = JSON::array() )
    {
        aProperties["file"] = { { "type", "string" }, { "description", "Project-relative .kicad_wks." } };
        aProperties["force"] = { { "type", "boolean" } };
        aRequired.push_back( "file" );
        return ProjectToolSchema( aProperties, aRequired );
    }


    JSON itemsJson( DS_DATA_MODEL& aModel )
    {
        JSON out = JSON::array();

        for( size_t i = 0; i < aModel.GetItems().size(); ++i )
            out.push_back( itemJson( aModel.GetItems()[i], i ) );

        return out;
    }


    JSON dsMutate( AGENT_SERVICES& aServices, const JSON& aArgs, const std::string& aLabel,
                   const std::function<JSON( SHEET_SESSION& )>& aEdit )
    {
        AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
        SHEET_SESSION& session = SheetSession( project, aArgs["file"] );

        return project.Mutate( aLabel, aArgs.value( "force", false ),
                               [&]() -> JSON
                               {
                                   JSON out = aEdit( session );
                                   session.MarkModified();
                                   session.RecordChange( { { "summary", aLabel } } );
                                   return out;
                               } );
    }

} // namespace


wxImage DrawingSheetPicture( DS_DATA_MODEL& aModel, PROJECT& aProject, const std::string& aPageType, int aWidth,
                             const std::string& aCapturePath )
{
    PAGE_INFO page;

    if( !page.SetType( wx( aPageType ) ) )
        throw TOOL_ERROR( "Unknown page size '" + aPageType + "' (A4, A3, A, B, USLetter, ...)." );

    const double dpi = aWidth * 1000.0 / page.GetWidthMils();
    const int    height = KiROUND( page.GetHeightMils() * dpi / 1000.0 );

    KIGFX::DS_RENDER_SETTINGS settings;
    settings.SetDefaultPenWidth( schIUScale.MilsToIU( 6 ) );

    PNG_PLOTTER plotter;
    plotter.SetPixelSize( aWidth, height );
    plotter.SetResolution( KiROUND( dpi ) );
    plotter.SetAntialias( true );
    plotter.SetRenderSettings( &settings );
    plotter.SetPageSettings( page );
    plotter.SetColorMode( true );
    plotter.SetViewport( VECTOR2I( 0, 0 ), schIUScale.IU_PER_MILS / 10, 1.0, false );

    if( !plotter.OpenFile( wx( aCapturePath ) ) )
        throw TOOL_ERROR( "Plotting the drawing sheet failed." );

    plotter.StartPlot( wxS( "1" ) );
    plotter.SetColor( COLOR4D::WHITE );
    plotter.Rect( VECTOR2I( 0, 0 ),
                  VECTOR2I( page.GetWidthIU( schIUScale.IU_PER_MILS ), page.GetHeightIU( schIUScale.IU_PER_MILS ) ),
                  FILL_T::FILLED_SHAPE, 0, 0 );

    TITLE_BLOCK titleBlock;

    // The plot reads KiCad's process-wide sheet model; point it at this one meanwhile.
    DS_DATA_MODEL::SetAltInstance( &aModel );

    try
    {
        PlotDrawingSheet( &plotter, &aProject, titleBlock, page, nullptr, wxS( "1" ), 1, wxS( "Sheet" ), wxS( "/" ),
                          wxS( "capture" ), COLOR4D( 0.5, 0.1, 0.1, 1 ), true );
    }
    catch( ... )
    {
        DS_DATA_MODEL::SetAltInstance( nullptr );
        throw;
    }

    DS_DATA_MODEL::SetAltInstance( nullptr );
    plotter.EndPlot();

    wxImage image;

    if( !image.LoadFile( wx( aCapturePath ), wxBITMAP_TYPE_PNG ) )
        throw TOOL_ERROR( "Reading the plotted sheet failed." );

    return image;
}


void RegisterDrawingSheetTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY& tools = aServices.Tools();

    tools.Add( { "ds_create",
                 "Create a drawing sheet (.kicad_wks) in the project: KiCad's default frame and title block, or an "
                 "empty sheet. Use it from a schematic with sch_drawing_sheet_set.",
                 "ds", dsSchema( { { "template", { { "type", "string" }, { "enum", { "default", "empty" } } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&    project = aServices.Sessions().Resolve( aArgs );
                     const std::string file = aArgs["file"];

                     if( fs::exists( project.Files().Resolve( file ) ) )
                         throw TOOL_ERROR( "'" + file + "' exists; open it with ds_open." );

                     JSON result = dsMutate( aServices, aArgs, "ds_create " + file,
                                             [&]( SHEET_SESSION& aSession ) -> JSON
                                             {
                                                 aSession.SetNew( aArgs.value( "template", "default" ) == "default" );
                                                 return JSON{ { "items", aSession.Model().GetCount() } };
                                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "ds_open", "Open a drawing sheet of the project: its margins and item count.", "ds", dsSchema( {} ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );

                     if( !fs::exists( project.Files().Resolve( aArgs["file"] ) ) )
                         throw TOOL_ERROR( "No drawing sheet '" + aArgs["file"].get<std::string>()
                                           + "'; ds_create makes one." );

                     DS_DATA_MODEL& model = SheetSession( project, aArgs["file"] ).Model();
                     return TOOL_RESULT::Data( { { "file", aArgs["file"] },
                                                 { "items", model.GetCount() },
                                                 { "margins",
                                                   { { "left", model.GetLeftMargin() },
                                                     { "right", model.GetRightMargin() },
                                                     { "top", model.GetTopMargin() },
                                                     { "bottom", model.GetBottomMargin() } } } } );
                 } } );

    tools.Add( { "ds_items",
                 "The items of a drawing sheet: index, type (text, line, rect, polygon, bitmap), name, text, start and "
                 "end ({x, y, anchor} in mm from a page corner), size, page (all, first, subsequent), repeat.",
                 "ds", dsSchema( {} ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                     return TOOL_RESULT::Data(
                             { { "items", itemsJson( SheetSession( project, aArgs["file"] ).Model() ) } } );
                 } } );

    JSON addProps = itemProperties();
    addProps["type"] = { { "type", "string" }, { "enum", { "text", "line", "rect" } } };

    tools.Add( { "ds_item_add",
                 "Add a text, line or rectangle to a drawing sheet. Positions are mm from a page corner (anchor rb "
                 "right bottom by default, rt, lb, lt), so items follow the page size.",
                 "ds", dsSchema( addProps, { "type", "start" } ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     JSON result = dsMutate( aServices, aArgs, "ds_item_add",
                                             [&]( SHEET_SESSION& aSession ) -> JSON
                                             {
                                                 DS_DATA_ITEM*     item = nullptr;
                                                 const std::string type = aArgs["type"];

                                                 if( type == "text" )
                                                 {
                                                     if( !aArgs.contains( "text" ) )
                                                         throw TOOL_ERROR( "A text item needs text." );

                                                     item = new DS_DATA_ITEM_TEXT( wx( aArgs["text"] ) );
                                                 }
                                                 else
                                                 {
                                                     if( !aArgs.contains( "end" ) )
                                                         throw TOOL_ERROR( "A " + type + " needs end." );

                                                     item = new DS_DATA_ITEM( TYPES.at( type ) );
                                                 }

                                                 std::unique_ptr<DS_DATA_ITEM> owned( item );
                                                 apply( item, aArgs );
                                                 aSession.Model().Append( owned.release() );
                                                 const size_t index = aSession.Model().GetCount() - 1;
                                                 return JSON{ { "added", itemJson( item, index ) } };
                                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    JSON setProps = itemProperties();
    setProps["item"] = { { "type", { "integer", "string" } }, { "description", "Index or name." } };

    tools.Add( { "ds_item_set", "Change a drawing sheet item (by index or name): any of its fields.", "ds",
                 dsSchema( setProps, { "item" } ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     JSON result =
                             dsMutate( aServices, aArgs, "ds_item_set",
                                       [&]( SHEET_SESSION& aSession ) -> JSON
                                       {
                                           size_t        index = 0;
                                           DS_DATA_ITEM* item = findItem( aSession.Model(), aArgs["item"], &index );
                                           JSON          before = itemJson( item, index );
                                           apply( item, aArgs );
                                           return JSON{ { "before", before }, { "after", itemJson( item, index ) } };
                                       } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "ds_item_delete", "Delete a drawing sheet item (by index or name).", "ds",
                 dsSchema( { { "item", { { "type", { "integer", "string" } } } } }, { "item" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     JSON result = dsMutate( aServices, aArgs, "ds_item_delete",
                                             [&]( SHEET_SESSION& aSession ) -> JSON
                                             {
                                                 size_t        index = 0;
                                                 DS_DATA_ITEM* item =
                                                         findItem( aSession.Model(), aArgs["item"], &index );
                                                 JSON removed = itemJson( item, index );
                                                 aSession.Model().Remove( item );
                                                 delete item;
                                                 return JSON{ { "deleted", removed } };
                                             } );

                     return TOOL_RESULT::Data( result );
                 } } );

    tools.Add( { "ds_capture", "Look at a drawing sheet on a page (A4 by default) with an empty title block, as a PNG.",
                 "ds",
                 dsSchema( { { "page", { { "type", "string" } } },
                             { "width", { { "type", "integer" }, { "minimum", 200 }, { "maximum", 4096 } } } } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                     SHEET_SESSION& session = SheetSession( project, aArgs["file"] );
                     const fs::path file = project.Root() / ".agentic" / "captures" / "sheet.png";
                     fs::create_directories( file.parent_path() );

                     wxImage picture =
                             DrawingSheetPicture( session.Model(), project.KicadProject(), aArgs.value( "page", "A4" ),
                                                  aArgs.value( "width", 1024 ), file.string() );

                     wxMemoryOutputStream stream;
                     picture.SaveFile( stream, wxBITMAP_TYPE_PNG );
                     std::string data( stream.GetSize(), '\0' );
                     stream.CopyTo( data.data(), data.size() );

                     TOOL_RESULT result;
                     result.content.push_back(
                             ImageContent( wxBase64Encode( data.data(), data.size() ).ToStdString(), "image/png" ) );
                     JSON info{ { "width", picture.GetWidth() }, { "height", picture.GetHeight() } };
                     result.content.push_back( TextContent( info.dump() ) );
                     result.structuredContent = info;
                     return result;
                 } } );
}

} // namespace AGENTIC
