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


#ifndef AGENTIC_CALC_FORMULAS_H
#define AGENTIC_CALC_FORMULAS_H

#include <array>
#include <string>

/**
 * The PCB calculator's formulas, out of its panels (D9).  Inputs and outputs are in SI units
 * (metres, amperes, volts, ohms, kelvin differences, seconds) unless a name says otherwise.
 * Each function is pinned to its upstream panel by qa_agentic_e2e (test_stage13_calculator).
 */
namespace AGENTIC::CALC
{

struct VIA_INPUT
{
    double holeDiameter, platingThickness, length, padDiameter, clearanceDiameter;
    double impedance;   ///< ohm
    double current;     ///< A
    double resistivity; ///< ohm m
    double permittivity;
    double temperatureRise; ///< K
    double riseTimeNs;      ///< ns
};

struct VIA_RESULT
{
    double resistance, voltageDrop, powerLoss, ampacity, thermalResistance;
    double capacitancePf, riseTimeDegradationPs, inductanceNh, reactance;
};

VIA_RESULT ViaSize( const VIA_INPUT& aIn );


/// IPC-2221 minimum spacings (m) for a voltage: B1..B4, A5..A7 and B4 (coated), as the panel's columns.
std::array<double, 8> Ipc2221Spacing( double aVoltage );

/// The panel's table rows: the voltage ranges up to 500 V, then the row for aVoltage above 500 V.
std::array<double, 8> Ipc2221Row( int aRow );


enum class FUSE_SOLVE
{
    WIDTH,
    THICKNESS,
    CURRENT,
    TIME
};

struct FUSE_INPUT
{
    double     meltingC, ambientC, current, width, thickness, time;
    FUSE_SOLVE solve;
};

/// The solved quantity, or NaN when the inputs are invalid.
double FusingCurrent( const FUSE_INPUT& aIn );


struct WAVE_RESULT
{
    double period, wavelengthVacuum, wavelengthMedium, speed;
};

WAVE_RESULT Wavelength( double aFrequency, double aPermittivity, double aPermeability );


struct CABLE_INPUT
{
    double diameter;
    double resistivityAt20C = 1.72e-8;
    double thermalCoefficient = 3.93e-3;
    double temperatureC = 20;
    double ampsPerMm2 = 3.0;
    double length = 1;
    double current = 1;
};

struct CABLE_RESULT
{
    double area, linearResistance, maxFrequency, ampacity, resistanceDc, voltageDrop, power;
};

CABLE_RESULT CableSize( const CABLE_INPUT& aIn );


enum class REG_SOLVE
{
    R1,
    R2,
    VOUT
};

struct REG_INPUT
{
    bool      threeTerminal = false; ///< adjustable 3-terminal (LM317 type), with Iadj
    REG_SOLVE solve = REG_SOLVE::VOUT;
    double    r1k = 0, r2k = 0; ///< kOhm
    double    vrefMin = 0, vrefTyp = 0, vrefMax = 0, vout = 0;
    double    iadjTypUa = 0, iadjMaxUa = 0; ///< uA
    double    tolerancePercent = 1;
};

struct REG_RESULT
{
    std::string error; ///< the panel's message when the inputs are refused
    double      r1Min, r1Typ, r1Max, r2Min, r2Typ, r2Max;
    double      voutMin, voutTyp, voutMax, tolMinPercent, tolMaxPercent;
};

REG_RESULT Regulator( const REG_INPUT& aIn );


struct BOARD_CLASS
{
    double lines, clearance, viaDiameterDiff, padDiameterDiffPlated, padDiameterDiffUnplated; ///< m; -1 none
};

/// IPC board classes 1 to 6.
BOARD_CLASS BoardClass( int aClass );


/// Resistor colour bands (IEC 60062) for a value: 3 significant digits when \a aBands is 5.
std::string ColorCode( double aOhms, int aBands, double aTolerancePercent );

} // namespace AGENTIC::CALC

#endif // AGENTIC_CALC_FORMULAS_H
