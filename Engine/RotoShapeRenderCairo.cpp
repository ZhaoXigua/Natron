/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2013-2017 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RotoShapeRenderCairo.h"

#ifdef ROTO_SHAPE_RENDER_ENABLE_CAIRO

#include <QLineF>

#include <cairo/cairo.h>

#include "Engine/BezierCP.h"
#include "Engine/Color.h"
#include "Engine/Image.h"
#include "Engine/Node.h"
#include "Engine/EffectInstance.h"
#include "Engine/RamBuffer.h"
#include "Engine/RotoShapeRenderNodePrivate.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/RotoShapeRenderNode.h"
#include "Engine/KnobTypes.h"



//This will enable correct evaluation of beziers
//#define ROTO_USE_MESH_PATTERN_ONLY


NATRON_NAMESPACE_ENTER;

QString
RotoShapeRenderCairo::getCairoVersion()
{
    return QString::fromUtf8(CAIRO_VERSION_STRING) + QString::fromUtf8(" / ") + QString::fromUtf8( cairo_version_string() );
}

RotoShapeRenderCairo::CairoImageWrapper::~CairoImageWrapper()
{
    if (ctx) {
        cairo_destroy(ctx);
    }
    ////Free the buffer used by Cairo
    if (cairoImg) {
        cairo_surface_destroy(cairoImg);
    }
}


static void
adjustToPointToScale(const RenderScale &scale,
                     double &x,
                     double &y)
{
    x *= scale.x;
    y *= scale.y;
}


template <int dstNComps, int srcNComps, bool useOpacity, bool inverted, bool accumulate>
static void
convertCairoImageToNatronImageForAccum_noColor(cairo_surface_t* cairoImg,
                                               const Image::CPUData& dstImageData,
                                               const RectI & srcBounds,
                                               const ColorRgbaD& shapeColor,
                                               double opacity,
                                               int nDivisions)
{
    unsigned char* cdata = cairo_image_surface_get_data(cairoImg);
    unsigned char* srcPix = cdata;
    int stride = cairo_image_surface_get_stride(cairoImg);

    assert(dstImageData.bounds.contains(srcBounds));

    double color[3];
    color[0] = useOpacity ? shapeColor.r * opacity : shapeColor.r;
    color[1] = useOpacity ? shapeColor.g * opacity : shapeColor.g;
    color[2] = useOpacity ? shapeColor.b * opacity : shapeColor.b;

    float *dst_pixels[4];
    int dstPixelStride;
    Image::getChannelPointers<float, dstNComps>((const float**)dstImageData.ptrs, srcBounds.x1, srcBounds.y1, dstImageData.bounds, (float**)dst_pixels, &dstPixelStride);

    float tmpPix[4] = {0.f, 0.f, 0.f, 1.f};

    for ( int y = srcBounds.y1; y < srcBounds.y2; ++y) {

        for (int x = srcBounds.x1; x < srcBounds.x2; ++x) {

            float cairoPixel = Image::convertPixelDepth<unsigned char, float>(*srcPix);

            // This is going to be optimized out by the compiler since this is a template parameter
            if (inverted) {
                cairoPixel = 1.f - cairoPixel;
            }
            if (dstNComps > 1) {
                const int nColorComps = std::min(dstNComps, 3);
                for (int c = 0; c < nColorComps; ++c) {
                    tmpPix[c] = cairoPixel * color[c];
                }
            }
            tmpPix[3] = useOpacity ? cairoPixel * opacity : cairoPixel;

            if (accumulate) {
                for (int c = 0; c < dstNComps; ++c) {
                    *dst_pixels[c] += tmpPix[c];
                }
                if (nDivisions > 0) {
                    for (int c = 0; c < dstNComps; ++c) {
                        *dst_pixels[c] /= nDivisions;
                    }
                }
            } else {
                for (int c = 0; c < dstNComps; ++c) {
                    *dst_pixels[c] = tmpPix[c];
                }
            }
            for (int c = 0; c < dstNComps; ++c) {
                assert(*dst_pixels[c] == *dst_pixels[c]); // check for NaN
                dst_pixels[c] += dstPixelStride;
            }
            srcPix += srcNComps;
        } // for each pixels along the line

        // Substract what was done on previous iteration and got to the next line
        for (int c = 0; c < dstNComps; ++c) {
            dst_pixels[c] += (dstImageData.bounds.width() - srcBounds.width()) * dstPixelStride;
        }
        srcPix += (stride - srcBounds.width() * srcNComps);
    } // for each scan-line

} // convertCairoImageToNatronImageForAccum_noColor



template <int dstNComps, int srcNComps, bool useOpacity, bool inverted>
static void
convertCairoImageToNatronImageForInverted_noColor(cairo_surface_t* cairoImg,
                                                  const Image::CPUData& image,
                                                  const RectI & pixelRod,
                                                  const ColorRgbaD& shapeColor,
                                                  double opacity,
                                                  bool accumulate,
                                                  int nDivisions)
{
    if (accumulate) {
         convertCairoImageToNatronImageForAccum_noColor<dstNComps, srcNComps, useOpacity, inverted, true>(cairoImg, image, pixelRod, shapeColor, opacity, nDivisions);
    } else {
        convertCairoImageToNatronImageForAccum_noColor<dstNComps, srcNComps, useOpacity, inverted, false>(cairoImg, image, pixelRod, shapeColor, opacity, nDivisions);
    }
} // convertCairoImageToNatronImageForInverted_noColor

template <int dstNComps, int srcNComps, bool useOpacity>
static void
convertCairoImageToNatronImageForDstComponents_noColor(cairo_surface_t* cairoImg,
                                                       const Image::CPUData& image,
                                                       const RectI & pixelRod,
                                                       const ColorRgbaD& shapeColor,
                                                       bool inverted,
                                                       double opacity,
                                                       bool accumulate,
                                                       int nDivisions)
{
    if (inverted) {
        convertCairoImageToNatronImageForInverted_noColor<dstNComps, srcNComps, useOpacity, true>(cairoImg, image, pixelRod, shapeColor, opacity, accumulate, nDivisions);
    } else {
        convertCairoImageToNatronImageForInverted_noColor<dstNComps, srcNComps, useOpacity, false>(cairoImg, image, pixelRod, shapeColor, opacity, accumulate, nDivisions);
    }
}

template <int dstNComps, int srcNComps>
static void
convertCairoImageToNatronImageForOpacity(cairo_surface_t* cairoImg,
                                         const Image::CPUData& image,
                                         const RectI & pixelRod,
                                         const ColorRgbaD& shapeColor,
                                         double opacity,
                                         bool inverted,
                                         bool useOpacity,
                                         bool accumulate,
                                         int nDivisions)
{
    if (useOpacity) {
        convertCairoImageToNatronImageForDstComponents_noColor<dstNComps, srcNComps, true>(cairoImg, image, pixelRod, shapeColor, inverted, opacity, accumulate, nDivisions);
    } else {
        convertCairoImageToNatronImageForDstComponents_noColor<dstNComps, srcNComps, false>(cairoImg, image, pixelRod, shapeColor, inverted, opacity, accumulate, nDivisions);
    }
}

template <int dstNComps>
static void
convertCairoImageToNatronImageForSrcComponents_noColor(cairo_surface_t* cairoImg,
                                                       int srcNComps,
                                                       const Image::CPUData& image,
                                                       const RectI & pixelRod,
                                                       const ColorRgbaD& shapeColor,
                                                       double opacity,
                                                       bool inverted,
                                                       bool useOpacity,
                                                       bool accumulate,
                                                       int nDivisions)
{
    if (srcNComps == 1) {
        convertCairoImageToNatronImageForOpacity<dstNComps, 1>(cairoImg, image, pixelRod, shapeColor, opacity, inverted, useOpacity, accumulate, nDivisions);
    } else if (srcNComps == 4) {
        convertCairoImageToNatronImageForOpacity<dstNComps, 4>(cairoImg, image, pixelRod, shapeColor, opacity, inverted, useOpacity, accumulate, nDivisions);
    } else {
        assert(false);
    }
}

static void
convertCairoImageToNatronImage_noColor(cairo_surface_t* cairoImg,
                                       int srcNComps,
                                       const Image::CPUData& image,
                                       const RectI & pixelRod,
                                       const ColorRgbaD& shapeColor,
                                       double opacity,
                                       bool inverted,
                                       bool useOpacity,
                                       bool accumulate,
                                       int nDivisions)
{
    switch (image.nComps) {
        case 1:
            convertCairoImageToNatronImageForSrcComponents_noColor<1>(cairoImg, srcNComps, image, pixelRod, shapeColor, opacity, inverted, useOpacity, accumulate, nDivisions);
            break;
        case 2:
            convertCairoImageToNatronImageForSrcComponents_noColor<2>(cairoImg, srcNComps, image, pixelRod, shapeColor, opacity, inverted, useOpacity, accumulate, nDivisions);
            break;
        case 3:
            convertCairoImageToNatronImageForSrcComponents_noColor<3>(cairoImg, srcNComps, image, pixelRod, shapeColor, opacity, inverted, useOpacity, accumulate, nDivisions);
            break;
        case 4:
            convertCairoImageToNatronImageForSrcComponents_noColor<4>(cairoImg, srcNComps, image, pixelRod, shapeColor, opacity, inverted, useOpacity, accumulate, nDivisions);
            break;
        default:
            break;
    }
}


template <typename PIX, int maxValue, int srcNComps, int dstNComps>
static void
convertNatronImageToCairoImageForComponents(unsigned char* cairoImg,
                                            std::size_t stride,
                                            const Image::CPUData& image,
                                            const RectI& roi,
                                            const RectI& dstBounds,
                                            const ColorRgbaD& shapeColor)
{
    assert(srcNComps == 1 || srcNComps == 4);
    assert(dstNComps == 1 || dstNComps == 4);

    unsigned char* dstPix = cairoImg;

    dstPix += ( (roi.y1 - dstBounds.y1) * stride + (roi.x1 - dstBounds.x1) );

    PIX *src_pixels[4];
    int srcPixelStride;
    Image::getChannelPointers<PIX, dstNComps>((const PIX**)image.ptrs, roi.x1, roi.y1, image.bounds, (PIX**)src_pixels, &srcPixelStride);


    for (int y = roi.y1; y < roi.y2; ++y) {

        for (int x = roi.x1; x < roi.x2; ++x) {

            if (dstNComps == 1) {
                *dstPix = Image::convertPixelDepth<PIX, unsigned char>(*src_pixels[0]);
            } else if (dstNComps == 4) {
                if (srcNComps == 4) {
                    dstPix[0] = shapeColor.b == 0 ? 0 : Image::convertPixelDepth<float, unsigned char>(Image::convertPixelDepth<PIX, float>(*src_pixels[2]) / shapeColor.b);
                    dstPix[1] = shapeColor.g == 0 ? 0 : Image::convertPixelDepth<float, unsigned char>(Image::convertPixelDepth<PIX, float>(*src_pixels[1]) / shapeColor.g);
                    dstPix[2] = shapeColor.r == 0 ? 0 : Image::convertPixelDepth<float, unsigned char>(Image::convertPixelDepth<PIX, float>(*src_pixels[0]) / shapeColor.r);
                    dstPix[3] = 255; //(float)srcPix[x * srcNComps + 3] / maxValue * 255.f;

                } else {
                    float pix = Image::convertPixelDepth<PIX, unsigned char>(*src_pixels[0]);
                    dstPix[0] = pix;
                    dstPix[1] = pix;
                    dstPix[2] = pix;
                    dstPix[3] = pix;
                }
            }

            dstPix += dstNComps;

            for (int c = 0; c < srcNComps; ++c) {
                src_pixels[c] += srcPixelStride;
            }
            // no need to check for NaN, dstPix is unsigned char
        } // for each pixels

        // Remove what was done on previous iteration and go to the next line
        for (int c = 0; c < srcNComps; ++c) {
            src_pixels[c] += (image.bounds.width() - roi.width()) * srcPixelStride;
        }
        dstPix += (stride - roi.width() * dstNComps);
    } // for each scan-line
}

