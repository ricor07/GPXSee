#include "common/rectc.h"
#include "common/garmin.h"
#include "deltastream.h"
#include "huffmanstream.h"
#include "style.h"
#include "lblfile.h"
#include "netfile.h"
#include "nodfile.h"
#include "rgnfile.h"

using namespace Garmin;
using namespace IMG;

#define MASK(bits) ((1U << (bits)) - 1U)

static quint64 pointId(const QPoint &pos, quint32 type, quint32 labelPtr)
{
	quint64 id;

	uint hash = (uint)qHash(QPair<uint,uint>((uint)qHash(
	  QPair<int, int>(pos.x(), pos.y())), labelPtr));
	id = ((quint64)type)<<32 | hash;

	// Increase rendering priorities for some special items
	if (!Style::isCountry(type) && !Style::isMarina(type))
		id |= 1ULL<<63;

	return id;
}

static double d2m(quint32 val, quint32 flags)
{
	return (flags & 1) ? val / 10.0 : val;
}

RGNFile::~RGNFile()
{
	delete _huffmanTable;
}

bool RGNFile::readRasterInfo(Handle &hdl, const LBLFile *lbl, quint32 size,
  MapData::Poly *poly) const
{
	quint32 id;
	quint32 top, right, bottom, left;

	if (!(lbl && lbl->imageIdSize()))
		return false;
	if (size < lbl->imageIdSize() + 16U)
		return false;

	if (!(readVUInt32(hdl, lbl->imageIdSize(), id)
	  && readUInt32(hdl, top) && readUInt32(hdl, right)
	  && readUInt32(hdl, bottom) && readUInt32(hdl, left)))
		return false;

	poly->raster = Raster(lbl, id, QRect(QPoint(left, top), QPoint(right,
	  bottom)));

	return true;
}

bool RGNFile::readDepthInfo(Handle &hdl, quint8 flags, quint32 size,
  MapData::Point *point) const
{
	quint32 depth = 0;
	quint32 units = (flags >> 3) & 3;
	quint32 val;

	if (!size) {
		depth = flags & 0x3f;
		units = (flags >> 5) & 2;
	} else if (size == 1) {
		if (!readUInt8(hdl, val))
			return false;
		depth = val | ((quint32)flags & 7) << 8;
	} else if (size < 4) {
		Q_ASSERT(!(flags & 4));
		if (!readVUInt32(hdl, size, val))
			return false;
		depth = val | ((quint32)flags & 3) << (size * 8);
	} else
		return false;

	point->label = QString::number(d2m(depth, units));

	return true;
}

bool RGNFile::readObstructionInfo(Handle &hdl, quint8 flags, quint32 size,
  MapData::Point *point) const
{
	quint32 val, rb = size;
	quint32 units = (flags >> 3) & 3;

	if (!size)
		return false;

	if ((flags & 7) == 7) {
		if (!readUInt8(hdl, val))
			return false;
		rb--;
	}
	if (!readVUInt32(hdl, rb, val))
		return false;

	point->label = QString::number(d2m(val, units));

	return true;
}

bool RGNFile::readLabel(Handle &hdl, const LBLFile *lbl, Handle &lblHdl,
  quint8 flags, quint32 size, MapData::Point *point) const
{
	if (!(flags & 1))
		return true;
	if (!lbl)
		return false;

	point->label = lbl->label(lblHdl, this, hdl, size);
	point->classLabel = true;

	return true;
}

bool RGNFile::readClassFields(Handle &hdl, SegmentType segmentType,
  void *object, const LBLFile *lbl, Handle &lblHdl) const
{
	quint8 flags;
	quint32 rs = 0;
	MapData::Poly *poly = (segmentType == Polygon)
	  ? (MapData::Poly *) object : 0;
	MapData::Point *point = (segmentType == Point)
	  ? (MapData::Point *) object : 0;

	if (!readByte(hdl, &flags))
		return false;

	switch (flags >> 5) {
		case 4:
			rs = 1;
			break;
		case 5:
			rs = 2;
			break;
		case 6:
			rs = 3;
			break;
		case 7:
			if (!readVUInt32(hdl, rs))
				return false;
			break;
	}

	quint32 off = pos(hdl);

	if (poly && Style::isRaster(poly->type))
		readRasterInfo(hdl, lbl, rs, poly);
	if (point && Style::isDepthPoint(point->type))
		readDepthInfo(hdl, flags, rs, point);
	if (point && Style::isObstructionPoint(point->type))
		readObstructionInfo(hdl, flags, rs, point);
	if (point && !Style::isMarinePoint(point->type))
		readLabel(hdl, lbl, lblHdl, flags, rs, point);

	return seek(hdl, off + rs);
}

