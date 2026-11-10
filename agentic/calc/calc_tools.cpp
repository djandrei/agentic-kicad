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


#include <agentic/calc/calc_formulas.h>
#include <agentic/calc/calc_tools.h>
#include <agentic/core/agent_project.h>
#include <agentic/core/core_tools.h>
#include <agentic/core/services.h>

#include <attenuators/attenuator_classes.h>
#include <calculator_panels/iec60664.h>
#include <eseries.h>
#include <resistor_substitution_utils.h>
#include <track_width_calculations.h>
#include <transline_calculations/coax.h>
#include <transline_calculations/coplanar.h>
#include <transline_calculations/coupled_microstrip.h>
#include <transline_calculations/coupled_stripline.h>
#include <transline_calculations/microstrip.h>
#include <transline_calculations/rectwaveguide.h>
#include <transline_calculations/stripline.h>
#include <transline_calculations/twistedpair.h>

#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>


namespace AGENTIC
{

using namespace CALC;

namespace
{

    constexpr double MM = 1e-3;
    constexpr double MIL = 25.4e-6;


    double num( const JSON& aIn, const char* aKey )
    {
        if( !aIn.contains( aKey ) || !aIn[aKey].is_number() )
            throw TOOL_ERROR( std::string( "Input '" ) + aKey + "' (a number) is needed." );

        return aIn[aKey].get<double>();
    }


    double num( const JSON& aIn, const char* aKey, double aDefault )
    {
        return aIn.contains( aKey ) ? num( aIn, aKey ) : aDefault;
    }


    /// A finite number, or null (JSON has no NaN).
    JSON value( double aValue )
    {
        return std::isfinite( aValue ) ? JSON( aValue ) : JSON( nullptr );
    }


    JSON numberProps( std::initializer_list<std::pair<const char*, const char*>> aProps )
    {
        JSON props = JSON::object();

        for( const auto& [name, description] : aProps )
            props[name] = { { "type", "number" }, { "description", description } };

        return props;
    }


    struct CALCULATOR
    {
        std::string                        name;
        std::string                        description;
        JSON                               inputs;
        std::function<JSON( const JSON& )> run;
    };


    // Transmission lines: the parameters by name, and which of them are lengths (mm here, m there).
    using TCP = TRANSLINE_PARAMETERS;

    const std::map<std::string, TCP> TL_PARAMS = { { "epsilonR", TCP::EPSILONR },
                                                   { "tanD", TCP::TAND },
                                                   { "rho", TCP::RHO },
                                                   { "h", TCP::H },
                                                   { "hTop", TCP::H_T },
                                                   { "t", TCP::T },
                                                   { "width", TCP::PHYS_WIDTH },
                                                   { "innerDiameter", TCP::PHYS_DIAM_IN },
                                                   { "gap", TCP::PHYS_S },
                                                   { "outerDiameter", TCP::PHYS_DIAM_OUT },
                                                   { "length", TCP::PHYS_LEN },
                                                   { "roughness", TCP::ROUGH },
                                                   { "muR", TCP::MUR },
                                                   { "muRConductor", TCP::MURC },
                                                   { "frequency", TCP::FREQUENCY },
                                                   { "striplineA", TCP::STRIPLINE_A },
                                                   { "twists", TCP::TWISTEDPAIR_TWIST },
                                                   { "epsilonREnvironment", TCP::TWISTEDPAIR_EPSILONR_ENV },
                                                   { "z0", TCP::Z0 },
                                                   { "z0Even", TCP::Z0_E },
                                                   { "z0Odd", TCP::Z0_O },
                                                   { "electricalLength", TCP::ANG_L },
                                                   { "skinDepth", TCP::SKIN_DEPTH },
                                                   { "lossDielectric", TCP::LOSS_DIELECTRIC },
                                                   { "lossConductor", TCP::LOSS_CONDUCTOR },
                                                   { "cutoffFrequency", TCP::CUTOFF_FREQUENCY },
                                                   { "epsilonEff", TCP::EPSILON_EFF },
                                                   { "epsilonEffEven", TCP::EPSILON_EFF_EVEN },
                                                   { "epsilonEffOdd", TCP::EPSILON_EFF_ODD },
                                                   { "unitDelay", TCP::UNIT_PROP_DELAY },
                                                   { "unitDelayOdd", TCP::UNIT_PROP_DELAY_ODD },
                                                   { "unitDelayEven", TCP::UNIT_PROP_DELAY_EVEN },
                                                   { "attenuationConductor", TCP::ATTEN_COND },
                                                   { "attenuationConductorEven", TCP::ATTEN_COND_EVEN },
                                                   { "attenuationConductorOdd", TCP::ATTEN_COND_ODD },
                                                   { "attenuationDielectric", TCP::ATTEN_DILECTRIC },
                                                   { "attenuationDielectricEven", TCP::ATTEN_DILECTRIC_EVEN },
                                                   { "attenuationDielectricOdd", TCP::ATTEN_DILECTRIC_ODD },
                                                   { "zDiff", TCP::Z_DIFF },
                                                   { "zCommon", TCP::Z_COMM },
                                                   { "couplingK", TCP::COUPLING_K } };