template <typename PIX, int maxValue, int srcComps>
static void
convertNatronImageToCairoImageForSrcComponents(unsigned char* cairoImg,
                                               int dstNComps,
                                               std::size_t stride,
                                               const Image::CPUData& image,
                                               const RectI& roi,
                                               const RectI& dstBounds,
                                               const ColorRgbaD& shapeColor)
{
    if (dstNComps == 1) {
        convertNatronImageToCairoImageForComponents<PIX, maxValue, srcComps, 1>(cairoImg, stride, image, roi, dstBounds, shapeColor);
    } else if (dstNComps == 4) {
        convertNatronImageToCairoImageForComponents<PIX, maxValue, srcComps, 4>(cairoImg, stride, image, roi, dstBounds, shapeColor);
    } else {
        assert(false);
    }
}

template <typename PIX, int maxValue>
static void
convertNatronImageToCairoImage(unsigned char* cairoImg,
                               int dstNComps,
                               std::size_t stride,
                               const Image::CPUData& image,
                               const RectI& roi,
                               const RectI& dstBounds,
                               const ColorRgbaD& shapeColor)
{

    switch (image.nComps) {
        case 1:
            convertNatronImageToCairoImageForSrcComponents<PIX, maxValue, 1>(cairoImg, dstNComps, stride, image, roi, dstBounds, shapeColor);
            break;
        case 2:
            convertNatronImageToCairoImageForSrcComponents<PIX, maxValue, 2>(cairoImg, dstNComps, stride, image, roi, dstBounds, shapeColor);
            break;
        case 3:
            convertNatronImageToCairoImageForSrcComponents<PIX, maxValue, 3>(cairoImg, dstNComps, stride, image, roi, dstBounds, shapeColor);
            break;
        case 4:
            convertNatronImageToCairoImageForSrcComponents<PIX, maxValue, 4>(cairoImg, dstNComps, stride, image, roi, dstBounds, shapeColor);
            break;
        default:
            break;
    }
}



struct qpointf_compare_less
{
    bool operator() (const QPointF& lhs,
                     const QPointF& rhs) const
    {
        if (std::abs( lhs.x() - rhs.x() ) < 1e-6) {
            if (std::abs( lhs.y() - rhs.y() ) < 1e-6) {
                return false;
            } else if ( lhs.y() < rhs.y() ) {
                return true;
            } else {
                return false;
            }
        } else if ( lhs.x() < rhs.x() ) {
            return true;
        } else {
            return false;
        }
    }
};

static bool
pointInPolygon(const Point & p,
               const std::list<Point> & polygon,
               const RectD & featherPolyBBox,
               Bezier::FillRuleEnum rule)
{
    ///first check if the point lies inside the bounding box
    if ( (p.x < featherPolyBBox.x1) || (p.x >= featherPolyBBox.x2) || (p.y < featherPolyBBox.y1) || (p.y >= featherPolyBBox.y2)
        || polygon.empty() ) {
        return false;
    }

    int winding_number = 0;
    std::list<Point>::const_iterator last_pt = polygon.begin();
    std::list<Point>::const_iterator last_start = last_pt;
    std::list<Point>::const_iterator cur = last_pt;
    ++cur;
    for (; cur != polygon.end(); ++cur, ++last_pt) {
        Bezier::point_line_intersection(*last_pt, *cur, p, &winding_number);
    }

    // implicitly close last subpath
    if (last_pt != last_start) {
        Bezier::point_line_intersection(*last_pt, *last_start, p, &winding_number);
    }

    return rule == Bezier::eFillRuleWinding
    ? (winding_number != 0)
    : ( (winding_number % 2) != 0 );
}


//From http://www.math.ualberta.ca/~bowman/publications/cad10.pdf
void
RotoShapeRenderCairo::bezulate(TimeValue time,
                             const BezierCPs& cps,
                             std::list<BezierCPs>* patches)
{
    BezierCPs simpleClosedCurve = cps;

    while (simpleClosedCurve.size() > 4) {
        bool found = false;
        for (int n = 3; n >= 2; --n) {
            assert( (int)simpleClosedCurve.size() > n );

            //next points at point i + n
            BezierCPs::iterator next = simpleClosedCurve.begin();
            std::advance(next, n);
            std::list<Point> polygon;
            RectD bbox;
            bbox.setupInfinity();
            for (BezierCPs::iterator it = simpleClosedCurve.begin(); it != simpleClosedCurve.end(); ++it) {
                Point p;
                (*it)->getPositionAtTime(time, &p.x, &p.y);
                polygon.push_back(p);
                if (p.x < bbox.x1) {
                    bbox.x1 = p.x;
                }
                if (p.x > bbox.x2) {
                    bbox.x2 = p.x;
                }
                if (p.y < bbox.y1) {
                    bbox.y1 = p.y;
                }
                if (p.y > bbox.y2) {
                    bbox.y2 = p.y;
                }
            }


            for (BezierCPs::iterator it = simpleClosedCurve.begin(); it != simpleClosedCurve.end(); ++it) {
                bool nextIsPassedEnd = false;
                if ( next == simpleClosedCurve.end() ) {
                    next = simpleClosedCurve.begin();
                    nextIsPassedEnd = true;
                }

                //mid-point of the line segment between points i and i + n
                Point nextPoint, curPoint;
                (*it)->getPositionAtTime(time, &curPoint.x, &curPoint.y);
                (*next)->getPositionAtTime(time,  &nextPoint.x, &nextPoint.y);

                /*
                 * Compute the number of intersections between the current line segment [it,next] and all other line segments
                 * If the number of intersections is different of 2, ignore this segment.
                 */
                QLineF line( QPointF(curPoint.x, curPoint.y), QPointF(nextPoint.x, nextPoint.y) );
                std::set<QPointF, qpointf_compare_less> intersections;
                std::list<Point>::const_iterator last_pt = polygon.begin();
                std::list<Point>::const_iterator cur = last_pt;
                ++cur;
                QPointF intersectionPoint;
                for (; cur != polygon.end(); ++cur, ++last_pt) {
                    QLineF polygonSegment( QPointF(last_pt->x, last_pt->y), QPointF(cur->x, cur->y) );
                    if (line.intersect(polygonSegment, &intersectionPoint) == QLineF::BoundedIntersection) {
                        intersections.insert(intersectionPoint);
                    }
                    if (intersections.size() > 2) {
                        break;
                    }
                }

                if (intersections.size() != 2) {
                    continue;
                }

                /*
                 * Check if the midpoint of the line segment [it,next] lies inside the simple closed curve (polygon), otherwise
                 * ignore it.
                 */
                Point midPoint;
                midPoint.x = (nextPoint.x + curPoint.x) / 2.;
                midPoint.y = (nextPoint.y + curPoint.y) / 2.;
                bool isInside = pointInPolygon(midPoint, polygon, bbox, Bezier::eFillRuleWinding);

                if (isInside) {
                    //Make the sub closed curve composed of the path from points i to i + n
                    BezierCPs subCurve;
                    subCurve.push_back(*it);
                    BezierCPs::iterator pointIt = it;
                    for (int i = 0; i < n - 1; ++i) {
                        ++pointIt;
                        if ( pointIt == simpleClosedCurve.end() ) {
                            pointIt = simpleClosedCurve.begin();
                        }
                        subCurve.push_back(*pointIt);
                    }
                    subCurve.push_back(*next);

                    // Ensure that all interior angles are less than 180 degrees.


                    patches->push_back(subCurve);

                    //Remove i + 1 to i + n
                    BezierCPs::iterator eraseStart = it;
                    ++eraseStart;
                    bool eraseStartIsPassedEnd = false;
                    if ( eraseStart == simpleClosedCurve.end() ) {
                        eraseStart = simpleClosedCurve.begin();
                        eraseStartIsPassedEnd = true;
                    }
                    //"it" is  invalidated after the next instructions but we leave the loop anyway
                    assert( !simpleClosedCurve.empty() );
                    if ( (!nextIsPassedEnd && !eraseStartIsPassedEnd) || (nextIsPassedEnd && eraseStartIsPassedEnd) ) {
                        simpleClosedCurve.erase(eraseStart, next);
                    } else {
                        simpleClosedCurve.erase( eraseStart, simpleClosedCurve.end() );
                        if ( !simpleClosedCurve.empty() ) {
                            simpleClosedCurve.erase(simpleClosedCurve.begin(), next);
                        }
                    }
                    found = true;
                    break;
                }

                // increment for next iteration
                if ( next != simpleClosedCurve.end() ) {
                    ++next;
                }
            } // for(it)
            if (found) {
                break;
            }
        } // for(n)

        if (!found) {
            BezierCPs subdivisedCurve;
            //Subdivise the curve at the midpoint of each segment
            BezierCPs::iterator next = simpleClosedCurve.begin();
            if ( next != simpleClosedCurve.end() ) {
                ++next;
            }
            for (BezierCPs::iterator it = simpleClosedCurve.begin(); it != simpleClosedCurve.end(); ++it) {
                if ( next == simpleClosedCurve.end() ) {
                    next = simpleClosedCurve.begin();
                }
                Point p0, p1, p2, p3, p0p1, p1p2, p2p3, p0p1_p1p2, p1p2_p2p3, dest;
                (*it)->getPositionAtTime(time,  &p0.x, &p0.y);
                (*it)->getRightBezierPointAtTime(time,  &p1.x, &p1.y);
                (*next)->getLeftBezierPointAtTime(time,  &p2.x, &p2.y);
                (*next)->getPositionAtTime(time,  &p3.x, &p3.y);
                Bezier::bezierFullPoint(p0, p1, p2, p3, 0.5, &p0p1, &p1p2, &p2p3, &p0p1_p1p2, &p1p2_p2p3, &dest);
                BezierCPPtr controlPoint(new BezierCP);
                controlPoint->setStaticPosition(dest.x, dest.y);
                controlPoint->setLeftBezierStaticPosition(p0p1_p1p2.x, p0p1_p1p2.y);
                controlPoint->setRightBezierStaticPosition(p1p2_p2p3.x, p1p2_p2p3.y);
                subdivisedCurve.push_back(*it);
                subdivisedCurve.push_back(controlPoint);

                // increment for next iteration
                if ( next != simpleClosedCurve.end() ) {
                    ++next;
                }
            } // for()
            simpleClosedCurve = subdivisedCurve;
        }
    }
    if ( !simpleClosedCurve.empty() ) {
        assert(simpleClosedCurve.size() >= 2);
        patches->push_back(simpleClosedCurve);
    }
} // RotoShapeRenderCairo::bezulate