bool RGNFile::skipLclFields(Handle &hdl, const quint32 flags[3]) const
{
	quint32 bitfield = 0xFFFFFFFF;

	if (flags[0] & 0x20000000)
		if (!readVBitfield32(hdl, bitfield))
			return false;

	for (int i = 0, j = 0; i < 29; i++) {
		if ((flags[0] >> i) & 1) {
			if (bitfield & 1) {
				quint32 m = flags[(j >> 4) + 1] >> ((j * 2) & 0x1e) & 3;

				quint32 skip = 0;
				if (m == 3) {
					if (!readVUInt32(hdl, skip))
						return false;
				} else
					skip = m + 1;
				if (!seek(hdl, pos(hdl) + skip))
					return false;
			}
			bitfield >>= 1;
			j++;
		}
	}

	return true;
}

bool RGNFile::skipGblFields(Handle &hdl, quint32 flags) const
{
	int cnt = 0;

	do {
		cnt = cnt + (flags & 3);
		flags = flags >> 2;
	} while (flags != 0);

	return seek(hdl, pos(hdl) + cnt);
}

bool RGNFile::load(Handle &hdl)
{
	quint16 hdrLen;

	if (!(seek(hdl, _gmpOffset) && readUInt16(hdl, hdrLen)
	  && seek(hdl, _gmpOffset + 0x15) && readUInt32(hdl, _base.offset)
	  && readUInt32(hdl, _base.size)))
		return false;

	if (hdrLen >= 0x68) {
		if (!(readUInt32(hdl, _polygons.offset) && readUInt32(hdl, _polygons.size)
		  && seek(hdl, _gmpOffset + 0x29) && readUInt32(hdl, _polygonsGblFlags)
		  && readUInt32(hdl, _polygonsLclFlags[0])
		  && readUInt32(hdl, _polygonsLclFlags[1])
		  && readUInt32(hdl, _polygonsLclFlags[2])
		  && readUInt32(hdl, _lines.offset) && readUInt32(hdl, _lines.size)
		  && seek(hdl, _gmpOffset + 0x45) && readUInt32(hdl, _linesGblFlags)
		  && readUInt32(hdl, _linesLclFlags[0])
		  && readUInt32(hdl, _linesLclFlags[1])
		  && readUInt32(hdl, _linesLclFlags[2])
		  && readUInt32(hdl, _points.offset) && readUInt32(hdl, _points.size)
		  && seek(hdl, _gmpOffset + 0x61) && readUInt32(hdl, _pointsGblFlags)
		  && readUInt32(hdl, _pointsLclFlags[0])
		  && readUInt32(hdl, _pointsLclFlags[1])
		  && readUInt32(hdl, _pointsLclFlags[2])))
			return false;
	}

	if (hdrLen >= 0x7D) {
		quint32 info;
		if (!(seek(hdl, _gmpOffset + 0x71) && readUInt32(hdl, _dict.offset)
		  && readUInt32(hdl, _dict.size) && readUInt32(hdl, info)))
			return false;

		if (_dict.size && _dict.offset && (info & 0x1E)) {
			_huffmanTable = new HuffmanTable(((info >> 1) & 0xF) - 1);
			if (!_huffmanTable->load(this, hdl))
				return false;
		}
	}

	return true;
}

void RGNFile::clear()
{
	delete _huffmanTable;
	_huffmanTable = 0;
}

