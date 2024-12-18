///////////////////////////////////////////////////////////////////////////////
// OSG plugin for reading OGC CityGML v0.3 - v1.0 format using libcitygml
// http://code.google.com/p/libcitygml
// Copyright(c) 2010 Joachim Pouderoux, BRGM
//////////////////////////////////////////////////////////////////////////
#include "ReaderWriterCityGML.h"

#include <osg/Array>
#include <osg/Node>
#include <osg/Group>
#include <osg/Image>
#include <osg/MatrixTransform>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/StateSet>
#include <osg/BlendFunc>
#include <osg/BlendColor>
#include <osg/Material>
#include <osg/CullFace>
#include <osg/Texture2D>
#include <osg/TexGen>
#include <osg/TexMat>
#include <osg/Depth>
#include <osg/LightModel>
#include <osg/ValueObject>
#include <osg/ProxyNode>

#include <osg/ref_ptr>
#include <osgText/Font>
#include <osgText/Text>

#include <osgDB/Registry>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgDB/FileUtils>
#include <osgDB/FileNameUtils>

#include <osgUtil/SmoothingVisitor>
#include <osgUtil/Optimizer>

#include <osg/Notify>

#include <citygml/citygml.h>
#include <citygml/envelope.h>
#include <citygml/citymodel.h>
#include <citygml/cityobject.h>
#include <citygml/geometry.h>
#include <citygml/polygon.h>
#include <citygml/material.h>
#include <citygml/texture.h>
#include <citygml/tesselator.h>

#include <cctype>

// Register with Registry to instantiate the reader/writer.
REGISTER_OSGPLUGIN( citygml, ReaderWriterCityGML )

ReaderWriterCityGML::ReaderWriterCityGML()
{
    supportsExtension( "citygml", "CityGML format" );
    supportsExtension( "gml", "CityGML format" );

    supportsOption( "names", "Add the name of the city objects on top of them" );
    supportsOption( "separateBuildingParts", "Separates building into all its building parts" );
    supportsOption( "mask", "Set the objects mask" );
    supportsOption( "minLOD", "Minimum LOD level to fetch" );
    supportsOption( "maxLOD", "Maximum LOD level to fetch" );
    supportsOption( "optimize", "Optimize the geometries & polygons of the CityGML model to reduce the number of instanced objects" );
    supportsOption( "optimizeRoot", "Optimize created root node." );
    supportsOption( "pruneEmptyObjects", "Prune empty objects (ie. without -supported- geometry)" );
    supportsOption( "destSRS", "Transform geometry to given reference system" );
    supportsOption( "useMaxLODonly", "Use the highest available LOD for geometry of one object" );
    supportsOption( "appearanceTheme", "Name of the appearance theme to use" );
    supportsOption( "storegeomids", "Store the citygml id of geometry objects in the corresponding osg::Geometry object as a description string." );

    m_logger = std::make_shared<CityGMLOSGPluginLogger>();
}

void ReaderWriterCityGML::optimizeNode(osg::Node *node) const
{
    osg::notify(osg::NOTICE) << "Optimizing..." << std::endl;
    osgUtil::Optimizer optimizer;
    optimizer.optimize(node, osgUtil::Optimizer::ALL_OPTIMIZATIONS);
    osg::notify(osg::NOTICE) << "Finished optimizing." << std::endl;
}

// Read CityGML file using libcitygml and generate the OSG scenegraph
osgDB::ReaderWriter::ReadResult ReaderWriterCityGML::readNode( const std::string& file, const osgDB::ReaderWriter::Options* options ) const
{
    std::string ext = osgDB::getLowerCaseFileExtension( file );

    if ( !acceptsExtension( ext ) ) return ReadResult::FILE_NOT_HANDLED;

    // try to open the file as is
    std::string fileName = osgDB::findDataFile( file, options );

    if ( fileName.empty() )
    {
        // not found, so remove the .citygml extension file
        std::string fname = osgDB::getNameLessExtension( file );
        fileName = osgDB::findDataFile( fname, options );
        if ( fileName.empty() ) return ReadResult::FILE_NOT_FOUND;
    }

    CityGMLSettings settings;
    settings.parseOptions( options );

    osgDB::getDataFilePathList().push_front( osgDB::getFilePath( fileName ) );

    // Redirect both std::cout & std::cerr (used by libcitygml) to osg::notify stream
    std::streambuf* coutsb = std::cout.rdbuf( osg::notify(osg::NOTICE).rdbuf() );
    std::streambuf* cerrsb = std::cerr.rdbuf( osg::notify(osg::NOTICE).rdbuf() );

    osg::notify(osg::NOTICE) << "Parsing CityGML file " << fileName << "..." << std::endl;
#ifdef WIN32
    if (fileName[0] == '/')
    {
        //xerces on windows wants \ as a start to figure out that it is an absolute path
        fileName[0] = '\\';
        if(fileName[1]=='/')
            fileName[1] = '\\';
    }
#endif

    std::unique_ptr<TesselatorBase> tesselator = std::unique_ptr<TesselatorBase>(new Tesselator(nullptr));
    std::shared_ptr<const citygml::CityModel> city = citygml::load( fileName, settings._params, std::move(tesselator), m_logger );

    ReadResult rr = readCity( city, settings );

    if ( rr.status() == ReadResult::FILE_LOADED && rr.getNode() ) {
        rr.getNode()->setName( fileName );

        // Let osg calculate the normals
        osgUtil::SmoothingVisitor sv;
        sv.setCreaseAngle(osg::PI_2);
        rr.getNode()->accept(sv);
    }

    osgDB::getDataFilePathList().pop_front();

    // Restore cout/cerr streams
    std::cout.rdbuf( coutsb );
    std::cerr.rdbuf( cerrsb );

    if (settings._optimizeRoot) {
        optimizeNode(rr.getNode());
    }
    return rr;
}