static inline
double
hardnessGaussLookup(double f)
{
    //2 hyperbolas + 1 parabola to approximate a gauss function
    if (f < -0.5) {
        f = -1. - f;

        return (2. * f * f);
    }

    if (f < 0.5) {
        return (1. - 2. * f * f);
    }
    f = 1. - f;

    return (2. * f * f);
}


static void
getRenderDotParams(double alpha,
                   double brushSizePixelX,
                   double brushSizePixelY,
                   double brushHardness,
                   double brushSpacing,
                   double pressure,
                   bool pressureAffectsOpacity,
                   bool pressureAffectsSize,
                   bool pressureAffectsHardness,
                   double* internalDotRadiusX,
                   double* internalDotRadiusY,
                   double* externalDotRadiusX,
                   double* externalDotRadiusY,
                   double * spacing,
                   std::vector<std::pair<double, double> >* opacityStops)
{
    if (pressureAffectsSize) {
        brushSizePixelX *= pressure;
        brushSizePixelY *= pressure;
    }
    if (pressureAffectsHardness) {
        brushHardness *= pressure;
    }
    if (pressureAffectsOpacity) {
        alpha *= pressure;
    }

    *internalDotRadiusX = std::max(brushSizePixelX * brushHardness, 1.) / 2.;
    *internalDotRadiusY = std::max(brushSizePixelY * brushHardness, 1.) / 2.;
    *externalDotRadiusX = std::max(brushSizePixelX, 1.) / 2.;
    *externalDotRadiusY = std::max(brushSizePixelY, 1.) / 2.;
    *spacing = std::max(*externalDotRadiusX, *externalDotRadiusY) * 2. * brushSpacing;

    if (opacityStops) {
        opacityStops->clear();

        double exp = brushHardness != 1.0 ?  0.4 / (1.0 - brushHardness) : 0.;
        const int maxStops = 8;
        double incr = 1. / maxStops;

        if (brushHardness != 1.) {
            for (double d = 0; d <= 1.; d += incr) {
                double o = hardnessGaussLookup( std::pow(d, exp) );
                opacityStops->push_back( std::make_pair(d, o * alpha) );
            }
        }
    }
} // getRenderDotParams


bool
RotoShapeRenderCairo::allocateAndRenderSingleDotStroke_cairo(double brushSizePixelX, double brushSizePixelY,
                                                             double brushHardness,
                                                             double alpha,
                                                             RotoShapeRenderCairo::CairoImageWrapper& wrapper)
{
    wrapper.cairoImg = cairo_image_surface_create(CAIRO_FORMAT_A8, std::ceil(brushSizePixelX), std::ceil(brushSizePixelY));
    cairo_surface_set_device_offset(wrapper.cairoImg, 0, 0);
    if (cairo_surface_status(wrapper.cairoImg) != CAIRO_STATUS_SUCCESS) {
        return false;
    }
    wrapper.ctx = cairo_create(wrapper.cairoImg);
    //cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD); // creates holes on self-overlapping shapes
    cairo_set_fill_rule(wrapper.ctx, CAIRO_FILL_RULE_WINDING);

    // these Roto shapes must be rendered WITHOUT antialias, or the junction between the inner
    // polygon and the feather zone will have artifacts. This is partly due to the fact that cairo
    // meshes are not antialiased.
    // Use a default feather distance of 1 pixel instead!
    // UPDATE: unfortunately, this produces less artifacts, but there are still some remaining (use opacity=0.5 to test)
    // maybe the inner polygon should be made of mesh patterns too?
    cairo_set_antialias(wrapper.ctx, CAIRO_ANTIALIAS_NONE);

    cairo_set_operator(wrapper.ctx, CAIRO_OPERATOR_OVER);

    double internalDotRadiusX, externalDotRadiusX, internalDotRadiusY, externalDotRadiusY, spacing;
    std::vector<std::pair<double, double> > opacityStops;
    Point p;
    p.x = brushSizePixelX / 2.;
    p.y = brushSizePixelY / 2.;

    const double pressure = 1.;
    const double brushspacing = 0.;

    getRenderDotParams(alpha, brushSizePixelX, brushSizePixelY, brushHardness, brushspacing, pressure, false, false, false, &internalDotRadiusX, &internalDotRadiusY, &externalDotRadiusX, &externalDotRadiusY, &spacing, &opacityStops);
    renderDot_cairo(wrapper.ctx, 0, p, internalDotRadiusX, internalDotRadiusY, externalDotRadiusX, externalDotRadiusY, pressure, true, opacityStops, alpha);
    
    return true;
}


void
RotoShapeRenderCairo::renderDot_cairo(cairo_t* cr,
                                      std::vector<cairo_pattern_t*>* dotPatterns,
                                      const Point &center,
                                      double internalDotRadiusX,
                                      double internalDotRadiusY,
                                      double externalDotRadiusX,
                                      double externalDotRadiusY,
                                      double pressure,
                                      bool doBuildUp,
                                      const std::vector<std::pair<double, double> >& opacityStops,
                                      double opacity)
{
    if ( !opacityStops.empty() ) {
        cairo_pattern_t* pattern;
        // sometimes, Qt gives a pressure level > 1... so we clamp it
        int pressureInt = int(std::max( 0., std::min(pressure, 1.) ) * (ROTO_PRESSURE_LEVELS - 1) + 0.5);
        assert(pressureInt >= 0 && pressureInt < ROTO_PRESSURE_LEVELS);
        if (dotPatterns && (*dotPatterns)[pressureInt]) {
            pattern = (*dotPatterns)[pressureInt];
        } else {
            pattern = cairo_pattern_create_radial(0, 0, std::max(internalDotRadiusX, internalDotRadiusY), 0, 0, std::max(externalDotRadiusX, externalDotRadiusY));
            for (std::size_t i = 0; i < opacityStops.size(); ++i) {
                if (doBuildUp) {
                    cairo_pattern_add_color_stop_rgba(pattern, opacityStops[i].first, 1., 1., 1., opacityStops[i].second);
                } else {
                    cairo_pattern_add_color_stop_rgba(pattern, opacityStops[i].first, opacityStops[i].second, opacityStops[i].second, opacityStops[i].second, 1);
                }
            }
            //dotPatterns[pressureInt] = pattern;
        }
        cairo_translate(cr, center.x, center.y);
        cairo_set_source(cr, pattern);
        cairo_translate(cr, -center.x, -center.y);
    } else {
        if (doBuildUp) {
            cairo_set_source_rgba(cr, 1., 1., 1., opacity);
        } else {
            cairo_set_source_rgba(cr, opacity, opacity, opacity, 1.);
        }
    }
#ifdef DEBUG
    //Make sure the dot we are about to render falls inside the clip region, otherwise the bounds of the image are mis-calculated.
    cairo_surface_t* target = cairo_get_target(cr);
    int w = cairo_image_surface_get_width(target);
    int h = cairo_image_surface_get_height(target);
    double x1, y1;
    cairo_surface_get_device_offset(target, &x1, &y1);
    assert(std::floor(center.x - externalDotRadiusX) >= -x1 && std::floor(center.x + externalDotRadiusX) < -x1 + w &&
           std::floor(center.y - externalDotRadiusY) >= -y1 && std::floor(center.y + externalDotRadiusY) < -y1 + h);
#endif
    cairo_arc(cr, center.x, center.y, std::max(externalDotRadiusX, externalDotRadiusY), 0, M_PI * 2);
    cairo_fill(cr);
} // RotoShapeRenderCairo::renderDot_cairo

void
RotoShapeRenderCairo::applyAndDestroyMask(cairo_t* cr,
                                        cairo_pattern_t* mesh)
{
    assert(cairo_pattern_status(mesh) == CAIRO_STATUS_SUCCESS);
    cairo_set_source(cr, mesh);

    ///paint with the feather with the pattern as a mask
    cairo_mask(cr, mesh);

    cairo_pattern_destroy(mesh);
}





struct RenderStrokeCairoData
{
    cairo_t* cr;
    std::vector<cairo_pattern_t*>* dotPatterns;
    double brushSizePixelX;
    double brushSizePixelY;
    double brushSpacing;
    double brushHardness;
    bool pressureAffectsOpacity;
    bool pressureAffectsHardness;
    bool pressureAffectsSize;
    bool buildUp;
    ColorRgbaD shapeColor;
    double opacity;
};

static void
renderStrokeBegin_cairo(RotoShapeRenderNodePrivate::RenderStrokeDataPtr userData,
                        double brushSizePixelX,
                        double brushSizePixelY,
                        double brushSpacing,
                        double brushHardness,
                        bool pressureAffectsOpacity,
                        bool pressureAffectsHardness,
                        bool pressureAffectsSize,
                        bool buildUp,
                        const ColorRgbaD& shapeColor,
                        double opacity)
{
    RenderStrokeCairoData* myData = (RenderStrokeCairoData*)userData;
    myData->brushSizePixelX = brushSizePixelX;
    myData->brushSizePixelY = brushSizePixelY;
    myData->brushSpacing = brushSpacing;
    myData->brushHardness = brushHardness;
    myData->pressureAffectsOpacity = pressureAffectsOpacity;
    myData->pressureAffectsHardness = pressureAffectsHardness;
    myData->pressureAffectsSize = pressureAffectsSize;
    myData->buildUp = buildUp;
    myData->shapeColor =  shapeColor;
    myData->opacity = opacity;
    cairo_set_operator(myData->cr, buildUp ? CAIRO_OPERATOR_OVER : CAIRO_OPERATOR_LIGHTEN);

}

static void
renderStrokeEnd_cairo(RotoShapeRenderNodePrivate::RenderStrokeDataPtr /*userData*/)
{

}

static bool
renderStrokeRenderDot_cairo(RotoShapeRenderNodePrivate::RenderStrokeDataPtr userData,
                            const Point &/*prevCenter*/,
                            const Point &center,
                            double pressure,
                            double* spacing)
{
    RenderStrokeCairoData* myData = (RenderStrokeCairoData*)userData;
    double internalDotRadiusX, internalDotRadiusY, externalDotRadiusX, externalDotRadiusY;
    std::vector<std::pair<double,double> > opacityStops;
    getRenderDotParams(myData->opacity, myData->brushSizePixelX, myData->brushSizePixelY, myData->brushHardness, myData->brushSpacing, pressure, myData->pressureAffectsOpacity, myData->pressureAffectsSize, myData->pressureAffectsHardness, &internalDotRadiusX, &internalDotRadiusY, &externalDotRadiusX, &externalDotRadiusY, spacing, &opacityStops);

    RotoShapeRenderCairo::renderDot_cairo(myData->cr, myData->dotPatterns, center, internalDotRadiusX, internalDotRadiusY, externalDotRadiusX, externalDotRadiusY, pressure, myData->buildUp, opacityStops, myData->opacity);
    return true;
}