    const std::set<TCP> TL_LENGTHS = {
        TCP::H,         TCP::H_T,           TCP::T,        TCP::PHYS_WIDTH, TCP::PHYS_DIAM_IN,
        TCP::PHYS_S,    TCP::PHYS_DIAM_OUT, TCP::PHYS_LEN, TCP::ROUGH,      TCP::STRIPLINE_A,
        TCP::SKIN_DEPTH
    };


    std::unique_ptr<TRANSLINE_CALCULATION_BASE> transline( const std::string& aType )
    {
        if( aType == "microstrip" )
            return std::make_unique<MICROSTRIP>();

        if( aType == "coplanar" || aType == "grounded_coplanar" )
            return std::make_unique<COPLANAR>();

        if( aType == "stripline" )
            return std::make_unique<STRIPLINE>();

        if( aType == "coax" )
            return std::make_unique<COAX>();

        if( aType == "twisted_pair" )
            return std::make_unique<TWISTEDPAIR>();

        if( aType == "rect_waveguide" )
            return std::make_unique<RECTWAVEGUIDE>();

        if( aType == "coupled_microstrip" )
            return std::make_unique<COUPLED_MICROSTRIP>();

        if( aType == "coupled_stripline" )
            return std::make_unique<COUPLED_STRIPLINE>();

        throw TOOL_ERROR( "type is microstrip, coplanar, grounded_coplanar, stripline, coax, twisted_pair, "
                          "rect_waveguide, coupled_microstrip or coupled_stripline." );
    }


    JSON runTransline( const JSON& aIn )
    {
        const std::string type = aIn.value( "type", "" );
        auto              line = transline( type );
        const JSON        params = aIn.value( "parameters", JSON::object() );

        auto trySet = [&]( TCP aParam, double aValue )
        {
            try
            {
                line->SetParameter( aParam, aValue );
                return true;
            }
            catch( const std::out_of_range& )
            {
                return false;
            }
        };

        // The panel's defaults for what the agent does not give.
        trySet( TCP::MUR, 1 );
        trySet( TCP::MURC, 1 );
        trySet( TCP::ROUGH, 0 );
        trySet( TCP::TAND, 0 );
        trySet( TCP::DIELECTRIC_MODEL_SEL, 0 );
        trySet( TCP::SOLDERMASK_PRESENT, 0 );
        trySet( TCP::CPW_BACKMETAL, type == "grounded_coplanar" ? 1 : 0 );
        trySet( TCP::EPSILON_EFF, 1 );
        trySet( TCP::H_T, 1e20 ); // no cover above a microstrip, as the panel's default

        for( const auto& [name, v] : params.items() )
        {
            auto it = TL_PARAMS.find( name );

            if( it == TL_PARAMS.end() || !v.is_number() )
                throw TOOL_ERROR( "Unknown or non-numeric parameter '" + name + "'." );

            const double si = TL_LENGTHS.count( it->second ) ? v.get<double>() * MM : v.get<double>();

            if( !trySet( it->second, si ) )
                throw TOOL_ERROR( "A " + type + " has no parameter '" + name + "'." );
        }

        const double frequency = params.value( "frequency", 1e9 );
        const double rho = params.value( "rho", 1.72e-8 );
        const double sigma = 1.0 / rho;
        trySet( TCP::FREQUENCY, frequency );
        trySet( TCP::RHO, rho );
        trySet( TCP::SIGMA, sigma );
        trySet( TCP::EPSILONR_SPEC_FREQ, frequency );
        trySet( TCP::SKIN_DEPTH,
                1.0 / std::sqrt( M_PI * frequency * params.value( "muRConductor", 1.0 ) * 4e-7 * M_PI * sigma ) );

        const bool synthesize = aIn.value( "mode", "analyse" ) == "synthesize";

        // The result maps are filled when they are asked for: after the calculation.
        if( synthesize && !line->Synthesize( SYNTHESIZE_OPTS::DEFAULT ) )
            throw TOOL_ERROR( "The synthesis did not converge; check z0 and the fixed dimensions." );

        if( !synthesize )
            line->Analyse();

        auto* results = synthesize ? &line->GetSynthesisResults() : &line->GetAnalysisResults();

        JSON out = JSON::object();
        JSON warnings = JSON::array();

        for( const auto& [name, param] : TL_PARAMS )
        {
            auto it = results->find( param );

            if( it == results->end() )
                continue;

            const double v = TL_LENGTHS.count( param ) ? it->second.first / MM : it->second.first;
            out[name] = value( v );

            if( it->second.second != TRANSLINE_STATUS::OK )
                warnings.push_back( name );
        }

        JSON result{ { "type", type }, { "mode", synthesize ? "synthesize" : "analyse" }, { "results", out } };

        if( !warnings.empty() )
            result["outOfRange"] = warnings;

        return result;
    }