osgDB::ReaderWriter::ReadResult ReaderWriterCityGML::readNode( std::istream& fin, const osgDB::ReaderWriter::Options* options ) const
{
    CityGMLSettings settings;
    settings.parseOptions( options );

    // Redirect both std::cout & std::cerr (used by CityGML parser) to osg::notify stream
    std::streambuf* coutsb = std::cout.rdbuf( osg::notify(osg::NOTICE).rdbuf() );
    std::streambuf* cerrsb = std::cerr.rdbuf( osg::notify(osg::NOTICE).rdbuf() );

    osg::notify(osg::NOTICE) << "Parsing CityGML stream..." << std::endl;
    std::unique_ptr<TesselatorBase> tesselator = std::unique_ptr<TesselatorBase>(new Tesselator(nullptr));
    std::shared_ptr<const citygml::CityModel> city = citygml::load( fin, settings._params, std::move(tesselator), m_logger);

    ReadResult rr = readCity( city, settings );

    // Restore cout/cerr streams
    std::cout.rdbuf( coutsb );
    std::cerr.rdbuf( cerrsb );

    if (settings._optimizeRoot) {
        optimizeNode(rr.getNode());
    }
    return rr;
}

void ReaderWriterCityGML::applyMaterialForSingleObject(MaterialArraysMap &matMap, osg::ref_ptr<osg::Geode> geode) const
{
    for (const auto& it : matMap)
    {
        auto arrays = it.second;
        if(arrays->vertices->size() > 0)
        {
			osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;
			geom->setVertexArray(arrays->vertices);
			osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES, arrays->indices.begin(), arrays->indices.end());
			geom->addPrimitiveSet(indices);

			// Appearance
			osg::ref_ptr<osg::StateSet> stateset = geom->getOrCreateStateSet();
			osg::ref_ptr<osg::Material> material = new osg::Material;
			material->setColorMode(osg::Material::OFF);
			if (it.first == "wall")
			{
				material->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4(0.9f, 0.9f, 0.9f, 1.0f));
			}
			else if (it.first == "roof")
			{
				material->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4(0.5f, 0.1f, 0.1f, 1.0f));
			}
			else // textured
			{
				material->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
                if (arrays->texture)
                {
                    if (arrays->texCoords->size() > 0)
                    {
                        geom->setTexCoordArray(0, arrays->texCoords);

						stateset->setTextureAttributeAndModes(0, arrays->texture, osg::StateAttribute::ON);
                    }
                }
			}
			material->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
			material->setEmission(osg::Material::FRONT_AND_BACK, osg::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
			material->setShininess(osg::Material::FRONT_AND_BACK, 128.f * 0.5f);
			material->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4(0.1f, 0.1f, 0.1f, 1.0f));
			stateset->setAttributeAndModes(material, osg::StateAttribute::ON);
			stateset->setMode(GL_LIGHTING, osg::StateAttribute::ON); 
            osg::ref_ptr<osg::CullFace> cullFace = new osg::CullFace();
			cullFace->setMode(osg::CullFace::BACK);
			stateset->setAttributeAndModes(cullFace, osg::StateAttribute::ON);
			geode->addDrawable(geom);
        }
        delete arrays;
    }
}

void ReaderWriterCityGML::handleCityAsSingleObject(CityGMLSettings& settings, const citygml::ConstCityObjects& roots, const osg::Vec3d& offset, osg::MatrixTransform* root) const
{
    osg::ref_ptr<osg::Geode> geode = new osg::Geode();
    MaterialArraysMap matMap;

    // Vertices
    matMap["wall"] = new MaterialArrays();
    matMap["roof"] = new MaterialArrays();

    for (unsigned int i = 0; i < roots.size(); ++i) 
        createSingleCityObject(*roots[i], settings, matMap, offset, root);
    
    applyMaterialForSingleObject(matMap, geode);

    matMap.clear();
    root->addChild(geode);
}

osgDB::ReaderWriter::ReadResult ReaderWriterCityGML::readCity(std::shared_ptr<const citygml::CityModel> city, CityGMLSettings& settings ) const
{
    if ( !city ) return nullptr;

    osg::notify(osg::NOTICE) << city->getNumRootCityObjects() << " city objects read." << std::endl;

    osg::notify(osg::NOTICE) << "Creation of the OSG city objects' geometry..." << std::endl;

    osg::MatrixTransform* root = new osg::MatrixTransform();
    root->setName( city->getId() );

    if (settings._theme.empty() && !city->themes().empty()) {
        settings._theme = city->themes().front();
    }

    const citygml::ConstCityObjects& roots = city->getRootCityObjects();

    if(roots.size() == 0) return nullptr;


    osg::Vec3d offset(0, 0, 0);
    if (city->getEnvelope().validBounds()) {
        TVec3d lb = city->getEnvelope().getLowerBound();
        offset = osg::Vec3d(lb.x, lb.y, lb.z);
    }
    else
    {
        for (unsigned int i = 0; i < roots.size(); ++i)
        {
            const citygml::CityObject& object = *roots[i];

            if (object.getEnvelope().validBounds()) {
                TVec3d lb = object.getEnvelope().getLowerBound();
                offset = osg::Vec3d(lb.x, lb.y, lb.z);
                break;
            }

            if (object.getGeometriesCount()>0)
            {
                const citygml::Geometry& geometry = object.getGeometry(0);

                if (geometry.getPolygonsCount() > 0)
                {
                    const citygml::Polygon& p = *geometry.getPolygon(0);
                    const std::vector<TVec3d>& vert = p.getVertices();

                    TVec3d v = vert[0];
                    offset = osg::Vec3d(v.x, v.y, v.z);
                    break;
                }
            }
        }
    }

    if(settings._singleObject)
    {
        handleCityAsSingleObject(settings, roots, offset, root);
    } else {
        for (unsigned int i = 0; i < roots.size(); ++i) createCityObject(*roots[i], settings, root, offset);
    }

    root->setMatrix(osg::Matrixd::translate(offset));
    osg::notify(osg::NOTICE) << "Done creating creating scenegraph." << std::endl;
    return root;
}