void
RotoShapeRenderCairo::renderStroke_cairo(cairo_t* cr,
                                         std::vector<cairo_pattern_t*>& dotPatterns,
                                         const std::list<std::list<std::pair<Point, double> > >& strokes,
                                         const double distToNextIn,
                                         const Point& lastCenterPointIn,
                                         const RotoDrawableItemPtr& stroke,
                                         bool doBuildup,
                                         double alpha,
                                         TimeValue time,
                                         ViewIdx view,
                                         const RenderScale& scale,
                                         double* distToNextOut,
                                         Point* lastCenterPoint)
{
    RenderStrokeCairoData data;
    data.cr = cr;
    data.dotPatterns = &dotPatterns;

    RotoShapeRenderNodePrivate::renderStroke_generic((RotoShapeRenderNodePrivate::RenderStrokeDataPtr)&data,
                                                     renderStrokeBegin_cairo,
                                                     renderStrokeRenderDot_cairo,
                                                     renderStrokeEnd_cairo,
                                                     strokes,
                                                     distToNextIn,
                                                     lastCenterPointIn,
                                                     stroke,
                                                     doBuildup,
                                                     alpha,
                                                     time,
                                                     view,
                                                     scale,
                                                     distToNextOut,
                                                     lastCenterPoint);
}

struct RenderSmearCairoData
{
    ImagePtr dstImage;
    double brushSizePixelX;
    double brushSizePixelY;
    double brushSpacing;
    double brushHardness;
    bool pressureAffectsOpacity;
    bool pressureAffectsHardness;
    bool pressureAffectsSize;
    double opacity;
    RotoShapeRenderCairo::CairoImageWrapper imgWrapper;
    int maskWidth;
    int maskHeight;
    int maskStride;
    const unsigned char* maskData;


};

static void
renderSmearBegin_cairo(RotoShapeRenderNodePrivate::RenderStrokeDataPtr userData,
                       double brushSizePixelX,
                       double brushSizePixelY,
                       double brushSpacing,
                       double brushHardness,
                       bool pressureAffectsOpacity,
                       bool pressureAffectsHardness,
                       bool pressureAffectsSize,
                       bool /*buildUp*/,
                       const ColorRgbaD& /*shapeColor*/,
                       double opacity)
{
    RenderSmearCairoData* myData = (RenderSmearCairoData*)userData;
    myData->brushSizePixelX = brushSizePixelX;
    myData->brushSizePixelY = brushSizePixelY;
    myData->brushSpacing = brushSpacing;
    myData->brushHardness = brushHardness;
    myData->pressureAffectsOpacity = pressureAffectsOpacity;
    myData->pressureAffectsHardness = pressureAffectsHardness;
    myData->pressureAffectsSize = pressureAffectsSize;
    myData->opacity = opacity;


    bool ok = RotoShapeRenderCairo::allocateAndRenderSingleDotStroke_cairo(brushSizePixelX, brushSizePixelY, brushHardness, opacity, myData->imgWrapper);
    assert(ok);
    Q_UNUSED(ok);
    myData->maskWidth = cairo_image_surface_get_width(myData->imgWrapper.cairoImg);
    myData->maskHeight = cairo_image_surface_get_height(myData->imgWrapper.cairoImg);
    myData->maskStride = cairo_image_surface_get_stride(myData->imgWrapper.cairoImg);
    myData->maskData = cairo_image_surface_get_data(myData->imgWrapper.cairoImg);
}


static void
renderSmearDot(const unsigned char* maskData,
               const int maskStride,
               const int maskWidth,
               const int maskHeight,
               const Point& prev,
               const Point& next,
               const double brushSizePixelsX,
               const double brushSizePixelsY,
               const ImagePtr& outputImage)
{
    /// First copy the portion of the image around the previous dot into tmpBuf
    RectD prevDotRoD(prev.x - brushSizePixelsX / 2., prev.y - brushSizePixelsY / 2., prev.x + brushSizePixelsX / 2., prev.y + brushSizePixelsY / 2.);
    RectI prevDotBounds;
    prevDotRoD.toPixelEnclosing(0, 1., &prevDotBounds);


    ImagePtr tmpBuf;
    {
        Image::InitStorageArgs initArgs;
        initArgs.bounds = prevDotBounds;
        initArgs.plane = outputImage->getLayer();
        initArgs.storage = eStorageModeRAM;
        initArgs.bitdepth = outputImage->getBitDepth();
        tmpBuf = Image::create(initArgs);
        if (!tmpBuf) {
            return;
        }
    }

    {
        Image::CopyPixelsArgs cpyArgs;
        cpyArgs.roi = prevDotBounds;
        tmpBuf->copyPixels(*outputImage, cpyArgs);
    }

    Image::CPUData dstImageData;
    outputImage->getCPUData(&dstImageData);

    Image::CPUData tmpImageData;
    tmpBuf->getCPUData(&tmpImageData);

    RectI nextDotBounds;
    nextDotBounds.x1 = next.x - maskWidth / 2;
    nextDotBounds.x2 = next.x + maskWidth / 2;
    nextDotBounds.y1 = next.y - maskHeight / 2;
    nextDotBounds.y2 = next.y + maskHeight / 2;

    const unsigned char* mask_pixels = maskData;
    int yPrev = prevDotBounds.y1;
    for (int y = nextDotBounds.y1; y < nextDotBounds.y2;
         ++y,
         ++yPrev,
         mask_pixels += maskStride) {


        float *dst_pixels[4];
        int dstPixelStride;
        Image::getChannelPointers<float>((const float**)dstImageData.ptrs, nextDotBounds.x1, y, dstImageData.bounds, dstImageData.nComps, (float**)dst_pixels, &dstPixelStride);

        int xPrev = prevDotBounds.x1;
        for (int x = nextDotBounds.x1; x < nextDotBounds.x2;
             ++x, ++xPrev) {

            float *tmp_pixels[4];
            int tmpPixelStride;
            Image::getChannelPointers<float>((const float**)tmpImageData.ptrs, xPrev, yPrev, tmpImageData.bounds, tmpImageData.nComps, (float**)tmp_pixels, &tmpPixelStride);

            if (!tmp_pixels[0]) {
                continue;
            }
            float mask_scale = Image::convertPixelDepth<unsigned char, float>(mask_pixels[x - nextDotBounds.x1]);
            float one_minus_mask_scale = 1. - mask_scale;

            for (int k = 0; k < dstImageData.nComps; ++k) {
                *dst_pixels[k] = *tmp_pixels[k] * mask_scale + *dst_pixels[k] * one_minus_mask_scale;
                dst_pixels[k] += dstPixelStride;
            }
        }
    }

} // renderSmearDot

static bool
renderSmearRenderDot_cairo(RotoShapeRenderNodePrivate::RenderStrokeDataPtr userData,
                            const Point &prevCenter,
                            const Point &center,
                            double pressure,
                            double* spacing)
{
    RenderSmearCairoData* myData = (RenderSmearCairoData*)userData;
    double internalRadiusX, internalRadiusY, externalRadiusX, externalRadiusY;
    getRenderDotParams(myData->opacity, myData->brushSizePixelX, myData->brushSizePixelY, myData->brushHardness, myData->brushSpacing, pressure, myData->pressureAffectsOpacity, myData->pressureAffectsSize, myData->pressureAffectsHardness, &internalRadiusX, &internalRadiusY, &externalRadiusX, &externalRadiusY, spacing, 0);
    if (prevCenter.x == INT_MIN || prevCenter.y == INT_MIN) {
        return false;
    }
    if (prevCenter.x == center.x && prevCenter.y == center.y) {
        return false;
    }


    // If we were to copy exactly the portion in prevCenter, the smear would leave traces
    // too long. To dampen the effect of the smear, we clamp the spacing
    Point prevPoint = RotoShapeRenderNodePrivate::dampenSmearEffect(prevCenter, center, *spacing);

    renderSmearDot(myData->maskData, myData->maskStride, myData->maskWidth, myData->maskHeight, prevPoint, center, myData->brushSizePixelX, myData->brushSizePixelY, myData->dstImage);
    return true;
}


bool
RotoShapeRenderCairo::renderSmear_cairo(TimeValue time,
                                        ViewIdx view,
                                        const RenderScale& scale,
                                        const RotoStrokeItemPtr& rotoItem,
                                        const RectI& /*roi*/,
                                        const ImagePtr& dstImage,
                                        const double distToNextIn,
                                        const Point& lastCenterPointIn,
                                        double* distToNextOut,
                                        Point* lastCenterPointOut)
{

    RenderSmearCairoData data;
    data.opacity = rotoItem->getOpacityKnob()->getValueAtTime(time, DimIdx(0), view);
    data.dstImage = dstImage;

    std::list<std::list<std::pair<Point, double> > > strokes;
    rotoItem->evaluateStroke(scale, time, view, &strokes, 0);

    bool renderedDot = RotoShapeRenderNodePrivate::renderStroke_generic((RotoShapeRenderNodePrivate::RenderStrokeDataPtr)&data,
                                                                        renderSmearBegin_cairo,
                                                                        renderSmearRenderDot_cairo,
                                                                        renderStrokeEnd_cairo,
                                                                        strokes,
                                                                        distToNextIn,
                                                                        lastCenterPointIn,
                                                                        rotoItem,
                                                                        false,
                                                                        data.opacity,
                                                                        time,
                                                                        view,
                                                                        scale,
                                                                        distToNextOut,
                                                                        lastCenterPointOut);
    return renderedDot;
    
} // RotoShapeRenderCairo::renderSmear_cairo


