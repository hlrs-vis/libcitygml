#include <osgDB/Registry>
#include <osg/Texture2D>
#include <citygml/citygml.h>

class CityGMLSettings
{
public:
    CityGMLSettings( void )
        : _printNames(false)
        , _useMaxLODOnly(false)
        , _singleObject(false)
        , _storeGeomIDs(false)
        , _theme("")
    {}

    void parseOptions( const osgDB::ReaderWriter::Options* options)
    {
        if ( !options ) return;
        std::istringstream iss( options->getOptionString() );
        std::string currentOption;
        while ( iss >> currentOption )
        {
            std::transform( currentOption.begin(), currentOption.end(), currentOption.begin(), ::tolower );
            if ( currentOption == "names" ) _printNames = true;
            else if ( currentOption == "mask" ) iss >> _params.objectsMask;
            else if ( currentOption == "minlod" ) iss >> _params.minLOD;
            else if ( currentOption == "maxlod" ) iss >> _params.maxLOD;
            else if ( currentOption == "optimize" ) _params.optimize = true;
            else if ( currentOption == "pruneemptyobjects" ) _params.pruneEmptyObjects = true;
            else if ( currentOption == "usemaxlodonly") _useMaxLODOnly = true;
            else if ( currentOption == "singleobject") _singleObject = true;
            else if ( currentOption == "usetheme" ) iss >> _theme;
            else if ( currentOption == "storegeomids" ) _storeGeomIDs = true;
        }
    }

public:
    citygml::ParserParams _params;
    bool _printNames;
    bool _useMaxLODOnly;
    bool _singleObject;
    bool _storeGeomIDs;
    std::map< std::string, osg::Texture2D* > _textureMap;
    std::string _theme;
};