bool RGNFile::polyObjects(Handle &hdl, const SubDiv *subdiv,
  SegmentType segmentType, const LBLFile *lbl, Handle &lblHdl, NETFile *net,
  Handle &netHdl, QList<MapData::Poly> *polys) const
{
	const SubDiv::Segment &segment = (segmentType == Line)
	 ? subdiv->lines() : subdiv->polygons();

	if (!segment.isValid())
		return true;
	if (!seek(hdl, segment.offset()))
		return false;

	quint32 labelPtr;
	quint8 type, len8, bitstreamInfo;
	qint16 lon, lat;
	quint16 len;

	while (pos(hdl) < segment.end()) {
		MapData::Poly poly;

		if (!(readByte(hdl, &type) && readUInt24(hdl, labelPtr)
		  && readInt16(hdl, lon) && readInt16(hdl, lat)))
			return false;
		if (type & 0x80) {
			if (!readUInt16(hdl, len))
				return false;
		} else {
			if (!readByte(hdl, &len8))
				return false;
			len = len8;
		}
		if (!readByte(hdl, &bitstreamInfo))
			return false;

		poly.type = (segmentType == Polygon)
		  ? ((quint32)(type & 0x7F)) << 8 : ((quint32)(type & 0x3F)) << 8;


		QPoint pos(subdiv->lon() + LS(lon, 24-subdiv->bits()),
		  subdiv->lat() + LS(lat, 24-subdiv->bits()));
		Coordinates c(toWGS24(pos.x()), toWGS24(pos.y()));
		poly.boundingRect = RectC(c, c);
		poly.points.append(QPointF(c.lon(), c.lat()));

		qint32 lonDelta, latDelta;
		DeltaStream stream(*this, hdl, len);
		if (!stream.init(bitstreamInfo, labelPtr & 0x400000, false))
			return false;
		while (stream.readNext(lonDelta, latDelta)) {
			pos.rx() += LS(lonDelta, (24-subdiv->bits()));
			if (pos.rx() >= 0x800000 && subdiv->lon() >= 0)
				pos.rx() = 0x7fffff;
			pos.ry() += LS(latDelta, (24-subdiv->bits()));

			Coordinates c(toWGS24(pos.x()), toWGS24(pos.y()));
			poly.points.append(QPointF(c.lon(), c.lat()));
			poly.boundingRect = poly.boundingRect.united(c);
		}
		if (!(stream.atEnd() && stream.flush()))
			return false;

		if (lbl && (labelPtr & 0x3FFFFF)) {
			if (labelPtr & 0x800000) {
				quint32 lblOff;
				if (net && net->lblOffset(netHdl, labelPtr & 0x3FFFFF, lblOff)
				  && lblOff)
					poly.label = lbl->label(lblHdl, lblOff, false, true,
					  Style::isContourLine(poly.type));
			} else
				poly.label = lbl->label(lblHdl, labelPtr & 0x3FFFFF, false,
				  true, Style::isContourLine(poly.type));
		}

		polys->append(poly);
	}

	return true;
}