void
RotoShapeRenderCairo::renderBezier_cairo(cairo_t* cr,
                                         const BezierPtr& bezier,
                                         double opacity,
                                         TimeValue time,
                                         ViewIdx view,
                                         const RenderScale& scale)
{
    const TimeValue t = time;
    double fallOff = bezier->getFeatherFallOffKnob()->getValueAtTime(t, DimIdx(0), view);
    double featherDist_canonical = bezier->getFeatherKnob()->getValueAtTime(t, DimIdx(0), view);
    ColorRgbaD shapeColor;
    {
        KnobColorPtr colorKnob = bezier->getColorKnob();
        shapeColor.r = colorKnob->getValueAtTime(t, DimIdx(0), view);
        shapeColor.g = colorKnob->getValueAtTime(t, DimIdx(1), view);
        shapeColor.b = colorKnob->getValueAtTime(t, DimIdx(2), view);
        shapeColor.a = colorKnob->getValueAtTime(t, DimIdx(3), view);
    }


    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_new_path(cr);

    ////Define the feather edge pattern
    cairo_pattern_t* mesh = cairo_pattern_create_mesh();
    if (cairo_pattern_status(mesh) != CAIRO_STATUS_SUCCESS) {
        cairo_pattern_destroy(mesh);

        return;
    }

    ///Adjust the feather distance so it takes the mipmap level into account
    double featherDist_pixelX = featherDist_canonical * scale.x;
    double featherDist_pixelY = featherDist_canonical * scale.y;




#ifdef ROTO_CAIRO_RENDER_TRIANGLES_ONLY
    PolygonData data;
    computeTriangles(bezier, t, scale, featherDist_pixelX, featherDist_pixelY, &data);
    renderFeather_cairo(data, shapeColor, fallOff, mesh);
    renderInternalShape_cairo(data, shapeColor, mesh);
    Q_UNUSED(opacity);
#else
    renderFeather_old_cairo(bezier, t, view, scale, shapeColor, opacity, featherDist_pixelX, featherDist_pixelY, fallOff, mesh);

    Transform::Matrix3x3 transform;
    bezier->getTransformAtTime(t, view, &transform);

    // strangely, the above-mentioned cairo bug doesn't affect this function
    BezierCPs cps = bezier->getControlPoints(view);
    renderInternalShape_old_cairo(t, scale, shapeColor, opacity, transform, cr, mesh, cps);

#endif


    RotoShapeRenderCairo::applyAndDestroyMask(cr, mesh);

} // RotoShapeRenderCairo::renderBezier_cairo

void
RotoShapeRenderCairo::renderFeather_old_cairo(const BezierPtr& bezier,
                                              TimeValue time,
                                              ViewIdx view,
                                              const RenderScale &scale,
                                              const ColorRgbaD& shapeColor,
                                              double /*opacity*/,
                                              double featherDist_pixelsX,
                                              double featherDist_pixelsY,
                                              double fallOff,
                                              cairo_pattern_t* mesh)
{
    ///Note that we do not use the opacity when rendering the bezier, it is rendered with correct floating point opacity/color when converting
    ///to the Natron image.

    double fallOffInverse = 1. / fallOff;
    /*
     * We descretize the feather control points to obtain a polygon so that the feather distance will be of the same thickness around all the shape.
     * If we were to extend only the end points, the resulting bezier interpolation would create a feather with different thickness around the shape,
     * yielding an unwanted behaviour for the end user.
     */
    ///here is the polygon of the feather bezier
    ///This is used only if the feather distance is different of 0 and the feather points equal
    ///the control points in order to still be able to apply the feather distance.
    std::vector<ParametricPoint> featherPolygon;
    std::vector<ParametricPoint> bezierPolygon;
    RectD featherPolyBBox;

    featherPolyBBox.setupInfinity();

    bezier->evaluateFeatherPointsAtTime_DeCasteljau(time, view, scale,
#ifdef ROTO_BEZIER_EVAL_ITERATIVE
                                                    50,
#else
                                                    1,
#endif
                                                    true, &featherPolygon, &featherPolyBBox);
    bezier->evaluateAtTime_DeCasteljau(time, view, scale,
#ifdef ROTO_BEZIER_EVAL_ITERATIVE
                                       50,
#else
                                       1,
#endif
                                       &bezierPolygon, NULL);

    bool clockWise = bezier->isFeatherPolygonClockwiseOriented(time, view);

    assert( !featherPolygon.empty() && !bezierPolygon.empty() );


    std::vector<Point> featherContour;

    // prepare iterators
    std::vector<ParametricPoint>::iterator next = featherPolygon.begin();
    ++next;  // can only be valid since we assert the list is not empty
    if ( next == featherPolygon.end() ) {
        next = featherPolygon.begin();
    }
    std::vector<ParametricPoint>::iterator prev = featherPolygon.end();
    --prev; // can only be valid since we assert the list is not empty
    std::vector<ParametricPoint>::iterator bezIT = bezierPolygon.begin();
    std::vector<ParametricPoint>::iterator prevBez = bezierPolygon.end();
    --prevBez; // can only be valid since we assert the list is not empty

    // prepare p1
    double absFeatherDistX = std::abs(featherDist_pixelsX);
    double absFeatherDistY = std::abs(featherDist_pixelsY);
    Point p1;
    p1.x = featherPolygon.begin()->x;
    p1.y = featherPolygon.begin()->y;
    double norm = sqrt( (next->x - prev->x) * (next->x - prev->x) + (next->y - prev->y) * (next->y - prev->y) );
    assert(norm != 0);
    double dx = (norm != 0) ? -( (next->y - prev->y) / norm ) : 0;
    double dy = (norm != 0) ? ( (next->x - prev->x) / norm ) : 1;

    if (!clockWise) {
        p1.x -= dx * absFeatherDistX;
        p1.y -= dy * absFeatherDistY;
    } else {
        p1.x += dx * absFeatherDistX;
        p1.y += dy * absFeatherDistY;
    }

    Point origin = p1;
    featherContour.push_back(p1);


    // increment for first iteration
    std::vector<ParametricPoint>::iterator cur = featherPolygon.begin();
    // ++cur, ++prev, ++next, ++bezIT, ++prevBez
    // all should be valid, actually
    assert( cur != featherPolygon.end() &&
           prev != featherPolygon.end() &&
           next != featherPolygon.end() &&
           bezIT != bezierPolygon.end() &&
           prevBez != bezierPolygon.end() );
    if ( cur != featherPolygon.end() ) {
        ++cur;
    }
    if ( prev != featherPolygon.end() ) {
        ++prev;
    }
    if ( next != featherPolygon.end() ) {
        ++next;
    }
    if ( bezIT != bezierPolygon.end() ) {
        ++bezIT;
    }
    if ( prevBez != bezierPolygon.end() ) {
        ++prevBez;
    }

    for (;; ++cur) { // for each point in polygon
        if ( next == featherPolygon.end() ) {
            next = featherPolygon.begin();
        }
        if ( prev == featherPolygon.end() ) {
            prev = featherPolygon.begin();
        }
        if ( bezIT == bezierPolygon.end() ) {
            bezIT = bezierPolygon.begin();
        }
        if ( prevBez == bezierPolygon.end() ) {
            prevBez = bezierPolygon.begin();
        }
        bool mustStop = false;
        if ( cur == featherPolygon.end() ) {
            mustStop = true;
            cur = featherPolygon.begin();
        }

        ///skip it
        if ( (cur->x == prev->x) && (cur->y == prev->y) ) {
            continue;
        }

        Point p0, p0p1, p1p0, p2, p2p3, p3p2, p3;
        p0.x = prevBez->x;
        p0.y = prevBez->y;
        p3.x = bezIT->x;
        p3.y = bezIT->y;

        if (!mustStop) {
            norm = sqrt( (next->x - prev->x) * (next->x - prev->x) + (next->y - prev->y) * (next->y - prev->y) );
            assert(norm != 0);
            dx = -( (next->y - prev->y) / norm );
            dy = ( (next->x - prev->x) / norm );
            p2.x = cur->x;
            p2.y = cur->y;

            if (!clockWise) {
                p2.x -= dx * absFeatherDistX;
                p2.y -= dy * absFeatherDistY;
            } else {
                p2.x += dx * absFeatherDistX;
                p2.y += dy * absFeatherDistY;
            }
        } else {
            p2.x = origin.x;
            p2.y = origin.y;
        }
        featherContour.push_back(p2);

        ///linear interpolation
        p0p1.x = (p0.x * fallOff * 2. + fallOffInverse * p1.x) / (fallOff * 2. + fallOffInverse);
        p0p1.y = (p0.y * fallOff * 2. + fallOffInverse * p1.y) / (fallOff * 2. + fallOffInverse);
        p1p0.x = (p0.x * fallOff + 2. * fallOffInverse * p1.x) / (fallOff + 2. * fallOffInverse);
        p1p0.y = (p0.y * fallOff + 2. * fallOffInverse * p1.y) / (fallOff + 2. * fallOffInverse);


        p2p3.x = (p3.x * fallOff + 2. * fallOffInverse * p2.x) / (fallOff + 2. * fallOffInverse);
        p2p3.y = (p3.y * fallOff + 2. * fallOffInverse * p2.y) / (fallOff + 2. * fallOffInverse);
        p3p2.x = (p3.x * fallOff * 2. + fallOffInverse * p2.x) / (fallOff * 2. + fallOffInverse);
        p3p2.y = (p3.y * fallOff * 2. + fallOffInverse * p2.y) / (fallOff * 2. + fallOffInverse);


        ///move to the initial point
        cairo_mesh_pattern_begin_patch(mesh);
        cairo_mesh_pattern_move_to(mesh, p0.x, p0.y);
        cairo_mesh_pattern_curve_to(mesh, p0p1.x, p0p1.y, p1p0.x, p1p0.y, p1.x, p1.y);
        cairo_mesh_pattern_line_to(mesh, p2.x, p2.y);
        cairo_mesh_pattern_curve_to(mesh, p2p3.x, p2p3.y, p3p2.x, p3p2.y, p3.x, p3.y);
        cairo_mesh_pattern_line_to(mesh, p0.x, p0.y);
        ///Set the 4 corners color
        ///inner is full color

        // IMPORTANT NOTE:
        // The two sqrt below are due to a probable cairo bug.
        // To check wether the bug is present is a given cairo version,
        // make any shape with a very large feather and set
        // opacity to 0.5. Then, zoom on the polygon border to check if the intensity is continuous
        // and approximately equal to 0.5.
        // If the bug if ixed in cairo, please use #if CAIRO_VERSION>xxx to keep compatibility with
        // older Cairo versions.
        cairo_mesh_pattern_set_corner_color_rgba(mesh, 0, shapeColor.r, shapeColor.g, shapeColor.b, 1.);
        ///outter is faded
        cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, shapeColor.r, shapeColor.g, shapeColor.b, 0.);
        cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, shapeColor.r, shapeColor.g, shapeColor.b, 0.);
        ///inner is full color
        cairo_mesh_pattern_set_corner_color_rgba(mesh, 3, shapeColor.r, shapeColor.g, shapeColor.b, 1.);
        assert(cairo_pattern_status(mesh) == CAIRO_STATUS_SUCCESS);

        cairo_mesh_pattern_end_patch(mesh);

        if (mustStop) {
            break;
        }

        p1 = p2;

        // increment for next iteration
        // ++prev, ++next, ++bezIT, ++prevBez
        if ( prev != featherPolygon.end() ) {
            ++prev;
        }
        if ( next != featherPolygon.end() ) {
            ++next;
        }
        if ( bezIT != bezierPolygon.end() ) {
            ++bezIT;
        }
        if ( prevBez != bezierPolygon.end() ) {
            ++prevBez;
        }
    }  // for each point in polygon
} // RotoShapeRenderCairo::renderFeather_old_cairo

