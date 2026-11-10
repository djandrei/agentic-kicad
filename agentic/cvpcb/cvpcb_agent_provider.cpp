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
#include <agentic/core/provider.h>
#include <agentic/core/services.h>
#include <agentic/cvpcb/cvpcb_agent_provider.h>
#include <agentic/viewer_shell/agent_viewer.h>

#include <board.h>
#include <footprint.h>
#include <footprint_filter.h>
#include <footprint_info_impl.h>
#include <footprint_library_adapter.h>
#include <gal_display_options_common.h>
#include <netlist_reader/netlist_reader.h>
#include <netlist_reader/pcb_netlist.h>
#include <pcb_draw_panel_gal.h>
#include <project/project_file.h>
#include <richio.h>
#include <string_utils.h>
#include <view/view.h>

#include <wx/listctrl.h>

#include <fstream>
#include <map>


// Defined in cvpcb/auto_associate.cpp (the same kiface): the text between the next two single quotes.
wxString GetQuotedText( wxString& text );


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


    /// The symbols as CvPcb sees them: the schematic's netlist (D2).
    std::unique_ptr<NETLIST> components( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject )
    {
        const std::string text = aServices.Netlist( aProject );
        auto              netlist = std::make_unique<NETLIST>();

        try
        {
            KICAD_NETLIST_READER reader( new STRING_LINE_READER( text, wxS( "schematic netlist" ) ), netlist.get() );
            reader.LoadNetlist();
        }
        catch( const IO_ERROR& e )
        {
            throw TOOL_ERROR( "The schematic netlist could not be read: " + utf8( e.What() ) );
        }

        return netlist;
    }


    JSON componentJson( COMPONENT& aComponent )
    {
        JSON filters = JSON::array();

        for( const wxString& f : aComponent.GetFootprintFilters() )
            filters.push_back( utf8( f ) );

        return { { "reference", utf8( aComponent.GetReference() ) },
                 { "value", utf8( aComponent.GetValue() ) },
                 { "footprint", utf8( aComponent.GetFPID().Format() ) },
                 { "filters", filters },
                 { "pins", aComponent.GetPinCount() } };
    }


    /// Every footprint of the project's footprint libraries, read as CvPcb reads them.
    std::unique_ptr<FOOTPRINT_LIST_IMPL> footprints( AGENT_PROJECT& aProject, TOOL_CONTEXT& aContext, JSON& aErrors )
    {
        auto& adapter = static_cast<FOOTPRINT_LIBRARY_ADAPTER&>(
                LoadLibraries( aProject, LIBRARY_TABLE_TYPE::FOOTPRINT, aContext, aErrors ) );
        auto list = std::make_unique<FOOTPRINT_LIST_IMPL>();
        list->ReadFootprintFiles( &adapter );
        return list;
    }


    COMPONENT& component( NETLIST& aNetlist, const std::string& aReference )
    {
        COMPONENT* c = aNetlist.GetComponentByReference( wx( aReference ) );

        if( !c )
            throw TOOL_ERROR( "No symbol '" + aReference + "' in the schematic; cvpcb_list lists them." );

        return *c;
    }


    /// Write footprint fields through the schematic's own tool, all in one batch (one undo step).
    JSON assign( AGENT_SERVICES& aServices, const JSON& aArgs, const std::map<std::string, std::string>& aAssignments,
                 TOOL_CONTEXT& aContext )
    {
        if( aAssignments.empty() )
            return JSON{ { "assigned", JSON::object() } };

        JSON calls = JSON::array();

        for( const auto& [reference, footprint] : aAssignments )
            calls.push_back( { { "tool", "sch_symbol_set" },
                               { "arguments", { { "item", reference }, { "footprint", footprint } } } } );

        JSON args{ { "calls", calls } };

        if( aArgs.contains( "project" ) )
            args["project"] = aArgs["project"];

        TOOL_RESULT result = aServices.Tools().Find( "batch" )->handler( args, aContext );

        if( result.isError )
            throw TOOL_ERROR( result.content.empty() ? "The assignment failed."
                                                     : result.content[0].value( "text", "" ) );

        JSON assigned = JSON::object();

        for( const auto& [reference, footprint] : aAssignments )
            assigned[reference] = footprint;

        return JSON{ { "assigned", assigned }, { "saved", result.structuredContent.value( "saved", JSON::array() ) } };
    }


    /// The value/footprint pairs of .equ files: lines of 'value' 'Library:Footprint'.
    std::vector<std::pair<wxString, wxString>> equivalences( const std::vector<fs::path>& aFiles, JSON& aErrors )
    {
        std::vector<std::pair<wxString, wxString>> out;

        for( const fs::path& file : aFiles )
        {
            std::ifstream in( file );

            if( !in )
            {
                aErrors.push_back( "Equivalence file '" + file.filename().string() + "' could not be read." );
                continue;
            }

            std::string line;

            while( std::getline( in, line ) )
            {
                wxString text = wx( line );
                wxString value = GetQuotedText( text );

                if( value.IsEmpty() )
                    continue;

                wxString footprint = GetQuotedText( text );

                if( footprint.IsEmpty() )
                    continue;

                value.Replace( wxT( " " ), wxT( "_" ) );
                out.emplace_back( value, footprint );
            }
        }

        // As CvPcb sorts them (sortListbyCmpValue in auto_associate.cpp).
        std::sort( out.begin(), out.end(),
                   []( const auto& a, const auto& b )
                   {
                       return a.first.Cmp( b.first ) > 0;
                   } );
        return out;
    }


    void registerTools( AGENT_SERVICES& aServices )
    {
        TOOL_REGISTRY& tools = aServices.Tools();

        tools.Add( { "cvpcb_list",
                     "The schematic's symbols as CvPcb lists them: reference, value, assigned footprint, footprint "
                     "filters and pin count.",
                     "cvpcb", ProjectToolSchema( WithPageArgs( { { "unassigned", { { "type", "boolean" } } } } ) ),
                     [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
                     {
                         AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                         auto           netlist = components( aServices, project );
                         JSON           out = JSON::array();

                         for( unsigned i = 0; i < netlist->GetCount(); ++i )
                         {
                             COMPONENT* c = netlist->GetComponent( i );

                             if( aArgs.value( "unassigned", false ) && !c->GetFPID().empty() )
                                 continue;

                             out.push_back( componentJson( *c ) );
                         }

                         return TOOL_RESULT::Data( { { "symbols", Page( out, aArgs ) }, { "total", out.size() } } );
                     } } );

        tools.Add(
                { "cvpcb_candidates",
                  "Footprints for a symbol from the project's footprint libraries, filtered as CvPcb filters: by the "
                  "symbol's footprint filters, its pin count and a library (each can be switched off); paged.",
                  "cvpcb",
                  ProjectToolSchema( { { "item", { { "type", "string" } } },
                                       { "filters", { { "type", "boolean" }, { "description", "Default true." } } },
                                       { "pinCount", { { "type", "boolean" }, { "description", "Default true." } } },
                                       { "library", { { "type", "string" } } },
                                       { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 500 } } },
                                       { "offset", { { "type", "integer" }, { "minimum", 0 } } } },
                                     { "item" } ),
                  [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                  {
                      AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                      auto           netlist = components( aServices, project );
                      COMPONENT&     c = component( *netlist, aArgs["item"] );
                      JSON           errors = JSON::array();
                      auto           list = footprints( project, aContext, errors );

                      FOOTPRINT_FILTER filter( *list );

                      if( aArgs.value( "filters", true ) )
                          filter.FilterByFootprintFilters( c.GetFootprintFilters() );

                      if( aArgs.value( "pinCount", true ) )
                          filter.FilterByPinCount( c.GetPinCount() );

                      if( aArgs.contains( "library" ) )
                          filter.FilterByLibrary( wx( aArgs["library"] ) );

                      const size_t limit = aArgs.value( "limit", 50 );
                      const size_t offset = aArgs.value( "offset", 0 );
                      size_t       total = 0;
                      JSON         out = JSON::array();

                      for( FOOTPRINT_INFO& fp : filter )
                      {
                          if( total >= offset && out.size() < limit )
                          {
                              out.push_back( { { "footprint",
                                                 utf8( fp.GetLibNickname() + wxS( ":" ) + fp.GetFootprintName() ) },
                                               { "pads", fp.GetNumberedPadCount() },
                                               { "description", utf8( fp.GetDesc() ) } } );
                          }

                          total++;
                      }

                      JSON result{ { "symbol", componentJson( c ) }, { "footprints", out }, { "total", total } };

                      if( !errors.empty() )
                          result["libraryErrors"] = errors;

                      return TOOL_RESULT::Data( result );
                  } } );

        tools.Add( { "cvpcb_assign",
                     "Assign footprints to symbols {reference: Library:Footprint}; written as the symbols' Footprint "
                     "fields and saved, as one undo step. Each footprint must exist in the project's libraries.",
                     "cvpcb", ProjectToolSchema( { { "assignments", { { "type", "object" } } } }, { "assignments" } ),
                     [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                     {
                         AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                         auto           netlist = components( aServices, project );
                         JSON           errors = JSON::array();
                         auto           list = footprints( project, aContext, errors );

                         std::map<std::string, std::string> assignments;
                         const JSON                         wanted = aArgs["assignments"];

                         for( const auto& [reference, footprint] : wanted.items() )
                         {
                             component( *netlist, reference );
                             LIB_ID id;

                             if( id.Parse( wx( footprint.get<std::string>() ) ) >= 0 || !id.IsValid()
                                 || !list->GetFootprintInfo( id.GetLibNickname(), id.GetLibItemName() ) )
                             {
                                 throw TOOL_ERROR( "No footprint '" + footprint.get<std::string>()
                                                   + "' in the project's libraries; cvpcb_candidates lists them." );
                             }

                             assignments[reference] = footprint;
                         }

                         return TOOL_RESULT::Data( assign( aServices, aArgs, assignments, aContext ) );
                     } } );

        tools.Add( { "cvpcb_auto_associate",
                     "Assign footprints automatically as CvPcb does: from equivalence files (.equ: 'value' "
                     "'Library:Footprint' lines; the project's, or the ones given), then from a symbol's single "
                     "footprint filter naming a footprint. Symbols that have a footprint are kept unless overwrite.",
                     "cvpcb",
                     ProjectToolSchema(
                             { { "equivalenceFiles", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                               { "overwrite", { { "type", "boolean" } } } } ),
                     [&aServices]( const JSON& aArgs, TOOL_CONTEXT& aContext )
                     {
                         AGENT_PROJECT& project = aServices.Sessions().Resolve( aArgs );
                         auto           netlist = components( aServices, project );
                         JSON           messages = JSON::array();
                         auto           list = footprints( project, aContext, messages );

                         std::vector<fs::path> files;

                         if( aArgs.contains( "equivalenceFiles" ) )
                         {
                             for( const JSON& f : aArgs["equivalenceFiles"] )
                                 files.push_back( project.Files().Resolve( f ) );
                         }
                         else
                         {
                             for( const wxString& f : project.KicadProject().GetProjectFile().m_EquivalenceFiles )
                             {
                                 fs::path p( utf8( wxExpandEnvVars( f ) ) );
                                 files.push_back( p.is_absolute() ? p : project.Root() / p );
                             }
                         }

                         const auto                         equiv = equivalences( files, messages );
                         std::map<std::string, std::string> assignments;

                         for( unsigned i = 0; i < netlist->GetCount(); ++i )
                         {
                             COMPONENT* c = netlist->GetComponent( i );

                             if( !c->GetFPID().empty() && !aArgs.value( "overwrite", false ) )
                                 continue;

                             wxString chosen;
                             wxString candidate;

                             // CVPCB_MAINFRAME::AutomaticFootprintMatching's rules.
                             for( size_t idx = 0; idx < equiv.size() && chosen.IsEmpty(); ++idx )
                             {
                                 const auto& [value, fpid] = equiv[idx];

                                 if( value.CmpNoCase( c->GetValue() ) != 0 )
                                     continue;

                                 LIB_ID                id;
                                 const FOOTPRINT_INFO* fp =
                                         id.Parse( fpid ) < 0
                                                 ? list->GetFootprintInfo( id.GetLibNickname(), id.GetLibItemName() )
                                                 : nullptr;
                                 const bool unique = ( idx + 1 >= equiv.size() || equiv[idx + 1].first != value )
                                                     && ( idx == 0 || equiv[idx - 1].first != value );

                                 if( !fp )
                                 {
                                     messages.push_back( utf8( c->GetReference() ) + ": footprint " + utf8( fpid )
                                                         + " is not in the project's footprint libraries." );
                                     continue;
                                 }

                                 if( unique )
                                 {
                                     chosen = fpid;
                                     break;
                                 }

                                 if( candidate.IsEmpty() )
                                     candidate = fpid;

                                 bool matches = c->GetFootprintFilters().IsEmpty();

                                 for( const wxString& f : c->GetFootprintFilters() )
                                     matches = matches || fp->GetFootprintName().Matches( f );

                                 if( matches )
                                     chosen = fpid;
                             }

                             if( chosen.IsEmpty() )
                                 chosen = candidate;

                             if( chosen.IsEmpty() && c->GetFootprintFilters().GetCount() == 1 )
                             {
                                 LIB_ID id;

                                 if( id.Parse( c->GetFootprintFilters()[0] ) < 0 && id.IsValid()
                                     && list->GetFootprintInfo( id.GetLibNickname(), id.GetLibItemName() ) )
                                 {
                                     chosen = c->GetFootprintFilters()[0];
                                 }
                             }

                             if( !chosen.IsEmpty() )
                                 assignments[utf8( c->GetReference() )] = utf8( chosen );
                         }

                         JSON out = assign( aServices, aArgs, assignments, aContext );
                         out["equivalences"] = equiv.size();

                         if( !messages.empty() )
                             out["messages"] = messages;

                         return TOOL_RESULT::Data( out );
                     } } );
    }


    /// The assignment table and a preview of the selected symbol's footprint.
    class CVPCB_VIEWER : public AGENT_VIEWER
    {
    public:
        CVPCB_VIEWER( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject ) :
                AGENT_VIEWER( aServices, aProject, "cvpcb", _( "Assign Footprints" ) ),
                m_services( aServices )
        {
            m_table = new wxListCtrl( this, wxID_ANY, wxDefaultPosition, wxSize( 520, 400 ),
                                      wxLC_REPORT | wxLC_SINGLE_SEL );
            m_table->AppendColumn( _( "Reference" ), wxLIST_FORMAT_LEFT, 80 );
            m_table->AppendColumn( _( "Value" ), wxLIST_FORMAT_LEFT, 120 );
            m_table->AppendColumn( _( "Footprint" ), wxLIST_FORMAT_LEFT, 300 );
            SetCentralView( m_table );

            m_canvas = new PCB_DRAW_PANEL_GAL( this, wxID_ANY, wxDefaultPosition, wxSize( 360, 360 ), m_galOptions,
                                               EDA_DRAW_PANEL_GAL::GAL_TYPE_OPENGL );
            AddDock( m_canvas, wxS( "preview" ), _( "Footprint" ), wxRIGHT );
            FinishLayout();

            m_table->Bind( wxEVT_LIST_ITEM_SELECTED,
                           [this]( wxListEvent& aEvent )
                           {
                               preview( utf8( m_table->GetItemText( aEvent.GetIndex(), 2 ) ) );
                           } );

            refill();
        }

        ~CVPCB_VIEWER() override
        {
            m_canvas->GetView()->Clear();
            m_shown.reset();
        }

        nlohmann::json UiState() const override
        {
            nlohmann::json state = AGENT_VIEWER::UiState();
            JSON           rows = JSON::object();

            for( int i = 0; i < m_table->GetItemCount(); ++i )
                rows[utf8( m_table->GetItemText( i, 0 ) )] = utf8( m_table->GetItemText( i, 2 ) );

            state["assignments"] = rows;
            state["previewPads"] =
                    m_shown && m_shown->GetFirstFootprint() ? m_shown->GetFirstFootprint()->Pads().size() : 0;
            return state;
        }

        void OnCommitApplied( DOC_SESSION& aSession, const nlohmann::json& aChange ) override
        {
            AGENT_VIEWER::OnCommitApplied( aSession, aChange );

            if( &aSession.Project() == &Project() && aSession.Program() == "sch" )
                refill();
        }

        void OnDocumentReloaded( DOC_SESSION& aSession ) override
        {
            if( &aSession.Project() == &Project() && aSession.Program() == "sch" )
                refill();
        }

    protected:
        bool shows( const DOC_SESSION& aSession ) const override
        {
            return &aSession.Project() == &Project() && aSession.Program() == "sch";
        }

    private:
        void refill()
        {
            m_table->DeleteAllItems();
            std::string last;

            try
            {
                auto netlist = components( m_services, Project() );

                for( unsigned i = 0; i < netlist->GetCount(); ++i )
                {
                    COMPONENT* c = netlist->GetComponent( i );
                    long       row = m_table->InsertItem( m_table->GetItemCount(), c->GetReference() );
                    m_table->SetItem( row, 1, c->GetValue() );
                    m_table->SetItem( row, 2, c->GetFPID().Format().wx_str() );

                    if( !c->GetFPID().empty() )
                        last = utf8( c->GetFPID().Format().wx_str() );
                }
            }
            catch( const TOOL_ERROR& )
            {
                // No schematic yet.
            }

            preview( last );
        }

        void preview( const std::string& aFootprint )
        {
            m_canvas->GetView()->Clear();
            m_shown = std::make_unique<BOARD>();
            m_shown->SetBoardUse( BOARD_USE::FPHOLDER );

            LIB_ID id;

            if( !aFootprint.empty() && id.Parse( wx( aFootprint ) ) < 0 )
            {
                if( FOOTPRINT_LIBRARY_ADAPTER* adapter = static_cast<FOOTPRINT_LIBRARY_ADAPTER*>(
                            Project().LibraryAdapter( LIBRARY_TABLE_TYPE::FOOTPRINT ) ) )
                {
                    if( FOOTPRINT* fp = adapter->LoadFootprint( id, false ) )
                    {
                        fp->SetParent( m_shown.get() );
                        m_shown->Add( fp );
                    }
                }
            }

            m_canvas->DisplayBoard( m_shown.get() );
            m_canvas->UpdateColors();

            if( FOOTPRINT* fp = m_shown->GetFirstFootprint() )
            {
                BOX2I box = fp->GetBoundingBox( true );
                box.Inflate( std::max( box.GetWidth(), box.GetHeight() ) / 5 );
                m_canvas->GetView()->SetViewport( BOX2D( box.GetOrigin(), box.GetSize() ) );
            }

            m_canvas->StartDrawing();
            m_canvas->ForceRefresh();
        }

        AGENT_SERVICES&          m_services;
        wxListCtrl*              m_table = nullptr;
        GAL_DISPLAY_OPTIONS_IMPL m_galOptions;
        PCB_DRAW_PANEL_GAL*      m_canvas = nullptr;
        std::unique_ptr<BOARD>   m_shown;
    };


    class CVPCB_AGENT_PROVIDER : public AGENT_PROVIDER
    {
    public:
        wxString Name() const override { return wxS( "cvpcb" ); }
        wxString Title() const override { return wxS( "Assign Footprints" ); }

        void Register( AGENT_SERVICES& aServices ) override { registerTools( aServices ); }

        AGENT_VIEWER* CreateViewer( AGENT_SERVICES& aServices, AGENT_PROJECT& aProject ) override
        {
            return new CVPCB_VIEWER( aServices, aProject );
        }
    };

} // namespace

} // namespace AGENTIC


AGENT_PROVIDER* CvpcbAgentProvider()
{
    static AGENTIC::CVPCB_AGENT_PROVIDER provider;
    return &provider;
}