void setTextureOnly(osg::ref_ptr<osg::StateSet> stateset, osg::Geometry* geom,CityGMLSettings& settings, std::shared_ptr<const citygml::Texture> citygmlTex) {

    if (!citygmlTex)
    {
        return;
    }
    osg::Texture2D* texture = nullptr;

    if (settings._textureMap.find(citygmlTex->getUrl()) == settings._textureMap.end()) {
        std::string fullPath = osgDB::findDataFile(citygmlTex->getUrl());

        if (fullPath.empty()) {
            osg::notify(osg::NOTICE) << "  Texture file " << citygmlTex->getUrl() << " not found..." << std::endl;
            return;
        }

        // Load a new texture
        osg::notify(osg::NOTICE) << "  Loading texture " << fullPath << "..." << std::endl;

        osg::Image* image = osgDB::readImageFile(citygmlTex->getUrl());

        if (!image) {
            osg::notify(osg::NOTICE) << "  Warning: Failed to read Texture " << fullPath << std::endl;
            return;
        }

        texture = new osg::Texture2D;
        texture->setImage(image);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        texture->setWrap(osg::Texture::WRAP_R, osg::Texture::REPEAT);

        settings._textureMap[citygmlTex->getUrl()] = texture;
    }
    else {
        texture = settings._textureMap[citygmlTex->getUrl()];
    }

    if (!texture)
    {
        return;
    }


    stateset->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);
    osg::CullFace* cullFace = new osg::CullFace();
    cullFace->setMode(osg::CullFace::BACK);
    stateset->setAttributeAndModes(cullFace, osg::StateAttribute::ON);
}

void setTexture(osg::ref_ptr<osg::StateSet> stateset, osg::Geometry* geom, const citygml::Polygon& polygon, CityGMLSettings& settings) {
    const auto citygmlTex = polygon.getTextureFor(settings._theme);

    if ( !citygmlTex )
    {
        return;
    }
    const std::vector<TVec2f>& texCoords = polygon.getTexCoordsForTheme(settings._theme, true);

    if (texCoords.empty()) {
        osg::notify(osg::WARN) << "Texture coordinates not found for poly " << polygon.getId() << std::endl;
    }

    osg::Texture2D* texture = nullptr;

    if ( settings._textureMap.find( citygmlTex->getUrl() ) == settings._textureMap.end() ) {
        std::string fullPath = osgDB::findDataFile(citygmlTex->getUrl());

        if (fullPath.empty()) {
            osg::notify(osg::NOTICE) << "  Texture file " << citygmlTex->getUrl() << " not found..." << std::endl;
            return;
        }

        // Load a new texture
        osg::notify(osg::NOTICE) << "  Loading texture " << fullPath << "..." << std::endl;

        osg::Image* image = osgDB::readImageFile( citygmlTex->getUrl() );

        if (!image) {
            osg::notify(osg::NOTICE) << "  Warning: Failed to read Texture " << fullPath << std::endl;
            return;
        }

        texture = new osg::Texture2D;
        texture->setImage( image );
        texture->setFilter( osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR );
        texture->setFilter( osg::Texture::MAG_FILTER, osg::Texture::NEAREST );
        texture->setWrap( osg::Texture::WRAP_S, osg::Texture::REPEAT );
        texture->setWrap( osg::Texture::WRAP_T, osg::Texture::REPEAT );
        texture->setWrap( osg::Texture::WRAP_R, osg::Texture::REPEAT );

        settings._textureMap[ citygmlTex->getUrl() ] = texture;
    }
    else {
        texture = settings._textureMap[ citygmlTex->getUrl() ];
    }

    if ( !texture )
    {
        return;
    }

    osg::ref_ptr<osg::Vec2Array> tex = new osg::Vec2Array;

    tex->reserve( texCoords.size() );
    for ( unsigned int k = 0; k < texCoords.size(); k++ )
        tex->push_back( osg::Vec2( texCoords[k].x, texCoords[k].y ) );

    geom->setTexCoordArray( 0, tex );

    stateset->setTextureAttributeAndModes( 0, texture, osg::StateAttribute::ON );
    osg::CullFace* cullFace = new osg::CullFace();
    cullFace->setMode(osg::CullFace::BACK);
    stateset->setAttributeAndModes(cullFace, osg::StateAttribute::ON);
}

void setMaterial(osg::ref_ptr<osg::StateSet> stateset, const citygml::Polygon& polygon, CityGMLSettings& settings) {

    const auto citygmlMaterial = polygon.getMaterialFor(settings._theme);

    if (!citygmlMaterial) {
        return;
    }

    TVec4f diffuse( citygmlMaterial->getDiffuse(), 0.f );
    TVec4f emissive( citygmlMaterial->getEmissive(), 0.f );
    TVec4f specular( citygmlMaterial->getSpecular(), 0.f );
    float ambient = citygmlMaterial->getAmbientIntensity();

    osg::Material* material = new osg::Material;
    material->setColorMode( osg::Material::OFF );
    material->setDiffuse( osg::Material::FRONT_AND_BACK, osg::Vec4(diffuse.r, diffuse.g, diffuse.b, diffuse.a ) );
    material->setSpecular( osg::Material::FRONT_AND_BACK, osg::Vec4(specular.r, specular.g, specular.b, specular.a ) );
    material->setEmission( osg::Material::FRONT_AND_BACK, osg::Vec4(emissive.r, emissive.g, emissive.b, emissive.a ) );
    material->setShininess( osg::Material::FRONT_AND_BACK, 128.f * citygmlMaterial->getShininess() );
    material->setAmbient( osg::Material::FRONT_AND_BACK, osg::Vec4( ambient, ambient, ambient, 1.0 ) );
    material->setTransparency( osg::Material::FRONT_AND_BACK, citygmlMaterial->getTransparency() );
    stateset->setAttributeAndModes( material, osg::StateAttribute::OVERRIDE | osg::StateAttribute::ON );
    stateset->setMode( GL_LIGHTING, osg::StateAttribute::OVERRIDE | osg::StateAttribute::ON );

    osg::CullFace *cullFace = new osg::CullFace();
    cullFace->setMode(osg::CullFace::BACK);
    stateset->setAttributeAndModes(cullFace, osg::StateAttribute::ON);
}