bool RGNFile::extPolyObjects(Handle &hdl, const SubDiv *subdiv, quint32 shift,
  SegmentType segmentType, const LBLFile *lbl, Handle &lblHdl,
  QList<MapData::Poly> *polys) const
{
	quint32 labelPtr, len;
	quint8 type, subtype;
	qint16 lon, lat;
	const SubDiv::Segment &segment = (segmentType == Line)
	 ? subdiv->extLines() : subdiv->extPolygons();


	if (!segment.isValid())
		return true;
	if (!seek(hdl, segment.offset()))
		return false;

	while (pos(hdl) < segment.end()) {
		MapData::Poly poly;
		QPoint pos;

		if (!(readByte(hdl, &type) && readByte(hdl, &subtype)
		  && readInt16(hdl, lon) && readInt16(hdl, lat)
		  && readVUInt32(hdl, len)))
			return false;
		Q_ASSERT(SubFile::pos(hdl) + len <= segment.end());

		poly.type = 0x10000 | (quint16(type)<<8) | (subtype & 0x1F);
		labelPtr = 0;

		if (_huffmanTable) {
			pos = QPoint(LS(subdiv->lon(), 8) + LS(lon, 32-subdiv->bits()),
			  LS(subdiv->lat(), 8) + LS(lat, (32-subdiv->bits())));

			qint32 lonDelta, latDelta;
			BitStream4F bs(*this, hdl, len);
			HuffmanDeltaStreamF stream(bs, *_huffmanTable);
			if (!stream.init(segmentType == Line))
				return false;

			if (shift) {
				if (!stream.readOffset(lonDelta, latDelta))
					return false;
				pos = QPoint(pos.x() | LS(lonDelta, 32-subdiv->bits()-shift),
				  pos.y() | LS(latDelta, 32-subdiv->bits()-shift));
			}
			Coordinates c(toWGS32(pos.x()), toWGS32(pos.y()));
			poly.boundingRect = RectC(c, c);
			poly.points.append(QPointF(c.lon(), c.lat()));

			while (stream.readNext(lonDelta, latDelta)) {
				pos.rx() += LS(lonDelta, 32-subdiv->bits()-shift);
				if (pos.rx() < 0 && subdiv->lon() >= 0)
					pos.rx() = 0x7fffffff;
				pos.ry() += LS(latDelta, 32-subdiv->bits()-shift);

				Coordinates c(toWGS32(pos.x()), toWGS32(pos.y()));
				poly.points.append(QPointF(c.lon(), c.lat()));
				poly.boundingRect = poly.boundingRect.united(c);
			}

			if (!(stream.atEnd() && bs.flush()))
				return false;
		} else {
			pos = QPoint(subdiv->lon() + LS(lon, 24-subdiv->bits()),
			  subdiv->lat() + LS(lat, 24-subdiv->bits()));
			Coordinates c(toWGS24(pos.x()), toWGS24(pos.y()));
			poly.boundingRect = RectC(c, c);
			poly.points.append(QPointF(c.lon(), c.lat()));

			quint8 bitstreamInfo;
			if (!readByte(hdl, &bitstreamInfo))
				return false;

			qint32 lonDelta, latDelta;
			DeltaStream stream(*this, hdl, len - 1);
			if (!stream.init(bitstreamInfo, false, true))
				return false;
			while (stream.readNext(lonDelta, latDelta)) {
				pos.rx() += LS(lonDelta, 24-subdiv->bits());
				if (pos.rx() >= 0x800000 && subdiv->lon() >= 0)
					pos.rx() = 0x7fffff;
				pos.ry() += LS(latDelta, 24-subdiv->bits());

				Coordinates c(toWGS24(pos.x()), toWGS24(pos.y()));
				poly.points.append(QPointF(c.lon(), c.lat()));
				poly.boundingRect = poly.boundingRect.united(c);
			}
			if (!(stream.atEnd() && stream.flush()))
				return false;
		}

		if (subtype & 0x20 && !readUInt24(hdl, labelPtr))
			return false;
		if (subtype & 0x80 && !readClassFields(hdl, segmentType, &poly, lbl,
		  lblHdl))
			return false;
		if (subtype & 0x40 && !skipLclFields(hdl, segmentType == Line
		  ? _linesLclFlags : _polygonsLclFlags))
			return false;
		quint32 gblFlags = (segmentType == Line)
		  ? _linesGblFlags : _polygonsGblFlags;
		if (gblFlags && !skipGblFields(hdl, gblFlags))
			return false;

		if (lbl && (labelPtr & 0x3FFFFF))
			poly.label = lbl->label(lblHdl, labelPtr & 0x3FFFFF, false, true,
			  Style::isContourLine(poly.type));

		polys->append(poly);
	}

	return true;
}