    const std::vector<CALCULATOR>& calculators()
    {
        static const std::vector<CALCULATOR> list = {
            { "track_width",
              "IPC-2152 (Brooks & Adam fit) track width for a current, or the current for a width: copper "
              "thickness mm (0.035 for 1 oz), temperature rise C; external and internal layers.",
              numberProps( { { "current", "A (give current, or externalWidth or internalWidth)" },
                             { "externalWidth", "mm" },
                             { "internalWidth", "mm" },
                             { "thickness", "mm, both layers (default 0.035)" },
                             { "externalThickness", "mm" },
                             { "internalThickness", "mm" },
                             { "temperatureRise", "C (default 10)" } } ),
              []( const JSON& aIn ) -> JSON
              {
                  const double t = num( aIn, "thickness", 0.035 );
                  const double extT = num( aIn, "externalThickness", t ) / 25.4e-3;
                  const double intT = num( aIn, "internalThickness", t ) / 25.4e-3;
                  const double dT = num( aIn, "temperatureRise", 10 );
                  double       current;

                  if( aIn.contains( "current" ) )
                      current = num( aIn, "current" );
                  else if( aIn.contains( "externalWidth" ) )
                      current = TRACK_WIDTH_CALCULATIONS::CurrentFromWidth( num( aIn, "externalWidth" ) / 25.4e-3, extT,
                                                                            dT, false );
                  else if( aIn.contains( "internalWidth" ) )
                      current = TRACK_WIDTH_CALCULATIONS::CurrentFromWidth( num( aIn, "internalWidth" ) / 25.4e-3, intT,
                                                                            dT, true );
                  else
                      throw TOOL_ERROR( "Give current, externalWidth or internalWidth." );

                  const double extW = TRACK_WIDTH_CALCULATIONS::WidthFromCurrent( current, extT, dT, false ) * 25.4e-3;
                  const double intW = TRACK_WIDTH_CALCULATIONS::WidthFromCurrent( current, intT, dT, true ) * 25.4e-3;

                  return { { "current", value( current ) },
                           { "externalWidth", value( extW ) },
                           { "internalWidth", value( intW ) },
                           { "externalArea", value( extW * extT * 25.4e-3 ) },
                           { "internalArea", value( intW * intT * 25.4e-3 ) },
                           { "units", "A, mm, mm2" } };
              } },

            { "via_size",
              "A via's resistance, voltage drop, power loss, ampacity, thermal resistance, capacitance, rise time "
              "degradation, inductance and reactance (lengths mm).",
              numberProps( { { "holeDiameter", "mm finished hole" },
                             { "platingThickness", "mm (default 0.035)" },
                             { "length", "mm, board thickness for through vias (default 1.6)" },
                             { "padDiameter", "mm" },
                             { "clearanceDiameter", "mm antipad" },
                             { "impedance", "ohm (default 50)" },
                             { "current", "A (default 1)" },
                             { "resistivity", "ohm m (default 1.72e-8)" },
                             { "permittivity", "relative (default 4.5)" },
                             { "temperatureRise", "C (default 10)" },
                             { "riseTime", "ns (default 1)" } } ),
              []( const JSON& aIn ) -> JSON
              {
                  VIA_INPUT  in{ num( aIn, "holeDiameter" ) * MM,
                                num( aIn, "platingThickness", 0.035 ) * MM,
                                num( aIn, "length", 1.6 ) * MM,
                                num( aIn, "padDiameter" ) * MM,
                                num( aIn, "clearanceDiameter" ) * MM,
                                num( aIn, "impedance", 50 ),
                                num( aIn, "current", 1 ),
                                num( aIn, "resistivity", 1.72e-8 ),
                                num( aIn, "permittivity", 4.5 ),
                                num( aIn, "temperatureRise", 10 ),
                                num( aIn, "riseTime", 1 ) };
                  VIA_RESULT r = ViaSize( in );
                  return { { "resistance", value( r.resistance ) },
                           { "voltageDrop", value( r.voltageDrop ) },
                           { "powerLoss", value( r.powerLoss ) },
                           { "ampacity", value( r.ampacity ) },
                           { "thermalResistance", value( r.thermalResistance ) },
                           { "capacitance", value( r.capacitancePf ) },
                           { "riseTimeDegradation", value( r.riseTimeDegradationPs ) },
                           { "inductance", value( r.inductanceNh ) },
                           { "reactance", value( r.reactance ) },
                           { "units", "ohm, V, W, A, C/W, pF, ps, nH, ohm" } };
              } },

            { "electrical_spacing_ipc2221",
              "IPC-2221 minimum conductor spacing (mm) for a voltage (DC or AC peak): B1 internal, B2 external "
              "uncoated sea level, B3 above 3050 m, B4 external coated, A5 coated assembly, A6 external component "
              "uncoated, A7 coated.",
              numberProps( { { "voltage", "V" } } ),
              []( const JSON& aIn ) -> JSON
              {
                  std::array<double, 8> s = Ipc2221Spacing( num( aIn, "voltage" ) );
                  static const char*    names[8] = { "B1", "B2", "B3", "B4", "A5", "A6", "A7", "A8" };
                  JSON                  out = JSON::object();

                  for( int i = 0; i < 8; ++i )
                      out[names[i]] = s[i] / MM;

                  return { { "spacing", out }, { "units", "mm" } };
              } },

            { "electrical_spacing_iec60664",
              "IEC 60664-1 clearance, creepage and minimum groove width (mm) with KiCad's implementation.",
              []()
              {
                  JSON p = numberProps( { { "ratedVoltage", "V rms (default 230)" },
                                          { "rmsVoltage", "V working rms (default ratedVoltage)" },
                                          { "peakVoltage", "kV recurring peak (default 0.5)" },
                                          { "transientVoltage", "kV transient overvoltage (default 1)" },
                                          { "frequency", "Hz (default 50)" },
                                          { "altitude", "m (default 2000)" },
                                          { "overvoltageCategory", "1 to 4 (default 3)" },
                                          { "pollutionDegree", "1 to 4 (default 2)" },
                                          { "materialGroup", "1, 2, 3 (IIIa), 4 (IIIb) (default 3)" } } );
                  p["insulation"] = { { "type", "string" }, { "enum", { "functional", "basic", "reinforced" } } };
                  p["pcb"] = { { "type", "boolean" }, { "description", "Printed board material (default true)." } };
                  return p;
              }(),
              []( const JSON& aIn ) -> JSON
              {
                  using I = IEC60664;
                  IEC60664          iec;
                  const double      rated = num( aIn, "ratedVoltage", 230 );
                  const double      rms = num( aIn, "rmsVoltage", rated );
                  const std::string ins = aIn.value( "insulation", "basic" );
                  iec.SetRatedVoltage( rated );
                  iec.SetRMSVoltage( rms );
                  iec.SetPeakVoltage( num( aIn, "peakVoltage", 0.5 ) );
                  iec.SetTransientVoltage( num( aIn, "transientVoltage", 1 ) );
                  iec.SetFrequency( num( aIn, "frequency", 50 ) );
                  iec.SetAltitude( num( aIn, "altitude", 2000 ) );
                  iec.SetOvervoltageCategory( static_cast<I::OV_CATEGORY>(
                          std::clamp( (int) num( aIn, "overvoltageCategory", 3 ), 1, 4 ) ) );
                  iec.SetPollutionDegree( static_cast<I::POLLUTION_DEGREE>(
                          std::clamp( (int) num( aIn, "pollutionDegree", 2 ), 1, 4 ) ) );
                  iec.SetMaterialGroup(
                          static_cast<I::MATERIAL_GROUP>( std::clamp( (int) num( aIn, "materialGroup", 3 ), 1, 4 ) ) );
                  iec.SetInsulationType( ins == "functional"   ? I::INSULATION_TYPE::FUNCTIONAL
                                         : ins == "reinforced" ? I::INSULATION_TYPE::REINFORCED
                                                               : I::INSULATION_TYPE::BASIC );
                  iec.SetPCBMaterial( aIn.value( "pcb", true ) );
                  iec.SetField( I::FIELD::INHOMOGENEOUS );

                  try
                  {
                      iec.Compute();
                  }
                  catch( const std::exception& e )
                  {
                      throw TOOL_ERROR( std::string( "IEC 60664: " ) + e.what() );
                  }

                  // As the panel shows them: negative is no value; creepage and groove width do not
                  // apply to pollution degree 4.
                  const bool pd4 = num( aIn, "pollutionDegree", 2 ) >= 4;
                  auto       mm = [&]( double aValue, bool aNa ) -> JSON
                  {
                      if( aNa )
                          return "n/a";

                      return aValue >= 0 ? JSON( aValue ) : JSON( nullptr );
                  };

                  return { { "clearance", mm( iec.GetClearanceDistance(), false ) },
                           { "creepage", mm( iec.GetCreepageDistance(), pd4 ) },
                           { "minGrooveWidth", mm( iec.GetMinGrooveWidth(), pd4 ) },
                           { "ratedImpulseVoltage", value( iec.GetRatedImpulseWithstandVoltage() ) },
                           { "units", "mm, V" } };
              } },

            { "fusing_current",
              "Fusing of a copper track (adiabatic, Onderdonk-type): solve for width, thickness, current or time "
              "from the others (melting and ambient temperature C, current A, width and thickness mm, time s).",
              []()
              {
                  JSON p = numberProps( { { "melting", "C (default 1084, copper)" },
                                          { "ambient", "C (default 25)" },
                                          { "current", "A" },
                                          { "width", "mm" },
                                          { "thickness", "mm" },
                                          { "time", "s" } } );
                  p["solve"] = { { "type", "string" }, { "enum", { "width", "thickness", "current", "time" } } };
                  return p;
              }(),
              []( const JSON& aIn ) -> JSON
              {
                  const std::string solve = aIn.value( "solve", "current" );
                  FUSE_INPUT        in{ num( aIn, "melting", 1084 ),
                                 num( aIn, "ambient", 25 ),
                                 num( aIn, "current", NAN ),
                                 num( aIn, "width", NAN ) * MM,
                                 num( aIn, "thickness", NAN ) * MM,
                                 num( aIn, "time", NAN ),
                                 solve == "width"       ? FUSE_SOLVE::WIDTH
                                        : solve == "thickness" ? FUSE_SOLVE::THICKNESS
                                        : solve == "time"      ? FUSE_SOLVE::TIME
                                                               : FUSE_SOLVE::CURRENT };
                  double            r = FusingCurrent( in );

                  if( !std::isfinite( r ) )
                      throw TOOL_ERROR( "Inputs are invalid: melting above ambient, and the other three positive." );

                  if( solve == "width" || solve == "thickness" )
                      r /= MM;

                  return { { solve, r }, { "units", solve == "current" ? "A" : solve == "time" ? "s" : "mm" } };
              } },

            { "wavelength", "Period, wavelength in vacuum and in a medium, and propagation speed for a frequency (Hz).",
              numberProps( { { "frequency", "Hz" },
                             { "permittivity", "relative (default 1)" },
                             { "permeability", "relative (default 1)" } } ),
              []( const JSON& aIn ) -> JSON
              {
                  WAVE_RESULT r = Wavelength( num( aIn, "frequency" ), num( aIn, "permittivity", 1 ),
                                              num( aIn, "permeability", 1 ) );
                  return { { "period", value( r.period ) },
                           { "wavelengthVacuum", value( r.wavelengthVacuum ) },
                           { "wavelengthMedium", value( r.wavelengthMedium ) },
                           { "speed", value( r.speed ) },
                           { "units", "s, m, m, m/s" } };
              } },

            { "cable_size",
              "A round conductor of a diameter (mm): area, linear resistance, frequency for a skin depth equal to "
              "the radius, ampacity at a current density, and DC resistance, voltage drop and power for a length "
              "and current.",
              numberProps( { { "diameter", "mm" },
                             { "resistivity", "ohm m at 20 C (default copper 1.72e-8)" },
                             { "thermalCoefficient", "1/C (default 3.93e-3)" },
                             { "temperature", "C (default 20)" },
                             { "currentDensity", "A/mm2 (default 3)" },
                             { "length", "m (default 1)" },
                             { "current", "A (default 1)" } } ),
              []( const JSON& aIn ) -> JSON
              {
                  CABLE_INPUT in;
                  in.diameter = num( aIn, "diameter" ) * MM;
                  in.resistivityAt20C = num( aIn, "resistivity", in.resistivityAt20C );
                  in.thermalCoefficient = num( aIn, "thermalCoefficient", in.thermalCoefficient );
                  in.temperatureC = num( aIn, "temperature", in.temperatureC );
                  in.ampsPerMm2 = num( aIn, "currentDensity", in.ampsPerMm2 );
                  in.length = num( aIn, "length", in.length );
                  in.current = num( aIn, "current", in.current );
                  CABLE_RESULT r = CableSize( in );
                  return { { "area", value( r.area * 1e6 ) },
                           { "linearResistance", value( r.linearResistance ) },
                           { "maxFrequency", value( r.maxFrequency ) },
                           { "ampacity", value( r.ampacity ) },
                           { "resistanceDc", value( r.resistanceDc ) },
                           { "voltageDrop", value( r.voltageDrop ) },
                           { "power", value( r.power ) },
                           { "units", "mm2, ohm/m, Hz, A, ohm, V, W" } };
              } },

            { "regulator",
              "Feedback resistors of a regulator: Vout = Vref (R1 + R2) / R2 for a standard regulator, or "
              "Vref (R1 + R2) / R1 + Iadj R2 for a 3-terminal adjustable one. Solve for r1, r2 or vout (kOhm, V, "
              "uA), with the spread from the resistor tolerance and the Vref range.",
              []()
              {
                  JSON p = numberProps( { { "r1", "kOhm" },
                                          { "r2", "kOhm" },
                                          { "vref", "V typical (vrefMin, vrefMax default to it)" },
                                          { "vrefMin", "V" },
                                          { "vrefMax", "V" },
                                          { "vout", "V" },
                                          { "iadj", "uA typical (3-terminal)" },
                                          { "iadjMax", "uA" },
                                          { "tolerance", "% resistor tolerance (default 1)" } } );
                  p["solve"] = { { "type", "string" }, { "enum", { "r1", "r2", "vout" } } };
                  p["threeTerminal"] = { { "type", "boolean" } };
                  return p;
              }(),
              []( const JSON& aIn ) -> JSON
              {
                  REG_INPUT         in;
                  const std::string solve = aIn.value( "solve", "vout" );
                  in.solve = solve == "r1" ? REG_SOLVE::R1 : solve == "r2" ? REG_SOLVE::R2 : REG_SOLVE::VOUT;
                  in.threeTerminal = aIn.value( "threeTerminal", false );
                  in.r1k = num( aIn, "r1", 0 );
                  in.r2k = num( aIn, "r2", 0 );
                  in.vrefTyp = num( aIn, "vref" );
                  in.vrefMin = num( aIn, "vrefMin", in.vrefTyp );
                  in.vrefMax = num( aIn, "vrefMax", in.vrefTyp );
                  in.vout = num( aIn, "vout", 0 );
                  in.iadjTypUa = num( aIn, "iadj", 0 );
                  in.iadjMaxUa = num( aIn, "iadjMax", in.iadjTypUa );
                  in.tolerancePercent = num( aIn, "tolerance", 1 );
                  REG_RESULT r = Regulator( in );

                  if( !r.error.empty() )
                      throw TOOL_ERROR( r.error );

                  return { { "r1", { r.r1Min, r.r1Typ, r.r1Max } },
                           { "r2", { r.r2Min, r.r2Typ, r.r2Max } },
                           { "vout", { r.voutMin, r.voutTyp, r.voutMax } },
                           { "tolerance", { r.tolMinPercent, r.tolMaxPercent } },
                           { "units", "[min, typ, max] kOhm, V; tolerance [min, max] %" } };
              } },

            { "board_class",
              "IPC board class minimums (mm), class 1 to 6 or all: track width, clearance, via diameter minus "
              "drill, plated and unplated pad diameter minus drill.",
              numberProps( { { "class", "1 to 6 (default all)" } } ),
              []( const JSON& aIn ) -> JSON
              {
                  JSON out = JSON::array();

                  for( int c = 1; c <= 6; ++c )
                  {
                      if( aIn.contains( "class" ) && (int) num( aIn, "class" ) != c )
                          continue;

                      BOARD_CLASS b = BoardClass( c );
                      auto        mm = []( double v )
                      {
                          return v < 0 ? JSON( nullptr ) : JSON( v / MM );
                      };
                      out.push_back( { { "class", c },
                                       { "trackWidth", mm( b.lines ) },
                                       { "clearance", mm( b.clearance ) },
                                       { "viaDiameterMinusDrill", mm( b.viaDiameterDiff ) },
                                       { "platedPadMinusDrill", mm( b.padDiameterDiffPlated ) },
                                       { "unplatedPadMinusDrill", mm( b.padDiameterDiffUnplated ) } } );
                  }

                  if( out.empty() )
                      throw TOOL_ERROR( "class is 1 to 6." );

                  return { { "classes", out }, { "units", "mm" } };
              } },

            { "color_code",
              "Resistor colour bands (IEC 60062) for a value in ohms: 4 bands (2 digits) or 5 (3 digits), with a "
              "tolerance band.",
              numberProps(
                      { { "value", "ohm" }, { "bands", "4 or 5 (default 4)" }, { "tolerance", "% (default 5)" } } ),
              []( const JSON& aIn ) -> JSON
              {
                  try
                  {
                      return { { "bands", ColorCode( num( aIn, "value" ), (int) num( aIn, "bands", 4 ),
                                                     num( aIn, "tolerance", 5 ) ) } };
                  }
                  catch( const std::invalid_argument& e )
                  {
                      throw TOOL_ERROR( std::string( "No colour code for this " ) + e.what()
                                        + " (0.01 ohm to 99 Gohm; tolerance 0.05 to 20 %)." );
                  }
              } },

            { "resistor_substitution",
              "Combinations of 2, 3 and 4 standard resistors (E1 to E192) closest to a target value, as the "
              "resistor calculator finds them; exclude values you do not have.",
              []()
              {
                  JSON p = numberProps( { { "target", "ohm" } } );
                  p["series"] = { { "type", "string" },
                                  { "enum", { "E1", "E3", "E6", "E12", "E24", "E48", "E96", "E192" } } };
                  p["exclude"] = { { "type", "array" }, { "items", { { "type", "number" } } } };
                  return p;
              }(),
              []( const JSON& aIn ) -> JSON
              {
                  static const std::map<std::string, int> series = {
                      { "E1", ESERIES::E1 },   { "E3", ESERIES::E3 },    { "E6", ESERIES::E6 },
                      { "E12", ESERIES::E12 }, { "E24", ESERIES::E24 },  { "E48", ESERIES::E48 },
                      { "E96", ESERIES::E96 }, { "E192", ESERIES::E192 }
                  };
                  auto s = series.find( aIn.value( "series", "E12" ) );

                  if( s == series.end() )
                      throw TOOL_ERROR( "series is E1 to E192." );

                  const double target = num( aIn, "target" );

                  if( target < RES_EQUIV_CALC_FIRST_VALUE || target > RES_EQUIV_CALC_LAST_VALUE )
                      throw TOOL_ERROR( "target is 10 ohm to 1 Mohm." );

                  RES_EQUIV_CALC calc;
                  calc.SetSeries( s->second );
                  calc.NewCalc( target );

                  for( const JSON& x : aIn.value( "exclude", JSON::array() ) )
                      calc.Exclude( x.get<double>() );

                  calc.Calculate();
                  JSON out = JSON::array();
                  int  count = 2;

                  for( const std::optional<RESISTANCE>& r : calc.GetResults() )
                  {
                      if( r )
                      {
                          out.push_back( { { "resistors", count },
                                           { "value", r->value },
                                           { "error", ( r->value - target ) / target * 100 },
                                           { "combination", r->name } } );
                      }

                      count++;
                  }

                  return { { "solutions", out }, { "units", "ohm, error %" } };
              } },

            { "eseries", "The values of an E series (E1 to E192) in one decade.",
              []()
              {
                  JSON p = numberProps( { { "decade", "power of ten (default 0: 1 to 9.x)" } } );
                  p["series"] = { { "type", "string" },
                                  { "enum", { "E1", "E3", "E6", "E12", "E24", "E48", "E96", "E192" } } };
                  return p;
              }(),
              []( const JSON& aIn ) -> JSON
              {
                  static const std::map<std::string, int> series = {
                      { "E1", ESERIES::E1 },   { "E3", ESERIES::E3 },    { "E6", ESERIES::E6 },
                      { "E12", ESERIES::E12 }, { "E24", ESERIES::E24 },  { "E48", ESERIES::E48 },
                      { "E96", ESERIES::E96 }, { "E192", ESERIES::E192 }
                  };
                  auto s = series.find( aIn.value( "series", "E12" ) );

                  if( s == series.end() )
                      throw TOOL_ERROR( "series is E1 to E192." );

                  ESERIES::ESERIES_IN_DECADE values( s->second, (int) num( aIn, "decade", 0 ) );
                  return { { "series", s->first }, { "values", std::vector<double>( values.begin(), values.end() ) } };
              } },

            { "transmission_line",
              "Transmission line analysis (z0, electrical length, losses, effective permittivity from the "
              "geometry) or synthesis (the width, or the gap for coupled lines, for a z0) with KiCad's "
              "transmission line models. Lengths in mm, frequency Hz, rho ohm m, electricalLength radians.",
              { { "type", { { "type", "string" } } },
                { "mode", { { "type", "string" }, { "enum", { "analyse", "synthesize" } } } },
                { "parameters",
                  { { "type", "object" },
                    { "description", "epsilonR, h, t, width, gap, frequency, z0, length, ..." } } } },
              runTransline },

            { "attenuator",
              "Resistor values of a pi, tee, bridged tee or resistive splitter attenuator for an attenuation (dB) "
              "and impedances (ohm).",
              []()
              {
                  JSON p = numberProps(
                          { { "attenuation", "dB" }, { "zin", "ohm (default 50)" }, { "zout", "ohm (default zin)" } } );
                  p["topology"] = { { "type", "string" }, { "enum", { "pi", "tee", "bridged_tee", "splitter" } } };
                  return p;
              }(),
              []( const JSON& aIn ) -> JSON
              {
                  const std::string           topology = aIn.value( "topology", "pi" );
                  std::unique_ptr<ATTENUATOR> att;

                  if( topology == "pi" )
                      att = std::make_unique<ATTENUATOR_PI>();
                  else if( topology == "tee" )
                      att = std::make_unique<ATTENUATOR_TEE>();
                  else if( topology == "bridged_tee" )
                      att = std::make_unique<ATTENUATOR_BRIDGE>();
                  else if( topology == "splitter" )
                      att = std::make_unique<ATTENUATOR_SPLITTER>();
                  else
                      throw TOOL_ERROR( "topology is pi, tee, bridged_tee or splitter." );

                  att->m_Attenuation = num( aIn, "attenuation", att->m_Attenuation );
                  att->m_Zin = num( aIn, "zin", 50 );
                  att->m_Zout = num( aIn, "zout", att->m_Zin );

                  if( !att->Calculate() )
                      throw TOOL_ERROR( "No attenuator for these values (the minimum attenuation is "
                                        + std::to_string( att->m_MinimumATT ) + " dB)." );

                  JSON out{ { "r1", value( att->m_R1 ) }, { "r2", value( att->m_R2 ) }, { "units", "ohm" } };

                  if( att->m_ResultCount > 2 )
                      out["r3"] = value( att->m_R3 );

                  return out;
              } },
        };

        return list;
    }