void createOsgGeometryFromCityGMLGeometry(const citygml::Geometry& geometry, CityGMLSettings& settings, osg::Geode* geometryContainer, const osg::Vec3d& offset ) {

    std::string texName = "axaxaxax";
    if (geometry.getPolygonsCount() > 0)
    {
        osg::Geometry* geom = new osg::Geometry;
        geom->setUserValue("cot_type", geometry.getTypeAsString());
        osg::Vec3Array* vertices = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec2Array> tex = new osg::Vec2Array;
        tex->reserve(3);
        vertices->reserve(3);
        std::vector<GLuint> indicesVec;
        indicesVec.reserve(2);

        for (unsigned int j = 0; j < geometry.getPolygonsCount(); j++)
        {
            const citygml::Polygon& p = *geometry.getPolygon(j);
            const auto citygmlTex = p.getTextureFor(settings._theme);

            if (p.getIndices().size() == 0) continue;

            // Geometry management

            if (texName != "axaxaxax" && (citygmlTex != nullptr && (citygmlTex->getUrl() != texName)) || (citygmlTex == nullptr && (texName != "")))
            {

                geom->setVertexArray(vertices);
                if (tex->size() > 0)
                    geom->setTexCoordArray(0, tex);

                // Indices
                osg::DrawElementsUInt* indices = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES, indicesVec.begin(), indicesVec.end());
                geom->addPrimitiveSet(indices);
                indicesVec.clear();

                // Appearance
                osg::ref_ptr<osg::StateSet> stateset = geom->getOrCreateStateSet();

                setMaterial(stateset, p, settings);
                setTextureOnly(stateset, geom, settings,citygmlTex);

#if OSG_VERSION_GREATER_OR_EQUAL(3,3,2)
                if (settings._storeGeomIDs) {
                    geom->addDescription(p.getId());
                }
#endif

                geometryContainer->addDrawable(geom);
                // create new Geometry
                geom = new osg::Geometry;
                geom->setUserValue("cot_type", geometry.getTypeAsString());
                vertices = new osg::Vec3Array;
                tex = new osg::Vec2Array;
                vertices->reserve(3);
                tex->reserve(3);

            }

            if (citygmlTex == nullptr)
                texName = "";
            else
				texName = citygmlTex->getUrl();
            geom->setName(p.getId());


            GLuint startIndex = vertices->size();
            for (const auto& i : p.getIndices())
            {
                indicesVec.push_back(i+ startIndex);
            }

            // Vertices
            const std::vector<TVec3d>& vert = p.getVertices();
            for (unsigned int k = 0; k < vert.size(); k++)
            {
                TVec3d v = vert[k];
                osg::Vec3d pt = osg::Vec3d(v.x, v.y, v.z) - offset;
                vertices->push_back(pt);
            }
            //texcoords
            const std::vector<TVec2f>& texCoords = p.getTexCoordsForTheme(settings._theme, true);

            if (!texCoords.empty()) {
                for (unsigned int k = 0; k < texCoords.size(); k++)
                    tex->push_back(osg::Vec2(texCoords[k].x, texCoords[k].y));
            }
        }
        if (vertices->size())
        {
            const citygml::Polygon& p = *geometry.getPolygon(geometry.getPolygonsCount() - 1);
            geom->setVertexArray(vertices);
            if (tex->size() > 0)
				geom->setTexCoordArray(0, tex);

			// Indices
			osg::DrawElementsUInt* indices = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES, indicesVec.begin(), indicesVec.end());
			geom->addPrimitiveSet(indices);

			// Appearance

			osg::ref_ptr<osg::StateSet> stateset = geom->getOrCreateStateSet();

			setMaterial(stateset, p, settings);
            const auto citygmlTex = p.getTextureFor(settings._theme);
			setTextureOnly(stateset, geom, settings,citygmlTex);

#if OSG_VERSION_GREATER_OR_EQUAL(3,3,2)
			if (settings._storeGeomIDs) {
				geom->addDescription(p.getId());
			}
#endif
			geometryContainer->addDrawable(geom);
        }
    }

    // Parse child geoemtries
    for (unsigned int i = 0; i < geometry.getGeometriesCount(); i++) {
        createOsgGeometryFromCityGMLGeometry(geometry.getGeometry(i), settings, geometryContainer, offset);
    }
}