bool RGNFile::pointObjects(Handle &hdl, const SubDiv *subdiv,
  SegmentType segmentType, const LBLFile *lbl, Handle &lblHdl,
  QList<MapData::Point> *points) const
{
	const SubDiv::Segment &segment = (segmentType == IndexedPoint)
	 ? subdiv->idxPoints() : subdiv->points();


	if (!segment.isValid())
		return true;
	if (!seek(hdl, segment.offset()))
		return false;

	while (pos(hdl) < segment.end()) {
		MapData::Point point;
		quint8 type, subtype;
		qint16 lon, lat;
		quint32 labelPtr;

		if (!(readByte(hdl, &type) && readUInt24(hdl, labelPtr)
		  && readInt16(hdl, lon) && readInt16(hdl, lat)))
			return false;
		if (labelPtr & 0x800000) {
			if (!readByte(hdl, &subtype))
				return false;
		} else
			subtype = 0;

		QPoint pos(subdiv->lon() + LS(lon, 24-subdiv->bits()),
		  subdiv->lat() + LS(lat, 24-subdiv->bits()));

		point.type = (quint16)type<<8 | subtype;
		point.coordinates = Coordinates(toWGS24(pos.x()), toWGS24(pos.y()));
		point.id = pointId(pos, point.type, labelPtr & 0x3FFFFF);
		if (lbl && (labelPtr & 0x3FFFFF))
			point.label = lbl->label(lblHdl, labelPtr & 0x3FFFFF,
			  labelPtr & 0x400000, !(Style::isCountry(point.type)
			  || Style::isState(point.type)), Style::isSpot(point.type));

		points->append(point);
	}

	return true;
}

bool RGNFile::extPointObjects(Handle &hdl, const SubDiv *subdiv,
  const LBLFile *lbl, Handle &lblHdl, QList<MapData::Point> *points) const
{
	const SubDiv::Segment &segment = subdiv->extPoints();


	if (!segment.isValid())
		return true;
	if (!seek(hdl, segment.offset()))
		return false;

	while (pos(hdl) < segment.end()) {
		MapData::Point point;
		qint16 lon, lat;
		quint8 type, subtype;
		quint32 labelPtr = 0;

		if (!(readByte(hdl, &type) && readByte(hdl, &subtype)
		  && readInt16(hdl, lon) && readInt16(hdl, lat)))
			return false;

		point.type = 0x10000 | (((quint32)type)<<8) | (subtype & 0x1F);

		if (subtype & 0x20 && !readUInt24(hdl, labelPtr))
			return false;
		if (subtype & 0x80 && !readClassFields(hdl, Point, &point, lbl, lblHdl))
			return false;
		if (subtype & 0x40 && !skipLclFields(hdl, _pointsLclFlags))
			return false;
		if (_pointsGblFlags && !skipGblFields(hdl, _pointsGblFlags))
			return false;

		QPoint p(subdiv->lon() + LS(lon, 24-subdiv->bits()),
		  subdiv->lat() + LS(lat, 24-subdiv->bits()));

		// Discard NT points breaking style draw order logic (and causing huge
		// performance drawback)
		if (point.type == 0x11400)
			continue;

		point.coordinates = Coordinates(toWGS24(p.x()), toWGS24(p.y()));
		point.id = pointId(p, point.type, point.classLabel ? pos(hdl)
		  : labelPtr & 0x3FFFFF);
		if (lbl && (labelPtr & 0x3FFFFF))
			point.label = lbl->label(lblHdl, labelPtr & 0x3FFFFF);

		points->append(point);
	}

	return true;
}

