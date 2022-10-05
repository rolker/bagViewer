#include "BagIO.h"

#include <memory>
#include <QString>
#include <QtConcurrent/QtConcurrent>
#include "BagGL.h"

#include "bag_vrrefinements.h"
#include "bag_vrrefinementsdescriptor.h"
#include "bag_vrmetadata.h"

BagIO::BagIO(QObject *parent):
    QThread(parent)
{
}

BagIO::~BagIO()
{
    close();
    mutex.lock();
    abort = true;
    condition.wakeOne();
    mutex.unlock();
    
    wait();
}

uint32_t BagIO::getTileSize() const
{
    return tileSize;
}

void BagIO::run()
{
    forever {
        {
            QMutexLocker locker(&mutex);
            if (restart)
                break;
            if (abort)
                return;
        }
        
        mutex.lock();
        QString filename = this->filename;
        mutex.unlock();

        auto bag = BAG::Dataset::open(filename.toStdString(), BAG::OpenMode::BAG_OPEN_READONLY);

        if (bag)
        {
            MetaData meta;
            
            
            const auto& bm = bag->getMetadata();
            
            meta.dx = bm.columnResolution();
            meta.dy = bm.rowResolution();

            const auto elevationLayer = bag->getLayer(BAG::LayerType::Elevation, "");

            if(elevationLayer)
            {
                auto minmax = elevationLayer->getDescriptor()->getMinMax();

                bool isVarRes = false;
                auto vrRefinements = bag->getVRRefinements();
                if(vrRefinements)
                {
                    isVarRes = true;
                    auto desc = std::dynamic_pointer_cast<BAG::VRRefinementsDescriptor>(vrRefinements->getDescriptor());
                    minmax = desc->getMinMaxDepth();
                    meta.minElevation = std::get<0>(minmax);
                    meta.maxElevation = std::get<1>(minmax);
                }
                qDebug() << "variable resolution? " << isVarRes;
                meta.variableResolution = isVarRes;
            }
            
            meta.ncols = bm.columns();
            meta.nrows = bm.rows();
            
            meta.size.setX(meta.dx*meta.ncols);
            meta.size.setY(meta.dy*meta.nrows);
            meta.size.setZ(meta.maxElevation-meta.minElevation);
            
            meta.swBottomCorner.setX(bm.llCornerX());
            meta.swBottomCorner.setY(bm.llCornerY());
            meta.swBottomCorner.setZ(meta.minElevation);
            
            qDebug() << meta.ncols << " columns, " << meta.nrows << " rows";
            qDebug() << "spacing: " << meta.dx << " x " << meta.dy;
            qDebug() << "ll corner: " << bm.llCornerX() << ", " << bm.llCornerY();
            qDebug() << "Horizontal coordinate system: " << bag->getDescriptor().getHorizontalReferenceSystem().c_str();
            qDebug() << "Vertical coordinate system: " << bag->getDescriptor().getVerticalReferenceSystem().c_str();
            
            auto layers = bag->getLayers();
            qDebug() << "layer count: " << layers.size();
            for(auto l: layers)
                if(l)
                    qDebug() << "\tlayer: " << l->getDescriptor()->getName().c_str();
                else
                    qDebug() << "\tnull layer";

            {
                QMutexLocker locker(&mutex);
                this->meta = meta;
            }
            emit metaLoaded();
            
            std::vector<TilePtr> goodOverviewTiles;
            
            for(uint32_t i = 0; i < meta.nrows; i += tileSize)
            {
                {
                    QMutexLocker locker(&mutex);
                    if (restart)
                        break;
                    if (abort)
                        return;
                }
                uint32_t trow = i/tileSize;
                qDebug() << "tile row: " << trow;
                for(uint32_t j = 0; j < meta.ncols; j += tileSize)
                {
                    TileIndex2D tindex(j/tileSize,trow);
                    //std::cerr << "tile: " << tindex.first << "," << tindex.second;
                    TilePtr t = loadTile(*bag, tindex, meta);
                    if(t)
                    {
                        goodOverviewTiles.push_back(t);
                        //std::cerr << "\tsaved" << std::endl;
                    }
                    else
                    {
                        //std::cerr << "\tdiscarded" << std::endl;
                    }
                    {
                        QMutexLocker locker(&mutex);
                        if (restart)
                            break;
                        if (abort)
                            return;
                        if(t)
                        {
                            //overviewTiles[tindex]=t;
                            emit tileLoaded(t,false);
                        }
                    }
                }
            }
            if(meta.variableResolution)
            {
                for(TilePtr t: goodOverviewTiles)
                    for (uint32_t ti = 0; ti < tileSize; ti++)
                        for(uint32_t tj = 0; tj < tileSize; tj++)
                        {
                            TilePtr vrTile = loadVarResTile(*bag, TileIndex2D(tj,ti), meta, *t);
                            if(vrTile)
                                //t->subTiles[vrTile->index]=vrTile;
                                emit tileLoaded(vrTile,true);
                        }
            }
            
        }
        
        {
            QMutexLocker locker(&mutex);
            if (!restart)
                condition.wait(&mutex);
            restart = false;
        }
    }
}


