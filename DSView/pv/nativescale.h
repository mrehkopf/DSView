/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef DSVIEW_PV_NATIVESCALE_H
#define DSVIEW_PV_NATIVESCALE_H

#include <QtGlobal>
#include <QtMath>

/*
 * Conversions between Qt's logical coordinates and native pixels, for the
 * Windows window code. The device pixel ratio k can be fractional: 1.5 at
 * 150% scaling.
 *
 * For an integer k the results are those of plain multiplication and C++
 * integer division. For any k >= 1, converting a native value to logical
 * and back gives a value that converts to the same logical value again.
 *
 * Convert a region by its edges (left and right, top and bottom), not by
 * its position and size. With a fractional k, rounding the size on its own
 * lets a window that is saved and restored grow by a pixel each time.
 */
namespace pv
{
    // Absorbs floating-point error when the exact result is a whole number.
    static const qreal NativeScaleEpsilon = 1e-6;

    // Native to logical, rounding toward zero like integer division.
    inline int FromNativePixels(int v, qreal k)
    {
        const qreal r = v / k;
        return int(v >= 0 ? r + NativeScaleEpsilon : r - NativeScaleEpsilon);
    }

    // Logical to native, rounding away from zero, the counterpart of
    // FromNativePixels() for coordinates that are converted back later.
    inline int ToNativePixels(int v, qreal k)
    {
        const qreal r = v * k;
        return v >= 0 ? qCeil(r - NativeScaleEpsilon) : qFloor(r + NativeScaleEpsilon);
    }

    // A fixed length in logical pixels, such as a border width, in native pixels.
    inline int ScaledToNative(int v, qreal k)
    {
        return qRound(v * k);
    }
}

#endif // DSVIEW_PV_NATIVESCALE_H