bool RGNFile::links(Handle &hdl, const SubDiv *subdiv, quint32 shift,
  const NETFile *net, Handle &netHdl, const NODFile *nod, Handle &nodHdl,
  Handle &nodHdl2, const LBLFile *lbl, Handle &lblHdl,
  QList<MapData::Poly> *lines) const
{
	quint32 size, blockIndexId;
	quint8 flags;
	const SubDiv::Segment &segment = subdiv->roadReferences();

	if (!net || !nod)
		return false;
	if (!segment.isValid())
		return true;
	if (!seek(hdl, segment.offset()))
		return false;

	while (pos(hdl) < segment.end()) {
		if (!readVUInt32(hdl, size))
			return false;

		quint32 entryStart = pos(hdl);

		if (!(readByte(hdl, &flags) && readVUInt32(hdl, nod->indexIdSize(),
		  blockIndexId)))
			return false;

		quint8 bits[3];
		for (int i = 0; i < 3; i++)
			bits[i] = 0x4000a08 >> (((flags >> (2*i) & 3) << 3) ^ 0x10);
		quint8 byteSize = bs(bits[0] + bits[1] + bits[2]);

		quint32 counts;
		if (!readVUInt32(hdl, byteSize, counts))
			return false;

		quint16 b8 = bits[0] ? (MASK(bits[0]) & counts) + 1 : 0;
		quint16 b10 = bits[1] ? (MASK(bits[1]) & (counts >> bits[0])) + 1 : 0;
		quint16 b16 = bits[2] ? (MASK(bits[2]) & (counts >> (bits[0] + bits[1])))
		  + 1 : 0;

		NODFile::BlockInfo blockInfo;
		if (!nod->blockInfo(nodHdl, blockIndexId, blockInfo))
			return false;

		quint8 linkId, lineId;
		for (int i = 0; i < b8 + b10 + b16; i++) {
			if (!b8 || b8 <= i) {
				quint16 v16;
				if (!readUInt16(hdl, v16))
					return false;

				if (!b16 || b8 + b16 <= i) {
					int shift = ((i - (b8 + b16)) * 10) & 7;
					linkId = (quint8)(v16 >> shift);
					lineId = (((v16 >> shift) >> 8) & 3) + 1;

					if (shift < 6 && i < b8 + b10 + b16 - 1)
						seek(hdl, pos(hdl) - 1);
				} else {
					linkId = (quint8)v16;
					lineId = v16 >> 8;
					Q_ASSERT(lineId > 4);
				}
			} else {
				if (!readByte(hdl, &linkId))
					return false;
				lineId = 0;
			}

			net->link(subdiv, shift, netHdl, nod, nodHdl, nodHdl2, lbl, lblHdl,
			  blockInfo, linkId, lineId, lines);
		}

		if (entryStart + size != pos(hdl))
			return false;
	}

	return true;
}

bool RGNFile::segments(Handle &hdl, SubDiv *subdiv, SubDiv::Segment seg[5]) const
{
	if (subdiv->offset() == subdiv->end() || !(subdiv->objects() & 0x1F))
		return true;

	quint32 offset = _base.offset + subdiv->offset();
	if (!seek(hdl, offset))
		return false;

	int no = 0;
	for (int i = 0; i < 5; i++)
		if (subdiv->objects() & (1<<i))
			no++;

	quint32 start = offset + 2 * (no - 1);
	quint32 ls = 0;
	int lt = 0;

	for (int i = 0; i < 5; i++) {
		if (subdiv->objects() & (1<<i)) {
			if (ls) {
				quint16 po;
				if (!readUInt16(hdl, po) || !po)
					return false;
				start = offset + po;
				seg[lt] = SubDiv::Segment(ls, start);
			}

			lt = i;
			ls = start;
		}
	}

	seg[lt] = SubDiv::Segment(ls,
	  subdiv->end() ? _base.offset + subdiv->end() : _base.offset + _base.size);

	return true;
}

bool RGNFile::subdivInit(Handle &hdl, SubDiv *subdiv) const
{
	SubDiv::Segment std[5], extPoints, extLines, extPolygons;

	if (!segments(hdl, subdiv, std))
		return false;

	if (subdiv->extPointsOffset() != subdiv->extPointsEnd()) {
		quint32 start = _points.offset + subdiv->extPointsOffset();
		quint32 end = subdiv->extPointsEnd()
		  ? _points.offset + subdiv->extPointsEnd()
		  : _points.offset + _points.size;
		extPoints = SubDiv::Segment(start, end);
	}
	if (subdiv->extPolygonsOffset() != subdiv->extPolygonsEnd()) {
		quint32 start = _polygons.offset + subdiv->extPolygonsOffset();
		quint32 end = subdiv->extPolygonsEnd()
		  ? _polygons.offset + subdiv->extPolygonsEnd()
		  : _polygons.offset + _polygons.size;
		extPolygons = SubDiv::Segment(start, end);
	}
	if (subdiv->extLinesOffset() != subdiv->extLinesEnd()) {
		quint32 start = _lines.offset + subdiv->extLinesOffset();
		quint32 end = subdiv->extLinesEnd()
		  ? _lines.offset + subdiv->extLinesEnd()
		  : _lines.offset + _lines.size;
		extLines = SubDiv::Segment(start, end);
	}

	subdiv->init(std[Point], std[IndexedPoint], std[Line], std[Polygon],
	  std[RoadReference], extPoints, extLines, extPolygons);

	return true;
}
