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
#include <agentic/pcb/board_doc_session.h>
#include <agentic/pcb/view3d_tools.h>

#include <3d_canvas/board_adapter.h>
#include <3d_rendering/raytracing/render_3d_raytrace_ram.h>
#include <3d_rendering/track_ball.h>
#include <3d_viewer/eda_3d_viewer_settings.h>
#include <board.h>
#include <project_pcb.h>
#include <reporter.h>
#include <settings/settings_manager.h>

#include <wx/base64.h>
#include <wx/image.h>
#include <wx/mstream.h>

#include <map>


namespace AGENTIC
{

namespace
{

    const std::map<std::string, VIEW3D_TYPE> VIEWS = {
        { "top", VIEW3D_TYPE::VIEW3D_TOP },     { "bottom", VIEW3D_TYPE::VIEW3D_BOTTOM },
        { "left", VIEW3D_TYPE::VIEW3D_LEFT },   { "right", VIEW3D_TYPE::VIEW3D_RIGHT },
        { "front", VIEW3D_TYPE::VIEW3D_FRONT }, { "back", VIEW3D_TYPE::VIEW3D_BACK }
    };


    COLOR4D colour( const std::string& aHex, const std::string& aField )
    {
        COLOR4D c;

        if( aHex.size() != 7 || aHex[0] != '#' || !c.SetFromHexString( wxString::FromUTF8( aHex ) ) )
            throw TOOL_ERROR( aField + " is a colour #rrggbb." );

        return c;
    }


    /// Update \a aState from the arguments of a camera or options call.
    void update( VIEW3D_STATE& aState, const JSON& aArgs )
    {
        if( aArgs.contains( "view" ) )
        {
            if( !VIEWS.count( aArgs["view"] ) )
                throw TOOL_ERROR( "view is top, bottom, left, right, front or back." );

            aState.view = aArgs["view"];

            // A named view starts from its own orientation.
            aState.rotateX = aState.rotateY = aState.rotateZ = 0;
        }

        if( aArgs.contains( "rotation" ) )
        {
            const JSON& r = aArgs["rotation"];

            if( !r.is_array() || r.size() != 3 )
                throw TOOL_ERROR( "rotation is [x, y, z] in degrees." );

            aState.rotateX = r[0];
            aState.rotateY = r[1];
            aState.rotateZ = r[2];
        }

        if( aArgs.contains( "zoom" ) )
        {
            aState.zoom = aArgs["zoom"];

            if( aState.zoom < 0.05 || aState.zoom > 50 )
                throw TOOL_ERROR( "zoom is 0.05 to 50 (1 fits the board)." );
        }

        aState.perspective = aArgs.value( "perspective", aState.perspective );
        aState.models = aArgs.value( "models", aState.models );
        aState.silkscreen = aArgs.value( "silkscreen", aState.silkscreen );
        aState.solderMask = aArgs.value( "solderMask", aState.solderMask );
        aState.solderPaste = aArgs.value( "solderPaste", aState.solderPaste );
        aState.floor = aArgs.value( "floor", aState.floor );

        if( aArgs.contains( "quality" ) )
            aState.high = aArgs["quality"] == "high";

        if( aArgs.contains( "maskColor" ) )
        {
            colour( aArgs["maskColor"], "maskColor" );
            aState.maskColor = aArgs["maskColor"].get<std::string>();
        }

        if( aArgs.contains( "silkscreenColor" ) )
        {
            colour( aArgs["silkscreenColor"], "silkscreenColor" );
            aState.silkscreenColor = aArgs["silkscreenColor"].get<std::string>();
        }
    }


    const JSON CAMERA_PROPERTIES = {
        { "view", { { "type", "string" }, { "enum", { "top", "bottom", "left", "right", "front", "back" } } } },
        { "rotation",
          { { "type", "array" },
            { "description", "[x, y, z] degrees after the view (e.g. [-30, 0, 30] for an angle)." } } },
        { "zoom", { { "type", "number" }, { "description", "1 fits the board; 2 is twice as close." } } },
        { "perspective", { { "type", "boolean" } } }
    };