void ReaderWriterCityGML::createSingleOsgGeometryFromCityGMLGeometry(const citygml::CityObject& object, MaterialArraysMap& matMap, const citygml::Geometry& geometry, CityGMLSettings& settings, const osg::Vec3d& offset) const
{
    for (unsigned int j = 0; j < geometry.getPolygonsCount(); j++)
    {
        const citygml::Polygon& p = *geometry.getPolygon(j);
        const auto citygmlTex = p.getTextureFor(settings._theme);

        if (p.getIndices().size() == 0) continue;

        const std::vector<TVec3d>& vert = p.getVertices();
        MaterialArrays* arrays = nullptr;

        if (citygmlTex)
        {
            auto it = matMap.find(citygmlTex->getUrl());
            if (it != matMap.end())
                arrays = it->second;
            else
            {
                arrays = new MaterialArrays();
                arrays->textureName = citygmlTex->getUrl();
                matMap[arrays->textureName] = arrays;

                if (settings._textureMap.find(citygmlTex->getUrl()) == settings._textureMap.end()) {
                    std::string fullPath = osgDB::findDataFile(citygmlTex->getUrl());

                    if (fullPath.empty()) {
                        osg::notify(osg::NOTICE) << "  Texture file " << citygmlTex->getUrl() << " not found..." << std::endl;
                        return;
                    }

                    // Load a new texture
                    osg::notify(osg::NOTICE) << "  Loading texture " << fullPath << "..." << std::endl;

                    osg::Image* image = osgDB::readImageFile(citygmlTex->getUrl());

                    if (!image) {
                        osg::notify(osg::NOTICE) << "  Warning: Failed to read Texture " << fullPath << std::endl;
                        return;
                    }

                    arrays->texture = new osg::Texture2D;
                    arrays->texture->setImage(image);
                    arrays->texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
                    arrays->texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
                    arrays->texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
                    arrays->texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
                    arrays->texture->setWrap(osg::Texture::WRAP_R, osg::Texture::REPEAT);

                    settings._textureMap[citygmlTex->getUrl()] = arrays->texture;
                }
                else {
                    arrays->texture = settings._textureMap[citygmlTex->getUrl()];
                }
            }
        }
        else
        {
            auto findInMatMap = [&matMap, &arrays](const std::string &name){
                auto it = matMap.find("roof");
                if (it != matMap.end())
                    arrays = it->second;
            };
            switch (object.getType())
            {
                case citygml::CityObject::CityObjectsType::COT_RoofSurface:
                {
                    findInMatMap("roof");
                    break;
                }
                case citygml::CityObject::CityObjectsType::COT_BuildingPart:
                case citygml::CityObject::CityObjectsType::COT_Building:
                case citygml::CityObject::CityObjectsType::COT_WallSurface:
                {
                    findInMatMap("wall");
                    break;
                }
                default:
                    break;
            }
        }
        if(arrays)
        {
			GLuint startIndex = arrays->vertices->size();
			for (const auto& i : p.getIndices())
			{
				arrays->indices.push_back(i + startIndex);
			}

			for (unsigned int k = 0; k < vert.size(); k++)
			{
				TVec3d v = vert[k];
				osg::Vec3d pt = osg::Vec3d(v.x, v.y, v.z) - offset;
				arrays->vertices->push_back(pt);
			}
            if (citygmlTex)
            {
                const std::vector<TVec2f>& texCoords = p.getTexCoordsForTheme(settings._theme, true);

                if (!texCoords.empty()) {
                    for (unsigned int k = 0; k < texCoords.size(); k++)
                        arrays->texCoords->push_back(osg::Vec2(texCoords[k].x, texCoords[k].y));
                }
            }
        }
    }

    // Parse child geoemtries
    for (unsigned int i = 0; i < geometry.getGeometriesCount(); i++) {
        createSingleOsgGeometryFromCityGMLGeometry(object, matMap, geometry.getGeometry(i), settings, offset);
    }
}

// bool ReaderWriterCityGML::createCityObjectWithSeparateBuildingParts(const citygml::CityObject& object, 
//     const CityGMLSettings& settings, osg::Group* grp, 
//     const osg::Vec3d& offset, unsigned int minimumLODToConsider) const {
    
// /*     std::string texName = "axaxaxax";
//     if (geometry.getPolygonsCount() > 0)
//     {
//         osg::Geometry* geom = new osg::Geometry;
//         geom->setUserValue("cot_type", geometry.getTypeAsString());
//         osg::Vec3Array* vertices = new osg::Vec3Array;
//         osg::ref_ptr<osg::Vec2Array> tex = new osg::Vec2Array;
//         tex->reserve(3);
//         vertices->reserve(3);
//         std::vector<GLuint> indicesVec;
//         indicesVec.reserve(2);

//         for (unsigned int j = 0; j < geometry.getPolygonsCount(); j++)
//         {
//             const citygml::Polygon& p = *geometry.getPolygon(j);
//             const auto citygmlTex = p.getTextureFor(settings._theme);

//             if (p.getIndices().size() == 0) continue;

//             // Geometry management
//             if (texName != "axaxaxax" && (citygmlTex != nullptr && (citygmlTex->getUrl() != texName)) || (citygmlTex == nullptr && (texName != "")))
//             {
//                 geom->setVertexArray(vertices);
//                 if (tex->size() > 0)
//                     geom->setTexCoordArray(0, tex);

//                 // Indices
//                 osg::DrawElementsUInt* indices = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES, indicesVec.begin(), indicesVec.end());
//                 geom->addPrimitiveSet(indices);
//                 indicesVec.clear();

//                 // Appearance
//                 osg::ref_ptr<osg::StateSet> stateset = geom->getOrCreateStateSet();

//                 setMaterial(stateset, p, settings);
//                 osg::ref_ptr<osg::Image> image;
//                 if (geometry.getType() == citygml::Geometry::GeometryType::GT_Wall) {
//                     osg::notify(osg::NOTICE) << "  GEOMETRY TYPE " << geometry.getTypeAsString() << "\n";
//                     image = osgDB::readImageFile("/home/hpcmdjur/data/textures/cracked_concrete_wall_1k.blend/textures/cracked_concrete_wall_diff_1k.jpg");
//                     // osg::ref_ptr<osg::Image> image = osgDB::readImageFile("/home/hpcmdjur/data/textures/cracked_concrete_wall_1k.blend/textures/ConcreteWallDifuse.png");
//                     osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D;
//                     texture->setImage(image);
//                     texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
//                     texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
//                     texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
//                     texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
//                     texture->setWrap(osg::Texture::WRAP_R, osg::Texture::REPEAT);
//                     texture->setTextureSize(1024, 1024);
                    
//                     stateset->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);
//                     osg::CullFace* cullFace = new osg::CullFace();
//                     cullFace->setMode(osg::CullFace::BACK);
//                     stateset->setAttributeAndModes(cullFace, osg::StateAttribute::ON);
//                 }

// #if OSG_VERSION_GREATER_OR_EQUAL(3,3,2)
//                 if (settings._storeGeomIDs) {
//                     geom->addDescription(p.getId());
//                 }
// #endif

//                 geometryContainer->addDrawable(geom);

