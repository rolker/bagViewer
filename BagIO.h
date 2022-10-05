#ifndef BAGIO_H
#define BAGIO_H

#include <QOpenGLContext>
#include <vector>
#include <memory>
#include <QVector3D>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QImage>
#include "bag_dataset.h"
#include "Bounds.h"

struct TileGL;

struct TileData
{
    std::vector<GLfloat> elevations;
    QImage normalMap;
    std::vector<GLfloat> uncertainties;
};

typedef std::shared_ptr<TileData> TileDataPtr;

typedef std::pair<uint32_t,uint32_t> TileIndex2D;

struct Tile
{
    TileIndex2D index;
    Bounds bounds;
    uint ncols,nrows;
    float dx,dy;
    TileDataPtr data;
    std::shared_ptr<TileGL> gl;
    TileIndex2D lowerLeftIndex;
    std::map<TileIndex2D,std::shared_ptr<Tile> > subTiles;
    
    std::shared_ptr<Tile> north;
    std::shared_ptr<Tile> east;
    std::shared_ptr<Tile> northEast;
};

typedef std::shared_ptr<Tile> TilePtr;
typedef std::map<TileIndex2D,TilePtr> TileMap;

class BagIO: public QThread
{
    Q_OBJECT
public:
    
    struct MetaData
    {
        float minElevation = 0.0;
        float maxElevation = 0.0;
        QVector3D size;
        QVector3D swBottomCorner;
        
        uint32_t ncols = 0;
        uint32_t nrows = 0;
        double dx = 0.0;
        double dy = 0.0;
        bool variableResolution;
    };
    
    BagIO(QObject *parent = 0);
    ~BagIO();
    
    bool open(QString const &bagFileName);
    void close();
    
    uint32_t getTileSize() const;
    
    //std::vector<TilePtr> getOverviewTiles();
    MetaData getMeta();
    
signals:
    void metaLoaded();
    void tileLoaded(TilePtr tile, bool isVR);
    
protected:
    void run() Q_DECL_OVERRIDE;
    
private:
    TilePtr loadTile(BAG::Dataset &bag, TileIndex2D tileIndex, MetaData &meta) const; 
    TilePtr loadVarResTile(BAG::Dataset &bag, TileIndex2D const tileIndex, MetaData const &meta, Tile const &parentTile) const; 
    
    QMutex mutex;
    QWaitCondition condition;
    bool restart = false;
    bool abort = false;
    
    uint32_t tileSize = 128;
    //TileMap overviewTiles;
    MetaData meta;
    QString filename;
};

#endif