#include <osg/Texture2D>
#include <osgDB/Registry>
#include <citygml/citygml.h>
#include <citygml/citymodel.h>

#include "CityGMLSettings.h"
#include "CityGMLOSGPluginLogger.h"

class ReaderWriterCityGML : public osgDB::ReaderWriter
{
public:

    struct MaterialArrays
    {
        MaterialArrays(int sizehint = 3) : 
            vertices(new osg::Vec3Array(sizehint)), 
            texCoords(new osg::Vec2Array(sizehint)),
            indices(sizehint)
        {}
    
        std::string textureName;
        osg::ref_ptr<osg::Texture2D> texture = nullptr;
        osg::ref_ptr<osg::Vec3Array> vertices;
        osg::ref_ptr<osg::Vec2Array> texCoords;
        std::vector<GLuint> indices;
    };

    typedef std::map<std::string, MaterialArrays*> MaterialArraysMap;
    ReaderWriterCityGML( void )
    {
        supportsExtension( "citygml", "CityGML format" );
        supportsExtension( "gml", "CityGML format" );

        supportsOption( "names", "Add the name of the city objects on top of them" );
        supportsOption( "mask", "Set the objects mask" );
        supportsOption( "minLOD", "Minimum LOD level to fetch" );
        supportsOption( "maxLOD", "Maximum LOD level to fetch" );
        supportsOption( "optimize", "Optimize the geometries & polygons of the CityGML model to reduce the number of instanced objects" );
        supportsOption( "pruneEmptyObjects", "Prune empty objects (ie. without -supported- geometry)" );
        supportsOption( "destSRS", "Transform geometry to given reference system" );
        supportsOption( "useMaxLODonly", "Use the highest available LOD for geometry of one object" );
        supportsOption( "appearanceTheme", "Name of the appearance theme to use" );
        supportsOption( "storegeomids", "Store the citygml id of geometry objects in the corresponding osg::Geometry object as a description string." );

        m_logger = std::make_shared<CityGMLOSGPluginLogger>();
    }

    virtual const char* className( void ) const override { return "CityGML Reader"; }

    virtual ReadResult readNode( const std::string&, const osgDB::ReaderWriter::Options* ) const override;
    virtual ReadResult readNode( std::istream&, const osgDB::ReaderWriter::Options* ) const override;
    virtual ReadResult readObject(const std::string& fileName, const osgDB::ReaderWriter::Options* options) const override
    {
        return readObjectAdapter(fileName, options);
    }

    virtual ReadResult readObject(std::istream& fin, const Options* options) const override
    {
        return readObjectAdapter(fin, options);
    }

private:

    std::shared_ptr<citygml::CityGMLLogger> m_logger;
    static unsigned int getHighestLodForObject(const citygml::CityObject& object);

    template<typename T>
    ReadResult readObjectAdapter(T&& input, const osgDB::ReaderWriter::Options* options) const;
    ReadResult readCity(std::shared_ptr<const citygml::CityModel>, CityGMLSettings& ) const;
    bool createCityObject(const citygml::CityObject&, CityGMLSettings&, osg::Group*, const osg::Vec3d& offset = osg::Vec3d(0.0, 0.0, 0.0), unsigned int minimumLODToConsider = 0) const;
    bool createSingleCityObject(const citygml::CityObject&, CityGMLSettings&, MaterialArraysMap&, const osg::Vec3d& offset, osg::MatrixTransform* root, unsigned int minimumLODToConsider = 0) const;
    void createSingleOsgGeometryFromCityGMLGeometry(const citygml::CityObject& object, MaterialArraysMap &, const citygml::Geometry& geometry, CityGMLSettings& settings, const osg::Vec3d& offset) const;
    void getCenterAndDirection(const citygml::CityObject& object, const citygml::Geometry& geometry, float& minz, const citygml::Geometry*& minGeometry) const;
    void getCenterAndDirection(const citygml::CityObject& object, osg::Vec3d& position, osg::Vec3& direction) const;
    void handleCityAsSingleObject(CityGMLSettings& settings, const citygml::ConstCityObjects& roots, const osg::Vec3d& offset, osg::MatrixTransform* root) const;
    void applyMaterialForSingleObject(MaterialArraysMap &matMap, osg::ref_ptr<osg::Geode> geode) const;
};

// use forwarding reference to avoid code duplication and to preserve type
template<typename T>
osgDB::ReaderWriter::ReadResult ReaderWriterCityGML::readObjectAdapter(T&& input, const osgDB::ReaderWriter::Options* options) const 
{
    ReadResult result = readNode(std::forward<T>(input), options);
    osg::Node* node = result.getNode();
    if (node) return node;
    else return result;
}