//                 // create new Geometry
//                 geom = new osg::Geometry;
//                 geom->setUserValue("cot_type", geometry.getTypeAsString());
//                 vertices = new osg::Vec3Array;
//                 tex = new osg::Vec2Array;
//                 vertices->reserve(3);
//                 tex->reserve(3);

//             }

//             if (citygmlTex == nullptr)
//                 texName = "default";
//             else
// 				texName = citygmlTex->getUrl();
//             geom->setName(p.getId());


//             GLuint startIndex = vertices->size();
//             for (const auto& i : p.getIndices())
//             {
//                 indicesVec.push_back(i+ startIndex);
//             }

//             // Vertices
//             const std::vector<TVec3d>& vert = p.getVertices();
//             for (unsigned int k = 0; k < vert.size(); k++)
//             {
//                 TVec3d v = vert[k];
//                 osg::Vec3d pt = osg::Vec3d(v.x, v.y, v.z) - offset;
//                 vertices->push_back(pt);
//             }
//             //texcoords
//             const std::vector<TVec2f>& texCoords = p.getTexCoordsForTheme(settings._theme, true);

//             if (!texCoords.empty()) {
//                 for (unsigned int k = 0; k < texCoords.size(); k++)
//                     tex->push_back(osg::Vec2(texCoords[k].x, texCoords[k].y));
//             } else {
//                 tex->push_back(osg::Vec2(0.0, 0.0));
//                 tex->push_back(osg::Vec2(2.0, 0.0));
//                 tex->push_back(osg::Vec2(2.0, 1.0));
//                 tex->push_back(osg::Vec2(0.0, 1.0));
//             }
//         }
//         if (vertices->size())
//         {
//             const citygml::Polygon& p = *geometry.getPolygon(geometry.getPolygonsCount() - 1);
//             geom->setVertexArray(vertices);
//             if (tex->size() > 0)
// 				geom->setTexCoordArray(0, tex);

// 			// Indices
// 			osg::DrawElementsUInt* indices = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES, indicesVec.begin(), indicesVec.end());
// 			geom->addPrimitiveSet(indices);

// 			// Appearance
// 			osg::ref_ptr<osg::StateSet> stateset = geom->getOrCreateStateSet();

// 			setMaterial(stateset, p, settings);
//             const auto citygmlTex = p.getTextureFor(settings._theme);
// 			setTextureOnly(stateset, geom, settings,citygmlTex);

// #if OSG_VERSION_GREATER_OR_EQUAL(3,3,2)
// 			if (settings._storeGeomIDs) {
// 				geom->addDescription(p.getId());
// 			}
// #endif
// 			geometryContainer->addDrawable(geom);
//         }
//     } */
// }

void ReaderWriterCityGML::printCityObjectNames(osg::Geode *geode,
                                               const citygml::CityObject &object) const {
    // Print the city object name on top of it
    geode->getBoundingBox().center();
    osg::ref_ptr<osgText::Text> text = new osgText::Text;
    text->setFont("arial.ttf");
    text->setCharacterSize(2);
    text->setBackdropType(osgText::Text::OUTLINE);
    text->setFontResolution(64, 64);
    text->setText(object.getId(), osgText::String::ENCODING_UTF8);
    text->setCharacterSizeMode(
        osgText::TextBase::OBJECT_COORDS_WITH_MAXIMUM_SCREEN_SIZE_CAPPED_BY_FONT_HEIGHT);
    text->setAxisAlignment(osgText::TextBase::SCREEN);
    text->setAlignment(osgText::TextBase::CENTER_BOTTOM);
    text->setPosition(geode->getBoundingBox().center() +
                      osg::Vec3(0, 0, geode->getBoundingBox().radius()));
    text->getOrCreateStateSet()->setMode(GL_LIGHTING,
                                         osg::StateAttribute::OVERRIDE | osg::StateAttribute::OFF);
    geode->addDrawable(text.get());
}

void ReaderWriterCityGML::handleWindowTransparency(osg::Geode *geode) const {
    osg::StateSet *geodeSS(geode->getOrCreateStateSet());

    osg::ref_ptr<osg::BlendFunc> blendFunc = new osg::BlendFunc(
        osg::BlendFunc::ONE_MINUS_CONSTANT_ALPHA, osg::BlendFunc::CONSTANT_ALPHA);
    geodeSS->setAttributeAndModes(blendFunc.get(),
                                  osg::StateAttribute::OVERRIDE | osg::StateAttribute::ON);

    osg::ref_ptr<osg::BlendColor> blendColor = new osg::BlendColor(osg::Vec4(1., 1., 1., 0.4));
    geodeSS->setAttributeAndModes(blendColor.get(),
                                  osg::StateAttribute::OVERRIDE | osg::StateAttribute::ON);

    osg::ref_ptr<osg::Depth> depth = new osg::Depth;
    depth->setWriteMask(false);
    geodeSS->setAttributeAndModes(depth.get(),
                                  osg::StateAttribute::OVERRIDE | osg::StateAttribute::ON);

    geodeSS->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
}