void
RotoShapeRenderCairo::renderFeather_cairo(const RotoBezierTriangulation::PolygonData& inArgs, const ColorRgbaD& shapeColor, double fallOff, cairo_pattern_t * mesh)
{
    // Roto feather is rendered as triangles
    assert(inArgs.featherMesh.size() >= 3 && inArgs.featherMesh.size() % 3 == 0);

    double fallOffInverse = 1. / fallOff;

    std::vector<RotoBezierTriangulation::RotoFeatherVertex>::const_iterator next = inArgs.featherMesh.begin();
    ++next;
    std::vector<RotoBezierTriangulation::RotoFeatherVertex>::const_iterator nextNext = next;
    ++nextNext;
    int index = 0;
    for (std::vector<RotoBezierTriangulation::RotoFeatherVertex>::const_iterator it = inArgs.featherMesh.begin(); it!=inArgs.featherMesh.end(); index += 3) {


        cairo_mesh_pattern_begin_patch(mesh);

        // Only 3 of the 4 vertices are valid
        const RotoBezierTriangulation::RotoFeatherVertex* innerVertices[2] = {0, 0};
        const RotoBezierTriangulation::RotoFeatherVertex* outterVertices[2] = {0, 0};

        {
            int innerIndex = 0;
            int outterIndex = 0;
            if (it->isInner) {
                innerVertices[innerIndex] = &(*it);
                ++innerIndex;
            } else {
                outterVertices[outterIndex] = &(*it);
                ++outterIndex;
            }
            if (next->isInner) {
                assert(innerIndex <= 1);
                if (innerIndex <= 1) {
                    innerVertices[innerIndex] = &(*next);
                    ++innerIndex;
                }
            } else {
                assert(outterIndex <= 1);
                if (outterIndex <= 1) {
                    outterVertices[outterIndex] = &(*next);
                    ++outterIndex;
                }
            }
            if (nextNext->isInner) {
                assert(innerIndex <= 1);
                if (innerIndex <= 1) {
                    innerVertices[innerIndex] = &(*nextNext);
                    ++innerIndex;
                }
            } else {
                assert(outterIndex <= 1);
                if (outterIndex <= 1) {
                    outterVertices[outterIndex] = &(*nextNext);
                    ++outterIndex;
                }
            }
            assert((outterIndex == 1 && innerIndex == 2) || (innerIndex == 1 && outterIndex == 2));
        }
        // make a degenerated coons patch out of the triangle so that we can assign a color to each vertex to emulate simple gouraud shaded triangles
        Point p0, p0p1, p1, p1p0, p2, p2p3, p3p2, p3;

        p0.x = innerVertices[0]->x;
        p0.y = innerVertices[0]->y;
        p1.x = outterVertices[0]->x;
        p1.y = outterVertices[0]->y;
        if (outterVertices[1]) {
            p2.x = outterVertices[1]->x;
            p2.y = outterVertices[1]->y;
        } else {
            // Repeat p1 if only 1 outter vertex
            p2 = p1;
        }
        if (innerVertices[1]) {
            p3.x = innerVertices[1]->x;
            p3.y = innerVertices[1]->y;
        } else {
            // Repeat p0 if only 1 inner vertex
            p3 = p0;
        }


        ///linear interpolation
        p0p1.x = (p0.x * fallOff * 2. + fallOffInverse * p1.x) / (fallOff * 2. + fallOffInverse);
        p0p1.y = (p0.y * fallOff * 2. + fallOffInverse * p1.y) / (fallOff * 2. + fallOffInverse);
        p1p0.x = (p0.x * fallOff + 2. * fallOffInverse * p1.x) / (fallOff + 2. * fallOffInverse);
        p1p0.y = (p0.y * fallOff + 2. * fallOffInverse * p1.y) / (fallOff + 2. * fallOffInverse);


        p2p3.x = (p3.x * fallOff + 2. * fallOffInverse * p2.x) / (fallOff + 2. * fallOffInverse);
        p2p3.y = (p3.y * fallOff + 2. * fallOffInverse * p2.y) / (fallOff + 2. * fallOffInverse);
        p3p2.x = (p3.x * fallOff * 2. + fallOffInverse * p2.x) / (fallOff * 2. + fallOffInverse);
        p3p2.y = (p3.y * fallOff * 2. + fallOffInverse * p2.y) / (fallOff * 2. + fallOffInverse);


        // move to the initial point
        cairo_mesh_pattern_move_to(mesh, p0.x, p0.y);
        cairo_mesh_pattern_curve_to(mesh, p0p1.x, p0p1.y, p1p0.x, p1p0.y, p1.x, p1.y);
        cairo_mesh_pattern_line_to(mesh, p2.x, p2.y);
        cairo_mesh_pattern_curve_to(mesh, p2p3.x, p2p3.y, p3p2.x, p3p2.y, p3.x, p3.y);
        cairo_mesh_pattern_line_to(mesh, p0.x, p0.y);

        // Set the 4 corners color
        // inner is full color

        // IMPORTANT NOTE:
        // The two sqrt below are due to a probable cairo bug.
        // To check wether the bug is present is a given cairo version,
        // make any shape with a very large feather and set
        // opacity to 0.5. Then, zoom on the polygon border to check if the intensity is continuous
        // and approximately equal to 0.5.
        // If the bug if ixed in cairo, please use #if CAIRO_VERSION>xxx to keep compatibility with
        // older Cairo versions.
        cairo_mesh_pattern_set_corner_color_rgba(mesh, 0, shapeColor.r, shapeColor.g, shapeColor.b, 1.);
        // outter is faded
        cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, shapeColor.r, shapeColor.g, shapeColor.b, 0.);
        cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, shapeColor.r, shapeColor.g, shapeColor.b, 0.);
        // inner is full color
        cairo_mesh_pattern_set_corner_color_rgba(mesh, 3, shapeColor.r, shapeColor.g, shapeColor.b, 1.);
        assert(cairo_pattern_status(mesh) == CAIRO_STATUS_SUCCESS);

        cairo_mesh_pattern_end_patch(mesh);


        assert(nextNext != inArgs.featherMesh.end());
        ++nextNext;

        // check if we reached the end
        if (nextNext == inArgs.featherMesh.end()) {
            break;
        }
        // advance a second time
        ++nextNext;
        if (nextNext == inArgs.featherMesh.end()) {
            break;
        }
        // advance a 3rd time
        ++nextNext;
        if (nextNext == inArgs.featherMesh.end()) {
            break;
        }

        assert(next != inArgs.featherMesh.end());
        ++next;
        assert(next != inArgs.featherMesh.end());
        ++next;
        assert(next != inArgs.featherMesh.end());
        ++next;
        assert(next != inArgs.featherMesh.end());

        assert(it != inArgs.featherMesh.end());
        ++it;
        assert(it != inArgs.featherMesh.end());
        ++it;
        assert(it != inArgs.featherMesh.end());
        ++it;
        assert(it != inArgs.featherMesh.end());
    } // for (std::list<RotoFeatherVertex>::const_iterator it = vertices.begin(); it!=vertices.end(); )
} // RotoShapeRenderCairo::renderFeather_cairo



void
RotoShapeRenderCairo::renderInternalShape_cairo(const RotoBezierTriangulation::PolygonData& inArgs, const ColorRgbaD& shapeColor, cairo_pattern_t * mesh)
{
    for (std::vector<RotoBezierTriangulation::RotoTriangles>::const_iterator it = inArgs.internalTriangles.begin(); it!=inArgs.internalTriangles.end(); ++it ) {

        assert(it->indices.size() >= 3 && it->indices.size() % 3 == 0);

        int c = 0;
        int coonsPatchStart = -1;
        for (std::vector<unsigned int>::const_iterator it2 = it->indices.begin(); it2!=it->indices.end(); ++it2) {
            if (c == 0) {
                cairo_mesh_pattern_begin_patch(mesh);
                cairo_mesh_pattern_move_to(mesh, inArgs.bezierPolygonJoined[*it2].x, inArgs.bezierPolygonJoined[*it2].y);
                coonsPatchStart = *it2;
            } else {
                cairo_mesh_pattern_line_to(mesh, inArgs.bezierPolygonJoined[*it2].x, inArgs.bezierPolygonJoined[*it2].y);
            }
            if (c == 2) {
                assert(coonsPatchStart);
                // close coons patch by transforming the triangle into a degenerated coons patch
                cairo_mesh_pattern_line_to(mesh, inArgs.bezierPolygonJoined[coonsPatchStart].x, inArgs.bezierPolygonJoined[coonsPatchStart].y);
                // IMPORTANT NOTE:
                // The two sqrt below are due to a probable cairo bug.
                // To check wether the bug is present is a given cairo version,
                // make any shape with a very large feather and set
                // opacity to 0.5. Then, zoom on the polygon border to check if the intensity is continuous
                // and approximately equal to 0.5.
                // If the bug if ixed in cairo, please use #if CAIRO_VERSION>xxx to keep compatibility with
                // older Cairo versions.
                cairo_mesh_pattern_set_corner_color_rgba(mesh, 0, shapeColor.r, shapeColor.g, shapeColor.b, 1);
                cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, shapeColor.r, shapeColor.g, shapeColor.b, 1);
                cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, shapeColor.r, shapeColor.g, shapeColor.b, 1);
                cairo_mesh_pattern_set_corner_color_rgba(mesh, 3, shapeColor.r, shapeColor.g, shapeColor.b, 1);
                assert(cairo_pattern_status(mesh) == CAIRO_STATUS_SUCCESS);

                cairo_mesh_pattern_end_patch(mesh);
                c = 0;
            } else {
                ++c;
            }
        }
    }
    for (std::vector<RotoBezierTriangulation::RotoTriangleFans>::const_iterator it = inArgs.internalFans.begin(); it!=inArgs.internalFans.end(); ++it ) {

        assert(it->indices.size() >= 3);
        std::vector<unsigned int>::const_iterator cur = it->indices.begin();
        unsigned int fanStart = *cur;
        ++cur;
        std::vector<unsigned int>::const_iterator next = cur;
        ++next;
        for (;next != it->indices.end();) {
            cairo_mesh_pattern_begin_patch(mesh);
            assert(fanStart < inArgs.bezierPolygonJoined.size() && *cur < inArgs.bezierPolygonJoined.size() && *next < inArgs.bezierPolygonJoined.size());
            const ParametricPoint &p0 = inArgs.bezierPolygonJoined[fanStart];
            const ParametricPoint &p3 = p0;
            const ParametricPoint &p1 = inArgs.bezierPolygonJoined[*cur];
            const ParametricPoint &p2 = inArgs.bezierPolygonJoined[*next];
            cairo_mesh_pattern_move_to(mesh, p0.x, p0.y);
            cairo_mesh_pattern_line_to(mesh, p1.x, p1.y);
            cairo_mesh_pattern_line_to(mesh, p2.x, p2.y);
            cairo_mesh_pattern_line_to(mesh, p3.x, p3.y);
            // IMPORTANT NOTE:
            // The two sqrt below are due to a probable cairo bug.
            // To check wether the bug is present is a given cairo version,
            // make any shape with a very large feather and set
            // opacity to 0.5. Then, zoom on the polygon border to check if the intensity is continuous
            // and approximately equal to 0.5.
            // If the bug if ixed in cairo, please use #if CAIRO_VERSION>xxx to keep compatibility with
            // older Cairo versions.
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 0, shapeColor.r, shapeColor.g, shapeColor.b, 1);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, shapeColor.r, shapeColor.g, shapeColor.b, 1);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, shapeColor.r, shapeColor.g, shapeColor.b, 1);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 3, shapeColor.r, shapeColor.g, shapeColor.b, 1);
            assert(cairo_pattern_status(mesh) == CAIRO_STATUS_SUCCESS);

            cairo_mesh_pattern_end_patch(mesh);

            ++next;
            ++cur;
        }
    }
    for (std::vector<RotoBezierTriangulation::RotoTriangleStrips>::const_iterator it = inArgs.internalStrips.begin(); it!=inArgs.internalStrips.end(); ++it ) {

        assert(it->indices.size() >= 3);

        std::vector<unsigned int>::const_iterator cur = it->indices.begin();
        unsigned int prevPrev = *cur;
        ++cur;
        unsigned int prev = *cur;
        ++cur;
        for (; cur != it->indices.end(); ++cur) {
            assert(prevPrev < inArgs.bezierPolygonJoined.size() && prev < inArgs.bezierPolygonJoined.size() && *cur < inArgs.bezierPolygonJoined.size());
            cairo_mesh_pattern_begin_patch(mesh);
            const ParametricPoint &p0 = inArgs.bezierPolygonJoined[prevPrev];
            const ParametricPoint &p3 = p0;
            const ParametricPoint &p1 = inArgs.bezierPolygonJoined[prev];
            const ParametricPoint &p2 = inArgs.bezierPolygonJoined[*cur];
            cairo_mesh_pattern_move_to(mesh, p0.x, p0.y);
            cairo_mesh_pattern_line_to(mesh, p1.x, p1.y);
            cairo_mesh_pattern_line_to(mesh, p2.x, p2.y);
            cairo_mesh_pattern_line_to(mesh, p3.x, p3.y);
            // IMPORTANT NOTE:
            // The two sqrt below are due to a probable cairo bug.
            // To check wether the bug is present is a given cairo version,
            // make any shape with a very large feather and set
            // opacity to 0.5. Then, zoom on the polygon border to check if the intensity is continuous
            // and approximately equal to 0.5.
            // If the bug if ixed in cairo, please use #if CAIRO_VERSION>xxx to keep compatibility with
            // older Cairo versions.
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 0, shapeColor.r, shapeColor.g, shapeColor.b, 1);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, shapeColor.r, shapeColor.g, shapeColor.b, 1);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, shapeColor.r, shapeColor.g, shapeColor.b, 1);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 3, shapeColor.r, shapeColor.g, shapeColor.b, 1);
            assert(cairo_pattern_status(mesh) == CAIRO_STATUS_SUCCESS);

            cairo_mesh_pattern_end_patch(mesh);

            prevPrev = prev;
            prev = *cur;
        }
    }
} // RotoShapeRenderCairo::renderInternalShape_cairo