    const JSON OPTION_PROPERTIES = { { "models", { { "type", "boolean" } } },
                                     { "silkscreen", { { "type", "boolean" } } },
                                     { "solderMask", { { "type", "boolean" } } },
                                     { "solderPaste", { { "type", "boolean" } } },
                                     { "floor", { { "type", "boolean" } } },
                                     { "quality", { { "type", "string" }, { "enum", { "basic", "high" } } } },
                                     { "maskColor", { { "type", "string" } } },
                                     { "silkscreenColor", { { "type", "string" } } } };


    JSON merged( JSON aA, const JSON& aB )
    {
        for( const auto& [key, value] : aB.items() )
            aA[key] = value;

        return aA;
    }


    /// Raytrace \a aBoard as \a aState says, like kicad-cli pcb render.
    wxImage render( BOARD& aBoard, const VIEW3D_STATE& aState, int aWidth, int aHeight, TOOL_CONTEXT& aContext )
    {
        BOARD_ADAPTER adapter;
        adapter.SetBoard( &aBoard );
        adapter.m_IsBoardView = false;

        EDA_3D_VIEWER_SETTINGS cfg;

        if( EDA_3D_VIEWER_SETTINGS* user = GetAppSettings<EDA_3D_VIEWER_SETTINGS>( "3d_viewer" ) )
        {
            cfg.m_Render = user->m_Render;
            cfg.m_LayerPresets = user->m_LayerPresets;
        }

        cfg.m_Render.show_footprints_not_in_posfile = true;
        cfg.m_Render.show_footprints_dnp = true;
        cfg.m_Render.show_footprints_insert = true;
        cfg.m_Render.show_footprints_normal = true;
        cfg.m_Render.show_footprints_virtual = true;

        // kicad-cli pcb render's basic and high qualities.
        cfg.m_Render.raytrace_anti_aliasing = true;
        cfg.m_Render.raytrace_backfloor = aState.floor || aState.high;
        cfg.m_Render.raytrace_post_processing = aState.floor || aState.high;
        cfg.m_Render.raytrace_procedural_textures = aState.high;
        cfg.m_Render.raytrace_reflections = aState.high;
        cfg.m_Render.raytrace_shadows = aState.floor || aState.high;
        cfg.m_Render.differentiate_plated_copper = true;
        cfg.m_Render.raytrace_refractions = true;
        cfg.m_Render.raytrace_recursivelevel_refractions = 1;
        cfg.m_UseStackupColors = true;
        adapter.m_Cfg = &cfg;

        View3dApplyOptions( adapter, aState );
        adapter.Set3dCacheManager( PROJECT_PCB::Get3DCacheManager( aBoard.GetProject() ) );

        const wxSize size( aWidth, aHeight );
        TRACK_BALL   camera( 2 * RANGE_SCALE_3D );
        camera.SetProjection( aState.perspective ? PROJECTION_TYPE::PERSPECTIVE : PROJECTION_TYPE::ORTHO );
        camera.SetCurWindowSize( size );

        RENDER_3D_RAYTRACE_RAM raytrace( adapter, camera );
        raytrace.SetCurWindowSize( size );

        auto reporter = std::make_shared<NULL_REPORTER>();
        raytrace.SetReporters( reporter, reporter );

        for( bool first = true; raytrace.Redraw( false ); first = false )
        {
            // The first redraw puts the look-at point on the board centre.
            if( first )
                View3dApplyCamera( camera, aState );

            if( aContext.IsCancelled() )
                throw TOOL_ERROR( "Cancelled.", JSON{ { "cancelled", true } } );
        }

        uint8_t*     rgba = raytrace.GetBuffer();
        const wxSize real = raytrace.GetRealBufferSize();

        if( !rgba || real.x <= 0 || real.y <= 0 )
            throw TOOL_ERROR( "The 3D renderer produced no picture." );

        wxImage image( real );
        image.InitAlpha();
        unsigned char* rgb = image.GetData();
        unsigned char* alpha = image.GetAlpha();

        for( int i = 0; i < real.x * real.y; ++i )
        {
            rgb[3 * i] = rgba[4 * i];
            rgb[3 * i + 1] = rgba[4 * i + 1];
            rgb[3 * i + 2] = rgba[4 * i + 2];
            alpha[i] = rgba[4 * i + 3];
        }

        return image.Mirror( false );
    }

} // namespace


JSON VIEW3D_STATE::Json() const
{
    JSON out{ { "view", view },
              { "rotation", { rotateX, rotateY, rotateZ } },
              { "zoom", zoom },
              { "perspective", perspective },
              { "models", models },
              { "silkscreen", silkscreen },
              { "solderMask", solderMask },
              { "solderPaste", solderPaste },
              { "floor", floor },
              { "quality", high ? "high" : "basic" } };

    if( maskColor )
        out["maskColor"] = *maskColor;

    if( silkscreenColor )
        out["silkscreenColor"] = *silkscreenColor;

    return out;
}


VIEW3D_STATE& View3dState( AGENT_PROJECT& aProject )
{
    // Keyed by the project folder; the state outlives nothing but the process.
    static std::map<std::string, VIEW3D_STATE> states;
    return states[aProject.Root().string()];
}


void View3dApplyCamera( CAMERA& aCamera, const VIEW3D_STATE& aState )
{
    aCamera.SetProjection( aState.perspective ? PROJECTION_TYPE::PERSPECTIVE : PROJECTION_TYPE::ORTHO );
    aCamera.ViewCommand_T1( VIEWS.at( aState.view ) );
    aCamera.Zoom_T1( static_cast<float>( aState.zoom ) );
    aCamera.RotateX_T1( DEG2RAD( aState.rotateX ) );
    aCamera.RotateY_T1( DEG2RAD( aState.rotateY ) );
    aCamera.RotateZ_T1( DEG2RAD( aState.rotateZ ) );
    aCamera.Interpolate( 1.0f );
    aCamera.SetT0_and_T1_current_T();
    aCamera.ParametersChanged();
}


void View3dApplyOptions( BOARD_ADAPTER& aAdapter, const VIEW3D_STATE& aState )
{
    std::bitset<LAYER_3D_END> layers = aAdapter.GetDefaultVisibleLayers();

    for( int layer : { LAYER_3D_TH_MODELS, LAYER_3D_SMD_MODELS, LAYER_3D_VIRTUAL_MODELS, LAYER_3D_MODELS_NOT_IN_POS,
                       LAYER_3D_MODELS_MARKED_DNP } )
        layers.set( layer, aState.models );

    layers.set( LAYER_3D_SILKSCREEN_TOP, aState.silkscreen );
    layers.set( LAYER_3D_SILKSCREEN_BOTTOM, aState.silkscreen );
    layers.set( LAYER_3D_SOLDERMASK_TOP, aState.solderMask );
    layers.set( LAYER_3D_SOLDERMASK_BOTTOM, aState.solderMask );
    layers.set( LAYER_3D_SOLDERPASTE, aState.solderPaste );
    aAdapter.SetVisibleLayers( layers );

    aAdapter.m_ColorOverrides.clear();

    if( aState.maskColor )
    {
        COLOR4D c = colour( *aState.maskColor, "maskColor" ).WithAlpha( 0.83 );
        aAdapter.m_ColorOverrides[LAYER_3D_SOLDERMASK_TOP] = c;
        aAdapter.m_ColorOverrides[LAYER_3D_SOLDERMASK_BOTTOM] = c;
    }

    if( aState.silkscreenColor )
    {
        COLOR4D c = colour( *aState.silkscreenColor, "silkscreenColor" );
        aAdapter.m_ColorOverrides[LAYER_3D_SILKSCREEN_TOP] = c;
        aAdapter.m_ColorOverrides[LAYER_3D_SILKSCREEN_BOTTOM] = c;
    }
}


void RegisterView3dTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY& tools = aServices.Tools();

