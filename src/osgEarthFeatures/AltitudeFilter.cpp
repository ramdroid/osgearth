/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2012 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarthFeatures/AltitudeFilter>
#include <osgEarth/ElevationQuery>
#include <osgEarth/GeoData>

#define LC "[AltitudeFilter] "

using namespace osgEarth;
using namespace osgEarth::Features;
using namespace osgEarth::Symbology;

//---------------------------------------------------------------------------

AltitudeFilter::AltitudeFilter() :
_maxRes ( 0.0f )
{
    //NOP
}

void
AltitudeFilter::setPropertiesFromStyle( const Style& style )
{
    _altitude = style.get<AltitudeSymbol>();
    if ( _altitude )
    {
        setMaxResolution( *_altitude->clampingResolution() );
    }
}

FilterContext
AltitudeFilter::push( FeatureList& features, FilterContext& cx )
{
    bool clamp = 
        _altitude.valid() && 
        _altitude->clamping() != AltitudeSymbol::CLAMP_NONE &&
        cx.getSession()       != 0L &&
        cx.profile()          != 0L;

    if ( clamp )
        pushAndClamp( features, cx );
    else
        pushAndDontClamp( features, cx );

    return cx;
}

void
AltitudeFilter::pushAndDontClamp( FeatureList& features, FilterContext& cx )
{
    NumericExpression scaleExpr;
    if ( _altitude.valid() && _altitude->verticalScale().isSet() )
        scaleExpr = *_altitude->verticalScale();

    NumericExpression offsetExpr;
    if ( _altitude.valid() && _altitude->verticalOffset().isSet() )
        offsetExpr = *_altitude->verticalOffset();

    for( FeatureList::iterator i = features.begin(); i != features.end(); ++i )
    {
        Feature* feature = i->get();

        double minHAT       =  DBL_MAX;
        double maxHAT       = -DBL_MAX;

        double scaleZ = 1.0;
        if ( _altitude.valid() && _altitude->verticalScale().isSet() )
            scaleZ = feature->eval( scaleExpr );

        double offsetZ = 0.0;
        if ( _altitude.valid() && _altitude->verticalOffset().isSet() )
            offsetZ = feature->eval( offsetExpr );
        
        GeometryIterator gi( feature->getGeometry() );
        while( gi.hasMore() )
        {
            Geometry* geom = gi.next();
            for( Geometry::iterator g = geom->begin(); g != geom->end(); ++g )
            {
                g->z() *= scaleZ;
                g->z() += offsetZ;

                if ( g->z() < minHAT )
                    minHAT = g->z();
                if ( g->z() > maxHAT )
                    maxHAT = g->z();
            }
        }

        if ( minHAT != DBL_MAX )
        {
            feature->set( "__min_hat", minHAT );
            feature->set( "__max_hat", maxHAT );
        }
    }
}

