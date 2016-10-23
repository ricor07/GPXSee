#include <QFile>
#include <QSet>
#include <QList>
#include "pathitem.h"
#include "waypointitem.h"
#include "ll.h"
#include "data.h"
#include "poi.h"


POI::POI(QObject *parent) : QObject(parent)
{
	_errorLine = 0;
	_radius = 0.01;
}

bool POI::loadFile(const QString &fileName)
{
	Data data;
	FileIndex index;

	_errorString.clear();
	_errorLine = 0;

	index.enabled = true;
	index.start = _data.size();

	if (!data.loadFile(fileName)) {
		_errorString = data.errorString();
		_errorLine = data.errorLine();
		return false;
	}

	for (int i = 0; i < data.waypoints().size(); i++)
		_data.append(data.waypoints().at(i));
	index.end = _data.size() - 1;

	for (int i = index.start; i <= index.end; i++) {
		const QPointF &p = _data.at(i).coordinates();
		qreal c[2];
		c[0] = p.x();
		c[1] = p.y();
		_tree.Insert(c, c, i);
	}

	_files.append(fileName);
	_indexes.append(index);

	emit pointsChanged();

	return true;
}

static bool cb(size_t data, void* context)
{
	QSet<int> *set = (QSet<int>*) context;
	set->insert((int)data);

	return true;
}

QVector<Waypoint> POI::points(const PathItem *path) const
{
	QVector<Waypoint> ret;
	QSet<int> set;
	qreal min[2], max[2];
	const QPainterPath &pp = path->path();

	for (int i = 0; i < pp.elementCount(); i++) {
		QPointF p = mercator2ll(pp.elementAt(i));
		min[0] = p.x() - _radius;
		min[1] = -p.y() - _radius;
		max[0] = p.x() + _radius;
		max[1] = -p.y() + _radius;
		_tree.Search(min, max, cb, &set);
	}

	QSet<int>::const_iterator i = set.constBegin();
	while (i != set.constEnd()) {
		ret.append(_data.at(*i));
		++i;
	}

	return ret;
}

QVector<Waypoint> POI::points(const QList<WaypointItem*> &list)
  const
{
	QVector<Waypoint> ret;
	QSet<int> set;
	qreal min[2], max[2];

	for (int i = 0; i < list.count(); i++) {
		const QPointF &p = list.at(i)->waypoint().coordinates();
		min[0] = p.x() - _radius;
		min[1] = p.y() - _radius;
		max[0] = p.x() + _radius;
		max[1] = p.y() + _radius;
		_tree.Search(min, max, cb, &set);
	}

	QSet<int>::const_iterator i = set.constBegin();
	while (i != set.constEnd()) {
		ret.append(_data.at(*i));
		++i;
	}

	return ret;
}

QVector<Waypoint> POI::points(const QList<Waypoint> &list) const
{
	QVector<Waypoint> ret;
	QSet<int> set;
	qreal min[2], max[2];

	for (int i = 0; i < list.count(); i++) {
		const QPointF &p = list.at(i).coordinates();
		min[0] = p.x() - _radius;
		min[1] = p.y() - _radius;
		max[0] = p.x() + _radius;
		max[1] = p.y() + _radius;
		_tree.Search(min, max, cb, &set);
	}

	QSet<int>::const_iterator i = set.constBegin();
	while (i != set.constEnd()) {
		ret.append(_data.at(*i));
		++i;
	}

	return ret;
}

void POI::enableFile(const QString &fileName, bool enable)
{
	int i;

	i = _files.indexOf(fileName);
	Q_ASSERT(i >= 0);
	_indexes[i].enabled = enable;

	_tree.RemoveAll();
	for (int i = 0; i < _indexes.count(); i++) {
		FileIndex idx = _indexes.at(i);
		if (!idx.enabled)
			continue;

		for (int j = idx.start; j <= idx.end; j++) {
			const QPointF &p = _data.at(j).coordinates();
			qreal c[2];
			c[0] = p.x();
			c[1] = p.y();
			_tree.Insert(c, c, j);
		}
	}

	emit pointsChanged();
}

void POI::clear()
{
	_tree.RemoveAll();
	_data.clear();
	_files.clear();
	_indexes.clear();

	emit pointsChanged();
}

void POI::setRadius(qreal radius)
{
	_radius = radius;

	emit pointsChanged();
}