bool BagIO::open(const QString& bagFileName)
{
    close();
    QMutexLocker locker(&mutex);
    filename = bagFileName;
    if (!isRunning()) {
        start(LowPriority);
    } else {
        restart = true;
        condition.wakeOne();
    }
    
    return true;
}

TilePtr BagIO::loadTile(BAG::Dataset &bag, TileIndex2D tileIndex, MetaData &meta) const
{
    TilePtr ret(new Tile);
    ret->index = tileIndex;
    ret->dx = meta.dx;
    ret->dy = meta.dy;
    
    uint32_t startRow = tileIndex.second * tileSize;
    uint32_t endRow = std::min(startRow+tileSize,meta.nrows-1);
    uint32_t startCol = tileIndex.first * tileSize;
    uint32_t endCol = std::min(startCol+tileSize,meta.ncols-1);
    
    ret->ncols = endCol-startCol+1;
    ret->nrows = endRow-startRow+1;
    
    ret->lowerLeftIndex = TileIndex2D(startCol,startRow);

    auto elevationLayer = bag.getLayer(BAG::LayerType::Elevation, "");
    auto data = elevationLayer->read(startRow, startCol, endRow, endCol);

    ret->data = TileDataPtr(new TileData);
    ret->data->elevations.resize(ret->nrows*ret->ncols,BAG_NULL_ELEVATION);

    memcpy(ret->data->elevations.data(), data.data(), data.size());
    
    bool notEmpty = false;
    for(auto e: ret->data->elevations)
    {
        if(e != BAG_NULL_ELEVATION)
        {
            notEmpty = true;
            break;
        }
    }
    
    if(!notEmpty)
        ret.reset();
    else
    {
        float minElevation = BAG_NULL_ELEVATION;
        float maxElevation = BAG_NULL_ELEVATION;
        for(auto e:ret->data->elevations)
        {
            if(e != BAG_NULL_ELEVATION)
            {
                if(minElevation == BAG_NULL_ELEVATION || e < minElevation)
                    minElevation = e;
                if(maxElevation == BAG_NULL_ELEVATION || e > maxElevation)
                    maxElevation = e;
            }
        }
        ret->bounds.add(QVector3D(startCol * meta.dx, startRow * meta.dy, minElevation));
        ret->bounds.add(QVector3D((endCol+1) * meta.dx, (endRow+1) * meta.dy, maxElevation));
        
        ret->data->normalMap = QImage(ret->ncols, ret->nrows, QImage::Format_RGB888);
        for(uint32_t ti = 0; ti < ret->nrows; ++ti)
        {
            for(uint32_t tj = 0; tj < ret->ncols; ++tj)
            {
                float p00 = ret->data->elevations[ti*ret->ncols+tj];
                float p10 = ret->data->elevations[ti*ret->ncols+tj+1];
                float p01 = p00;
                if (ti < ret->nrows-1)
                    p01 = ret->data->elevations[(ti+1)*ret->ncols+tj];;
                if(p00 != BAG_NULL_ELEVATION && p10 != BAG_NULL_ELEVATION && p01 != BAG_NULL_ELEVATION)
                {
                    QVector3D v1(meta.dx,0.0,p10-p00);
                    QVector3D v2(0.0,meta.dy,p01-p00);
                    QVector3D n = QVector3D::normal(v1,v2);
                    ret->data->normalMap.setPixel(tj,ti,QColor(127+128*n.x(),127+128*n.y(),127+128*n.z()).rgb());
                }
                else
                    ret->data->normalMap.setPixel(tj,ti,QColor(127,127,255).rgb());
            }
        }
    }
    
    return ret;
}