    // The 3D viewer follows the agent's camera; it is told through the board's results.
    auto notify = [&aServices]( AGENT_PROJECT& aProject, BOARD_DOC_SESSION& aSession )
    {
        const JSON state = View3dState( aProject ).Json();
        aServices.Observers().Notify(
                [&]( SESSION_OBSERVER& aObserver )
                {
                    aObserver.OnResults( aSession, JSON{ { "checker", "view3d" }, { "state", state } } );
                } );
    };

    tools.Add( { "view3d_camera_get", "The 3D camera and display options the agent's captures use.", "view3d",
                 ProjectToolSchema( {} ), [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     return TOOL_RESULT::Data( View3dState( aServices.Sessions().Resolve( aArgs ) ).Json() );
                 } } );

    tools.Add( { "view3d_camera_set",
                 "Point the 3D camera: a named view (top, bottom, left, right, front, back), then rotation [x, y, z] "
                 "degrees, zoom (1 fits the board) and perspective. The 3D viewer follows.",
                 "view3d", ProjectToolSchema( CAMERA_PROPERTIES ),
                 [&aServices, notify]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                     VIEW3D_STATE   state = View3dState( project );
                     update( state, aArgs );
                     View3dState( project ) = state;
                     notify( project, BoardSession( project ) );
                     return TOOL_RESULT::Data( state.Json() );
                 } } );

    tools.Add( { "view3d_options",
                 "What 3D shows: models, silkscreen, solder mask, solder paste, a floor with shadows, quality basic or "
                 "high, mask and silkscreen colours (#rrggbb).",
                 "view3d", ProjectToolSchema( OPTION_PROPERTIES ),
                 [&aServices, notify]( const JSON& aArgs, TOOL_CONTEXT& )
                 {
                     AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                     VIEW3D_STATE   state = View3dState( project );
                     update( state, aArgs );
                     View3dState( project ) = state;
                     notify( project, BoardSession( project ) );
                     return TOOL_RESULT::Data( state.Json() );
                 } } );

    tools.Add( { "view3d_capture",
                 "A raytraced 3D picture of the board (PNG), headless, with the current camera and options; any "
                 "camera or option argument applies to this picture only. Width and height in pixels (the "
                 "renderer may round them down; the result gives the real size).",
                 "view3d",
                 ProjectToolSchema( merged(
                         merged( CAMERA_PROPERTIES, OPTION_PROPERTIES ),
                         { { "width", { { "type", "integer" }, { "minimum", 64 }, { "maximum", 4096 } } },
                           { "height", { { "type", "integer" }, { "minimum", 64 }, { "maximum", 4096 } } } } ) ),
                 [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                 {
                     AGENT_PROJECT&     project = aServices.Sessions().Resolve( aArgs );
                     BOARD_DOC_SESSION& session = BoardSession( project );
                     VIEW3D_STATE       state = View3dState( project );
                     update( state, aArgs );

                     wxImage image = render( session.Board(), state, aArgs.value( "width", 1024 ),
                                             aArgs.value( "height", 768 ), aContext );

                     wxMemoryOutputStream stream;
                     image.SaveFile( stream, wxBITMAP_TYPE_PNG );
                     std::string data( stream.GetSize(), '\0' );
                     stream.CopyTo( data.data(), data.size() );

                     TOOL_RESULT result;
                     result.content.push_back(
                             ImageContent( wxBase64Encode( data.data(), data.size() ).ToStdString(), "image/png" ) );
                     JSON info{ { "width", image.GetWidth() },
                                { "height", image.GetHeight() },
                                { "camera", state.Json() } };
                     result.content.push_back( TextContent( info.dump() ) );
                     result.structuredContent = info;
                     return result;
                 } } );
}

} // namespace AGENTIC