void
AltitudeFilter::pushAndClamp( FeatureList& features, FilterContext& cx )
{
    const Session* session = cx.getSession();

    // the map against which we'll be doing elevation clamping
    //MapFrame mapf = session->createMapFrame( Map::ELEVATION_LAYERS );
    MapFrame mapf = session->createMapFrame( Map::TERRAIN_LAYERS );

    const SpatialReference* mapSRS = mapf.getProfile()->getSRS();
    osg::ref_ptr<const SpatialReference> featureSRS = cx.profile()->getSRS();

    // establish an elevation query interface based on the features' SRS.
    ElevationQuery eq( mapf );

    NumericExpression scaleExpr;
    if ( _altitude->verticalScale().isSet() )
        scaleExpr = *_altitude->verticalScale();

    NumericExpression offsetExpr;
    if ( _altitude->verticalOffset().isSet() )
        offsetExpr = *_altitude->verticalOffset();

    // whether to record the min/max height-above-terrain values.
    bool collectHATs =
        _altitude->clamping() == AltitudeSymbol::CLAMP_RELATIVE_TO_TERRAIN ||
        _altitude->clamping() == AltitudeSymbol::CLAMP_ABSOLUTE;

    // whether the SRS's have a compatible vertical datum.
    bool vertEquiv =
        featureSRS->isVertEquivalentTo( mapSRS );

    for( FeatureList::iterator i = features.begin(); i != features.end(); ++i )
    {
        Feature* feature = i->get();
        //double maxGeomZ     = -DBL_MAX;
        //double minGeomZ     =  DBL_MAX;
        double maxTerrainZ  = -DBL_MAX;
        double minTerrainZ  =  DBL_MAX;
        double minHAT       =  DBL_MAX;
        double maxHAT       = -DBL_MAX;

        double scaleZ = 1.0;
        if ( _altitude.valid() && _altitude->verticalScale().isSet() )
            scaleZ = feature->eval( scaleExpr, &cx );

        double offsetZ = 0.0;
        if ( _altitude.valid() && _altitude->verticalOffset().isSet() )
            offsetZ = feature->eval( offsetExpr, &cx );
        
        GeometryIterator gi( feature->getGeometry() );
        while( gi.hasMore() )
        {
            Geometry* geom = gi.next();

            // Absolute heights in Z. Only need to collect the HATs; the geometry
            // remains unchanged.
            if ( _altitude->clamping() == AltitudeSymbol::CLAMP_ABSOLUTE )
            {
                std::vector<double> elevations;
                elevations.reserve( geom->size() );

                if ( eq.getElevations( geom->asVector(), featureSRS, elevations, _maxRes ) )
                {
                    for( unsigned i=0; i<geom->size(); ++i )
                    {
                        osg::Vec3d& p = (*geom)[i];
                        double z = p.z();

                        if ( !vertEquiv )
                        {
                            osg::Vec3d tempgeo;
                            if ( !featureSRS->transform(p, mapSRS->getGeographicSRS(), tempgeo) )
                                z = tempgeo.z();
                        }

                        double hat = z - elevations[i];

                        if ( hat > maxHAT )
                            maxHAT = hat;
                        if ( hat < minHAT )
                            minHAT = hat;

                        if ( elevations[i] > maxTerrainZ )
                            maxTerrainZ = elevations[i];
                        if ( elevations[i] < minTerrainZ )
                            minTerrainZ = elevations[i];
                    }
                }
            }

            // Heights-above-ground in Z. Need to resolve this to an absolute number
            // and record HATs along the way.
            else if ( _altitude->clamping() == AltitudeSymbol::CLAMP_RELATIVE_TO_TERRAIN )
            {
                osg::ref_ptr<const SpatialReference> featureSRSwithMapVertDatum = !vertEquiv ?
                    SpatialReference::create(featureSRS->getHorizInitString(), mapSRS->getVertInitString()) : 0L;

                std::vector<double> elevations;
                elevations.reserve( geom->size() );

                if ( eq.getElevations( geom->asVector(), featureSRS, elevations, _maxRes ) )
                {
                    for( unsigned i=0; i<geom->size(); ++i )
                    {
                        osg::Vec3d& p = (*geom)[i];

                        double hat = p.z();
                        p.z() = elevations[i] + p.z();

                        // if necessary, convert the Z value (which is now in the map's SRS) back to
                        // the feature's SRS.
                        if ( !vertEquiv )
                        {
                            featureSRSwithMapVertDatum->transform(p, featureSRS, p);
                        }

                        if ( hat > maxHAT )
                            maxHAT = hat;
                        if ( hat < minHAT )
                            minHAT = hat;

                        if ( elevations[i] > maxTerrainZ )
                            maxTerrainZ = elevations[i];
                        if ( elevations[i] < minTerrainZ )
                            minTerrainZ = elevations[i];
                    }
                }

            }

            // Clamp - replace the geometry's Z with the terrain height.
            else // CLAMP_TO_TERRAIN
            {
                eq.getElevations( geom->asVector(), featureSRS, true, _maxRes );
                
                // if necessary, transform the Z values (which are now in the map SRS) back
                // into the feature's SRS.
                if ( !vertEquiv )
                {
                    osg::ref_ptr<const SpatialReference> featureSRSwithMapVertDatum = !vertEquiv ?
                        SpatialReference::create(featureSRS->getHorizInitString(), mapSRS->getVertInitString()) : 0L;

                    osg::Vec3d tempgeo;
                    for( unsigned i=0; i<geom->size(); ++i )
                    {
                        osg::Vec3d& p = (*geom)[i];
                        featureSRSwithMapVertDatum->transform(p, featureSRS, p);
                    }
                }
            }

            if ( !collectHATs )
            {
                for( Geometry::iterator i = geom->begin(); i != geom->end(); ++i )
                {
                    i->z() *= scaleZ;
                    i->z() += offsetZ;
                }
            }
        }

        if ( minHAT != DBL_MAX )
        {
            feature->set( "__min_hat", minHAT );
            feature->set( "__max_hat", maxHAT );
        }

        if ( minTerrainZ != DBL_MAX )
        {
            feature->set( "__min_terrain_z", minTerrainZ );
            feature->set( "__max_terrain_z", maxTerrainZ );
        }
    }
}