void
RotoShapeRenderCairo::renderInternalShape_old_cairo(TimeValue time,
                                                  const RenderScale &scale,
                                                  const ColorRgbaD& shapeColor,
                                                  double /*opacity*/,
                                                  const Transform::Matrix3x3& transform,
                                                  cairo_t* cr,
#ifdef ROTO_USE_MESH_PATTERN_ONLY
                                                  cairo_pattern_t* mesh,
#else
                                                  cairo_pattern_t* /*mesh*/,
#endif
                                                  const BezierCPs & cps)
{
    assert( !cps.empty() );
#ifdef ROTO_USE_MESH_PATTERN_ONLY
    std::list<BezierCPs> coonPatches;
    bezulate(time, cps, &coonPatches);

    for (std::list<BezierCPs>::iterator it = coonPatches.begin(); it != coonPatches.end(); ++it) {
        std::list<BezierCPs> fixedPatch;
        CoonsRegularization::regularize(*it, time, &fixedPatch);
        for (std::list<BezierCPs>::iterator it2 = fixedPatch.begin(); it2 != fixedPatch.end(); ++it2) {
            std::size_t size = it2->size();
            assert(size <= 4 && size >= 2);

            BezierCPs::iterator patchIT = it2->begin();
            BezierCPPtr p0ptr, p1ptr, p2ptr, p3ptr;
            p0ptr = *patchIT;
            ++patchIT;
            if (size == 2) {
                p1ptr = p0ptr;
                p2ptr = *patchIT;
                p3ptr = p2ptr;
            } else if (size == 3) {
                p1ptr = *patchIT;
                p2ptr = *patchIT;
                ++patchIT;
                p3ptr = *patchIT;
            } else if (size == 4) {
                p1ptr = *patchIT;
                ++patchIT;
                p2ptr = *patchIT;
                ++patchIT;
                p3ptr = *patchIT;
            }
            assert(p0ptr && p1ptr && p2ptr && p3ptr);

            Point p0, p0p1, p1p0, p1, p1p2, p2p1, p2p3, p3p2, p2, p3, p3p0, p0p3;

            p0ptr->getLeftBezierPointAtTime(time, &p0p3.x, &p0p3.y);
            p0ptr->getPositionAtTime(time, &p0.x, &p0.y);
            p0ptr->getRightBezierPointAtTime(time, &p0p1.x, &p0p1.y);

            p1ptr->getLeftBezierPointAtTime(time, &p1p0.x, &p1p0.y);
            p1ptr->getPositionAtTime(time, &p1.x, &p1.y);
            p1ptr->getRightBezierPointAtTime(time, &p1p2.x, &p1p2.y);

            p2ptr->getLeftBezierPointAtTime(time, &p2p1.x, &p2p1.y);
            p2ptr->getPositionAtTime(time, &p2.x, &p2.y);
            p2ptr->getRightBezierPointAtTime(time, &p2p3.x, &p2p3.y);

            p3ptr->getLeftBezierPointAtTime(time, &p3p2.x, &p3p2.y);
            p3ptr->getPositionAtTime(time, &p3.x, &p3.y);
            p3ptr->getRightBezierPointAtTime(time, &p3p0.x, &p3p0.y);


            adjustToPointToScale(scale, p0.x, p0.y);
            adjustToPointToScale(scale, p0p1.x, p0p1.y);
            adjustToPointToScale(scale, p1p0.x, p1p0.y);
            adjustToPointToScale(scale, p1.x, p1.y);
            adjustToPointToScale(scale, p1p2.x, p1p2.y);
            adjustToPointToScale(scale, p2p1.x, p2p1.y);
            adjustToPointToScale(scale, p2.x, p2.y);
            adjustToPointToScale(scale, p2p3.x, p2p3.y);
            adjustToPointToScale(scale, p3p2.x, p3p2.y);
            adjustToPointToScale(scale, p3.x, p3.y);
            adjustToPointToScale(scale, p3p0.x, p3p0.y);
            adjustToPointToScale(scale, p0p3.x, p0p3.y);

            // Add a Coons patch such as:

            //         C1  Side 1   C2
            //        +---------------+
            //        |               |
            //        |  P1       P2  |
            //        |               |
            // Side 0 |               | Side 2
            //        |               |
            //        |               |
            //        |  P0       P3  |
            //        |               |
            //        +---------------+
            //        C0     Side 3   C3

            // In the above drawing, C0 is p0, P0 is p0p1, P1 is p1p0, C1 is p1 and so on...

            ///move to C0
            cairo_mesh_pattern_begin_patch(mesh);
            cairo_mesh_pattern_move_to(mesh, p0.x, p0.y);
            if (size == 4) {
                cairo_mesh_pattern_curve_to(mesh, p0p1.x, p0p1.y, p1p0.x, p1p0.y, p1.x, p1.y);
                cairo_mesh_pattern_curve_to(mesh, p1p2.x, p1p2.y, p2p1.x, p2p1.y, p2.x, p2.y);
                cairo_mesh_pattern_curve_to(mesh, p2p3.x, p2p3.y, p3p2.x, p3p2.y, p3.x, p3.y);
                cairo_mesh_pattern_curve_to(mesh, p3p0.x, p3p0.y, p0p3.x, p0p3.y, p0.x, p0.y);
            } else if (size == 3) {
                cairo_mesh_pattern_curve_to(mesh, p0p1.x, p0p1.y, p1p0.x, p1p0.y, p1.x, p1.y);
                cairo_mesh_pattern_line_to(mesh, p2.x, p2.y);
                cairo_mesh_pattern_curve_to(mesh, p2p3.x, p2p3.y, p3p2.x, p3p2.y, p3.x, p3.y);
                cairo_mesh_pattern_curve_to(mesh, p3p0.x, p3p0.y, p0p3.x, p0p3.y, p0.x, p0.y);
            } else {
                assert(size == 2);
                cairo_mesh_pattern_line_to(mesh, p1.x, p1.y);
                cairo_mesh_pattern_curve_to(mesh, p1p2.x, p1p2.y, p2p1.x, p2p1.y, p2.x, p2.y);
                cairo_mesh_pattern_line_to(mesh, p3.x, p3.y);
                cairo_mesh_pattern_curve_to(mesh, p3p0.x, p3p0.y, p0p3.x, p0p3.y, p0.x, p0.y);
            }
            ///Set the 4 corners color

            // IMPORTANT NOTE:
            // The two sqrt below are due to a probable cairo bug.
            // To check wether the bug is present is a given cairo version,
            // make any shape with a very large feather and set
            // opacity to 0.5. Then, zoom on the polygon border to check if the intensity is continuous
            // and approximately equal to 0.5.
            // If the bug if ixed in cairo, please use #if CAIRO_VERSION>xxx to keep compatibility with
            // older Cairo versions.
            cairo_mesh_pattern_set_corner_color_rgba( mesh, 0, shapeColor.r, shapeColor.g, shapeColor.b,
                                                     std::sqrt(opacity) );
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, shapeColor.r, shapeColor.g, shapeColor.b,
                                                     std::sqrt(opacity) );
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, shapeColor.r, shapeColor.g, shapeColor.b,
                                                     std::sqrt(opacity) );
            cairo_mesh_pattern_set_corner_color_rgba( mesh, 3, shapeColor.r, shapeColor.g, shapeColor.b,
                                                     std::sqrt(opacity) );
            assert(cairo_pattern_status(mesh) == CAIRO_STATUS_SUCCESS);

            cairo_mesh_pattern_end_patch(mesh);
        }
    }