    const CALCULATOR& calculator( const std::string& aName )
    {
        for( const CALCULATOR& c : calculators() )
        {
            if( c.name == aName )
                return c;
        }

        std::string known;

        for( const CALCULATOR& c : calculators() )
            known += ( known.empty() ? "" : ", " ) + c.name;

        throw TOOL_ERROR( "No calculator '" + aName + "'. Known: " + known + "." );
    }

} // namespace


std::deque<JSON>& CalcHistory( AGENT_PROJECT& aProject )
{
    static std::map<std::string, std::deque<JSON>> histories;
    return histories[aProject.Root().string()];
}


JSON RunCalculator( const std::string& aName, const JSON& aInputs )
{
    return calculator( aName ).run( aInputs );
}


void RegisterCalculatorTools( AGENT_SERVICES& aServices )
{
    TOOL_REGISTRY& tools = aServices.Tools();

    tools.Add( { "calc_list", "The PCB calculator's calculators: name, what it computes, its inputs and units.", "calc",
                 JSON{ { "type", "object" }, { "properties", JSON::object() } }, []( const JSON&, TOOL_CONTEXT& )
                 {
                     JSON out = JSON::array();

                     for( const CALCULATOR& c : calculators() )
                         out.push_back(
                                 { { "name", c.name }, { "description", c.description }, { "inputs", c.inputs } } );

                     return TOOL_RESULT::Data( { { "calculators", out } } );
                 } } );

    tools.Add(
            { "calc_run",
              "Run one calculator (calc_list lists them) with named inputs. Lengths are mm unless the calculator "
              "says otherwise; the result names its units.",
              "calc",
              ProjectToolSchema( { { "calculator", { { "type", "string" } } }, { "inputs", { { "type", "object" } } } },
                                 { "calculator" } ),
              [&aServices]( const JSON& aArgs, TOOL_CONTEXT& )
              {
                  const std::string name = aArgs["calculator"];
                  const JSON        inputs = aArgs.value( "inputs", JSON::object() );
                  JSON              out = RunCalculator( name, inputs );

                  // The history is per project when one is open (the viewer belongs to it).
                  if( AGENT_PROJECT* project = aServices.Sessions().Current() )
                  {
                      std::deque<JSON>& history = CalcHistory( *project );
                      history.push_front( { { "calculator", name }, { "inputs", inputs }, { "outputs", out } } );

                      while( history.size() > 200 )
                          history.pop_back();

                      aServices.Observers().Notify(
                              [&]( SESSION_OBSERVER& aObserver )
                              {
                                  aObserver.OnFilesChanged( *project );
                              } );
                  }

                  return TOOL_RESULT::Data( { { "calculator", name }, { "outputs", out } } );
              } } );

    if( std::getenv( "AGENTIC_TEST_HOOKS" ) && aServices.ViewerMode() )
    {
        tools.Add( { "debug_calc_panel",
                     "Test hook: the upstream calculator panel's outputs for the same inputs (viewer mode).",
                     "debug",
                     { { "type", "object" },
                       { "properties",
                         { { "calculator", { { "type", "string" } } }, { "inputs", { { "type", "object" } } } } },
                       { "required", { "calculator" } } },
                     []( const JSON& aArgs, TOOL_CONTEXT& )
                     {
                         return TOOL_RESULT::Data(
                                 RunCalculatorPanel( aArgs["calculator"], aArgs.value( "inputs", JSON::object() ) ) );
                     } } );
    }
}

} // namespace AGENTIC
