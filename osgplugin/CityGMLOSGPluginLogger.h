#include <citygml/citygmllogger.h>
#include <osg/Notify>

class CityGMLOSGPluginLogger : public citygml::CityGMLLogger {
public:

    virtual void log(LOGLEVEL level, const std::string& message, const char* file, int line) const
    {
        std::ostream& stream = getLogStreamFor(level);

        if (file) {
            stream << " [" << file;
            if (line > -1) {
                stream << ":" << line;
            }
            stream << "]";
        }

        stream << " " << message << std::endl;
    }

private:
    std::ostream& getLogStreamFor(LOGLEVEL level) const {
        switch(level) {
        case LOGLEVEL::LL_DEBUG:
            return osg::notify(osg::DEBUG_INFO);
        case LOGLEVEL::LL_WARNING:
            return osg::notify(osg::WARN);
        case LOGLEVEL::LL_TRACE:
            return osg::notify(osg::DEBUG_FP);
        case LOGLEVEL::LL_ERROR:
            return osg::notify(osg::FATAL);
        case LOGLEVEL::LL_INFO:
            return osg::notify(osg::INFO);
        default:
            return osg::notify(osg::INFO);
        }
    }
};