#else // ifdef ROTO_USE_MESH_PATTERN_ONLY
    Q_UNUSED(shapeColor);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);

    BezierCPs::const_iterator point = cps.begin();
    assert( point != cps.end() );
    if ( point == cps.end() ) {
        return;
    }
    BezierCPs::const_iterator nextPoint = point;
    if ( nextPoint != cps.end() ) {
        ++nextPoint;
    }


    Transform::Point3D initCp;
    (*point)->getPositionAtTime(time,  &initCp.x, &initCp.y);
    initCp.z = 1.;
    initCp = Transform::matApply(transform, initCp);

    adjustToPointToScale(scale, initCp.x, initCp.y);

    cairo_move_to(cr, initCp.x, initCp.y);

    while ( point != cps.end() ) {
        if ( nextPoint == cps.end() ) {
            nextPoint = cps.begin();
        }

        Transform::Point3D right, nextLeft, next;
        (*point)->getRightBezierPointAtTime(time,  &right.x, &right.y);
        right.z = 1;
        (*nextPoint)->getLeftBezierPointAtTime(time,  &nextLeft.x, &nextLeft.y);
        nextLeft.z = 1;
        (*nextPoint)->getPositionAtTime(time,  &next.x, &next.y);
        next.z = 1;

        right = Transform::matApply(transform, right);
        nextLeft = Transform::matApply(transform, nextLeft);
        next = Transform::matApply(transform, next);

        adjustToPointToScale(scale, right.x, right.y);
        adjustToPointToScale(scale, next.x, next.y);
        adjustToPointToScale(scale, nextLeft.x, nextLeft.y);
        cairo_curve_to(cr, right.x, right.y, nextLeft.x, nextLeft.y, next.x, next.y);

        // increment for next iteration
        ++point;
        if ( nextPoint != cps.end() ) {
            ++nextPoint;
        }
    } // while()
    //    if (cairo_get_antialias(cr) != CAIRO_ANTIALIAS_NONE ) {
    //        cairo_fill_preserve(cr);
    //        // These line properties make for a nicer looking polygon mesh
    //        cairo_set_line_join(cr, CAIRO_LINE_JOIN_BEVEL);
    //        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    //        // Comment out the following call to cairo_set_line width
    //        // since the hard-coded width value of 1.0 is not appropriate
    //        // for fills of small areas. Instead, use the line width that
    //        // has already been set by the user via the above call of
    //        // poly_line which in turn calls set_current_context which in
    //        // turn calls cairo_set_line_width for the user-specified
    //        // width.
    //        cairo_set_line_width(cr, 1.0);
    //        cairo_stroke(cr);
    //    } else {
    cairo_fill(cr);
    //    }
#endif // ifdef ROTO_USE_MESH_PATTERN_ONLY
} // RotoShapeRenderCairo::renderInternalShape_old_cairo


void
RotoShapeRenderCairo::renderMaskInternal_cairo(const RotoDrawableItemPtr& rotoItem,
                                               const RectI & roi,
                                               const ImagePlaneDesc& components,
                                               const TimeValue time,
                                               ViewIdx view,
                                               const RangeD& shutterRange,
                                               int nDivisions,
                                               const RenderScale &scale,
                                               const bool isDuringPainting,
                                               const double distToNextIn,
                                               const Point& lastCenterPointIn,
                                               const ImagePtr &dstImage,
                                               double* distToNextOut,
                                               Point* lastCenterPointOut)
{

    //NodePtr node = rotoItem->getContext()->getNode();
    RotoStrokeItemPtr isStroke = toRotoStrokeItem(rotoItem);
    BezierPtr isBezier = toBezier(rotoItem);
    cairo_format_t cairoImgFormat;
    int srcNComps;
    bool doBuildUp = true;

    if (isStroke) {
        doBuildUp = isStroke->getBuildupKnob()->getValueAtTime(time, DimIdx(0), view);
        //For the non build-up case, we use the LIGHTEN compositing operator, which only works on colors
        if ( !doBuildUp || (components.getNumComponents() > 1) ) {
            cairoImgFormat = CAIRO_FORMAT_ARGB32;
            srcNComps = 4;
        } else {
            cairoImgFormat = CAIRO_FORMAT_A8;
            srcNComps = 1;
        }
    } else {
        cairoImgFormat = CAIRO_FORMAT_A8;
        srcNComps = 1;
    }

    assert(rotoItem->isActivated(time, view));

    double interval = nDivisions >= 1 ? (shutterRange.max - shutterRange.min) / nDivisions : 1.;
    for (int d = 0; d < nDivisions; ++d) {

        const TimeValue t = nDivisions > 1 ? TimeValue(shutterRange.min + d * interval) : time;

        std::list<std::list<std::pair<Point, double> > > strokes;
        if (isStroke) {
            isStroke->evaluateStroke(scale, t, view, &strokes, 0);
            if (strokes.empty()) {
                continue;
            }

        } else if (isBezier && isBezier->isOpenBezier()) {
            std::vector<std::vector< ParametricPoint> > decastelJauPolygon;
            isBezier->evaluateAtTime_DeCasteljau_autoNbPoints(t, view, scale, &decastelJauPolygon, 0);
            std::list<std::pair<Point, double> > points;
            for (std::vector<std::vector< ParametricPoint> > ::iterator it = decastelJauPolygon.begin(); it != decastelJauPolygon.end(); ++it) {
                for (std::vector< ParametricPoint>::iterator it2 = it->begin(); it2 != it->end(); ++it2) {
                    Point p = {it2->x, it2->y};
                    points.push_back( std::make_pair(p, 1.) );
                }
            }
            if ( !points.empty() ) {
                strokes.push_back(points);
            }
            if (strokes.empty()) {
                continue;
            }

        }



        ColorRgbaD shapeColor;
        {
            KnobColorPtr colorKnob = rotoItem->getColorKnob();
            shapeColor.r = colorKnob->getValueAtTime(t, DimIdx(0), view);
            shapeColor.g = colorKnob->getValueAtTime(t, DimIdx(1), view);
            shapeColor.b = colorKnob->getValueAtTime(t, DimIdx(2), view);
            shapeColor.a = colorKnob->getValueAtTime(t, DimIdx(3), view);
        }


        double opacity = rotoItem->getOpacityKnob()->getValueAtTime(t, DimIdx(0), view);

        Image::CPUData imageData;
        dstImage->getCPUData(&imageData);



        ////Allocate the cairo temporary buffer
        CairoImageWrapper imgWrapper;

        RamBuffer<unsigned char> buf;
        if (isDuringPainting) {

            std::size_t stride = cairo_format_stride_for_width( cairoImgFormat, roi.width() );
            std::size_t memSize = stride * roi.height();
            buf.resize(memSize);
            std::memset(buf.getData(), 0, sizeof(unsigned char) * memSize);


            convertNatronImageToCairoImage<float, 1>(buf.getData(), srcNComps, stride, imageData, roi, roi, shapeColor);
            imgWrapper.cairoImg = cairo_image_surface_create_for_data(buf.getData(), cairoImgFormat, roi.width(), roi.height(),
                                                                      stride);
        } else {
            imgWrapper.cairoImg = cairo_image_surface_create( cairoImgFormat, roi.width(), roi.height() );
        }

        if (cairo_surface_status(imgWrapper.cairoImg) != CAIRO_STATUS_SUCCESS) {
            return;
        }
        cairo_surface_set_device_offset(imgWrapper.cairoImg, -roi.x1, -roi.y1);
        imgWrapper.ctx = cairo_create(imgWrapper.cairoImg);
        //cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD); // creates holes on self-overlapping shapes
        cairo_set_fill_rule(imgWrapper.ctx, CAIRO_FILL_RULE_WINDING);

        // these Roto shapes must be rendered WITHOUT antialias, or the junction between the inner
        // polygon and the feather zone will have artifacts. This is partly due to the fact that cairo
        // meshes are not antialiased.
        // Use a default feather distance of 1 pixel instead!
        // UPDATE: unfortunately, this produces less artifacts, but there are still some remaining (use opacity=0.5 to test)
        // maybe the inner polygon should be made of mesh patterns too?
        cairo_set_antialias(imgWrapper.ctx, CAIRO_ANTIALIAS_NONE);


        assert(isStroke || isBezier);
        if ( isStroke  || ( isBezier && isBezier->isOpenBezier() ) ) {
            std::vector<cairo_pattern_t*> dotPatterns;
            if (isDuringPainting && isStroke) {
                dotPatterns = isStroke->getPatternCache();
            }
            if ( dotPatterns.empty() ) {
                dotPatterns.resize(ROTO_PRESSURE_LEVELS);
                for (std::size_t i = 0; i < dotPatterns.size(); ++i) {
                    dotPatterns[i] = (cairo_pattern_t*)0;
                }
            }

            RotoShapeRenderCairo::renderStroke_cairo(imgWrapper.ctx, dotPatterns, strokes, distToNextIn, lastCenterPointIn, isStroke, doBuildUp, opacity, t, view, scale, distToNextOut, lastCenterPointOut);


            if (isDuringPainting) {
                if (isStroke) {
                    isStroke->updatePatternCache(dotPatterns);
                }
            } else {
                for (std::size_t i = 0; i < dotPatterns.size(); ++i) {
                    if (dotPatterns[i]) {
                        cairo_pattern_destroy(dotPatterns[i]);
                        dotPatterns[i] = 0;
                    }
                }
            }
        } else {
            RotoShapeRenderCairo::renderBezier_cairo(imgWrapper.ctx, isBezier, opacity, t, view, scale);
        }

        bool useOpacityToConvert = (isBezier != 0);


        assert(cairo_surface_status(imgWrapper.cairoImg) == CAIRO_STATUS_SUCCESS);

        ///A call to cairo_surface_flush() is required before accessing the pixel data
        ///to ensure that all pending drawing operations are finished.
        cairo_surface_flush(imgWrapper.cairoImg);

        // When motion blur is enabled, divide by the number of samples for the last sample.
        int nDivisionsToApply = nDivisions > 1 && d == nDivisions - 1 ? nDivisions : 0;

        // Accumulate if there's more than one sample and we are not at the first sample.
        bool doAccumulation = nDivisions > 1 && d > 0;

        convertCairoImageToNatronImage_noColor(imgWrapper.cairoImg, srcNComps, imageData, roi, shapeColor, opacity, false, useOpacityToConvert, doAccumulation, nDivisionsToApply);
    } // for all divisions
} // RotoShapeRenderNodePrivate::renderMaskInternal_cairo

void
RotoShapeRenderCairo::purgeCaches_cairo_internal(std::vector<cairo_pattern_t*>& cache)
{
    for (std::size_t i = 0; i < cache.size(); ++i) {
        if (cache[i]) {
            cairo_pattern_destroy(cache[i]);
            cache[i] = 0;
        }
    }
    cache.clear();
}

void
RotoShapeRenderCairo::purgeCaches_cairo(const RotoDrawableItemPtr& rotoItem)
{
    RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(rotoItem.get());
    if (isStroke) {
        std::vector<cairo_pattern_t*> dotPatterns = isStroke->getPatternCache();
        purgeCaches_cairo_internal(dotPatterns);
        isStroke->updatePatternCache(dotPatterns);
    }
}
NATRON_NAMESPACE_EXIT;

#endif //ROTO_SHAPE_RENDER_ENABLE_CAIRO