bool ReaderWriterCityGML::createSingleCityObjectGeode(const citygml::CityObject &object,
                                                      CityGMLSettings &settings, osg::Group *parent,
                                                      const osg::Vec3d &offset,
                                                      unsigned int minimumLODToConsider) const {
    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->setName( object.getId() );

    osg::Group* grp = new osg::Group;
    grp->setName( object.getId() );
    grp->setUserValue("cot_type", object.getTypeAsString());
    grp->addChild( geode );
    parent->addChild( grp );

    unsigned int highestLOD = ReaderWriterCityGML::getHighestLodForObject(object);

    bool gotGeometry = false;
    for ( unsigned int i = 0; i < object.getGeometriesCount(); ++i )
    {
        const citygml::Geometry& geometry = object.getGeometry( i );
        const unsigned int currentLOD = geometry.getLOD();

        if (settings._useMaxLODOnly && (currentLOD < highestLOD || currentLOD < minimumLODToConsider )){
            continue;
        }
        gotGeometry = true;
        createOsgGeometryFromCityGMLGeometry(geometry, settings, geode, offset);
    }

    if ( settings._printNames )
    {
        // Print the city object name on top of it
        geode->getBoundingBox().center();
        osg::ref_ptr<osgText::Text> text = new osgText::Text;
        text->setFont( "arial.ttf" );
        text->setCharacterSize( 2 );
        text->setBackdropType( osgText::Text::OUTLINE );
        text->setFontResolution( 64, 64 );
        text->setText( object.getId(), osgText::String::ENCODING_UTF8 );
        text->setCharacterSizeMode( osgText::TextBase::OBJECT_COORDS_WITH_MAXIMUM_SCREEN_SIZE_CAPPED_BY_FONT_HEIGHT );
        text->setAxisAlignment( osgText::TextBase::SCREEN );
        text->setAlignment( osgText::TextBase::CENTER_BOTTOM );
        text->setPosition( geode->getBoundingBox().center() + osg::Vec3( 0, 0, geode->getBoundingBox().radius() ) );
        text->getOrCreateStateSet()->setMode( GL_LIGHTING, osg::StateAttribute::OVERRIDE|osg::StateAttribute::OFF );
        geode->addDrawable( text.get() );
    }

    // Manage transparency for windows
    if ( object.getType() == citygml::CityObject::CityObjectsType::COT_Window )
    {
        osg::StateSet *geodeSS( geode->getOrCreateStateSet() );

        osg::ref_ptr<osg::BlendFunc> blendFunc = new osg::BlendFunc(osg::BlendFunc::ONE_MINUS_CONSTANT_ALPHA,osg::BlendFunc::CONSTANT_ALPHA);
        geodeSS->setAttributeAndModes( blendFunc.get(), osg::StateAttribute::OVERRIDE|osg::StateAttribute::ON );

        osg::ref_ptr<osg::BlendColor> blendColor = new osg::BlendColor(osg::Vec4( 1., 1., 1., 0.4 ));
        geodeSS->setAttributeAndModes( blendColor.get(), osg::StateAttribute::OVERRIDE|osg::StateAttribute::ON );

        osg::ref_ptr<osg::Depth> depth = new osg::Depth;
        depth->setWriteMask( false );
        geodeSS->setAttributeAndModes( depth.get(), osg::StateAttribute::OVERRIDE|osg::StateAttribute::ON );

        geodeSS->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
    }

    if(!gotGeometry)
    {
		for (unsigned int i = 0; i < object.getChildCityObjectsCount(); ++i)
			createCityObject(object.getChildCityObject(i), settings, grp, offset, highestLOD);
    }

    return true;
}

bool ReaderWriterCityGML::createSeparateCityObjectGeode(const citygml::CityObject &object,
                                                        CityGMLSettings &settings,
                                                        osg::Group *parent, osg::Geode *geode,
                                                        const osg::Vec3d &offset,
                                                        unsigned int minimumLODToConsider) const {
    if (!parent) return false;
    if (!geode) {
        osg::ref_ptr<osg::Group> grp = new osg::Group;
        grp->setName(object.getId());
        grp->setUserValue("cot_type", object.getTypeAsString());
        grp->addChild(geode);
        parent->addChild(grp);

        parent = grp;
    }

    unsigned int highestLOD = ReaderWriterCityGML::getHighestLodForObject(object);
    std::map<std::string, osg::ref_ptr<osg::Geode>> parts;

    // Parse BuildingParts
    for (unsigned int i = 0; i < object.getChildCityObjectsCount(); ++i) {
        const auto &child = object.getChildCityObject(i);
        const auto &type = child.getTypeAsString();
        osg::ref_ptr<osg::Geode> partGeode;
        if (parts.find(type) == parts.end()) {
            partGeode = new osg::Geode;
            partGeode->setName(type);

            osg::ref_ptr<osg::Group> partGrp = new osg::Group;
            partGrp->setName(type);
            partGrp->addChild(partGeode);
            parent->addChild(partGrp);

            parts[type] = partGeode;
        } else {
            partGeode = parts.at(type);
        }
        for (unsigned int i = 0; i < child.getGeometriesCount(); ++i) {
            const citygml::Geometry &geometry = child.getGeometry(i);
            const unsigned int currentLOD = geometry.getLOD();

            if (settings._useMaxLODOnly &&
                (currentLOD < highestLOD || currentLOD < minimumLODToConsider)) {
                continue;
            }
            createOsgGeometryFromCityGMLGeometry(geometry, settings, partGeode, offset);
        }
    }

    return true;
}

bool ReaderWriterCityGML::createCityObject(const citygml::CityObject &object,
                                           CityGMLSettings &settings, osg::Group *parent,
                                           const osg::Vec3d &offset,
                                           unsigned int minimumLODToConsider) const
{

    // Skip objects without geometry
    if (!parent) return false;

    if (settings._separateBuildingParts) {
        return createSeparateCityObjectGeode(object, settings, parent, nullptr, offset,
                                             minimumLODToConsider);
    }

    return createSingleCityObjectGeode(object, settings, parent, offset, minimumLODToConsider);
}

