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

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>


namespace AGENTIC::CALC
{

namespace
{
    constexpr double MM = 1e-3;
    constexpr double MIL = 25.4e-6;
    constexpr double ABS_ZERO = -273.15;
    constexpr double SPEED_LIGHT = 299792458;
    constexpr double VACUUM_PERMEABILITY = 1.256637e-6;


    double roundTo( double aValue, double aPrecision = 0.001 )
    {
        return std::round( aValue / aPrecision ) * aPrecision;
    }
} // namespace


VIA_RESULT ViaSize( const VIA_INPUT& aIn )
{
    VIA_RESULT out;
    double     area = M_PI * ( aIn.holeDiameter + aIn.platingThickness ) * aIn.platingThickness; // m^2

    out.resistance = aIn.resistivity * aIn.length / area;
    out.thermalResistance = 2.49e-3 * aIn.length / area; // copper's thermal resistivity, m K/W
    out.voltageDrop = aIn.current * out.resistance;
    out.powerLoss = aIn.current * out.voltageDrop;

    // IPC-2152 fit for the ampacity, area in mil^2.
    area /= MIL * MIL;
    out.ampacity = 0.048 * std::pow( aIn.temperatureRise, 0.44 ) * std::pow( area, 0.725 );

    out.capacitancePf =
            55.51 * aIn.permittivity * aIn.length * aIn.padDiameter / ( aIn.clearanceDiameter - aIn.padDiameter );
    out.riseTimeDegradationPs = 2.2 * out.capacitancePf * aIn.impedance / 2;
    out.inductanceNh = 200 * aIn.length * ( std::log( 4 * aIn.length / aIn.holeDiameter ) + 1 );
    out.reactance = M_PI * out.inductanceNh / aIn.riseTimeNs;
    return out;
}


std::array<double, 8> Ipc2221Row( int aRow )
{
    // IPC-2221 table 6-1, rows 0-15, 16-30, 31-50, 51-100, 101-150, 151-170, 171-250, 251-300,
    // 301-500 V, then the increment per volt above 500 V.
    static const double table[10][8] = {
        { 0.05, 0.1, 0.1, 0.075, 0.075, 0.13, 0.13, 0.13 },
        { 0.05, 0.1, 0.1, 0.075, 0.075, 0.13, 0.25, 0.25 },
        { 0.1, 0.64, 0.64, 0.3, 0.13, 0.13, 0.4, 0.8 },
        { 0.1, 0.64, 1.5, 0.3, 0.13, 0.13, 0.5, 1 },
        { 0.2, 0.64, 3.2, 0.8, 0.4, 0.4, 0.8, 1.6 },
        { 0.2, 1.25, 3.2, 0.8, 0.4, 0.4, 0.8, 1.6 },
        { 0.2, 1.25, 6.4, 0.8, 0.4, 0.4, 0.8, 1.6 },
        { 0.2, 1.25, 12.5, 0.8, 0.4, 0.4, 0.8, 1.6 },
        { 0.25, 2.5, 12.5, 1.6, 0.8, 0.8, 1.5, 3 },
        { 0.0025, 0.005, 0.025, 0.00305, 0.00305, 0.00305, 0.00305, 0.0061 },
    };

    if( aRow < 0 || aRow > 9 )
        throw std::out_of_range( "IPC-2221 row" );

    std::array<double, 8> out;

    for( int i = 0; i < 8; ++i )
        out[i] = table[aRow][i] * MM;

    return out;
}


std::array<double, 8> Ipc2221Spacing( double aVoltage )
{
    static const double limits[9] = { 15, 30, 50, 100, 150, 170, 250, 300, 500 };

    for( int row = 0; row < 9; ++row )
    {
        if( aVoltage <= limits[row] )
            return Ipc2221Row( row );
    }

    // Above 500 V: the 301-500 V row plus the increment per volt (the panel's last row).
    std::array<double, 8> base = Ipc2221Row( 8 );
    std::array<double, 8> extra = Ipc2221Row( 9 );

    for( int i = 0; i < 8; ++i )
        base[i] += extra[i] * ( aVoltage - 500.0 );

    return base;
}


double FusingCurrent( const FUSE_INPUT& aIn )
{
    bool valid = std::isfinite( aIn.meltingC ) && std::isfinite( aIn.ambientC ) && aIn.meltingC > aIn.ambientC
                 && aIn.ambientC > ABS_ZERO;

    // Onderdonk-type adiabatic model for copper, as the panel computes it.
    const double latentHeat = 205350.0;
    const double cp = 385;
    const double density = 8940;
    const double volumicEnergy = density * ( ( aIn.meltingC - aIn.ambientC ) * cp + latentHeat );
    const double Ra = ( ( aIn.ambientC - ABS_ZERO - 293 ) * 0.00393 + 1 ) * 1.72e-8;
    const double Rm = ( ( aIn.meltingC - ABS_ZERO - 293 ) * 0.00393 + 1 ) * 1.72e-8;
    const double coeff = volumicEnergy / ( ( Rm + Ra ) / 2 );
    const double nan = std::numeric_limits<double>::quiet_NaN();

    switch( aIn.solve )
    {
    case FUSE_SOLVE::WIDTH:
        if( !valid || !( aIn.current > 0 && aIn.thickness > 0 && aIn.time > 0 ) )
            return nan;

        return aIn.current / std::sqrt( coeff / aIn.time ) / aIn.thickness;

    case FUSE_SOLVE::THICKNESS:
        if( !valid || !( aIn.current > 0 && aIn.width > 0 && aIn.time > 0 ) )
            return nan;

        return aIn.current / std::sqrt( coeff / aIn.time ) / aIn.width;

    case FUSE_SOLVE::CURRENT:
        if( !valid || !( aIn.width > 0 && aIn.thickness > 0 && aIn.time > 0 ) )
            return nan;

        return aIn.width * aIn.thickness * std::sqrt( coeff / aIn.time );

    case FUSE_SOLVE::TIME:
    {
        if( !valid || !( aIn.current > 0 && aIn.width > 0 && aIn.thickness > 0 ) )
            return nan;

        const double a = aIn.width * aIn.thickness;
        return coeff * a * a / aIn.current / aIn.current;
    }
    }

    return nan;
}


WAVE_RESULT Wavelength( double aFrequency, double aPermittivity, double aPermeability )
{
    WAVE_RESULT out;
    out.period = 1 / aFrequency;
    out.wavelengthVacuum = SPEED_LIGHT / aFrequency;
    out.wavelengthMedium = SPEED_LIGHT / aFrequency / std::sqrt( aPermittivity * aPermeability );
    out.speed = SPEED_LIGHT / std::sqrt( aPermittivity * aPermeability );
    return out;
}


CABLE_RESULT CableSize( const CABLE_INPUT& aIn )
{
    CABLE_RESULT out;
    const double radius = aIn.diameter / 2;
    const double resistivity = aIn.resistivityAt20C * ( 1 + aIn.thermalCoefficient * ( aIn.temperatureC - 20 ) );

    out.area = M_PI * radius * radius;
    out.linearResistance = resistivity / out.area;
    out.maxFrequency = resistivity / ( M_PI * radius * radius * VACUUM_PERMEABILITY );
    out.ampacity = out.area / ( 1 / aIn.ampsPerMm2 / 1e6 );
    out.resistanceDc = out.linearResistance * aIn.length;
    out.voltageDrop = out.resistanceDc * aIn.current;
    out.power = out.voltageDrop * aIn.current;
    return out;
}


REG_RESULT Regulator( const REG_INPUT& aIn )
{
    REG_RESULT   out{};
    const double scale = 1000; // the panel's kOhm
    const double restol = aIn.tolerancePercent / 100;
    double       r1typ = aIn.r1k * scale;
    double       r2typ = aIn.r2k * scale;
    double       vouttyp = aIn.vout;
    double       r1min, r1max, r2min, r2max, voutmin, voutmax;

    if( ( vouttyp < aIn.vrefMin || vouttyp < aIn.vrefTyp || vouttyp < aIn.vrefMax ) && aIn.solve != REG_SOLVE::VOUT )
    {
        out.error = "Vout must be greater than Vref";
        return out;
    }

    if( aIn.vrefMin == 0.0 || aIn.vrefTyp == 0.0 || aIn.vrefMax == 0.0 )
    {
        out.error = "Vref set to 0 !";
        return out;
    }

    if( aIn.vrefMin > aIn.vrefTyp || aIn.vrefTyp > aIn.vrefMax )
    {
        out.error = "Vref must VrefMin < VrefTyp < VrefMax";
        return out;
    }

    if( ( r1typ < 0 && aIn.solve != REG_SOLVE::R1 ) || ( r2typ <= 0 && aIn.solve != REG_SOLVE::R2 ) )
    {
        out.error = "Incorrect value for R1 R2";
        return out;
    }

    if( aIn.threeTerminal )
    {
        if( aIn.iadjTypUa > aIn.iadjMaxUa )
        {
            out.error = "Iadj must IadjTyp < IadjMax";
            return out;
        }

        const double iadjtyp = aIn.iadjTypUa / 1000000;
        const double iadjmax = aIn.iadjMaxUa / 1000000;

        switch( aIn.solve )
        {
        case REG_SOLVE::R1: r1typ = aIn.vrefTyp * r2typ / ( vouttyp - aIn.vrefTyp - ( r2typ * iadjtyp ) ); break;
        case REG_SOLVE::R2: r2typ = ( vouttyp - aIn.vrefTyp ) / ( iadjtyp + ( aIn.vrefTyp / r1typ ) ); break;
        case REG_SOLVE::VOUT: vouttyp = aIn.vrefTyp * ( r1typ + r2typ ) / r1typ + r2typ * iadjtyp; break;
        }

        r1min = r1typ - r1typ * restol;
        r1max = r1typ + r1typ * restol;
        r2min = r2typ - r2typ * restol;
        r2max = r2typ + r2typ * restol;
        voutmin = aIn.vrefMin * ( r1max + r2min ) / r1max + r2min * iadjtyp;
        voutmax = aIn.vrefMax * ( r1min + r2max ) / r1min + r2max * iadjmax;
    }
    else
    {
        switch( aIn.solve )
        {
        case REG_SOLVE::R1: r1typ = ( vouttyp / aIn.vrefTyp - 1 ) * r2typ; break;
        case REG_SOLVE::R2: r2typ = r1typ / ( vouttyp / aIn.vrefTyp - 1 ); break;
        case REG_SOLVE::VOUT: vouttyp = aIn.vrefTyp * ( r1typ + r2typ ) / r2typ; break;
        }

        r1min = r1typ - r1typ * restol;
        r1max = r1typ + r1typ * restol;
        r2min = r2typ - r2typ * restol;
        r2max = r2typ + r2typ * restol;
        voutmin = aIn.vrefMin * ( r1min + r2max ) / r2max;
        voutmax = aIn.vrefMax * ( r1max + r2min ) / r2min;
    }

    // Rounded as the panel shows them.
    out.r1Min = roundTo( r1min / scale );
    out.r1Typ = roundTo( r1typ / scale );
    out.r1Max = roundTo( r1max / scale );
    out.r2Min = roundTo( r2min / scale );
    out.r2Typ = roundTo( r2typ / scale );
    out.r2Max = roundTo( r2max / scale );
    out.voutMin = roundTo( voutmin );
    out.voutTyp = roundTo( vouttyp );
    out.voutMax = roundTo( voutmax );
    out.tolMinPercent = roundTo( ( voutmin - vouttyp ) / vouttyp * 100.0, 0.01 );
    out.tolMaxPercent = roundTo( ( voutmax - vouttyp ) / voutmax * 100.0, 0.01 );
    return out;
}


BOARD_CLASS BoardClass( int aClass )
{
    static const BOARD_CLASS classes[6] = {
        { 0.80 * MM, 0.68 * MM, -1.0, 1.19 * MM, 1.57 * MM },     { 0.50 * MM, 0.50 * MM, -1.0, 0.78 * MM, 1.13 * MM },
        { 0.31 * MM, 0.31 * MM, 0.45 * MM, 0.6 * MM, 0.90 * MM }, { 0.21 * MM, 0.21 * MM, 0.34 * MM, 0.49 * MM, -1.0 },
        { 0.15 * MM, 0.15 * MM, 0.24 * MM, 0.39 * MM, -1.0 },     { 0.12 * MM, 0.12 * MM, 0.20 * MM, 0.35 * MM, -1.0 }
    };

    if( aClass < 1 || aClass > 6 )
        throw std::out_of_range( "board class" );

    return classes[aClass - 1];
}


std::string ColorCode( double aOhms, int aBands, double aTolerancePercent )
{
    static const char*                         digits[] = { "black", "brown", "red",    "orange", "yellow",
                                                            "green", "blue",  "violet", "grey",   "white" };
    static const std::map<double, const char*> tolerances = { { 1, "brown" },   { 2, "red" },      { 0.5, "green" },
                                                              { 0.25, "blue" }, { 0.1, "violet" }, { 0.05, "grey" },
                                                              { 5, "gold" },    { 10, "silver" },  { 20, "none" } };

    if( !( aOhms >= 0.01 ) || !std::isfinite( aOhms ) )
        throw std::invalid_argument( "value" );

    const int significant = aBands == 5 ? 3 : 2;
    int       exponent = static_cast<int>( std::floor( std::log10( aOhms ) ) ) - ( significant - 1 );
    long      mantissa = std::lround( aOhms / std::pow( 10.0, exponent ) );

    if( mantissa >= std::lround( std::pow( 10.0, significant ) ) )
    {
        mantissa /= 10;
        exponent++;
    }

    if( exponent < -2 || exponent > 9 )
        throw std::invalid_argument( "value" );

    auto tol = tolerances.find( aTolerancePercent );

    if( tol == tolerances.end() )
        throw std::invalid_argument( "tolerance" );

    std::vector<std::string> bands;
    const std::string        text = std::to_string( mantissa );

    for( char c : text )
        bands.push_back( digits[c - '0'] );

    bands.push_back( exponent == -1 ? "gold" : exponent == -2 ? "silver" : digits[exponent] );
    bands.push_back( tol->second );

    std::string out;

    for( const std::string& b : bands )
        out += ( out.empty() ? "" : " " ) + b;

    return out;
}

} // namespace AGENTIC::CALC
