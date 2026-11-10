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
#include <agentic/gerb/gerber_tools.h>

// excellon_image.h needs gerber_file_image.h first.
#include <gerber_file_image.h>

#include <base_units.h>
#include <dcode.h>
#include <excellon_image.h>
#include <export_to_pcbnew.h>
#include <gerber_diff.h>
#include <gerber_file_image.h>
#include <gerber_file_image_list.h>
#include <gerber_to_png.h>
#include <gerber_to_polyset.h>
#include <layer_ids.h>
#include <lset.h>
#include <X2_gerber_attributes.h>

#include <wx/base64.h>
#include <wx/mstream.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

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


    double mm( double aIu )
    {
        return std::round( aIu / gerbIUScale.IU_PER_MM * 1e4 ) / 1e4;
    }


    GERBER_FILE_IMAGE* image( GERBER_STATE& aState, int aIndex )
    {
        GERBER_FILE_IMAGE* img =
                aIndex >= 0 && aIndex < (int) aState.files.size() ? aState.images->GetGbrImage( aIndex ) : nullptr;

        if( !img )
            throw TOOL_ERROR( "No layer " + std::to_string( aIndex ) + "; gerber_layers lists them." );

        return img;
    }


    /// A loaded layer by index, or by its file name (whole or the end of it, e.g. "F_Cu.gbr").
    int layerIndex( GERBER_STATE& aState, const JSON& aLayer )
    {
        if( aLayer.is_number_integer() )
        {
            image( aState, aLayer.get<int>() );
            return aLayer.get<int>();
        }

        const std::string name = aLayer.get<std::string>();
        int               found = -1;

        for( size_t i = 0; i < aState.files.size(); ++i )
        {
            const std::string& file = aState.files[i];

            // The end of the name counts after a separator: "PTH.drl" is not "NPTH.drl".
            const bool suffix =
                    file.size() > name.size() && file.compare( file.size() - name.size(), name.size(), name ) == 0
                    && std::string( "-_/." ).find( file[file.size() - name.size() - 1] ) != std::string::npos;

            if( file == name || fs::path( file ).filename() == name || suffix )
            {
                if( found >= 0 )
                    throw TOOL_ERROR( "'" + name + "' matches several layers; give the index." );

                found = static_cast<int>( i );
            }
        }

        if( found < 0 )
            throw TOOL_ERROR( "No loaded layer '" + name + "'; gerber_layers lists them." );

        return found;
    }


    std::string typeOf( GERBER_FILE_IMAGE* aImage )
    {
        return dynamic_cast<EXCELLON_IMAGE*>( aImage ) ? "Excellon Drill" : "Gerber";
    }


    JSON x2( GERBER_FILE_IMAGE* aImage )
    {
        JSON out = JSON::object();

        if( X2_ATTRIBUTE_FILEFUNCTION* f = aImage->m_FileFunction )
        {
            out["fileFunction"] = utf8( f->GetFileType() );
            out["side"] = utf8( f->GetBrdLayerSide() );

            if( f->IsCopper() )
                out["layer"] = utf8( f->GetBrdLayerId() );

            if( !f->GetLabel().IsEmpty() )
                out["label"] = utf8( f->GetLabel() );
        }

        if( !aImage->m_PartString.IsEmpty() )
            out["part"] = utf8( aImage->m_PartString );

        if( !aImage->m_MD5_value.IsEmpty() )
            out["md5"] = utf8( aImage->m_MD5_value );

        return out;
    }


    JSON layerJson( GERBER_STATE& aState, int aIndex )
    {
        GERBER_FILE_IMAGE* img = image( aState, aIndex );
        BOX2I              box = CalculateGerberBoundingBox( img );

        return JSON{ { "index", aIndex },
                     { "file", aState.files[aIndex] },
                     { "type", typeOf( img ) },
                     { "x2", x2( img ) },
                     { "items", img->GetItemsCount() },
                     { "bbox",
                       { { "x1", mm( box.GetLeft() ) },
                         { "y1", mm( box.GetTop() ) },
                         { "x2", mm( box.GetRight() ) },
                         { "y2", mm( box.GetBottom() ) } } } };
    }


    /// The PCB layer of a loaded layer: from its X2 file function, else from KiCad's file names.
    int pcbLayerOf( GERBER_FILE_IMAGE* aImage, const std::string& aFile, int aCopperLayers )
    {
        if( dynamic_cast<EXCELLON_IMAGE*>( aImage ) )
            return UNDEFINED_LAYER; // holes

        const bool top = aImage->m_FileFunction && aImage->m_FileFunction->GetBrdLayerSide() == wxS( "Top" );
        const bool bottom = aImage->m_FileFunction && aImage->m_FileFunction->GetBrdLayerSide() == wxS( "Bot" );

        if( X2_ATTRIBUTE_FILEFUNCTION* f = aImage->m_FileFunction )
        {
            const wxString type = f->GetFileType();

            if( f->IsCopper() )
            {
                long n = 0;
                f->GetBrdLayerId().Mid( 1 ).ToLong( &n );

                if( top || n == 1 )
                    return F_Cu;

                if( bottom || n == aCopperLayers )
                    return B_Cu;

                return n >= 2 && n - 1 <= 30 ? static_cast<int>( In1_Cu ) + ( n - 2 ) * 2 : UNSELECTED_LAYER;
            }

            if( type == wxS( "Soldermask" ) )
                return top ? F_Mask : B_Mask;

            if( type == wxS( "Legend" ) )
                return top ? F_SilkS : B_SilkS;

            if( type == wxS( "Paste" ) )
                return top ? F_Paste : B_Paste;

            if( type == wxS( "Profile" ) )
                return Edge_Cuts;
        }

        static const std::map<std::string, int> names = { { "F_Cu", F_Cu },
                                                          { "B_Cu", B_Cu },
                                                          { "F_Mask", F_Mask },
                                                          { "B_Mask", B_Mask },
                                                          { "F_Silkscreen", F_SilkS },
                                                          { "B_Silkscreen", B_SilkS },
                                                          { "F_Paste", F_Paste },
                                                          { "B_Paste", B_Paste },
                                                          { "Edge_Cuts", Edge_Cuts } };

        const std::string stem = fs::path( aFile ).stem().string();

        for( const auto& [suffix, layer] : names )
        {
            if( stem.size() > suffix.size() && stem.compare( stem.size() - suffix.size(), suffix.size(), suffix ) == 0 )
                return layer;
        }

        return UNSELECTED_LAYER;
    }


    KIGFX::COLOR4D colourOf( GERBER_FILE_IMAGE* aImage, const std::string& aFile, size_t aIndex )
    {
        switch( pcbLayerOf( aImage, aFile, 2 ) )
        {
        case F_Cu: return KIGFX::COLOR4D( 0.78, 0.20, 0.20, 1 );
        case B_Cu: return KIGFX::COLOR4D( 0.30, 0.50, 0.85, 1 );
        case F_Mask:
        case B_Mask: return KIGFX::COLOR4D( 0.55, 0.20, 0.55, 1 );
        case F_SilkS:
        case B_SilkS: return KIGFX::COLOR4D( 0.85, 0.75, 0.15, 1 );
        case F_Paste:
        case B_Paste: return KIGFX::COLOR4D( 0.55, 0.55, 0.55, 1 );
        case Edge_Cuts: return KIGFX::COLOR4D( 0.10, 0.10, 0.10, 1 );
        case UNDEFINED_LAYER: return KIGFX::COLOR4D( 0.0, 0.0, 0.0, 1 );
        default: break;
        }

        static const KIGFX::COLOR4D cycle[] = { KIGFX::COLOR4D( 0.2, 0.6, 0.3, 1 ), KIGFX::COLOR4D( 0.9, 0.5, 0.1, 1 ),
                                                KIGFX::COLOR4D( 0.4, 0.3, 0.8, 1 ) };
        return cycle[aIndex % 3];
    }


    /// Extract a zip's files (base names only) into \a aDir.
    std::vector<fs::path> unzip( const fs::path& aZip, const fs::path& aDir )
    {
        wxFFileInputStream    file( wx( aZip.string() ) );
        wxZipInputStream      zip( file );
        std::vector<fs::path> out;

        if( !file.IsOk() || !zip.IsOk() )
            throw TOOL_ERROR( "Cannot read the zip " + aZip.filename().string() + "." );

        fs::create_directories( aDir );

        while( std::unique_ptr<wxZipEntry> entry{ zip.GetNextEntry() } )
        {
            if( entry->IsDir() )
                continue;

            // Only the base name: an entry cannot write outside the folder.
            const std::string name = utf8( wxFileName( entry->GetName() ).GetFullName() );

            if( name.empty() || name[0] == '.' )
                continue;

            std::string content;
            char        buffer[16384];

            while( zip.Read( buffer, sizeof( buffer ) ).LastRead() > 0 )
                content.append( buffer, zip.LastRead() );

            FILE_STORE::WriteAtomic( aDir / name, content );
            out.push_back( aDir / name );
        }

        return out;
    }

} // namespace