void ReaderWriterCityGML::getCenterAndDirection(const citygml::CityObject& object, osg::Vec3d& position, osg::Vec3& direction) const
{
    float minz=100000.0;
    const citygml::Geometry* minGeometry = nullptr;
    auto iterOverObjAndGetMinGeo = [&](const citygml::CityObject& object, float &minz) {
        for (unsigned int i = 0; i < object.getGeometriesCount(); ++i)
        {
            const citygml::Geometry& geometry = object.getGeometry(i);
            const unsigned int currentLOD = geometry.getLOD();
            minGeometry = getMinGeometry(object, geometry, minz);
        }
    };
    auto childCityObjectsCount = object.getChildCityObjectsCount();
    if (childCityObjectsCount > 0)
    {
        for (unsigned int i = 0; i < childCityObjectsCount; ++i)
        {
            const citygml::CityObject& obj = object.getChildCityObject(i);
            iterOverObjAndGetMinGeo(obj, minz);
        }
    } else {
        iterOverObjAndGetMinGeo(object, minz);
    }

    if (minGeometry != nullptr)
    {
        if (minGeometry->getPolygonsCount() > 0)
        {
            const citygml::Polygon& p = *minGeometry->getPolygon(0);
            const std::vector<TVec3d>& vert = p.getVertices();

            TVec3d v = vert[0];
            TVec3d v2 = vert[1];
            osg::Vec3d pt = osg::Vec3d(v.x, v.y, v.z);
            osg::Vec3d pt2 = osg::Vec3d(v2.x, v2.y, v2.z);
            direction = pt2 - pt;
        }
        position = osg::Vec3(0, 0, 0);
        for (unsigned int j = 0; j < minGeometry->getPolygonsCount(); j++)
        {
            const citygml::Polygon& p = *minGeometry->getPolygon(j);
            if (p.getIndices().size() == 0) continue;
            const std::vector<TVec3d>& vert = p.getVertices();
            osg::Vec3d pos;
            for (unsigned int k = 0; k < vert.size(); k++)
            {
                TVec3d v = vert[k];
                pos += osg::Vec3d(v.x, v.y, v.z);
            }
            pos /= vert.size();
            position += pos;
        }
        position /= minGeometry->getPolygonsCount();
        position[2] = minz;
    }
}

const citygml::Geometry* ReaderWriterCityGML::getMinGeometry(const citygml::CityObject& object, const citygml::Geometry& geometry, float &minz) const
{
    const citygml::Geometry* minGeometry = nullptr;
    for (unsigned int j = 0; j < geometry.getPolygonsCount(); j++)
    {
        auto p = geometry.getPolygon(j);
        if (p->getIndices().size() == 0) continue;

        const std::vector<TVec3d>& vert = p->getVertices();
        for (unsigned int k = 0; k < vert.size(); k++)
        {
            TVec3d v = vert[k];
            if (v.z < minz)
            {
                minz = v.z;
                minGeometry = &geometry;
            }
        }
    }

    // Parse child geoemtries
    for (unsigned int i = 0; i < geometry.getGeometriesCount(); i++) {
        minGeometry = getMinGeometry(object, geometry.getGeometry(i), minz);
    }
    
    return minGeometry;
}

bool ReaderWriterCityGML::createSingleCityObject(const citygml::CityObject& object, CityGMLSettings& settings, MaterialArraysMap& map, const osg::Vec3d& offset, osg::MatrixTransform* root, unsigned int minimumLODToConsider) const
{
    osg::ref_ptr<osg::Vec4Array> roof_color = new osg::Vec4Array;
    roof_color->push_back(osg::Vec4(0.9f, 0.1f, 0.1f, 1.0f));

    bool isSpecial = false;
    if (object.getType() == citygml::CityObject::CityObjectsType::COT_Building)
    {
        std::string func = object.getAttribute("bldg:function");
        std::string fileName = "";
        std::string sheight = object.getAttribute("bldg:measuredheight");
        float height = std::stof(sheight);
        float scale = 1.0;
        if (func == "51002_1251")
        {
            fileName = "Freileitung.ive";
            if(height>=30)
            {
                scale = height / 31.0; // our model is 31 m high
            }
            else if(height>=15)
            {
                scale = height / 20.0; 
                fileName = "Freileitung20.ive";
            }
            else
            {
                scale = height / 10.0; 
                fileName = "FreileitungSmall.ive";
            }
            isSpecial = true;
        }
        if (func == "51002_1220")
        {
            std::string sheight = object.getAttribute("bldg:measuredheight");
            height = std::stof(sheight);
            scale = height / 1.053; // our model is 1.053 m high
            fileName = "Windrad.ive";
            isSpecial = true;
        }
        if (isSpecial) {
            osg::ProxyNode* p = new osg::ProxyNode();
            osg::MatrixTransform* m = new osg::MatrixTransform();
            osg::Vec3d position;
            osg::Vec3 direction;
            getCenterAndDirection(object, position, direction);
            float angle = atan2(direction.x(), direction.y());
            m->setMatrix(osg::Matrix::scale(osg::Vec3(scale, scale, scale)) * osg::Matrix::rotate(angle, osg::Vec3(0, 0, 1)) * osg::Matrix::translate(position - offset));

            p->setFileName(0, fileName);
            m->addChild(p);
            root->addChild(m);
        }
    }
    if (!isSpecial)
    {
        unsigned int highestLOD = ReaderWriterCityGML::getHighestLodForObject(object);

        for (unsigned int i = 0; i < object.getGeometriesCount(); i++)
        {
            const citygml::Geometry& geometry = object.getGeometry(i);

            const unsigned int currentLOD = geometry.getLOD();

            if (settings._useMaxLODOnly && (currentLOD < highestLOD || currentLOD < minimumLODToConsider)) {
                continue;
            }

            createSingleOsgGeometryFromCityGMLGeometry(object, map, geometry, settings, offset);
        }

        for (unsigned int i = 0; i < object.getChildCityObjectsCount(); ++i)
        {
            createSingleCityObject(object.getChildCityObject(i), settings, map, offset, root, highestLOD);

        }
    }
    return true;
}

unsigned int ReaderWriterCityGML::getHighestLodForObject( const citygml::CityObject& object){
    unsigned int highestLOD = 0;
    // first find out highest LOD for this object
    for (unsigned int i = 0; i < object.getGeometriesCount(); i++) {
        const citygml::Geometry &geometry = object.getGeometry(i);

        if (geometry.getLOD() > highestLOD){
            highestLOD = geometry.getLOD();
        }
    }

    //check for the highest LODs of Children
    for (unsigned int i = 0; i < object.getChildCityObjectsCount(); ++i){
        unsigned int tempHighestLOD = ReaderWriterCityGML::getHighestLodForObject(object.getChildCityObject(i));
        if (tempHighestLOD > highestLOD){
            tempHighestLOD = highestLOD;
        }
    }

    return highestLOD;
}