TilePtr BagIO::loadVarResTile(BAG::Dataset& bag, const TileIndex2D tileIndex, const BagIO::MetaData& meta, const Tile& parentTile) const
{
    TilePtr ret(new Tile);
    ret->data = TileDataPtr(new TileData);
    
    ret->index = tileIndex;
    auto vrMetadata = bag.getVRMetadata();

    //std::vector<bagVarResRefinementGroup> refinements;
    uint i = parentTile.lowerLeftIndex.first+tileIndex.first;
    uint j = parentTile.lowerLeftIndex.second+tileIndex.second;
    if(i >= meta.ncols || j >= meta.nrows)
        return TilePtr();
    ret->lowerLeftIndex = TileIndex2D(i,j);

    BAG::UInt8Array data;
    try
    {
        data = vrMetadata->read(j,i,j,i);
    }
    catch(const BAG::InvalidReadSize& e)
    {
        return TilePtr();
    }
    
    const BagVRMetadataItem& vrMetadataItem = *reinterpret_cast<BagVRMetadataItem*>(data.data());

    if(vrMetadataItem.dimensions_x == 0)
        return TilePtr();
    //refinements.resize(vrMetadata.dimensions_x*vrMetadata.dimensions_y);
    ret->data->normalMap = QImage(vrMetadataItem.dimensions_x, vrMetadataItem.dimensions_y, QImage::Format_RGB888);
    ret->data->normalMap.fill(QColor(127,127,255).rgb());

    BAG::UInt8Array refinements_data;
    try
    {
        refinements_data = bag.getVRRefinements()->read(0,vrMetadataItem.index,0,vrMetadataItem.index+vrMetadataItem.dimensions_x*vrMetadataItem.dimensions_y-1);    }
    catch(const std::exception& e)
    {
        return TilePtr();
    }

    const BagVRRefinementsItem* refinements = reinterpret_cast<const BagVRRefinementsItem*>(refinements_data.data());

    float minz = BAG_NULL_ELEVATION;
    float maxz = BAG_NULL_ELEVATION;
    for(int i = 0; i < vrMetadataItem.dimensions_x*vrMetadataItem.dimensions_y; i++)
    {
        ret->data->elevations.push_back(refinements[i].depth);
        if(minz == BAG_NULL_ELEVATION || refinements[i].depth < minz)
            minz = refinements[i].depth;
        if(maxz == BAG_NULL_ELEVATION || refinements[i].depth > maxz)
            maxz = refinements[i].depth;
    }
    
    for(uint32_t ti = 0; ti < vrMetadataItem.dimensions_y; ++ti)
    {
        if(ti < vrMetadataItem.dimensions_y-1)
            for(uint32_t tj = 0; tj < vrMetadataItem.dimensions_x; ++tj)
            {
                if(tj < vrMetadataItem.dimensions_x-1)
                {
                    float p00 = ret->data->elevations[ti*vrMetadataItem.dimensions_x+tj];
                    float p10 = ret->data->elevations[ti*vrMetadataItem.dimensions_x+tj+1];
                    float p01 = ret->data->elevations[(ti+1)*vrMetadataItem.dimensions_x+tj];
                    if(p00 != BAG_NULL_ELEVATION && p10 != BAG_NULL_ELEVATION && p01 != BAG_NULL_ELEVATION)
                    {
                        QVector3D v1(vrMetadataItem.resolution_x,0.0,p10-p00);
                        QVector3D v2(0.0,vrMetadataItem.resolution_y,p01-p00);
                        QVector3D n = QVector3D::normal(v1,v2);
                        ret->data->normalMap.setPixel(tj,ti,QColor(127+128*n.x(),127+128*n.y(),127+128*n.z()).rgb());
                    }
                    else
                        ret->data->normalMap.setPixel(tj,ti,QColor(127,127,255).rgb());
                }
                else
                    ret->data->normalMap.setPixel(tj,ti,ret->data->normalMap.pixel(tj-1,ti));
            }
        else
            for(uint32_t tj = 0; tj < vrMetadataItem.dimensions_x; ++tj)
                ret->data->normalMap.setPixel(tj,ti,ret->data->normalMap.pixel(tj,ti-1));
    }
    
    
    float cx = i*meta.dx;
    float cy = j*meta.dy;
    float pllx = cx-meta.dx/2.0;
    float plly = cy-meta.dy/2.0;
    float llx = pllx+vrMetadataItem.sw_corner_x;
    float lly = plly+vrMetadataItem.sw_corner_y;
    ret->dx = vrMetadataItem.resolution_x;
    ret->dy = vrMetadataItem.resolution_y;
    ret->ncols = vrMetadataItem.dimensions_x;
    ret->nrows = vrMetadataItem.dimensions_y;
    ret->bounds.add(QVector3D(llx,lly,minz));
    ret->bounds.add(QVector3D(llx+(vrMetadataItem.dimensions_x-1)*vrMetadataItem.resolution_x,lly+(vrMetadataItem.dimensions_y-1)*vrMetadataItem.resolution_y,maxz));
    return ret;
}

void BagIO::close()
{
    //mutex.lock();
    //overviewTiles.clear();
    //mutex.unlock();
}


BagIO::MetaData BagIO::getMeta()
{
    MetaData ret;
    QMutexLocker locker(&mutex);
    ret = meta;
    return ret;
}