GERBER_STATE::GERBER_STATE() :
        images( std::make_unique<GERBER_FILE_IMAGE_LIST>() )
{
}


GERBER_STATE::~GERBER_STATE() = default;


GERBER_STATE& GerberState( AGENT_PROJECT& aProject )
{
    static std::map<std::string, std::unique_ptr<GERBER_STATE>> states;
    std::unique_ptr<GERBER_STATE>&                              state = states[aProject.Root().string()];

    if( !state )
        state = std::make_unique<GERBER_STATE>();

    return *state;
}


wxImage GerberPicture( AGENT_PROJECT& aProject, const std::vector<int>& aIndices, int aWidth )
{
    GERBER_STATE&    state = GerberState( aProject );
    std::vector<int> indices = aIndices;

    if( indices.empty() )
    {
        for( size_t i = 0; i < state.files.size(); ++i )
            indices.push_back( static_cast<int>( i ) );
    }

    if( indices.empty() )
        throw TOOL_ERROR( "No Gerber files loaded; call gerber_load." );

    // Empty layers draw nothing (and upstream's renderer refuses them).
    BOX2I            box;
    std::vector<int> drawn;

    for( int i : indices )
    {
        GERBER_FILE_IMAGE* img = image( state, i );
        BOX2I              b = CalculateGerberBoundingBox( img );

        if( img->GetItemsCount() == 0 || ( b.GetWidth() <= 0 && b.GetHeight() <= 0 ) )
            continue;

        if( drawn.empty() )
            box = b;
        else
            box.Merge( b );

        drawn.push_back( i );
    }

    if( drawn.empty() )
        throw TOOL_ERROR( "The layers are empty." );

    box.Inflate( std::max( box.GetWidth(), box.GetHeight() ) / 40 );

    const int height = std::max( 50, KiROUND( aWidth * double( box.GetHeight() ) / box.GetWidth() ) );
    wxImage   canvas( aWidth, height );
    canvas.SetRGB( wxRect( 0, 0, aWidth, height ), 255, 255, 255 );

    const fs::path tmp = aProject.Root() / ".agentic" / "captures" / "gerber-layer.png";
    fs::create_directories( tmp.parent_path() );

    for( int i : drawn )
    {
        GERBER_RENDER_OPTIONS options;
        options.width = aWidth;
        options.height = height;
        options.dpi = std::max( 10, KiROUND( aWidth / ( box.GetWidth() / gerbIUScale.IU_PER_MM ) * 25.4 ) );
        options.foregroundColor = colourOf( image( state, i ), state.files[i], i );
        options.originXMm = box.GetLeft() / gerbIUScale.IU_PER_MM;
        // The viewport is in Gerber coordinates (Y up); KiCad's box is Y down.
        options.originYMm = -box.GetBottom() / gerbIUScale.IU_PER_MM;
        options.windowWidthMm = box.GetWidth() / gerbIUScale.IU_PER_MM;
        options.windowHeightMm = box.GetHeight() / gerbIUScale.IU_PER_MM;

        wxString error;

        if( !RenderGerberToPng( wx( aProject.Files().Resolve( state.files[i] ).string() ), wx( tmp.string() ), options,
                                &error ) )
        {
            throw TOOL_ERROR( "Rendering " + state.files[i] + " failed: " + utf8( error ) );
        }

        wxImage layer;

        if( !layer.LoadFile( wx( tmp.string() ), wxBITMAP_TYPE_PNG ) )
            throw TOOL_ERROR( "Reading the rendered layer failed." );

        if( layer.GetWidth() != aWidth || layer.GetHeight() != height )
            layer.Rescale( aWidth, height );

        // Layers over each other, 70% opaque.
        for( int y = 0; y < height; ++y )
        {
            for( int x = 0; x < aWidth; ++x )
            {
                const double a = ( layer.HasAlpha() ? layer.GetAlpha( x, y ) / 255.0 : 1.0 ) * 0.7;

                if( a <= 0 )
                    continue;

                canvas.SetRGB( x, y, KiROUND( canvas.GetRed( x, y ) * ( 1 - a ) + layer.GetRed( x, y ) * a ),
                               KiROUND( canvas.GetGreen( x, y ) * ( 1 - a ) + layer.GetGreen( x, y ) * a ),
                               KiROUND( canvas.GetBlue( x, y ) * ( 1 - a ) + layer.GetBlue( x, y ) * a ) );
            }
        }
    }

    return canvas;
}


void RegisterGerberTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY& tools = aServices.Tools();

    auto notify = [&aServices]( AGENT_PROJECT& aProject )
    {
        aServices.Observers().Notify(
                [&]( SESSION_OBSERVER& aObserver )
                {
                    aObserver.OnFilesChanged( aProject );
                } );
    };

    tools.Add( { "gerber_load",
                 "Load Gerber and Excellon drill files into the Gerber viewer: files (project-relative), a job file "
                 "(.gbrjob: its listed files), or a zip (extracted under .agentic/gerbers). Replaces what was loaded "
                 "unless append.",
                 "gerber",
                 ProjectToolSchema( { { "files", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                                      { "job", { { "type", "string" } } },
                                      { "zip", { { "type", "string" } } },
                                      { "append", { { "type", "boolean" } } } } ),
                 [&aServices, notify]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&        project = aServices.Sessions().Resolve( aArgs );
                     GERBER_STATE&         state = GerberState( project );
                     std::vector<fs::path> paths;

                     for( const JSON& file : aArgs.value( "files", JSON::array() ) )
                         paths.push_back( project.Files().Resolve( file ) );

                     if( aArgs.contains( "job" ) )
                     {
                         const fs::path job = project.Files().Resolve( aArgs["job"] );
                         JSON           content;

                         try
                         {
                             content = JSON::parse( FILE_STORE::ReadFile( job ) );
                         }
                         catch( const std::exception& e )
                         {
                             throw TOOL_ERROR( "The job file is not readable JSON: " + std::string( e.what() ) );
                         }

                         for( const JSON& entry : content.value( "FilesAttributes", JSON::array() ) )
                         {
                             const std::string name = entry.value( "Path", "" );

                             if( name.empty() || fs::path( name ).filename().string() != name )
                                 throw TOOL_ERROR( "The job file lists '" + name
                                                   + "', which is not a file next to it." );

                             paths.push_back( job.parent_path() / name );
                         }
                     }

                     if( aArgs.contains( "zip" ) )
                     {
                         const fs::path zip = project.Files().Resolve( aArgs["zip"] );

                         for( const fs::path& p : unzip( zip, project.Root() / ".agentic" / "gerbers" / zip.stem() ) )
                         {
                             if( p.extension() != ".gbrjob" )
                                 paths.push_back( p );
                         }
                     }

                     if( paths.empty() )
                         throw TOOL_ERROR( "Give files, a job file or a zip." );

                     if( !aArgs.value( "append", false ) )
                     {
                         state.images->DeleteAllImages();
                         state.files.clear();
                     }

                     JSON loaded = JSON::array();
                     JSON errors = JSON::array();

                     for( const fs::path& path : paths )
                     {
                         if( state.files.size() >= (size_t) GERBER_DRAWLAYERS_COUNT )
                             throw TOOL_ERROR( "At most " + std::to_string( GERBER_DRAWLAYERS_COUNT ) + " layers." );

                         wxString                           error;
                         wxArrayString                      messages;
                         std::unique_ptr<GERBER_FILE_IMAGE> img =
                                 LoadGerberOrExcellon( wx( path.string() ), &error, &messages );

                         if( !img )
                         {
                             errors.push_back(
                                     { { "file", project.Files().Relative( path ) }, { "error", utf8( error ) } } );
                             continue;
                         }

                         const int index = static_cast<int>( state.files.size() );
                         state.images->AddGbrImage( img.release(), index );
                         state.files.push_back( project.Files().Relative( path ) );

                         JSON entry = layerJson( state, index );

                         if( !messages.IsEmpty() )
                         {
                             JSON warnings = JSON::array();

                             for( const wxString& m : messages )
                                 warnings.push_back( utf8( m ) );

                             entry["messages"] = warnings;
                         }

                         loaded.push_back( entry );
                     }

                     state.loads++;
                     notify( project );

                     JSON out{ { "loaded", loaded }, { "layers", state.files.size() } };

                     if( !errors.empty() )
                         out["errors"] = errors;

                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add( { "gerber_layers",
                 "The loaded Gerber and drill layers: index, file, type, X2 attributes, items, extents (mm).", "gerber",
                 ProjectToolSchema( {} ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     GERBER_STATE& state = GerberState( aServices.Sessions().Resolve( aArgs ) );
                     JSON          out = JSON::array();

                     for( size_t i = 0; i < state.files.size(); ++i )
                         out.push_back( layerJson( state, static_cast<int>( i ) ) );

                     return TOOL_RESULT::Data( { { "layers", out } } );
                 } } );

    tools.Add( { "gerber_info",
                 "One loaded layer in detail, as kicad-cli gerber info gives it: type, item and aperture counts, "
                 "bounding box (mm), X2 attributes, the apertures (D code, shape, size, function), and the copper "
                 "area when area is true.",
                 "gerber",
                 ProjectToolSchema( { { "layer", { { "type", { "integer", "string" } } } },
                                      { "area", { { "type", "boolean" } } } },
                                    { "layer" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     GERBER_STATE&      state = GerberState( aServices.Sessions().Resolve( aArgs ) );
                     const int          index = layerIndex( state, aArgs["layer"] );
                     GERBER_FILE_IMAGE* img = image( state, index );
                     BOX2I              box = CalculateGerberBoundingBox( img );

                     JSON apertures = JSON::array();

                     for( int code = FIRST_DCODE;
                          code < FIRST_DCODE + 10000 && apertures.size() < (size_t) img->GetDcodesCount(); ++code )
                     {
                         D_CODE* dcode = img->GetDCODE( code );

                         if( !dcode || !dcode->m_Defined )
                             continue;

                         JSON a{ { "dcode", code },
                                 { "shape", utf8( D_CODE::ShowApertureType( dcode->m_ApertType ) ) },
                                 { "size", { mm( dcode->m_Size.x ), mm( dcode->m_Size.y ) } },
                                 { "inUse", dcode->m_InUse } };

                         if( !dcode->m_AperFunction.IsEmpty() )
                             a["function"] = utf8( dcode->m_AperFunction );

                         apertures.push_back( a );
                     }

                     JSON out{ { "index", index },
                               { "file", state.files[index] },
                               { "type", typeOf( img ) },
                               { "item_count", img->GetItemsCount() },
                               { "aperture_count", img->GetDcodesCount() },
                               { "units", "mm" },
                               { "bounding_box",
                                 { { "origin_x", box.GetOrigin().x / gerbIUScale.IU_PER_MM },
                                   { "origin_y", box.GetOrigin().y / gerbIUScale.IU_PER_MM },
                                   { "width", box.GetWidth() / gerbIUScale.IU_PER_MM },
                                   { "height", box.GetHeight() / gerbIUScale.IU_PER_MM } } },
                               { "x2", x2( img ) },
                               { "apertures", apertures } };

                     if( aArgs.value( "area", false ) )
                         out["copper_area"] = img->CalculateCopperArea();

                     return TOOL_RESULT::Data( out );
                 } } );

    tools.Add(
            { "gerber_capture",
              "Look at loaded layers (all, or the ones listed by index or file name) drawn over each other in "
              "their layer colours, as a PNG.",
              "gerber",
              ProjectToolSchema( { { "layers", { { "type", "array" } } },
                                   { "width", { { "type", "integer" }, { "minimum", 100 }, { "maximum", 4000 } } } } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  AGENT_PROJECT&   project = aServices.Sessions().Resolve( aArgs );
                  GERBER_STATE&    state = GerberState( project );
                  std::vector<int> indices;

                  for( const JSON& layer : aArgs.value( "layers", JSON::array() ) )
                      indices.push_back( layerIndex( state, layer ) );

                  wxImage picture = GerberPicture( project, indices, aArgs.value( "width", 800 ) );

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

    tools.Add( { "gerber_diff",
                 "Compare two Gerber files (loaded layers by index or name, or project-relative paths) as kicad-cli "
                 "gerber diff does: areas added, removed and kept (mm^2), aligned on their bounding boxes unless "
                 "align is false.",
                 "gerber",
                 ProjectToolSchema( { { "a", { { "type", { "integer", "string" } } } },
                                      { "b", { { "type", { "integer", "string" } } } },
                                      { "align", { { "type", "boolean" } } },
                                      { "toleranceNm", { { "type", "integer" }, { "minimum", 0 } } } },
                                    { "a", "b" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                     GERBER_STATE&  state = GerberState( project );

                     auto pathOf = [&]( const JSON& aWhich ) -> fs::path
                     {
                         if( aWhich.is_string() && fs::exists( project.Files().Resolve( aWhich.get<std::string>() ) ) )
                             return project.Files().Resolve( aWhich.get<std::string>() );

                         return project.Files().Resolve( state.files[layerIndex( state, aWhich )] );
                     };

                     const fs::path a = pathOf( aArgs["a"] );
                     const fs::path b = pathOf( aArgs["b"] );
                     wxString       error;
                     auto           imageA = LoadGerberOrExcellon( wx( a.string() ), &error );

                     if( !imageA )
                         throw TOOL_ERROR( "Cannot read " + a.filename().string() + ": " + utf8( error ) );

                     auto imageB = LoadGerberOrExcellon( wx( b.string() ), &error );

                     if( !imageB )
                         throw TOOL_ERROR( "Cannot read " + b.filename().string() + ": " + utf8( error ) );

                     const int      tolerance = gerbIUScale.NmToIU( aArgs.value( "toleranceNm", 0 ) );
                     SHAPE_POLY_SET polyA = ConvertGerberToPolySet( imageA.get(), tolerance );
                     SHAPE_POLY_SET polyB = ConvertGerberToPolySet( imageB.get(), tolerance );

                     if( aArgs.value( "align", true ) )
                         polyB.Move( CalculateAlignment( polyA, polyB ) );

                     GERBER_DIFF_RESULT diff = CalculateGerberDiff( polyA, polyB );
                     return TOOL_RESULT::Data( FormatDiffResultJson( diff, wx( project.Files().Relative( a ) ),
                                                                     wx( project.Files().Relative( b ) ) ) );
                 } } );

    tools.Add( { "gerber_to_pcb",
                 "Write the loaded layers as a new KiCad board file inside the project (drill files become vias), "
                 "as gerbview's Export to PCB editor. Layers map from their X2 file function or KiCad file names; "
                 "give layers {index or file: layer name} for others. Undo removes the file.",
                 "gerber",
                 ProjectToolSchema(
                         { { "output", { { "type", "string" }, { "description", "Project-relative .kicad_pcb." } } },
                           { "layers", { { "type", "object" } } },
                           { "copperLayers", { { "type", "integer" }, { "minimum", 2 }, { "maximum", 32 } } } },
                         { "output" } ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT&    project = aServices.Sessions().Resolve( aArgs );
                     GERBER_STATE&     state = GerberState( project );
                     const std::string output = aArgs["output"];
                     const fs::path    path = project.Files().Resolve( output );
                     const int         copper = aArgs.value( "copperLayers", 2 );

                     if( path.extension() != ".kicad_pcb" )
                         throw TOOL_ERROR( "output is a .kicad_pcb file." );

                     if( fs::exists( path ) )
                         throw TOOL_ERROR( "'" + output + "' exists; choose a new file name." );

                     if( state.files.empty() )
                         throw TOOL_ERROR( "No Gerber files loaded; call gerber_load." );

                     std::vector<int> lookup( GERBER_DRAWLAYERS_COUNT, UNSELECTED_LAYER );
                     JSON             mapping = JSON::object();

                     for( size_t i = 0; i < state.files.size(); ++i )
                         lookup[i] = pcbLayerOf( image( state, static_cast<int>( i ) ), state.files[i], copper );

                     const JSON layers = aArgs.value( "layers", JSON::object() );

                     for( const auto& [key, name] : layers.items() )
                     {
                         const int index = std::isdigit( static_cast<unsigned char>( key[0] ) )
                                                   ? layerIndex( state, std::stoi( key ) )
                                                   : layerIndex( state, key );
                         wxString  layerName = wx( name );
                         const int layer = layerName == wxS( "holes" ) ? static_cast<int>( UNDEFINED_LAYER )
                                                                       : LSET::NameToLayer( layerName );

                         if( layer < 0 && layer != UNDEFINED_LAYER )
                             throw TOOL_ERROR( "Unknown layer '" + name.get<std::string>()
                                               + "' (F.Cu, B.Mask, ..., or holes)." );

                         lookup[index] = layer;
                     }

                     for( size_t i = 0; i < state.files.size(); ++i )
                     {
                         mapping[state.files[i]] = lookup[i] == UNDEFINED_LAYER ? "holes"
                                                   : lookup[i] == UNSELECTED_LAYER
                                                           ? "skipped"
                                                           : utf8( LSET::Name( PCB_LAYER_ID( lookup[i] ) ) );
                     }

                     JSON result = project.Mutate( "gerber_to_pcb " + output, false,
                                                   [&]() -> JSON
                                                   {
                                                       project.BeforeFileWrite( output );
                                                       GBR_TO_PCB_EXPORTER exporter( state.images.get(), nullptr,
                                                                                     wx( path.string() ) );

                                                       if( !exporter.ExportPcb( lookup.data(), copper ) )
                                                           throw TOOL_ERROR( "Writing " + output + " failed." );

                                                       project.AfterFileWrite( output );
                                                       return JSON{ { "file", output }, { "layers", mapping } };
                                                   } );

                     return TOOL_RESULT::Data( result );
                 } } );
}

} // namespace AGENTIC
